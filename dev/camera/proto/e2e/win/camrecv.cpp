// SPDX-License-Identifier: GPL-3.0-only
// C4 PC receiver (stand-in for the future beamer-camera.exe helper, runs in the user's session).
//
//   camrecv --mac 192.168.1.138 --log FILE [--seconds N] [--idr-pace 2.5] [--key-every N] [--audio]
//           [--bitrate720 2500000] [--bitrate1080 5000000] [--loss PLAN] [--no-oracle]
//
// Waits for the virtual camera's demand (C2's source DLL: shared header + Global\ demand event), sends
// kStart/kStop to the Mac (camsend), receives the video packets (C3 format), reassembles frames, NACKs
// holes (<= 64 packets, <= 3 tries), decodes in order with the MF H.264 decoder (D3D11, AVLowLatencyMode,
// staging readback: C2), detects unrepairable loss and then decodes without showing until a recovery
// frame (IDR or LTR refresh) arrives (C2 found corrupt frames decode without error), sends kPli, acks
// cleanly decoded frames (kRR: the Mac turns them into AcknowledgedLTRTokens), and writes each good frame
// into the ring at its playout deadline (adaptive: p99 of the last 10 s of completion delays + 8 ms, in
// [50, 150] ms, from the encoder output time: C3). An oracle thread regenerates C1's synthetic picture
// for the frame number and computes PSNR-Y of every frame written, to catch any corrupt frame shown.
//
// Loss injection (--loss): "T:KIND,..." with T in seconds after the first video packet of a session and
// KIND drop1 | burstN (N consecutive video packets) | blackoutMS (every packet, any type), optionally
// "@P" at the end to repeat the plan every P seconds, e.g. "10:drop1,20:burst5,30:burst30,40:burst80,50:blackout1500@60".
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#include <windows.h>
#include <psapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "common.h"
#include "proto.h"

using Microsoft::WRL::ComPtr;
using namespace c2;
using namespace e2e;

// ------------------------------------------------------------------------------------------- log
static FILE* g_log = stdout;
static std::mutex g_log_mu;
static void logj(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_log_mu);
  va_list ap;
  va_start(ap, fmt);
  vfprintf(g_log, fmt, ap);
  va_end(ap);
  fputc('\n', g_log);
  fflush(g_log);
}
static double pctl(std::vector<double> v, double p) {
  if (v.empty()) return -1;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))];
}

// ------------------------------------------------------------------------------------------- config
struct Cfg {
  std::string mac = "192.168.1.138", log;
  double seconds = 1e9, idr_pace = 2.5;
  int key_every = 0, bitrate720 = 2500000, bitrate1080 = 5000000;
  bool audio = false, oracle = true;
  std::string loss;
  int fixed_d = 0;  // >0: fixed playout delay (ms) instead of adaptive
} cfg;

// ------------------------------------------------------------------------------------------- ring (C2)
struct Ring {
  HANDLE map = nullptr, frame_ev = nullptr, demand_ev = nullptr;
  Header* hdr = nullptr;
  int64_t frame = 0;
  bool create_demand() {
    SECURITY_ATTRIBUTES sa;
    make_sa(&sa);
    demand_ev = CreateEventW(&sa, FALSE, FALSE, kDemandEventName);
    return demand_ev != nullptr;
  }
  bool try_open() {
    if (hdr) return true;
    map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, kShmName);
    if (!map) return false;
    hdr = (Header*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kShmBytes);
    frame_ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kFrameEventName);
    logj("{\"ev\":\"ring_open\",\"t\":%lld,\"ok\":%d}", (long long)unix_us(), hdr && hdr->magic == kMagic);
    return hdr != nullptr;
  }
  int64_t write(const uint8_t* nv12, int w, int h) {
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
    return s.write_us;
  }
} ring;

