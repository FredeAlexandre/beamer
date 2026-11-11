// SPDX-License-Identifier: GPL-3.0-only
// C2 prototype consumer: what an app sees of "Beamer Camera".
//   camprobe list                              MF (MFEnumDeviceSources) and DirectShow enumeration
//   camprobe read [--name S] [--seconds N] [--size WxH] [--subtype nv12|yuy2|any] [--rgb] [--tag T] [--per-second]
// read opens the camera through an IMFSourceReader (as Chromium, Teams and the Camera app do), decodes the
// stamp of every frame (common.h) and reports frames, gaps, origins and latency = now - stamp time.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <dshow.h>
#include <io.h>
#include <wrl/client.h>
#include <algorithm>
#include <string>
#include <vector>
#include "common.h"

using Microsoft::WRL::ComPtr;
using namespace c2;

static std::string narrow(const wchar_t* w) {
  char b[1024];
  WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
  std::string s = b;
  for (auto& c : s)
    if (c == '\\' || c == '"') c = '/';
  return s;
}

static int list() {
  ComPtr<IMFAttributes> a;
  MFCreateAttributes(&a, 1);
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devs = nullptr;
  UINT32 n = 0;
  HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
  printf("{\"mf_hr\":\"0x%08lx\",\"mf\":[", (unsigned long)hr);
  for (UINT32 i = 0; i < n; i++) {
    LPWSTR name = nullptr, link = nullptr;
    UINT32 l;
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &l);
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &l);
    printf("%s{\"name\":\"%s\",\"link\":\"%s\"}", i ? "," : "", name ? narrow(name).c_str() : "", link ? narrow(link).c_str() : "");
    CoTaskMemFree(name);
    CoTaskMemFree(link);
    devs[i]->Release();
  }
  CoTaskMemFree(devs);
  printf("],\"dshow\":[");
  ComPtr<ICreateDevEnum> de;
  ComPtr<IEnumMoniker> em;
  int k = 0;
  if (SUCCEEDED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&de))) &&
      de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0) == S_OK) {
    ComPtr<IMoniker> m;
    while (em->Next(1, &m, nullptr) == S_OK) {
      ComPtr<IPropertyBag> pb;
      if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&pb)))) {
        VARIANT v;
        VariantInit(&v);
        std::string nm, path, clsid;
        if (SUCCEEDED(pb->Read(L"FriendlyName", &v, nullptr))) nm = narrow(v.bstrVal);
        VariantClear(&v);
        if (SUCCEEDED(pb->Read(L"DevicePath", &v, nullptr))) path = narrow(v.bstrVal);
        VariantClear(&v);
        if (SUCCEEDED(pb->Read(L"CLSID", &v, nullptr))) clsid = narrow(v.bstrVal);
        VariantClear(&v);
        printf("%s{\"name\":\"%s\",\"path\":\"%s\",\"clsid\":\"%s\"}", k++ ? "," : "", nm.c_str(), path.c_str(), clsid.c_str());
      }
      m.Reset();
    }
  }
  printf("]}\n");
  return 0;
}

static double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

