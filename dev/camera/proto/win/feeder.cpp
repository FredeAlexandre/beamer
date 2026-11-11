// SPDX-License-Identifier: GPL-3.0-only
// C2 prototype helper (stand-in for the future beamer-camera.exe in the user session).
//   feeder pattern [--size WxH] [--follow] [--seconds N] [--always] [--switch-every S]
//        on demand (source Start) produce stamped NV12 test frames at 30 fps into the shared ring
//   feeder h264 FILE [--hw] [--seconds N] [--always] [--sw]
//        on demand decode an Annex-B H.264 file at 30 fps (looping), stamp, write into the ring
//   feeder decbench FILE [--hw] [--frames N] [--fps F]     per-frame decode latency, no ring
//   feeder genyuv OUT --size WxH --frames N                raw NV12 with a counter stamp (to encode)
//   feeder cpu PID SECONDS                                 CPU of another process (cycles + times)
// Events are JSON lines on stdout.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "common.h"

using Microsoft::WRL::ComPtr;
using namespace c2;

static double pct(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}
static double mean(const std::vector<double>& v) {
  double s = 0;
  for (double x : v) s += x;
  return v.empty() ? -1 : s / v.size();
}

// ---- process CPU ------------------------------------------------------------------------------------
static double nominal_hz() {
  DWORD mhz = 0, sz = sizeof mhz;
  RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &sz);
  return mhz * 1e6;
}
struct CpuMeter {
  HANDLE p;
  ULONG64 c0 = 0;
  ULONGLONG t0 = 0;
  int64_t w0 = 0;
  explicit CpuMeter(HANDLE h) : p(h) { reset(); }
  static ULONGLONG times(HANDLE p) {
    FILETIME a, b, k, u;
    GetProcessTimes(p, &a, &b, &k, &u);
    return (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
  }
  void reset() {
    QueryProcessCycleTime(p, &c0);
    t0 = times(p);
    w0 = unix_us();
  }
  // % of one logical core
  void read(double* by_cycles, double* by_times) {
    ULONG64 c1;
    QueryProcessCycleTime(p, &c1);
    double wall = (unix_us() - w0) / 1e6;
    *by_cycles = (double)(c1 - c0) / nominal_hz() / wall * 100.0;
    *by_times = (double)(times(p) - t0) / 1e7 / wall * 100.0;
  }
};

// ---- ring writer ------------------------------------------------------------------------------------
struct Ring {
  HANDLE map = nullptr, frame_ev = nullptr, demand_ev = nullptr;
  Header* hdr = nullptr;
  int64_t frame = 0;
  bool create_demand() {
    SECURITY_ATTRIBUTES sa;
    make_sa(&sa);
    demand_ev = CreateEventW(&sa, FALSE, FALSE, kDemandEventName);
    DWORD e = GetLastError();
    printf("{\"event\":\"demand_event\",\"ok\":%s,\"err\":%lu}\n", demand_ev ? "true" : "false", demand_ev ? 0 : e);
    return demand_ev != nullptr;
  }
  bool try_open() {
    if (hdr) return true;
    map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kShmName);
    if (!map) return false;
    hdr = (Header*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kShmBytes);
    frame_ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kFrameEventName);
    printf("{\"event\":\"ring_open\",\"t\":%lld,\"ok\":%s}\n", (long long)unix_us(), hdr && hdr->magic == kMagic ? "true" : "false");
    return hdr != nullptr;
  }
  // copies an NV12 frame (stride == w) into the next slot
  void write(const uint8_t* nv12, int w, int h) {
    LONG next = (hdr->latest + 1) % (LONG)kSlots;
    if (next < 0) next = 0;
    Slot& s = hdr->slots[next];
    InterlockedIncrement64(&s.seq);
    memcpy(slot_data(hdr, next), nv12, (size_t)w * h * 3 / 2);
    s.width = w;
    s.height = h;
    s.frame = ++frame;
    s.write_us = unix_us();
    InterlockedIncrement64(&s.seq);
    InterlockedExchange(&hdr->latest, next);
    hdr->helper_heartbeat_us = s.write_us;
    if (frame_ev) SetEvent(frame_ev);
  }
};

