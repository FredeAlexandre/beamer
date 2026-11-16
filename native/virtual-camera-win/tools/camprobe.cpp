// SPDX-License-Identifier: GPL-3.0-only
// What an app sees of "Beamer Camera" (port of C2's proto/win/camprobe.cpp, + C1's code strip, + a classifier
// of every frame for the source's acceptance tests). JSON lines on stdout (t = unix µs).
//
//   camprobe list                              MF (MFEnumDeviceSources) and DirectShow enumeration
//   camprobe read [--name S] [--seconds N] [--size WxH] [--rgb] [--no-set] [--tag T] [--per-second]
//                 [--strip c2|c1] [--events] [--dump FILE]
//       Opens the camera through an IMFSourceReader (as Chromium, Teams and the Camera app do) and classifies
//       each frame: `helper` (C2 stamp origin 3: a live picture from feeder/shmfuzz; latency = now - stamp),
//       `slot3` (C2 stamp origin 2: feeder's waiting picture), `c1` (C1 code strip, --strip c1), `builtin` (the
//       DLL's built-in image, compared with builtin_image.h's rendering outside the dot), `uniform` (one luma
//       value everywhere but the dot: a fuzzed slot that passed validation) and `other` (none of these).
//       --events prints every class change; --dump writes "t_us class counter_or_frame ts" per frame.
//   camprobe yuvcheck FILE|- WxH [TAG]         raw NV12 frames (e.g. ffmpeg -f dshow ... -f rawvideo -) classified the same way
#include "tool_common.h"
#include "../builtin_image.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <dshow.h>
#include <io.h>
#include <wrl/client.h>
#include <map>

using Microsoft::WRL::ComPtr;
using namespace tools;