static int read(int argc, wchar_t** argv) {
  std::wstring want = L"Beamer Camera", tag = L"";
  double seconds = 10;
  UINT32 ww = 0, wh = 0;
  std::wstring sub = L"any";
  bool rgb = false, per_second = false, no_set = false;
  for (int i = 2; i < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--name" && i + 1 < argc) want = argv[++i];
    else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
    else if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%ux%u", &ww, &wh);
    else if (a == L"--subtype" && i + 1 < argc) sub = argv[++i];
    else if (a == L"--tag" && i + 1 < argc) tag = argv[++i];
    else if (a == L"--rgb") rgb = true;
    else if (a == L"--per-second") per_second = true;
    else if (a == L"--no-set") no_set = true;  // keep whatever type the device has (a shared client)
  }
  std::string tg = narrow(tag.c_str());
  int64_t t_begin = unix_us();
  ComPtr<IMFAttributes> a;
  MFCreateAttributes(&a, 1);
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devs = nullptr;
  UINT32 n = 0;
  HRESULT hr = MFEnumDeviceSources(a.Get(), &devs, &n);
  ComPtr<IMFActivate> dev;
  for (UINT32 i = 0; i < n; i++) {
    LPWSTR name = nullptr;
    UINT32 l;
    devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &l);
    if (name && wcsstr(name, want.c_str()) && !dev) dev = devs[i];
    CoTaskMemFree(name);
    devs[i]->Release();
  }
  CoTaskMemFree(devs);
  if (!dev) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"not found\",\"devices\":%u}\n", tg.c_str(), n);
    return 1;
  }
  int64_t t_enum = unix_us();
  ComPtr<IMFMediaSource> src;
  hr = dev->ActivateObject(IID_PPV_ARGS(&src));
  int64_t t_activate = unix_us();
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"ActivateObject\",\"hr\":\"0x%08lx\"}\n", tg.c_str(), (unsigned long)hr);
    return 1;
  }
  ComPtr<IMFAttributes> ra;
  MFCreateAttributes(&ra, 2);
  if (rgb) ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> rd;
  hr = MFCreateSourceReaderFromMediaSource(src.Get(), ra.Get(), &rd);
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"reader\",\"hr\":\"0x%08lx\"}\n", tg.c_str(), (unsigned long)hr);
    return 1;
  }
  // list native types, pick the requested one
  std::string types;
  ComPtr<IMFMediaType> pick;
  for (DWORD i = 0;; i++) {
    ComPtr<IMFMediaType> t;
    if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t))) break;
    GUID g;
    UINT32 w, h, fn, fd;
    t->GetGUID(MF_MT_SUBTYPE, &g);
    MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
    MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &fn, &fd);
    const char* s = g == MFVideoFormat_NV12 ? "NV12" : g == MFVideoFormat_YUY2 ? "YUY2" : "other";
    char b[64];
    snprintf(b, sizeof b, "%s\"%s %ux%u@%u/%u\"", types.empty() ? "" : ",", s, w, h, fn, fd);
    types += b;
    bool sub_ok = sub == L"any" || (sub == L"nv12" && g == MFVideoFormat_NV12) || (sub == L"yuy2" && g == MFVideoFormat_YUY2);
    if (!pick && sub_ok && (ww == 0 || (w == ww && h == wh))) pick = t;
  }
  if (!pick) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"no matching type\",\"types\":[%s]}\n", tg.c_str(), types.c_str());
    return 1;
  }
  hr = no_set ? S_OK : rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pick.Get());
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"SetCurrentMediaType\",\"hr\":\"0x%08lx\"}\n", tg.c_str(), (unsigned long)hr);
    return 1;
  }
  if (rgb) {
    ComPtr<IMFMediaType> o;
    MFCreateMediaType(&o);
    o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, o.Get());
    if (FAILED(hr)) {
      printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"RGB32 output\",\"hr\":\"0x%08lx\"}\n", tg.c_str(), (unsigned long)hr);
      return 1;
    }
  }
  ComPtr<IMFMediaType> cur;
  rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
  GUID cg;
  UINT32 W, H;
  cur->GetGUID(MF_MT_SUBTYPE, &cg);
  MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &W, &H);
  INT32 stride = (INT32)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0);
  int bpp = cg == MFVideoFormat_RGB32 ? 4 : cg == MFVideoFormat_YUY2 ? 2 : 1;
  if (stride == 0) stride = (INT32)W * bpp;

  std::vector<double> lat, sec_lat;
  uint64_t frames = 0, bad = 0, gaps = 0, dups = 0, origin[4] = {0, 0, 0, 0}, sec_frames = 0;
  int64_t last_counter = -1, t_first = 0, t_end = t_begin + (int64_t)(seconds * 1e6), next_sec = 0;
  uint8_t last_origin = 0;
  int64_t t_read0 = unix_us();
  while (unix_us() < t_end) {
    DWORD flags = 0, si;
    LONGLONG ts;
    ComPtr<IMFSample> s;
    hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &ts, &s);
    int64_t now = unix_us();
    if (FAILED(hr)) {
      printf("{\"tag\":\"%s\",\"event\":\"read_failed\",\"hr\":\"0x%08lx\",\"t\":%lld}\n", tg.c_str(), (unsigned long)hr, (long long)now);
      break;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      printf("{\"tag\":\"%s\",\"event\":\"reader_flags\",\"flags\":\"0x%lx\",\"t\":%lld}\n", tg.c_str(), flags, (long long)now);
      break;
    }
    if (!s) continue;
    if (!t_first) t_first = now;
    frames++;
    sec_frames++;
    ComPtr<IMFMediaBuffer> b;
    s->ConvertToContiguousBuffer(&b);
    BYTE* p;
    DWORD len;
    b->Lock(&p, nullptr, &len);
    BYTE* base = stride < 0 ? p + (size_t)(-stride) * (H - 1) : p;
    auto luma = [&](int x, int y) -> int {
      const BYTE* row = base + (ptrdiff_t)stride * y;
      if (bpp == 4) return (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
      if (bpp == 2) return row[x * 2];
      return row[x];
    };
    uint32_t c, tlo;
    uint8_t org;
    if (unstamp((int)W, (int)H, luma, &c, &tlo, &org)) {
      double l = (double)(uint32_t)((uint32_t)unix_us() - tlo) / 1000.0;
      if (org == kOriginHelper || org == kOriginPattern) {
        lat.push_back(l);
        sec_lat.push_back(l);
      }
      if (last_counter >= 0 && org == last_origin && c == (uint32_t)last_counter) dups++;
      else if (last_counter >= 0 && org == last_origin && c != (uint32_t)(last_counter + 1)) gaps++;
      last_counter = c;
      if (org != last_origin)
        printf("{\"tag\":\"%s\",\"event\":\"origin\",\"origin\":%u,\"t\":%lld,\"counter\":%u}\n", tg.c_str(), org, (long long)now, c);
      last_origin = org;
      if (org < 4) origin[org]++;
    } else {
      bad++;
    }
    b->Unlock();
    if (per_second && now >= next_sec) {
      if (next_sec)
        printf("{\"tag\":\"%s\",\"t\":%lld,\"fps\":%llu,\"lat_p50\":%.2f,\"origin\":%u}\n", tg.c_str(), (long long)now, sec_frames,
               pct(sec_lat, 0.5), last_origin);
      fflush(stdout);
      sec_frames = 0;
      sec_lat.clear();
      next_sec = now + 1000000;
    }
  }
  int64_t t_stop = unix_us();
  rd.Reset();
  src->Shutdown();
  src.Reset();
  dev->ShutdownObject();
  int64_t t_closed = unix_us();
  double dur = (t_stop - (t_first ? t_first : t_stop)) / 1e6;
  printf(
      "{\"tag\":\"%s\",\"ok\":true,\"type\":\"%s %ux%u\",\"types\":[%s],\"frames\":%llu,\"fps\":%.2f,\"unstamped\":%llu,\"gaps\":%llu,\"dups\":%llu,"
      "\"origin_pattern\":%llu,\"origin_placeholder\":%llu,\"origin_helper\":%llu,\"lat_ms_p50\":%.2f,\"lat_ms_p95\":%.2f,\"lat_ms_max\":%.2f,"
      "\"t_begin\":%lld,\"enum_ms\":%.1f,\"activate_ms\":%.1f,\"first_frame_ms\":%.1f,\"t_read0\":%lld,\"t_first\":%lld,\"t_stop\":%lld,\"t_closed\":%lld}\n",
      tg.c_str(), cg == MFVideoFormat_NV12 ? "NV12" : cg == MFVideoFormat_YUY2 ? "YUY2" : cg == MFVideoFormat_RGB32 ? "RGB32" : "other", W, H,
      types.c_str(), frames, dur > 0 ? (frames - 1) / dur : 0.0, bad, gaps, dups, origin[1], origin[2], origin[3], pct(lat, 0.5), pct(lat, 0.95),
      pct(lat, 1.0), (long long)t_begin, (t_enum - t_begin) / 1000.0, (t_activate - t_enum) / 1000.0, t_first ? (t_first - t_read0) / 1000.0 : -1.0,
      (long long)t_read0, (long long)t_first, (long long)t_stop, (long long)t_closed);
  return 0;
}