// ---- H.264 --------------------------------------------------------------------------------------------
static std::vector<std::vector<uint8_t>> split_annexb(const std::vector<uint8_t>& d) {
  std::vector<size_t> starts;  // positions of start codes (index of first 0)
  for (size_t i = 0; i + 3 < d.size(); i++)
    if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (d[i + 2] == 0 && d[i + 3] == 1))) {
      starts.push_back(i);
      i += 2;
    }
  std::vector<std::vector<uint8_t>> aus;
  std::vector<uint8_t> cur;
  bool has_vcl = false;
  for (size_t k = 0; k < starts.size(); k++) {
    size_t s = starts[k], e = k + 1 < starts.size() ? starts[k + 1] : d.size();
    size_t hdr = s + (d[s + 2] == 1 ? 3 : 4);
    if (hdr >= d.size()) break;
    int type = d[hdr] & 31;
    bool vcl = type == 1 || type == 5;
    bool first_mb0 = vcl && hdr + 1 < d.size() && (d[hdr + 1] & 0x80);
    bool new_au = has_vcl && (type == 9 || type == 7 || type == 8 || type == 6 || first_mb0);
    if (new_au) {
      aus.push_back(cur);
      cur.clear();
      has_vcl = false;
    }
    cur.insert(cur.end(), d.begin() + s, d.begin() + e);
    if (vcl) has_vcl = true;
  }
  if (!cur.empty()) aus.push_back(cur);
  return aus;
}

struct Decoder {
  ComPtr<IMFTransform> t;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IMFDXGIDeviceManager> mgr;
  bool provides = false, hw = false;
  static inline bool g_no_lowlat = false, g_staging = false;
  ComPtr<ID3D11Texture2D> staging;
  DWORD out_size = 0;
  int w = 0, h = 0, coded_h = 0;
  std::string info;
  int64_t readback_us = 0;