// ------------------------------------------------------------------------------------------- decoder (C2, staging readback)
struct Decoder {
  ComPtr<IMFTransform> t;
  ComPtr<ID3D11Device> dev;
  ComPtr<ID3D11DeviceContext> ctx;
  ComPtr<IMFDXGIDeviceManager> mgr;
  ComPtr<ID3D11Texture2D> staging;
  bool provides = false;
  DWORD out_size = 0;
  int w = 0, h = 0, coded_h = 0;
  HRESULT init() {
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&t));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> a;
    t->GetAttributes(&a);
    if (a) a->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(t.As(&api))) {
      VARIANT v;
      v.vt = VT_UI4;
      v.ulVal = 1;
      api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
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
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, 1280, 720);
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
  void flush() { t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }
  HRESULT set_output() {
    for (DWORD i = 0;; i++) {
      ComPtr<IMFMediaType> o;
      HRESULT hr = t->GetOutputAvailableType(0, i, &o);
      if (FAILED(hr)) return hr;
      GUID g;
      o->GetGUID(MF_MT_SUBTYPE, &g);
      if (g != MFVideoFormat_NV12) continue;
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
      staging.Reset();
      return S_OK;
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
  HRESULT output(std::vector<uint8_t>& out, int64_t* ts) {
    for (;;) {
      MFT_OUTPUT_DATA_BUFFER db = {};
      ComPtr<IMFSample> mine;
      if (!provides) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateSample(&mine);
        MFCreateMemoryBuffer(out_size ? out_size : (DWORD)(w * coded_h * 3 / 2), &b);
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
      if (provides) s.Attach(db.pSample);
      else s = mine;
      LONGLONG st = 0;
      s->GetSampleTime(&st);
      *ts = st;
      ComPtr<IMFMediaBuffer> b;
      s->GetBufferByIndex(0, &b);
      out.resize((size_t)w * h * 3 / 2);
      ComPtr<IMFDXGIBuffer> dx;
      if (SUCCEEDED(b.As(&dx))) {
        ComPtr<ID3D11Texture2D> tex;
        UINT sub = 0;
        dx->GetResource(IID_PPV_ARGS(&tex));
        dx->GetSubresourceIndex(&sub);
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        D3D11_TEXTURE2D_DESC cur = {};
        if (staging) staging->GetDesc(&cur);
        if (!staging || cur.Width != td.Width || cur.Height != td.Height) {
          D3D11_TEXTURE2D_DESC sd = td;
          sd.ArraySize = 1;
          sd.MipLevels = 1;
          sd.Usage = D3D11_USAGE_STAGING;
          sd.BindFlags = 0;
          sd.MiscFlags = 0;
          sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
          staging.Reset();
          dev->CreateTexture2D(&sd, nullptr, &staging);
        }
        ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) {
          const BYTE* sp = (const BYTE*)m.pData;
          for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, sp + (size_t)y * m.RowPitch, w);
          const BYTE* uvp = sp + (size_t)m.RowPitch * td.Height;
          for (int y = 0; y < h / 2; y++) memcpy(out.data() + (size_t)w * h + (size_t)y * w, uvp + (size_t)y * m.RowPitch, w);
          ctx->Unmap(staging.Get(), 0);
        }
        return S_OK;
      }
      ComPtr<IMF2DBuffer2> b2;
      BYTE *p, *start;
      LONG pitch;
      DWORD blen;
      if (SUCCEEDED(b.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &p, &pitch, &start, &blen))) {
        for (int y = 0; y < h; y++) memcpy(out.data() + (size_t)y * w, p + (size_t)y * pitch, w);
        BYTE* uvp = p + (size_t)pitch * coded_h;
        for (int y = 0; y < h / 2; y++) memcpy(out.data() + (size_t)w * h + (size_t)y * w, uvp + (size_t)y * pitch, w);
        b2->Unlock2D();
      }
      return S_OK;
    }
  }
};