namespace {

enum Class { kHelper, kSlot3, kC1, kBuiltin, kUniform, kOther, kClassCount };
const char* kClassName[] = {"helper", "slot3", "c1", "builtin", "uniform", "other"};

struct Frame {
  int w, h;
  int bpp;   // 1 = NV12/luma plane, 2 = YUY2, 4 = RGB32
  const uint8_t* base;
  ptrdiff_t stride;
  int luma(int x, int y) const {
    const uint8_t* row = base + stride * y;
    if (bpp == 4) return (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
    if (bpp == 2) return row[x * 2];
    return row[x];
  }
};

// The DLL's built-in image at a size (luma only), for comparison.
const std::vector<uint8_t>& builtin_ref(int w, int h) {
  static std::map<int64_t, std::vector<uint8_t>> cache;
  auto& v = cache[((int64_t)w << 32) | h];
  if (v.empty()) beamer::vcam::builtin::render_nv12(w, h, v);
  return v;
}

struct Result {
  Class cls;
  uint32_t counter = 0, time_lo = 0;
  int uniform_value = -1;
};

Result classify(const Frame& f, bool c1) {
  Result r;
  auto L = [&](int x, int y) { return f.luma(x, y); };
  uint8_t origin;
  if (unstamp(f.w, f.h, L, &r.counter, &r.time_lo, &origin)) {
    if (origin == kOriginHelper) return r.cls = kHelper, r;
    if (origin == kOriginPlaceholder) return r.cls = kSlot3, r;
  }
  if (c1 && read_c1(f.w, f.h, L, &r.counter, &r.time_lo)) return r.cls = kC1, r;
  // outside the dot band (the dot moves over builtin and slot-3 pictures)
  int dr = beamer::vcam::builtin::dot_radius(f.h), dy = (int)(f.h * beamer::vcam::builtin::kDotY);
  auto in_band = [&](int y) { return y >= dy - dr - 3 && y <= dy + dr + 3; };
  if (f.bpp == 1) {
    const auto& ref = builtin_ref(f.w, f.h);
    long long diff = 0, n = 0;
    for (int y = 1; y < f.h; y += 4) {
      if (in_band(y)) continue;
      for (int x = 1; x < f.w; x += 4) {
        diff += std::abs(f.luma(x, y) - (int)ref[(size_t)y * f.w + x]);
        n++;
      }
    }
    if (n && diff / (double)n < 1.5) return r.cls = kBuiltin, r;
  }
  int hist[256] = {0}, n = 0;
  for (int y = 2; y < f.h; y += 7) {
    if (in_band(y)) continue;
    for (int x = 3; x < f.w; x += 7) {
      hist[f.luma(x, y)]++;
      n++;
    }
  }
  int mode = 0;
  for (int i = 1; i < 256; i++)
    if (hist[i] > hist[mode]) mode = i;
  int around = 0;
  for (int i = std::max(0, mode - 2); i <= std::min(255, mode + 2); i++) around += hist[i];
  if (n && around >= n * 99 / 100) return r.cls = kUniform, r.uniform_value = mode, r;
  return r.cls = kOther, r;
}

struct Stats {
  uint64_t count[kClassCount] = {0}, frames = 0, gaps = 0, dups = 0, unread = 0;
  std::map<int, uint64_t> uniform_values;
  std::vector<double> lat_helper, lat_slot3;
  int64_t last_counter = -1;
  Class last_cls = kClassCount;
  void add(const Result& r, int64_t now, bool events, const std::string& tag) {
    frames++;
    count[r.cls]++;
    if (r.cls == kUniform) uniform_values[r.uniform_value]++;
    if (r.cls == kHelper || r.cls == kSlot3) {
      double l = (double)(uint32_t)((uint32_t)now - r.time_lo) / 1000.0;
      (r.cls == kHelper ? lat_helper : lat_slot3).push_back(l);
    }
    if (r.cls == kHelper || r.cls == kC1) {
      if (last_counter >= 0 && r.cls == last_cls && r.counter == (uint32_t)last_counter) dups++;
      else if (last_counter >= 0 && r.cls == last_cls && r.counter != (uint32_t)(last_counter + 1)) gaps++;
      last_counter = r.counter;
    }
    if (events && r.cls != last_cls)
      printf("{\"tag\":\"%s\",\"event\":\"class\",\"class\":\"%s\",\"t\":%lld,\"counter\":%u}\n", tag.c_str(), kClassName[r.cls], (long long)now, r.counter);
    last_cls = r.cls;
  }
  std::string json() const {
    std::string s;
    char b[160];
    for (int c = 0; c < kClassCount; c++) {
      snprintf(b, sizeof b, "%s\"%s\":%llu", c ? "," : "", kClassName[c], (unsigned long long)count[c]);
      s += b;
    }
    s += ",\"uniform_values\":{";
    bool first = true;
    for (auto& [v, c] : uniform_values) {
      snprintf(b, sizeof b, "%s\"%d\":%llu", first ? "" : ",", v, (unsigned long long)c);
      s += b;
      first = false;
    }
    snprintf(b, sizeof b, "},\"gaps\":%llu,\"dups\":%llu", (unsigned long long)gaps, (unsigned long long)dups);
    s += b;
    snprintf(b, sizeof b, ",\"lat_ms_p50\":%.3f,\"lat_ms_p95\":%.3f,\"lat_ms_max\":%.3f", pct(lat_helper, 0.5), pct(lat_helper, 0.95), pct(lat_helper, 1.0));
    s += b;
    snprintf(b, sizeof b, ",\"slot3_lat_ms_p50\":%.3f,\"slot3_lat_ms_p95\":%.3f", pct(lat_slot3, 0.5), pct(lat_slot3, 0.95));
    s += b;
    return s;
  }
};

int list() {
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
        std::string nm;
        if (SUCCEEDED(pb->Read(L"FriendlyName", &v, nullptr))) nm = narrow(v.bstrVal);
        VariantClear(&v);
        printf("%s{\"name\":\"%s\"}", k++ ? "," : "", nm.c_str());
      }
      m.Reset();
    }
  }
  printf("]}\n");
  return 0;
}