  HRESULT init(bool want_hw, int iw, int ih) {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> a;
    t->GetAttributes(&a);
    if (a && !g_no_lowlat) a->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(t.As(&api))) {
      VARIANT v;
      v.vt = VT_UI4;
      v.ulVal = g_no_lowlat ? 0 : 1;
      HRESULT h2 = api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
      info += h2 == S_OK ? "lowlatency=ok " : "lowlatency=fail ";
    }
    if (want_hw) {
      UINT32 aware = 0;
      if (a) a->GetUINT32(MF_SA_D3D11_AWARE, &aware);
      D3D_FEATURE_LEVEL fl;
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
      if (FAILED(hr)) return hr;
      ComPtr<ID3D10Multithread> mt;
      if (SUCCEEDED(dev.As(&mt))) mt->SetMultithreadProtected(TRUE);
      UINT token;
      MFCreateDXGIDeviceManager(&token, &mgr);
      mgr->ResetDevice(dev.Get(), token);
      hr = t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)mgr.Get());
      if (FAILED(hr)) return hr;
      hw = true;
      info += aware ? "d3d11_aware " : "d3d11_not_aware ";
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, iw, ih);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, 30, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = t->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) return hr;
    hr = set_output();
    if (FAILED(hr)) return hr;
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return S_OK;
  }
  HRESULT set_output() {
    for (DWORD i = 0;; i++) {
      ComPtr<IMFMediaType> o;
      HRESULT hr = t->GetOutputAvailableType(0, i, &o);
      if (FAILED(hr)) return hr;
      GUID g;
      o->GetGUID(MF_MT_SUBTYPE, &g);
      if (g == MFVideoFormat_NV12) {
        hr = t->SetOutputType(0, o.Get(), 0);
        if (FAILED(hr)) return hr;
        UINT32 ow, oh;
        MFGetAttributeSize(o.Get(), MF_MT_FRAME_SIZE, &ow, &oh);
        w = ow;
        h = coded_h = oh;
        MFVideoArea area;
        if (SUCCEEDED(o->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof area, nullptr)) && area.Area.cy > 0) h = area.Area.cy;
        MFT_OUTPUT_STREAM_INFO si = {};
        t->GetOutputStreamInfo(0, &si);
        provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        out_size = si.cbSize;
        return S_OK;
      }
    }
  }
  HRESULT input(const std::vector<uint8_t>& au, int64_t ts) {
    ComPtr<IMFSample> s;
    ComPtr<IMFMediaBuffer> b;
    MFCreateSample(&s);
    MFCreateMemoryBuffer((DWORD)au.size(), &b);
    BYTE* p;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, au.data(), au.size());
    b->Unlock();
    b->SetCurrentLength((DWORD)au.size());
    s->AddBuffer(b.Get());
    s->SetSampleTime(ts);
    s->SetSampleDuration(333333);
    return t->ProcessInput(0, s.Get(), 0);
  }
  // S_OK + frame in out (stride w), S_FALSE = needs more input
  HRESULT output(std::vector<uint8_t>& out, int64_t* ts) {
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER db = {};
      ComPtr<IMFSample> mine;
      if (!provides) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateSample(&mine);
        MFCreateMemoryBuffer(out_size ? out_size : (DWORD)(w * h * 3 / 2), &b);
        mine->AddBuffer(b.Get());
        db.pSample = mine.Get();
      }
      DWORD status = 0;
      HRESULT hr = t->ProcessOutput(0, 1, &db, &status);
      if (db.pEvents) db.pEvents->Release();
      if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_FALSE;
      if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        if (db.pSample && provides) db.pSample->Release();
        HRESULT h2 = set_output();
        if (FAILED(h2)) return h2;
        continue;
      }
      if (FAILED(hr)) return hr;
      ComPtr<IMFSample> s;
      if (provides)
        s.Attach(db.pSample);
      else
        s = mine;
      LONGLONG st = 0;
      s->GetSampleTime(&st);
      *ts = st;
      int64_t r0 = unix_us();
      ComPtr<IMFMediaBuffer> b;
      s->GetBufferByIndex(0, &b);
      ComPtr<IMFDXGIBuffer> dx;
      if (g_staging && SUCCEEDED(b.As(&dx))) {  // explicit GPU -> staging copy + Map
        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        dx->GetResource(IID_PPV_ARGS(&tex));
        dx->GetSubresourceIndex(&sub);
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        if (!staging) {
          D3D11_TEXTURE2D_DESC sd = td;
          sd.ArraySize = 1;
          sd.MipLevels = 1;
          sd.Usage = D3D11_USAGE_STAGING;
          sd.BindFlags = 0;
          sd.MiscFlags = 0;
          sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          dev->CreateTexture2D(&sd, nullptr, &staging);
        }
        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        out.resize((size_t)w * h * 3 / 2);
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
          const BYTE* sp = (const BYTE*)m.pData;
          for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, sp + (size_t)y * m.RowPitch, w);
          const BYTE* uvp = sp + (size_t)m.RowPitch * td.Height;
          for (int y = 0; y < h / 2; y++) memcpy(out.data() + (size_t)w * h + (size_t)y * w, uvp + (size_t)y * m.RowPitch, w);
          ctx->Unmap(staging.Get(), 0);
        }
        readback_us = unix_us() - r0;
        return S_OK;
      }
      ComPtr<IMF2DBuffer2> b2;
      out.resize((size_t)w * h * 3 / 2);
      BYTE *p, *start;
      LONG pitch;
      DWORD blen;
      if (SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &p, &pitch, &start, &blen))) {
        for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, p + (size_t)y * pitch, w);
        // the UV plane follows the coded (possibly padded, 1080 -> 1088) luma plane
        BYTE* uvp = p + (size_t)pitch * coded_h;
        for (int y = 0; y < h / 2; y++) memcpy(out.data() + (size_t)w * h + (size_t)y * w, uvp + (size_t)y * pitch, w);
        b2->Unlock2D();
      } else {
        DWORD len;
        b->Lock(&p, nullptr, &len);
        for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, p + (size_t)y * w, w);
        for (int y = 0; y < h / 2; y++) memcpy(out.data() + (size_t)w * h + (size_t)y * w, p + (size_t)w * coded_h + (size_t)y * w, w);
        b->Unlock();
      }
      readback_us = unix_us() - r0;
      return S_OK;
    }
  }
};