// ------------------------------------------------------------------------------------------- C1 synthetic luma (oracle)
static uint8_t g_noise[65536];
static void init_noise() {
  uint32_t s = 0x12345678;
  for (int i = 0; i < 65536; i++) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    g_noise[i] = (uint8_t)(s >> 24);
  }
}
// PSNR-Y of a decoded frame against C1's synth_fill(frame f, noise 3), on a 2x2 grid below the code strip.
static double oracle_psnr(const uint8_t* Y, int w, int h, uint32_t f) {
  const int B = w / 80, noise = 3, amp = 2 * noise + 1;
  const double t = f / 30.0, PI = 3.14159265358979323846;
  const int cx = (int)(w * (0.5 + 0.22 * std::sin(2 * PI * t / 4.0)));
  const int cy = (int)(h * (0.48 + 0.10 * std::sin(2 * PI * t / 6.0)));
  const int r = h / 5, panel_y0 = h * 3 / 4, panel_y1 = h - h / 20, scroll = (int)(f * 3);
  double se = 0;
  long n = 0;
  for (int y = B; y < h; y += 2) {
    const bool in_panel = y >= panel_y0 && y < panel_y1;
    const int dy = y - cy;
    for (int x = 0; x < w; x += 2) {
      int v;
      const int dx = x - cx, d2 = dx * dx + dy * dy;
      if (d2 < r * r) {
        int d = (int)std::sqrt((double)d2);
        v = 120 + ((d / 6) & 1) * 30 + (dx > 0 ? 20 : 0);
      } else if (in_panel) {
        int xs = x + scroll;
        int cell = (xs / 6) * 31 + ((y - panel_y0) / 12) * 977;
        int bits = g_noise[cell & 0xFFFF];
        int gx = xs % 6, gy = (y - panel_y0) % 12;
        bool on = gx < 5 && gy < 10 && ((bits >> ((gx + gy * 3) & 7)) & 1);
        v = on ? 40 : 200;
      } else {
        v = 50 + (((x + 2 * y + (int)f * 4) % 1024) * 140) / 1024;
      }
      v += (int)(g_noise[(uint32_t)(x * 7 + y * 1031 + f * 40503u) & 0xFFFF] % amp) - noise;
      v = v < 16 ? 16 : (v > 235 ? 235 : v);
      int e = (int)Y[(size_t)y * w + x] - v;
      se += e * e;
      n++;
    }
  }
  double mse = se / n;
  return mse <= 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

// ------------------------------------------------------------------------------------------- shared state
static SOCKET g_sock;
static void tx(const void* p, int n) { send(g_sock, (const char*)p, n, 0); }

struct FrameBuf {
  uint16_t nfrag = 0, got = 0;
  std::vector<std::vector<uint8_t>> frags;
  uint8_t vflags = 0;
  uint64_t cap_ns = 0, out_ns = 0;
  int64_t a0 = 0, ac = 0;
  uint32_t seq0 = 0;
  int repaired = 0;
  bool complete() const { return nfrag && got == nfrag; }
};
struct MissState { int64_t t_detect, t_last; int tries; bool exhausted; };

static std::mutex mu;
static std::condition_variable cv;
// clock (Mac ns -> PC unix us)
struct PingS { int64_t t1, t2; uint64_t mac; };
static std::deque<PingS> g_pings;
static double g_off_us = 0;  // pc_us = mac_ns/1000 + g_off_us
static bool g_off_ok = false;
static double g_rtt_min_ms = 0;
static double mac_to_pc(uint64_t mac_ns) { return mac_ns / 1000.0 + g_off_us; }
// session
static uint16_t g_sess = 0;
static bool g_sess_new = false;
static std::map<uint32_t, FrameBuf> g_frames;
static uint32_t g_next = 0;         // next frame to decode
static bool g_have_next = false;
static uint32_t g_hi_seq = 0;
static bool g_have_seq = false;
static std::map<uint32_t, MissState> g_miss;
static uint32_t g_big_hole_seq = 0;  // frames with packets below this seq and incomplete: give up now
static bool g_big_hole = false;
static uint32_t g_hi_frame = 0;
// decoder state
static bool g_broken = false;
static uint32_t g_recover_min = 0;  // a recovery frame must be >= this (lost frame + 1, or the session's first frame)
static uint32_t g_broken_frame = 0, g_clean_from = UINT32_MAX, g_acked = UINT32_MAX;
static int64_t g_last_pli = 0;
static uint32_t g_lost_frames = 0, g_late = 0, g_shown = 0, g_hidden = 0, g_lost_pk = 0, g_injected = 0;
// playout
static std::deque<std::pair<int64_t, double>> g_delays;  // (t, completion delay ms)
static double g_D = 70;
static int64_t g_D_t = 0;
// demand
static bool g_want = false;
static uint32_t g_req = 0;
static int64_t g_req_t = 0;
static bool g_req_acked = true;
static int g_want_w = 1280, g_want_h = 720;
static std::atomic<bool> g_quit{false};
static int64_t g_first_data_us = 0;

static void send_pli(uint8_t reason) {
  Pli p = {};
  p.ptype = kPli;
  p.reason = reason;
  p.sess = g_sess;
  p.lost_frame = g_broken_frame;
  p.last_clean = g_acked;
  tx(&p, sizeof p);
  g_last_pli = unix_us();
  logj("{\"ev\":\"pli\",\"t\":%lld,\"lost\":%u,\"reason\":%u}", (long long)g_last_pli, g_broken_frame, reason);
}

// ------------------------------------------------------------------------------------------- display + oracle
struct Shown { uint32_t f; int w, h; std::vector<uint8_t> nv12; int64_t deadline; };
static std::mutex dmu;
static std::condition_variable dcv;
static std::deque<Shown> g_dq;
static std::mutex omu;
static std::condition_variable ocv;
static std::deque<Shown> g_oq;
static std::atomic<uint32_t> g_oracle_bad{0}, g_oracle_n{0}, g_oracle_skip{0};
static std::atomic<uint64_t> g_oracle_cpu_100ns{0};  // the oracle is test-only: its CPU is reported apart

static void oracle_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  while (!g_quit) {
    Shown s;
    {
      std::unique_lock<std::mutex> lk(omu);
      ocv.wait_for(lk, std::chrono::milliseconds(200), [] { return !g_oq.empty(); });
      if (g_oq.empty()) continue;
      s = std::move(g_oq.front());
      g_oq.pop_front();
    }
    ULONG64 c0 = 0, c1 = 0;  // cycles: thread times only advance at clock ticks and miss short bursts
    QueryThreadCycleTime(GetCurrentThread(), &c0);
    double p = oracle_psnr(s.nv12.data(), s.w, s.h, s.f);
    QueryThreadCycleTime(GetCurrentThread(), &c1);
    g_oracle_cpu_100ns += c1 - c0;
    g_oracle_n++;
    if (p < 30) {
      g_oracle_bad++;
      logj("{\"ev\":\"oracle_bad\",\"f\":%u,\"psnr\":%.2f}", s.f, p);
    }
    logj("{\"ev\":\"o\",\"f\":%u,\"psnr\":%.2f}", s.f, p);
  }
}

static void display_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  while (!g_quit) {
    Shown s;
    {
      std::unique_lock<std::mutex> lk(dmu);
      dcv.wait_for(lk, std::chrono::milliseconds(100), [] { return !g_dq.empty(); });
      if (g_dq.empty()) continue;
      int64_t now = unix_us();
      if (g_dq.front().deadline > now + 200) {
        int64_t wait = g_dq.front().deadline - now;
        lk.unlock();
        LARGE_INTEGER li;
        li.QuadPart = -wait * 10;
        SetWaitableTimer(timer, &li, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, (DWORD)(wait / 1000 + 50));
        continue;
      }
      s = std::move(g_dq.front());
      g_dq.pop_front();
    }
    int64_t wr = 0;
    if (ring.hdr) wr = ring.write(s.nv12.data(), s.w, s.h);
    logj("{\"ev\":\"w\",\"f\":%u,\"wr\":%lld,\"dl\":%lld}", s.f, (long long)wr, (long long)s.deadline);
    if (cfg.oracle) {
      std::lock_guard<std::mutex> lk(omu);
      if (g_oq.size() < 6) {
        Shown c{s.f, s.w, s.h, std::vector<uint8_t>(s.nv12.begin(), s.nv12.begin() + (size_t)s.w * s.h), 0};
        g_oq.push_back(std::move(c));
        ocv.notify_one();
      } else {
        g_oracle_skip++;
      }
    }
  }
}