// Reads raw NV12 frames (file or "-" = stdin, e.g. piped from ffmpeg -f dshow) and checks the stamps live.
static int yuvcheck(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  int W = 0, H = 0;
  swscanf_s(argv[3], L"%dx%d", &W, &H);
  std::string tg = argc > 4 ? narrow(argv[4]) : "";
  FILE* f = wcscmp(argv[2], L"-") ? _wfopen(argv[2], L"rb") : stdin;
  if (!f) return 1;
  if (f == stdin) _setmode(_fileno(stdin), 0x8000 /*_O_BINARY*/);
  std::vector<uint8_t> fr((size_t)W * H * 3 / 2);
  std::vector<double> lat;
  uint64_t frames = 0, bad = 0, gaps = 0, origin[4] = {0, 0, 0, 0};
  int64_t last = -1, t_first = 0, t_last = 0;
  while (fread(fr.data(), 1, fr.size(), f) == fr.size()) {
    int64_t now = unix_us();
    if (!t_first) t_first = now;
    t_last = now;
    frames++;
    uint32_t c, tlo;
    uint8_t org;
    if (unstamp(W, H, [&](int x, int y) { return (int)fr[(size_t)y * W + x]; }, &c, &tlo, &org)) {
      lat.push_back((double)(uint32_t)((uint32_t)now - tlo) / 1000.0);
      if (last >= 0 && c != (uint32_t)(last + 1)) gaps++;
      last = c;
      if (org < 4) origin[org]++;
    } else {
      bad++;
    }
  }
  double dur = (t_last - t_first) / 1e6;
  printf("{\"tag\":\"%s\",\"frames\":%llu,\"fps\":%.2f,\"unstamped\":%llu,\"gaps\":%llu,\"origin_pattern\":%llu,\"origin_placeholder\":%llu,"
         "\"origin_helper\":%llu,\"lat_ms_p50\":%.2f,\"lat_ms_p95\":%.2f,\"lat_ms_max\":%.2f}\n",
         tg.c_str(), frames, dur > 0 ? (frames - 1) / dur : 0.0, bad, gaps, origin[1], origin[2], origin[3], pct(lat, 0.5), pct(lat, 0.95), pct(lat, 1.0));
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && !wcscmp(argv[1], L"list")) return list();
  if (argc >= 2 && !wcscmp(argv[1], L"read")) return read(argc, argv);
  if (argc >= 2 && !wcscmp(argv[1], L"yuvcheck")) return yuvcheck(argc, argv);
  printf("usage: camprobe list | read [...]\n");
  return 2;
}