static std::vector<uint8_t> read_file(const wchar_t* path) {
  std::vector<uint8_t> d;
  FILE* f = _wfopen(path, L"rb");
  if (!f) return d;
  fseek(f, 0, SEEK_END);
  d.resize(ftell(f));
  fseek(f, 0, SEEK_SET);
  fread(d.data(), 1, d.size(), f);
  fclose(f);
  return d;
}

// ---- modes --------------------------------------------------------------------------------------------
static int decbench(const wchar_t* file, bool hw, int frames, double fps, int iw, int ih) {
  auto data = read_file(file);
  auto aus = split_annexb(data);
  if (aus.empty()) {
    printf("{\"ok\":false,\"error\":\"no AUs\"}\n");
    return 1;
  }
  Decoder d;
  HRESULT hr = d.init(hw, iw, ih);
  if (FAILED(hr)) {
    printf("{\"ok\":false,\"error\":\"init\",\"hr\":\"0x%08lx\",\"info\":\"%s\"}\n", (unsigned long)hr, d.info.c_str());
    return 1;
  }
  std::vector<double> lat, rb;
  std::vector<int64_t> in_t(frames + 1, 0);
  int out_n = 0, late = 0, order_err = 0;
  int64_t last_counter = -1;
  std::vector<uint8_t> out;
  CpuMeter cpu(GetCurrentProcess());
  int64_t t0 = unix_us();
  for (int i = 0; i < frames; i++) {
    if (fps > 0) {
      int64_t due = t0 + (int64_t)(i * 1e6 / fps);
      while (unix_us() < due) Sleep(1);
    }
    in_t[i] = unix_us();
    hr = d.input(aus[i % aus.size()], (int64_t)i * 333333);
    if (FAILED(hr)) {
      printf("{\"ok\":false,\"error\":\"ProcessInput\",\"hr\":\"0x%08lx\",\"i\":%d}\n", (unsigned long)hr, i);
      return 1;
    }
    int64_t ts;
    bool got_this = false;
    while ((hr = d.output(out, &ts)) == S_OK) {
      int idx = (int)(ts / 333333);
      if (idx != i) late++;
      else got_this = true;
      lat.push_back((unix_us() - in_t[idx]) / 1000.0);
      rb.push_back(d.readback_us / 1000.0);
      uint32_t c, tl;
      uint8_t o;
      const uint8_t* y = out.data();
      int W = d.w;
      if (unstamp(d.w, d.h, [&](int x, int yy) { return (int)y[(size_t)yy * W + x]; }, &c, &tl, &o)) {
        if (last_counter >= 0 && (int64_t)c != last_counter + 1 && c != 0) order_err++;
        last_counter = c;
      }
      out_n++;
    }
    (void)got_this;
  }
  double cyc, tim;
  cpu.read(&cyc, &tim);
  printf("{\"ok\":true,\"mode\":\"%s\",\"info\":\"%s\",\"size\":\"%dx%d\",\"aus\":%zu,\"in\":%d,\"out\":%d,\"out_after_later_input\":%d,"
         "\"counter_breaks\":%d,\"lat_ms_p50\":%.2f,\"lat_ms_p95\":%.2f,\"lat_ms_max\":%.2f,\"readback_ms_mean\":%.2f,\"cpu_pct_cycles\":%.1f,\"cpu_pct_times\":%.1f}\n",
         hw ? "hw" : "sw", d.info.c_str(), d.w, d.h, aus.size(), frames, out_n, late, order_err, pct(lat, 0.5), pct(lat, 0.95), pct(lat, 1.0), mean(rb), cyc,
         tim);
  return 0;
}