// ------------------------------------------------------------------------------------------- decode thread
static void update_D(int64_t now) {
  while (!g_delays.empty() && now - g_delays.front().first > 10000000) g_delays.pop_front();
  if (now - g_D_t < 100000) return;
  double prev = g_D;
  if (cfg.fixed_d > 0) { g_D = cfg.fixed_d; return; }
  std::vector<double> v;
  for (auto& d : g_delays) v.push_back(d.second);
  double target = v.size() >= 20 ? pctl(v, 0.99) + 8 : 70;
  target = std::min(150.0, std::max(50.0, target));
  if (target > g_D) g_D = target;
  else g_D = std::max(target, g_D - 15.0 * (now - g_D_t) / 500000.0);
  g_D_t = now;
  (void)prev;
}

static void decode_thread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  Decoder dec;
  HRESULT hr = dec.init();
  logj("{\"ev\":\"decoder\",\"hr\":\"0x%08lx\"}", (unsigned long)hr);
  if (FAILED(hr)) { g_quit = true; return; }
  std::vector<uint8_t> out;
  std::unique_lock<std::mutex> lk(mu);
  while (!g_quit) {
    cv.wait_for(lk, std::chrono::milliseconds(2));
    int64_t now = unix_us();
    update_D(now);
    if (g_sess_new) {
      g_sess_new = false;
      lk.unlock();
      dec.flush();
      { std::lock_guard<std::mutex> dl(dmu); g_dq.clear(); }
      lk.lock();
    }
    if (!g_have_next) continue;
    for (;;) {
      auto it = g_frames.find(g_next);
      now = unix_us();
      if (it != g_frames.end() && it->second.complete()) {
        FrameBuf fb = std::move(it->second);
        g_frames.erase(it);
        uint32_t f = g_next++;
        std::vector<uint8_t> au;
        for (auto& p : fb.frags) au.insert(au.end(), p.begin(), p.end());
        bool recovery = (fb.vflags & (kIdr | kRefresh)) != 0;
        if (g_broken && recovery && f >= g_recover_min) {
          g_broken = false;
          g_clean_from = f;
          logj("{\"ev\":\"recovered\",\"f\":%u,\"t\":%lld,\"idr\":%d,\"from\":%u}", f, (long long)now, (fb.vflags & kIdr) != 0, g_broken_frame);
        }
        bool clean = !g_broken;
        double D = g_D;
        double outpc = mac_to_pc(fb.out_ns);
        if (!(fb.vflags & (kIdr | kBehindIdr)) && g_off_ok) g_delays.push_back({now, (fb.ac - outpc) / 1000.0});
        lk.unlock();
        int64_t d0 = unix_us();
        HRESULT h1 = dec.input(au, (int64_t)f * 333333);
        int64_t ts = 0;
        HRESULT h2 = S_FALSE;
        bool got = false;
        while ((h2 = dec.output(out, &ts)) == S_OK) got = true;
        int64_t d1 = unix_us();
        lk.lock();
        const char* st;
        int64_t deadline = (int64_t)(outpc + D * 1000);
        if (FAILED(h1) || (FAILED(h2) && h2 != S_FALSE)) {
          st = "decode_error";
          if (!g_broken) { g_broken = true; g_broken_frame = f; g_recover_min = f + 1; send_pli(2); }
        } else if (!got) {
          st = "no_output";
        } else if (!clean) {
          st = "hidden";
          g_hidden++;
        } else {
          g_acked = f;
          bool late = d1 > deadline;
          if ((fb.vflags & kIdr) || late) deadline = d1;
          st = (fb.vflags & kIdr) ? "idr_now" : late ? "late" : "ok";
          if (late && !(fb.vflags & kIdr)) g_late++;
          g_shown++;
          Shown s{f, dec.w, dec.h, out, deadline};
          std::lock_guard<std::mutex> dl(dmu);
          g_dq.push_back(std::move(s));
          dcv.notify_one();
        }
        logj("{\"ev\":\"d\",\"f\":%u,\"s\":%u,\"vf\":%u,\"cap\":%llu,\"out\":%llu,\"seq0\":%u,\"npk\":%u,\"rep\":%d,\"a0\":%lld,\"ac\":%lld,\"d0\":%lld,"
             "\"d1\":%lld,\"dl\":%lld,\"D\":%.1f,\"off\":%.1f,\"st\":\"%s\",\"w\":%d,\"h\":%d}",
             f, g_sess, fb.vflags, fb.cap_ns, fb.out_ns, fb.seq0, fb.nfrag, fb.repaired, (long long)fb.a0, (long long)fb.ac, (long long)d0,
             (long long)d1, (long long)deadline, D, g_off_us, st, dec.w, dec.h);
        continue;
      }
      // next frame incomplete: give up?
      bool later = g_hi_frame > g_next;
      if (!later) break;
      bool give_up = false;
      const char* why = "";
      if (it != g_frames.end() && it->second.got) {
        double dl = mac_to_pc(it->second.out_ns) + g_D * 1000;
        if (now > dl) { give_up = true; why = "deadline"; }
        if (g_big_hole && it->second.seq0 < g_big_hole_seq) { give_up = true; why = "big_hole"; }
        // all NACK tries spent on one of its packets
        for (uint32_t q = it->second.seq0; q < it->second.seq0 + it->second.nfrag && !give_up; q++) {
          auto m = g_miss.find(q);
          if (m != g_miss.end() && m->second.exhausted) { give_up = true; why = "nack_exhausted"; }
        }
      } else {
        // nothing of this frame arrived: use the next known frame's deadline
        auto nx = g_frames.upper_bound(g_next);
        if (nx != g_frames.end() && nx->second.got) {
          double dl = mac_to_pc(nx->second.out_ns) + g_D * 1000 - (nx->first - g_next) * 33333.0;
          if (now > dl) { give_up = true; why = "deadline_unseen"; }
          if (g_big_hole && nx->second.seq0 <= g_big_hole_seq) { give_up = true; why = "big_hole"; }
        }
      }
      if (!give_up) break;
      uint32_t f = g_next++;
      if (it != g_frames.end()) g_frames.erase(it);
      g_lost_frames++;
      logj("{\"ev\":\"lost_frame\",\"f\":%u,\"t\":%lld,\"why\":\"%s\",\"broken\":%d}", f, (long long)now, why, g_broken);
      if (!g_broken) {
        g_broken = true;
        g_broken_frame = f;
        g_recover_min = f + 1;
        send_pli(1);
      }
    }
    if (g_big_hole && (g_frames.empty() || g_frames.begin()->second.seq0 >= g_big_hole_seq)) g_big_hole = false;
    if (g_broken && unix_us() - g_last_pli > 300000) send_pli(3);
  }
  CoUninitialize();
}