int read(int argc, wchar_t** argv) {
  std::wstring want = vc::kFriendlyName, tagw, dump;
  double seconds = 10;
  UINT32 ww = 0, wh = 0;
  bool rgb = false, per_second = false, no_set = false, c1 = false, events = false;
  for (int i = 2; i < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--name" && i + 1 < argc) want = argv[++i];
    else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
    else if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%ux%u", &ww, &wh);
    else if (a == L"--tag" && i + 1 < argc) tagw = argv[++i];
    else if (a == L"--strip" && i + 1 < argc) c1 = !wcscmp(argv[++i], L"c1");
    else if (a == L"--dump" && i + 1 < argc) dump = argv[++i];
    else if (a == L"--rgb") rgb = true;
    else if (a == L"--per-second") per_second = true;
    else if (a == L"--no-set") no_set = true;
    else if (a == L"--events") events = true;
  }
  std::string tag = narrow(tagw);
  FILE* df = dump.empty() ? nullptr : _wfopen(dump.c_str(), L"w");
  int64_t t_begin = unix_us();
  ComPtr<IMFAttributes> a;
  MFCreateAttributes(&a, 1);
  a->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
  IMFActivate** devs = nullptr;
  UINT32 n = 0;
  MFEnumDeviceSources(a.Get(), &devs, &n);
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
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"not found\",\"devices\":%u}\n", tag.c_str(), n);
    return 1;
  }
  int64_t t_enum = unix_us();
  ComPtr<IMFMediaSource> src;
  HRESULT hr = dev->ActivateObject(IID_PPV_ARGS(&src));
  int64_t t_activate = unix_us();
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"ActivateObject\",\"hr\":\"0x%08lx\"}\n", tag.c_str(), (unsigned long)hr);
    return 1;
  }
  ComPtr<IMFAttributes> ra;
  MFCreateAttributes(&ra, 2);
  if (rgb) ra->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
  ComPtr<IMFSourceReader> rd;
  hr = MFCreateSourceReaderFromMediaSource(src.Get(), ra.Get(), &rd);
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"reader\",\"hr\":\"0x%08lx\"}\n", tag.c_str(), (unsigned long)hr);
    return 1;
  }
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
    char b[64];
    snprintf(b, sizeof b, "%s\"%s %ux%u@%u/%u\"", types.empty() ? "" : ",", g == MFVideoFormat_NV12 ? "NV12" : g == MFVideoFormat_YUY2 ? "YUY2" : "other", w,
             h, fn, fd);
    types += b;
    if (!pick && (ww == 0 || (w == ww && h == wh))) pick = t;
  }
  if (!pick) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"no matching type\",\"types\":[%s]}\n", tag.c_str(), types.c_str());
    return 1;
  }
  hr = no_set ? S_OK : rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pick.Get());
  if (SUCCEEDED(hr) && rgb) {
    ComPtr<IMFMediaType> o;
    MFCreateMediaType(&o);
    o->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    o->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, o.Get());
  }
  if (FAILED(hr)) {
    printf("{\"tag\":\"%s\",\"ok\":false,\"error\":\"SetCurrentMediaType\",\"hr\":\"0x%08lx\"}\n", tag.c_str(), (unsigned long)hr);
    return 1;
  }
  ComPtr<IMFMediaType> cur;
  rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
  GUID cg;
  UINT32 W, H;
  cur->GetGUID(MF_MT_SUBTYPE, &cg);
  MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &W, &H);
  int bpp = cg == MFVideoFormat_RGB32 ? 4 : cg == MFVideoFormat_YUY2 ? 2 : 1;
  INT32 stride = (INT32)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, 0);
  if (stride == 0) stride = (INT32)W * bpp;

  Stats st, sec;
  int64_t t_first = 0, t_end = t_begin + (int64_t)(seconds * 1e6), next_sec = 0;
  int64_t t_read0 = unix_us();
  bool failed = false;
  while (unix_us() < t_end) {
    DWORD flags = 0, si;
    LONGLONG ts;
    ComPtr<IMFSample> s;
    hr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &ts, &s);
    int64_t now = unix_us();
    if (FAILED(hr)) {
      printf("{\"tag\":\"%s\",\"event\":\"read_failed\",\"hr\":\"0x%08lx\",\"t\":%lld}\n", tag.c_str(), (unsigned long)hr, (long long)now);
      failed = true;
      break;
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
      printf("{\"tag\":\"%s\",\"event\":\"reader_flags\",\"flags\":\"0x%lx\",\"t\":%lld}\n", tag.c_str(), flags, (long long)now);
      failed = true;
      break;
    }
    if (!s) continue;
    if (!t_first) t_first = now;
    ComPtr<IMFMediaBuffer> b;
    s->ConvertToContiguousBuffer(&b);
    BYTE* p;
    DWORD len;
    b->Lock(&p, nullptr, &len);
    Frame f{(int)W, (int)H, bpp, stride < 0 ? p + (size_t)(-stride) * (H - 1) : p, stride};
    Result r = classify(f, c1);
    b->Unlock();
    st.add(r, now, events, tag);
    if (per_second) sec.add(r, now, false, tag);
    if (df) fprintf(df, "%lld %s %u %u\n", (long long)now, kClassName[r.cls], r.counter, r.time_lo);
    if (per_second && now >= next_sec) {
      if (next_sec) printf("{\"tag\":\"%s\",\"t\":%lld,\"frames\":%llu,%s}\n", tag.c_str(), (long long)now, (unsigned long long)sec.frames, sec.json().c_str());
      sec = Stats();
      next_sec = now + 1000000;
    }
  }
  int64_t t_stop = unix_us();
  rd.Reset();
  src->Shutdown();
  src.Reset();
  dev->ShutdownObject();
  int64_t t_closed = unix_us();
  if (df) fclose(df);
  double dur = (t_stop - (t_first ? t_first : t_stop)) / 1e6;
  printf("{\"tag\":\"%s\",\"ok\":%s,\"type\":\"%s %ux%u\",\"types\":[%s],\"frames\":%llu,\"fps\":%.2f,%s,\"enum_ms\":%.1f,\"activate_ms\":%.1f,"
         "\"first_frame_ms\":%.1f,\"t_begin\":%lld,\"t_first\":%lld,\"t_stop\":%lld,\"t_closed\":%lld}\n",
         tag.c_str(), failed ? "false" : "true", cg == MFVideoFormat_NV12 ? "NV12" : cg == MFVideoFormat_YUY2 ? "YUY2" : cg == MFVideoFormat_RGB32 ? "RGB32" : "other",
         W, H, types.c_str(), (unsigned long long)st.frames, dur > 0 ? (st.frames - 1) / dur : 0.0, st.json().c_str(), (t_enum - t_begin) / 1000.0,
         (t_activate - t_enum) / 1000.0, t_first ? (t_first - t_read0) / 1000.0 : -1.0, (long long)t_begin, (long long)t_first, (long long)t_stop,
         (long long)t_closed);
  return failed ? 1 : 0;
}