// Decodes FILE fully (reference), then again without the access units in `drops`, and compares the luma
// of every output frame with the reference (PSNR-Y; identical = 999). Checks LTR loss recovery.
static int ltrcheck(const wchar_t* file, bool hw, int iw, int ih, const std::vector<int>& drops) {
  auto aus = split_annexb(read_file(file));
  auto run = [&](bool drop, std::vector<std::vector<uint8_t>>& ys, int* errs) -> int {
    Decoder d;
    if (FAILED(d.init(hw, iw, ih))) return -1;
    ys.assign(aus.size(), {});
    std::vector<uint8_t> out;
    for (size_t i = 0; i < aus.size(); i++) {
      if (drop && std::find(drops.begin(), drops.end(), (int)i) != drops.end()) continue;
      if (FAILED(d.input(aus[i], (int64_t)i * 333333))) (*errs)++;
      int64_t ts;
      HRESULT hr;
      while ((hr = d.output(out, &ts)) == S_OK) {
        size_t k = (size_t)(ts / 333333);
        if (k < ys.size()) ys[k].assign(out.begin(), out.begin() + (size_t)d.w * d.h);
      }
      if (FAILED(hr)) (*errs)++;
    }
    return d.w * d.h;
  };
  std::vector<std::vector<uint8_t>> ref, got;
  int e1 = 0, e2 = 0;
  int n = run(false, ref, &e1);
  run(true, got, &e2);
  if (n <= 0) {
    printf("{\"ok\":false}\n");
    return 1;
  }
  int first_clean_after = -1, missing = 0, last_dirty = -1;
  std::string list;
  for (size_t i = 0; i < aus.size(); i++) {
    if (got[i].empty() || ref[i].empty()) {
      missing++;
      continue;
    }
    double se = 0;
    for (int k = 0; k < n; k++) {
      double dd = (double)got[i][k] - ref[i][k];
      se += dd * dd;
    }
    double psnr = se == 0 ? 999 : 10 * log10(255.0 * 255.0 * n / se);
    if (psnr < 999) last_dirty = (int)i;
    if (!drops.empty() && (int)i > drops.back() && i < (size_t)drops.back() + 12) {
      char b[48];
      snprintf(b, sizeof b, "%s\"%zu\":%.1f", list.empty() ? "" : ",", i, psnr);
      list += b;
    }
  }
  for (size_t i = last_dirty + 1; i < aus.size(); i++)
    if (!got[i].empty()) {
      first_clean_after = (int)i;
      break;
    }
  printf("{\"ok\":true,\"mode\":\"%s\",\"aus\":%zu,\"decode_errors_full\":%d,\"decode_errors_dropped\":%d,\"frames_missing\":%d,"
         "\"last_dirty\":%d,\"clean_from\":%d,\"psnr_y_after_drop\":{%s}}\n",
         hw ? "hw" : "sw", aus.size(), e1, e2, missing, last_dirty, first_clean_after, list.c_str());
  return 0;
}

struct Opts {
  int w = 1280, h = 720;
  bool follow = false, always = false, hw = false;
  double seconds = 60, switch_every = 0;
  std::wstring file;
};