// ------------------------------------------------------------------------------------------- loss plan
struct LossEv { double t; int kind; int n; };  // kind 1 = burst of n video packets, 2 = blackout n ms
static std::vector<LossEv> g_plan;
static double g_plan_period = 0;
static size_t g_plan_i = 0;
static int g_plan_cycle = 0;
static int g_drop_left = 0;
static int64_t g_blackout_until = 0;
static void parse_loss(const std::string& s) {
  std::string body = s;
  size_t at = s.find('@');
  if (at != std::string::npos) { body = s.substr(0, at); g_plan_period = atof(s.c_str() + at + 1); }
  size_t i = 0;
  while (i < body.size()) {
    size_t j = body.find(',', i);
    if (j == std::string::npos) j = body.size();
    std::string e = body.substr(i, j - i);
    size_t c = e.find(':');
    if (c != std::string::npos) {
      LossEv ev{atof(e.c_str()), 0, 0};
      std::string k = e.substr(c + 1);
      if (k.rfind("drop", 0) == 0) { ev.kind = 1; ev.n = atoi(k.c_str() + 4); }
      else if (k.rfind("burst", 0) == 0) { ev.kind = 1; ev.n = atoi(k.c_str() + 5); }
      else if (k.rfind("blackout", 0) == 0) { ev.kind = 2; ev.n = atoi(k.c_str() + 8); }
      if (ev.kind) g_plan.push_back(ev);
    }
    i = j + 1;
  }
}
// returns true if this packet must be dropped
static bool inject(int64_t now, bool video) {
  if (!g_plan.empty() && g_first_data_us) {
    double t = (now - g_first_data_us) / 1e6;
    while (g_plan_i < g_plan.size() && t >= g_plan[g_plan_i].t + g_plan_cycle * g_plan_period) {
      auto& e = g_plan[g_plan_i];
      if (e.kind == 1) g_drop_left = e.n;
      else g_blackout_until = now + (int64_t)e.n * 1000;
      logj("{\"ev\":\"inject\",\"t\":%lld,\"kind\":\"%s\",\"n\":%d,\"seq_next\":%u}", (long long)now, e.kind == 1 ? "burst" : "blackout", e.n, g_hi_seq + 1);
      g_plan_i++;
      if (g_plan_i == g_plan.size() && g_plan_period > 0) { g_plan_i = 0; g_plan_cycle++; }
    }
  }
  if (now < g_blackout_until) { g_injected++; return true; }
  if (video && g_drop_left > 0) { g_drop_left--; g_injected++; return true; }
  return false;
}

// ------------------------------------------------------------------------------------------- net
static FILE* g_audio_f = nullptr;  // streamed (the run scripts kill the receiver)