int yuvcheck(int argc, wchar_t** argv) {
  if (argc < 4) return 2;
  int W = 0, H = 0;
  swscanf_s(argv[3], L"%dx%d", &W, &H);
  if (W <= 0 || H <= 0) return 2;
  std::string tag = argc > 4 ? narrow(argv[4]) : "";
  FILE* f = wcscmp(argv[2], L"-") ? _wfopen(argv[2], L"rb") : stdin;
  if (!f) return 1;
  if (f == stdin) _setmode(_fileno(stdin), 0x8000 /*_O_BINARY*/);
  std::vector<uint8_t> fr((size_t)W * H * 3 / 2);
  Stats st;
  int64_t t_first = 0, t_last = 0;
  while (fread(fr.data(), 1, fr.size(), f) == fr.size()) {
    int64_t now = unix_us();
    if (!t_first) t_first = now;
    t_last = now;
    st.add(classify(Frame{W, H, 1, fr.data(), W}, false), now, false, tag);
  }
  double dur = (t_last - t_first) / 1e6;
  printf("{\"tag\":\"%s\",\"frames\":%llu,\"fps\":%.2f,%s}\n", tag.c_str(), (unsigned long long)st.frames, dur > 0 ? (st.frames - 1) / dur : 0.0, st.json().c_str());
  return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 2 && !wcscmp(argv[1], L"list")) return list();
  if (argc >= 2 && !wcscmp(argv[1], L"read")) return read(argc, argv);
  if (argc >= 2 && !wcscmp(argv[1], L"yuvcheck")) return yuvcheck(argc, argv);
  printf("usage: camprobe list | read [...] | yuvcheck FILE|- WxH [TAG]\n");
  return 2;
}