// Producer loop shared by pattern and h264 modes.
static int produce(const Opts& o, bool h264) {
  Ring ring;
  if (!ring.create_demand()) return 1;
  timeBeginPeriod(1);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  std::vector<std::vector<uint8_t>> aus;
  Decoder dec;
  if (h264) {
    aus = split_annexb(read_file(o.file.c_str()));
    HRESULT hr = dec.init(o.hw, o.w, o.h);
    if (FAILED(hr) || aus.empty()) {
      printf("{\"ok\":false,\"error\":\"decoder\",\"hr\":\"0x%08lx\",\"aus\":%zu}\n", (unsigned long)hr, aus.size());
      return 1;
    }
    printf("{\"event\":\"decoder\",\"info\":\"%s\",\"aus\":%zu}\n", dec.info.c_str(), aus.size());
  }
  std::vector<uint8_t> frame((size_t)kMaxW * kMaxH * 3 / 2);
  int64_t t_end = unix_us() + (int64_t)(o.seconds * 1e6);
  bool streaming = false;
  LONG64 seen_state = -1;
  int64_t n = 0, due = 0, t_switch = 0, au_i = 0;
  int cw = o.w, ch = o.h;
  std::vector<double> dec_lat, write_us;
  CpuMeter cpu(GetCurrentProcess());
  int64_t produced = 0;
  while (unix_us() < t_end) {
    ring.try_open();
    // state
    if (ring.hdr && ring.hdr->state_seq != seen_state) {
      seen_state = ring.hdr->state_seq;
      bool s = ring.hdr->streaming != 0;
      int64_t now = unix_us();
      printf("{\"event\":\"demand\",\"t\":%lld,\"streaming\":%s,\"want\":\"%ldx%ld\",\"signal_ms\":%.2f}\n", (long long)now, s ? "true" : "false",
             ring.hdr->want_w, ring.hdr->want_h, (now - ring.hdr->state_us) / 1000.0);
      if (s && !streaming) {
        due = unix_us();
        cpu.reset();
        produced = 0;
        dec_lat.clear();
        write_us.clear();
        t_switch = due + (int64_t)(o.switch_every * 1e6);
      }
      if (!s && streaming) {
        double cyc, tim;
        cpu.read(&cyc, &tim);
        printf("{\"event\":\"stats\",\"produced\":%lld,\"cpu_pct_cycles\":%.2f,\"cpu_pct_times\":%.2f,\"decode_ms_p50\":%.2f,\"decode_ms_p95\":%.2f,"
               "\"write_ms_mean\":%.3f,\"delivered\":%lld,\"placeholders\":%lld,\"dropped\":%lld}\n",
               produced, cyc, tim, pct(dec_lat, 0.5), pct(dec_lat, 0.95), mean(write_us), ring.hdr->delivered, ring.hdr->placeholders,
               ring.hdr->dropped);
      }
      streaming = s;
      if (o.follow && s && ring.hdr->want_w > 0) {
        cw = ring.hdr->want_w;
        ch = ring.hdr->want_h;
      }
    }
    bool run = ring.hdr && (streaming || o.always);
    if (!run) {
      WaitForSingleObject(ring.demand_ev, ring.hdr ? 1000 : 250);
      continue;
    }
    // wait for the next frame time or a demand change
    int64_t now = unix_us();
    if (due > now) {
      LARGE_INTEGER li;
      li.QuadPart = -(due - now) * 10;
      SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
      HANDLE hs[2] = {timer, ring.demand_ev};
      if (WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) continue;
    }
    due += 33333;
    if (due < unix_us() - 100000) due = unix_us() + 33333;
    if (o.switch_every > 0 && unix_us() >= t_switch) {
      bool big = cw == 1920;
      cw = big ? 1280 : 1920;
      ch = big ? 720 : 1080;
      t_switch += (int64_t)(o.switch_every * 1e6);
      printf("{\"event\":\"switch\",\"t\":%lld,\"size\":\"%dx%d\"}\n", (long long)unix_us(), cw, ch);
    }
    if (h264) {
      int64_t t0 = unix_us();
      dec.input(aus[au_i % aus.size()], au_i * 333333);
      au_i++;
      int64_t ts;
      bool got = false;
      while (dec.output(frame, &ts) == S_OK) got = true;
      if (!got) continue;
      dec_lat.push_back((unix_us() - t0) / 1000.0);
      cw = dec.w;
      ch = dec.h;
    } else {
      pattern_nv12(frame.data(), cw, cw, ch, (uint32_t)n, false);
    }
    int64_t w0 = unix_us();
    stamp_nv12(frame.data(), cw, cw, ch, (uint32_t)n, (uint32_t)unix_us(), kOriginHelper);
    ring.write(frame.data(), cw, ch);
    write_us.push_back((unix_us() - w0) / 1000.0);
    n++;
    produced++;
  }
  printf("{\"event\":\"exit\",\"produced_total\":%lld}\n", n);
  return 0;
}