static void on_data(const uint8_t* b, int n, int64_t now) {
  const DataHdr* h = (const DataHdr*)b;
  int payload = n - (int)sizeof(DataHdr) - kTag;
  if (payload < 0 || h->nfrag == 0 || h->frag >= h->nfrag) return;
  if (h->sess != g_sess) {
    if ((int16_t)(h->sess - g_sess) < 0 && g_sess) return;  // old session
    g_sess = h->sess;
    g_sess_new = true;
    g_frames.clear();
    g_miss.clear();
    g_have_next = false;
    g_have_seq = false;
    // nothing is decodable until the session's IDR: start "broken" just before the first frame seen
    g_broken = true;
    g_broken_frame = h->frame;
    g_recover_min = h->frame;
    g_last_pli = now;  // first PLI only if no IDR completes within 300 ms
    g_big_hole = false;
    g_clean_from = g_acked = UINT32_MAX;
    g_delays.clear();
    g_D = cfg.fixed_d > 0 ? cfg.fixed_d : 70;
    g_first_data_us = now;
    g_plan_i = 0;
    g_plan_cycle = 0;
    logj("{\"ev\":\"session\",\"sess\":%u,\"t\":%lld,\"f\":%u}", g_sess, (long long)now, h->frame);
  }
  if (!g_have_next) { g_next = h->frame - h->frag * 0; g_have_next = true; g_hi_frame = h->frame; }
  if (h->frame < g_next) return;  // given up already (late retransmission)
  // sequence tracking, NACK bookkeeping
  bool repaired = false;
  if (!g_have_seq) { g_hi_seq = h->seq; g_have_seq = true; }
  else if ((int32_t)(h->seq - g_hi_seq) > 0) {
    uint32_t gap = h->seq - g_hi_seq - 1;
    if (gap > 0) {
      logj("{\"ev\":\"hole\",\"t\":%lld,\"seq0\":%u,\"n\":%u}", (long long)now, g_hi_seq + 1, gap);
      g_lost_pk += gap;
      if (gap > 64) {
        g_big_hole = true;
        g_big_hole_seq = h->seq;
      } else {
        for (uint32_t q = g_hi_seq + 1; q < h->seq; q++) g_miss[q] = {now, 0, 0, false};
      }
    }
    g_hi_seq = h->seq;
  } else {
    auto m = g_miss.find(h->seq);
    if (m != g_miss.end()) { repaired = true; g_miss.erase(m); }
  }
  if (h->frame > g_hi_frame) g_hi_frame = h->frame;
  FrameBuf& fb = g_frames[h->frame];
  if (!fb.nfrag) {
    fb.nfrag = h->nfrag;
    fb.frags.resize(h->nfrag);
    fb.vflags = h->vflags;
    fb.cap_ns = h->cap_ns;
    fb.out_ns = h->out_ns;
    fb.a0 = now;
    fb.seq0 = h->seq - h->frag;
  }
  if (!fb.frags[h->frag].empty() || payload == 0 && h->nfrag > 1) return;  // duplicate
  fb.frags[h->frag].assign(b + sizeof(DataHdr), b + sizeof(DataHdr) + payload);
  if (fb.frags[h->frag].empty()) fb.frags[h->frag].push_back(0);  // never happens with H.264
  fb.got++;
  if (repaired) fb.repaired++;
  if (fb.complete()) { fb.ac = now; cv.notify_one(); }
}

static void net_loop() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  WSAEVENT sev = WSACreateEvent();
  WSAEventSelect(g_sock, sev, FD_READ);
  uint8_t b[2048];
  int64_t t_end = unix_us() + (int64_t)(cfg.seconds * 1e6), next_hello = 0, next_ping = 0, next_rr = 0, next_stats = unix_us() + 10000000;
  bool mac_seen = false;
  uint32_t ping_id = 0;
  LONG64 seen_state = -1;
  FILETIME fa, fb2, k0, u0;
  GetProcessTimes(GetCurrentProcess(), &fa, &fb2, &k0, &u0);
  auto ft = [](FILETIME f) { return (((ULONGLONG)f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  ULONGLONG cpu0 = ft(k0) + ft(u0);
  ULONG64 cyc0 = 0;
  QueryProcessCycleTime(GetCurrentProcess(), &cyc0);
  DWORD mhz = 0, mhz_sz = sizeof mhz;
  RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &mhz_sz);
  int64_t wall0 = unix_us();
  uint64_t rx_bytes = 0;
  while (!g_quit && unix_us() < t_end) {
    HANDLE hs[2] = {sev, ring.demand_ev};
    WaitForMultipleObjects(2, hs, FALSE, 2);
    WSAResetEvent(sev);  // manual-reset network event: re-armed by the recv() calls below
    int64_t now = unix_us();
    // demand from the virtual camera
    ring.try_open();
    if (ring.hdr && ring.hdr->state_seq != seen_state) {
      seen_state = ring.hdr->state_seq;
      bool s = ring.hdr->streaming != 0;
      std::lock_guard<std::mutex> lk(mu);
      logj("{\"ev\":\"demand\",\"t\":%lld,\"src_t\":%lld,\"streaming\":%d,\"want\":\"%ldx%ld\"}", (long long)now, (long long)ring.hdr->state_us, s,
           ring.hdr->want_w, ring.hdr->want_h);
      if (s != g_want || (s && (ring.hdr->want_w != g_want_w || ring.hdr->want_h != g_want_h))) {
        g_want = s;
        if (s) { g_want_w = ring.hdr->want_w; g_want_h = ring.hdr->want_h; }
        g_req++;
        g_req_acked = false;
        g_req_t = 0;
      }
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      if (!g_req_acked && now - g_req_t > 100000) {
        if (g_want) {
          Start m = {};
          m.ptype = kStart;
          m.idr_pace_x10 = (uint8_t)std::lround(cfg.idr_pace * 10);
          m.w = (uint16_t)g_want_w;
          m.h = (uint16_t)g_want_h;
          m.key_every = (uint16_t)cfg.key_every;
          m.req = g_req;
          m.bitrate = g_want_h >= 1080 ? cfg.bitrate1080 : cfg.bitrate720;
          m.audio = cfg.audio;
          tx(&m, sizeof m);
        } else {
          Stop m = {};
          m.ptype = kStop;
          m.req = g_req;
          tx(&m, sizeof m);
        }
        logj("{\"ev\":\"%s_tx\",\"t\":%lld,\"req\":%u}", g_want ? "start" : "stop", (long long)now, g_req);
        g_req_t = now;
      }
    }
    if (now >= next_hello) {
      Hello h = {kHello, 2, 0, 0};
      tx(&h, sizeof h);
      next_hello = now + (mac_seen ? 1000000 : 100000);
    }
    if (now >= next_ping) {
      Ping p = {};
      p.ptype = kPing;
      p.id = ++ping_id;
      p.pc_us = unix_us();
      tx(&p, sizeof p);
      next_ping = now + 200000;
    }
    {
      std::lock_guard<std::mutex> lk(mu);
      if (g_want && g_sess && now >= next_rr) {
        RR r = {};
        r.ptype = kRR;
        r.flags = g_broken ? 1 : 0;
        r.sess = g_sess;
        r.acked_frame = g_acked;
        r.clean_from = g_clean_from;
        r.hi_seq = g_hi_seq;
        r.lost = g_lost_pk;
        r.late = g_late;
        r.shown = g_shown;
        r.playout_ms = (uint16_t)g_D;
        tx(&r, sizeof r);
        next_rr = now + 10000;
      }
      // NACKs: new holes at once, repeats after max(2 RTT, 15 ms), 3 tries
      Nack k = {};
      k.ptype = kNack;
      k.sess = g_sess;
      double rep = std::max(15000.0, 2 * g_rtt_min_ms * 1000 * 2);
      for (auto& m : g_miss) {
        MissState& s = m.second;
        if (s.exhausted) continue;
        if (s.tries == 0 || now - s.t_last > rep) {
          if (s.tries >= 3) { s.exhausted = true; continue; }
          if (k.n < 32) { k.seq[k.n++] = m.first; s.tries++; s.t_last = now; }
        }
      }
      if (k.n) {
        tx(&k, 4 + 4 * k.n);
        logj("{\"ev\":\"nack\",\"t\":%lld,\"n\":%u,\"seq0\":%u}", (long long)now, k.n, k.seq[0]);
      }
      // forget missing entries far behind
      while (!g_miss.empty() && (int32_t)(g_hi_seq - g_miss.begin()->first) > 2048) g_miss.erase(g_miss.begin());
    }
    if (now >= next_stats) {
      FILETIME a, c, k1, u1;
      GetProcessTimes(GetCurrentProcess(), &a, &c, &k1, &u1);
      ULONGLONG cpu1 = ft(k1) + ft(u1);
      ULONG64 cyc1 = 0;
      QueryProcessCycleTime(GetCurrentProcess(), &cyc1);
      PROCESS_MEMORY_COUNTERS_EX pm = {};
      GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
      std::lock_guard<std::mutex> lk(mu);
      double wall = (now - wall0) / 1e6;
      logj("{\"ev\":\"stats\",\"t\":%lld,\"cpu_pct\":%.2f,\"cpu_cyc_pct\":%.2f,\"oracle_cpu_pct\":%.2f,\"ws_mb\":%.1f,\"priv_mb\":%.1f,\"rx_kbps\":%.0f,\"D\":%.1f,\"off\":%.1f,\"rtt_min\":%.2f,"
           "\"shown\":%u,\"late\":%u,\"hidden\":%u,\"lost_frames\":%u,\"lost_pk\":%u,\"injected\":%u,\"oracle_n\":%u,\"oracle_bad\":%u,"
           "\"oracle_skip\":%u,\"frames_buf\":%zu,\"dq\":%zu,\"delivered\":%lld,\"placeholders\":%lld}",
           (long long)now, (cpu1 - cpu0) / 1e7 / wall * 100, (double)(cyc1 - cyc0) / (mhz * 1e6) / wall * 100, (double)g_oracle_cpu_100ns.exchange(0) / (mhz * 1e6) / wall * 100, pm.WorkingSetSize / 1048576.0, pm.PrivateUsage / 1048576.0, rx_bytes * 8 / wall / 1000, g_D,
           g_off_us, g_rtt_min_ms, g_shown, g_late, g_hidden, g_lost_frames, g_lost_pk, g_injected, g_oracle_n.load(), g_oracle_bad.load(),
           g_oracle_skip.load(), g_frames.size(), g_dq.size(), ring.hdr ? ring.hdr->delivered : -1, ring.hdr ? ring.hdr->placeholders : -1);
      cpu0 = cpu1;
      cyc0 = cyc1;
      wall0 = now;
      rx_bytes = 0;
      next_stats = now + 10000000;
      if (g_audio_f) fflush(g_audio_f);
    }
    // receive everything pending
    for (;;) {
      int n = recv(g_sock, (char*)b, sizeof b, 0);
      if (n <= 0) break;
      int64_t t = unix_us();
      rx_bytes += n;
      std::lock_guard<std::mutex> lk(mu);
      if (inject(t, b[0] == kData)) continue;
      switch (b[0]) {
        case kData: on_data(b, n, t); break;
        case kHello:
          if (!mac_seen) logj("{\"ev\":\"mac_hello\",\"t\":%lld}", (long long)t);
          mac_seen = true;
          break;
        case kPong:
          if (n >= (int)sizeof(Pong)) {
            const Pong* p = (const Pong*)b;
            g_pings.push_back({p->pc_us, t, p->mac_ns});
            logj("{\"ev\":\"ping\",\"t1\":%lld,\"t2\":%lld,\"mac\":%llu}", (long long)p->pc_us, (long long)t, p->mac_ns);
            while (!g_pings.empty() && t - g_pings.front().t2 > 10000000) g_pings.pop_front();
            const PingS* best = nullptr;
            for (auto& q : g_pings)
              if (!best || q.t2 - q.t1 < best->t2 - best->t1) best = &q;
            g_off_us = (best->t1 + best->t2) / 2.0 - best->mac / 1000.0;
            g_rtt_min_ms = (best->t2 - best->t1) / 1000.0;
            g_off_ok = true;
          }
          break;
        case kState:
          if (n >= (int)sizeof(State)) {
            const State* s = (const State*)b;
            if (s->req == g_req && !g_req_acked) {
              g_req_acked = true;
              logj("{\"ev\":\"state_rx\",\"t\":%lld,\"req\":%u,\"running\":%u,\"sess\":%u,\"mac\":%llu}", (long long)t, s->req, s->running, s->sess, s->mac_ns);
            }
          }
          break;
        case kAudio:
          if (n >= (int)sizeof(Audio) && g_audio_f) fprintf(g_audio_f, "%u,%llu,%lld\n", ((const Audio*)b)->seq, ((const Audio*)b)->send_ns, (long long)t);
          break;
      }
    }
  }
  g_quit = true;
}

int main(int argc, char** argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto nx = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--mac") cfg.mac = nx();
    else if (a == "--log") cfg.log = nx();
    else if (a == "--seconds") cfg.seconds = atof(nx().c_str());
    else if (a == "--idr-pace") cfg.idr_pace = atof(nx().c_str());
    else if (a == "--key-every") cfg.key_every = atoi(nx().c_str());
    else if (a == "--bitrate720") cfg.bitrate720 = atoi(nx().c_str());
    else if (a == "--bitrate1080") cfg.bitrate1080 = atoi(nx().c_str());
    else if (a == "--audio") cfg.audio = true;
    else if (a == "--no-oracle") cfg.oracle = false;
    else if (a == "--loss") cfg.loss = nx();
    else if (a == "--fixed-d") cfg.fixed_d = atoi(nx().c_str());
    else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
  }
  if (!cfg.log.empty()) g_log = fopen(cfg.log.c_str(), "w");
  if (!cfg.loss.empty()) parse_loss(cfg.loss);
  if (cfg.audio && !cfg.log.empty()) {
    g_audio_f = fopen((cfg.log + ".audio.csv").c_str(), "w");
    fprintf(g_audio_f, "seq,send_ns,arr_us\n");
  }
  init_noise();
  timeBeginPeriod(1);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  MFStartup(MF_VERSION);
  WSADATA wd;
  WSAStartup(MAKEWORD(2, 2), &wd);
  if (!ring.create_demand()) { logj("{\"ev\":\"error\",\"what\":\"demand event\",\"err\":%lu}", GetLastError()); return 1; }
  g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  BOOL no = FALSE;
  DWORD ret = 0;
  WSAIoctl(g_sock, SIO_UDP_CONNRESET, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
  int buf = 4 << 20;
  setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, (const char*)&buf, sizeof buf);
  sockaddr_in me = {};
  me.sin_family = AF_INET;
  me.sin_port = htons(kPcPort);
  if (bind(g_sock, (sockaddr*)&me, sizeof me) != 0) { logj("{\"ev\":\"error\",\"what\":\"bind\",\"err\":%d}", WSAGetLastError()); return 1; }
  sockaddr_in peer = {};
  peer.sin_family = AF_INET;
  peer.sin_port = htons(kMacPort);
  inet_pton(AF_INET, cfg.mac.c_str(), &peer.sin_addr);
  connect(g_sock, (sockaddr*)&peer, sizeof peer);  // the PC sends first (punch): Windows admits the Mac's answers
  logj("{\"ev\":\"boot\",\"t\":%lld,\"mac\":\"%s\",\"idr_pace\":%.1f,\"key_every\":%d,\"loss\":\"%s\",\"session_id\":%lu}", (long long)unix_us(), cfg.mac.c_str(),
       cfg.idr_pace, cfg.key_every, cfg.loss.c_str(), [] { DWORD s = 0; ProcessIdToSessionId(GetCurrentProcessId(), &s); return s; }());
  std::thread td(decode_thread), tdisp(display_thread), tor(oracle_thread);
  net_loop();
  g_quit = true;
  cv.notify_all();
  dcv.notify_all();
  ocv.notify_all();
  td.join();
  tdisp.join();
  tor.join();
  if (g_audio_f) fclose(g_audio_f);
  logj("{\"ev\":\"exit\",\"t\":%lld}", (long long)unix_us());
  return 0;
}