static int genyuv(const wchar_t* out, int w, int h, int frames) {
  FILE* f = _wfopen(out, L"wb");
  if (!f) return 1;
  std::vector<uint8_t> fr((size_t)w * h * 3 / 2);
  for (int i = 0; i < frames; i++) {
    pattern_nv12(fr.data(), w, w, h, i, false);
    stamp_nv12(fr.data(), w, w, h, i, 0, kOriginHelper);
    fwrite(fr.data(), 1, fr.size(), f);
  }
  fclose(f);
  printf("{\"ok\":true,\"frames\":%d}\n", frames);
  return 0;
}

static int cpu_of(DWORD pid, double secs) {
  HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!p) {
    printf("{\"ok\":false,\"err\":%lu}\n", GetLastError());
    return 1;
  }
  CpuMeter m(p);
  Sleep((DWORD)(secs * 1000));
  double cyc, tim;
  m.read(&cyc, &tim);
  printf("{\"ok\":true,\"pid\":%lu,\"seconds\":%.1f,\"cpu_pct_cycles\":%.2f,\"cpu_pct_times\":%.2f}\n", pid, secs, cyc, tim);
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc < 2) return 2;
  std::wstring cmd = argv[1];
  Opts o;
  int frames = 300;
  double fps = 30;
  std::vector<int> drops;
  for (int i = 2; i < argc; i++) {
    std::wstring a = argv[i];
    if (a == L"--size" && i + 1 < argc) swscanf_s(argv[++i], L"%dx%d", &o.w, &o.h);
    else if (a == L"--follow") o.follow = true;
    else if (a == L"--always") o.always = true;
    else if (a == L"--hw") o.hw = true;
    else if (a == L"--no-lowlatency") Decoder::g_no_lowlat = true;
    else if (a == L"--staging") Decoder::g_staging = true;
    else if (a == L"--seconds" && i + 1 < argc) o.seconds = _wtof(argv[++i]);
    else if (a == L"--switch-every" && i + 1 < argc) o.switch_every = _wtof(argv[++i]);
    else if (a == L"--frames" && i + 1 < argc) frames = _wtoi(argv[++i]);
    else if (a == L"--fps" && i + 1 < argc) fps = _wtof(argv[++i]);
    else if (a == L"--drop" && i + 1 < argc) {
      for (wchar_t* t = argv[++i]; *t;) {
        drops.push_back((int)wcstol(t, &t, 10));
        if (*t == L',') t++;
      }
    }
    else if (o.file.empty() && a[0] != L'-') o.file = a;
  }
  if (cmd == L"pattern") return produce(o, false);
  if (cmd == L"h264") return produce(o, true);
  if (cmd == L"decbench") return decbench(o.file.c_str(), o.hw, frames, fps, o.w, o.h);
  if (cmd == L"ltrcheck") return ltrcheck(o.file.c_str(), o.hw, o.w, o.h, drops);
  if (cmd == L"genyuv") return genyuv(o.file.c_str(), o.w, o.h, frames);
  if (cmd == L"cpu") return cpu_of((DWORD)_wtoi(argv[2]), argc > 3 ? _wtof(argv[3]) : 10);
  return 2;
}
