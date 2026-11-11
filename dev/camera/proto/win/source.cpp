// SPDX-License-Identifier: GPL-3.0-only
// C2 prototype: custom Media Foundation media source for a Windows 11 virtual camera
// (MFCreateVirtualCamera). Loaded by the Windows Camera Frame Server (svchost -k Camera, LocalService).
// Design follows Microsoft's VirtualCamera sample (activate -> source -> one shared stream); own code.
//
// Frames: a helper in the user session writes NV12 frames into a shared-memory ring (common.h) and
// signals an event; the stream hands the newest frame to Frame Server as soon as it has a pending
// RequestSample. Without a live helper it delivers a stamped test pattern (helper never seen) or a
// placeholder (helper went away). Start/Stop are published in the shared header and signalled to the
// helper through a named event, so the helper can turn the Mac camera on and off on demand.
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>
#include <wrl.h>
#include <deque>
#include <mutex>
#include <vector>
#include <string>
#include "common.h"

using namespace Microsoft::WRL;
using namespace c2;

static HMODULE g_module;
static volatile LONG g_objects;

// ---- log ------------------------------------------------------------------------------------------
static std::mutex g_log_mu;
static void logf(const char* fmt, ...) {
  char buf[1024];
  int n = snprintf(buf, sizeof buf, "%lld pid=%lu tid=%lu ", (long long)unix_us(), GetCurrentProcessId(), GetCurrentThreadId());
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(buf + n, sizeof buf - n - 2, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
  buf[n++] = '\n';
  std::lock_guard<std::mutex> g(g_log_mu);
  HANDLE f = CreateFileA("C:\\ProgramData\\BeamerCamC2\\source.log", FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD w;
    WriteFile(f, buf, n, &w, nullptr);
    CloseHandle(f);
  }
  OutputDebugStringA(buf);
}
#define LOGHR(expr)                                                       \
  do {                                                                    \
    HRESULT _hr = (expr);                                                 \
    if (FAILED(_hr)) {                                                    \
      logf("FAILED 0x%08lx %s (line %d)", (unsigned long)_hr, #expr, __LINE__); \
      return _hr;                                                         \
    }                                                                     \
  } while (0)

struct ObjCount {
  ObjCount() { InterlockedIncrement(&g_objects); }
  ~ObjCount() { InterlockedDecrement(&g_objects); }
};

// ---- config (C:\ProgramData\BeamerCamC2\config.txt: "formats=nv12" or "formats=nv12,yuy2") ----------
static bool cfg_yuy2() {
  char buf[256] = {0};
  HANDLE f = CreateFileA("C:\\ProgramData\\BeamerCamC2\\config.txt", GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD r = 0;
  ReadFile(f, buf, sizeof buf - 1, &r, nullptr);
  CloseHandle(f);
  return strstr(buf, "yuy2") != nullptr;
}

// ---- IPC (owned by the source) ----------------------------------------------------------------------
struct Ipc {
  HANDLE map = nullptr, frame_ev = nullptr;
  Header* hdr = nullptr;
  bool open() {
    SECURITY_ATTRIBUTES sa;
    if (!make_sa(&sa)) return false;
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, (DWORD)(kShmBytes >> 32), (DWORD)kShmBytes, kShmName);
    if (!map) {
      logf("ipc: CreateFileMapping failed %lu", GetLastError());
      return false;
    }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    hdr = (Header*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kShmBytes);
    if (!hdr) return false;
    if (!existed || hdr->magic != kMagic) {
      memset(hdr, 0, sizeof(Header));
      hdr->latest = -1;
      hdr->version = 1;
      hdr->magic = kMagic;
    }
    frame_ev = CreateEventW(&sa, FALSE, FALSE, kFrameEventName);
    logf("ipc: section %s (%zu bytes), frame event %p", existed ? "opened" : "created", kShmBytes, frame_ev);
    return true;
  }
  void signal_demand() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, kDemandEventName);
    if (ev) {
      SetEvent(ev);
      CloseHandle(ev);
    } else {
      logf("ipc: no demand event (helper not running?) err=%lu", GetLastError());
    }
  }
  void set_state(bool streaming, int w, int h) {
    if (!hdr) return;
    hdr->want_w = w;
    hdr->want_h = h;
    hdr->state_us = unix_us();
    InterlockedExchange(&hdr->streaming, streaming ? 1 : 0);
    InterlockedIncrement64(&hdr->state_seq);
    signal_demand();
  }
  void close() {
    if (hdr) {
      InterlockedExchange(&hdr->streaming, 0);
      InterlockedIncrement64(&hdr->state_seq);
      signal_demand();
      UnmapViewOfFile(hdr);
    }
    if (map) CloseHandle(map);
    if (frame_ev) CloseHandle(frame_ev);
    hdr = nullptr;
    map = frame_ev = nullptr;
  }
};

// ---- media types ------------------------------------------------------------------------------------
static HRESULT make_type(const GUID& sub, UINT32 w, UINT32 h, IMFMediaType** out) {
  ComPtr<IMFMediaType> t;
  LOGHR(MFCreateMediaType(&t));
  t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  t->SetGUID(MF_MT_SUBTYPE, sub);
  MFSetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, w, h);
  MFSetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, 30, 1);
  MFSetAttributeRatio(t.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
  t->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
  t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
  t->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
  bool yuy2 = sub == MFVideoFormat_YUY2;
  t->SetUINT32(MF_MT_DEFAULT_STRIDE, yuy2 ? w * 2 : w);
  t->SetUINT32(MF_MT_SAMPLE_SIZE, yuy2 ? w * h * 2 : w * h * 3 / 2);
  t->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)((yuy2 ? w * h * 2 : w * h * 3 / 2) * 8 * 30));
  *out = t.Detach();
  return S_OK;
}

class Source;

// ---- stream -----------------------------------------------------------------------------------------
class Stream : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                                   IKsControl, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize(Source* parent, Ipc* ipc);
  void StartStreaming(IMFMediaType* type);
  void StopStreaming();
  void Shutdown();
  ComPtr<IMFAttributes> attrs;

  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return q_ ? q_->BeginGetEvent(cb, st) : MF_E_SHUTDOWN; }
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return q_ ? q_->EndGetEvent(r, e) : MF_E_SHUTDOWN; }
  IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
    ComPtr<IMFMediaEventQueue> q;
    {
      std::lock_guard<std::recursive_mutex> g(mu_);
      q = q_;
    }
    return q ? q->GetEvent(f, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
    return q_ ? q_->QueueEventParamVar(t, g, s, v) : MF_E_SHUTDOWN;
  }
  // IMFMediaStream
  IFACEMETHODIMP GetMediaSource(IMFMediaSource** out) override;
  IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** out) override {
    if (!out) return E_POINTER;
    if (!sd_) return MF_E_SHUTDOWN;
    return sd_.CopyTo(out);
  }
  IFACEMETHODIMP RequestSample(IUnknown* token) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;
    pending_.push_back(ComPtr<IUnknown>(token));
    requests_++;
    SetEvent(request_ev_);
    return S_OK;
  }
  // IMFMediaStream2
  IFACEMETHODIMP SetStreamState(MF_STREAM_STATE v) override {
    logf("stream: SetStreamState %d (was %d)", (int)v, (int)state_);
    if (v == MF_STREAM_STATE_RUNNING) {
      std::lock_guard<std::recursive_mutex> g(mu_);
      if (state_ != MF_STREAM_STATE_RUNNING) {
        state_ = MF_STREAM_STATE_RUNNING;
        publish(true);
      }
    } else {
      std::lock_guard<std::recursive_mutex> g(mu_);
      if (state_ == MF_STREAM_STATE_RUNNING) publish(false);
      state_ = v;
      pending_.clear();
    }
    return S_OK;
  }
  IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* v) override {
    if (!v) return E_POINTER;
    *v = state_;
    return S_OK;
  }
  // IKsControl
  IFACEMETHODIMP KsProperty(PKSPROPERTY p, ULONG, LPVOID, ULONG, ULONG*) override {
    (void)p;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }
  IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

  ~Stream() { Shutdown(); }

 private:
  void publish(bool on) {
    if (ipc_) ipc_->set_state(on, (int)w_, (int)h_);
    logf("stream: %s %ux%u %s", on ? "START" : "STOP", w_, h_, yuy2_ ? "YUY2" : "NV12");
  }
  void run();
  bool deliver(uint8_t origin);
  static DWORD WINAPI thread_main(void* p) {
    ((Stream*)p)->run();
    return 0;
  }

  std::recursive_mutex mu_;
  Source* parent_ = nullptr;  // raw: the source owns the stream, Shutdown breaks the link
  Ipc* ipc_ = nullptr;
  ComPtr<IMFMediaEventQueue> q_;
  ComPtr<IMFStreamDescriptor> sd_;
  ComPtr<IMFVideoSampleAllocatorEx> alloc_;
  MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
  std::deque<ComPtr<IUnknown>> pending_;
  HANDLE stop_ev_ = nullptr, request_ev_ = nullptr, thread_ = nullptr;
  UINT32 w_ = 1280, h_ = 720;
  bool yuy2_ = false;
  std::vector<uint8_t> tmp_;  // NV12 scratch (YUY2 or scaling)
  uint32_t counter_ = 0;
  int64_t last_helper_frame_ = -1;
  bool helper_seen_ = false;
  int64_t next_due_us_ = 0;
  uint64_t requests_ = 0, delivered_ = 0, empty_ = 0;
  int64_t copy_us_sum_ = 0;
};

// ---- source -----------------------------------------------------------------------------------------
class Source : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                                   IMFGetService, IKsControl, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize(IMFAttributes* activate_attrs) {
    logf("source: Initialize");
    LOGHR(MFCreateAttributes(&attrs_, 10));
    if (activate_attrs) activate_attrs->CopyAllItems(attrs_.Get());
    LOGHR(MFCreateEventQueue(&q_));
    ipc_.open();
    stream_ = Make<Stream>();
    if (!stream_) return E_OUTOFMEMORY;
    LOGHR(stream_->Initialize(this, &ipc_));
    ComPtr<IMFStreamDescriptor> sd;
    LOGHR(stream_->GetStreamDescriptor(&sd));
    IMFStreamDescriptor* sds[] = {sd.Get()};
    LOGHR(MFCreatePresentationDescriptor(1, sds, &pd_));
    LOGHR(pd_->SelectStream(0));
    return S_OK;
  }
  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* cb, IUnknown* st) override { return q_ ? q_->BeginGetEvent(cb, st) : MF_E_SHUTDOWN; }
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* r, IMFMediaEvent** e) override { return q_ ? q_->EndGetEvent(r, e) : MF_E_SHUTDOWN; }
  IFACEMETHODIMP GetEvent(DWORD f, IMFMediaEvent** e) override {
    ComPtr<IMFMediaEventQueue> q;
    {
      std::lock_guard<std::recursive_mutex> g(mu_);
      q = q_;
    }
    return q ? q->GetEvent(f, e) : MF_E_SHUTDOWN;
  }
  IFACEMETHODIMP QueueEvent(MediaEventType t, REFGUID g, HRESULT s, const PROPVARIANT* v) override {
    return q_ ? q_->QueueEventParamVar(t, g, s, v) : MF_E_SHUTDOWN;
  }
  // IMFMediaSource
  IFACEMETHODIMP GetCharacteristics(DWORD* c) override {
    if (!c) return E_POINTER;
    if (!q_) return MF_E_SHUTDOWN;
    *c = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
  }
  IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** out) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!out) return E_POINTER;
    if (!q_) return MF_E_SHUTDOWN;
    return pd_->Clone(out);
  }
  IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* fmt, const PROPVARIANT* pos) override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!pd || !pos) return E_INVALIDARG;
    if (!q_) return MF_E_SHUTDOWN;
    if (fmt && *fmt != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    DWORD n = 0;
    LOGHR(pd->GetStreamDescriptorCount(&n));
    for (DWORD i = 0; i < n; i++) {
      BOOL selected = FALSE;
      ComPtr<IMFStreamDescriptor> sd;
      LOGHR(pd->GetStreamDescriptorByIndex(i, &selected, &sd));
      if (!selected) continue;
      ComPtr<IMFMediaTypeHandler> h;
      ComPtr<IMFMediaType> type;
      LOGHR(sd->GetMediaTypeHandler(&h));
      LOGHR(h->GetCurrentMediaType(&type));
      LOGHR(q_->QueueEventParamUnk(started_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK, (IMFMediaStream*)stream_.Get()));
      stream_->StartStreaming(type.Get());
    }
    started_ = true;
    logf("source: Start");
    return q_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pos);
  }
  IFACEMETHODIMP Stop() override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    stream_->StopStreaming();
    logf("source: Stop");
    return q_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
  }
  IFACEMETHODIMP Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
  IFACEMETHODIMP Shutdown() override {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_) return MF_E_SHUTDOWN;
    logf("source: Shutdown");
    if (stream_) stream_->Shutdown();
    q_->Shutdown();
    q_.Reset();
    ipc_.close();
    return S_OK;
  }
  // IMFMediaSourceEx
  IFACEMETHODIMP GetSourceAttributes(IMFAttributes** out) override {
    if (!out) return E_POINTER;
    return attrs_.CopyTo(out);
  }
  IFACEMETHODIMP GetStreamAttributes(DWORD id, IMFAttributes** out) override {
    if (!out) return E_POINTER;
    if (id != 0) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->attrs.CopyTo(out);
  }
  IFACEMETHODIMP SetD3DManager(IUnknown* m) override {
    logf("source: SetD3DManager %p (ignored, system-memory samples)", m);
    return S_OK;
  }
  // IMFGetService
  IFACEMETHODIMP GetService(REFGUID, REFIID, LPVOID*) override { return MF_E_UNSUPPORTED_SERVICE; }
  // IKsControl
  IFACEMETHODIMP KsProperty(PKSPROPERTY p, ULONG, LPVOID, ULONG, ULONG*) override {
    (void)p;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
  }
  IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }
  IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override { return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND); }

  ~Source() {
    logf("source: destroyed");
    ipc_.close();
  }

 private:
  std::recursive_mutex mu_;
  ComPtr<IMFAttributes> attrs_;
  ComPtr<IMFMediaEventQueue> q_;
  ComPtr<IMFPresentationDescriptor> pd_;
  ComPtr<Stream> stream_;
  Ipc ipc_;
  bool started_ = false;
};

HRESULT Stream::Initialize(Source* parent, Ipc* ipc) {
  parent_ = parent;
  ipc_ = ipc;
  yuy2_ = false;
  LOGHR(MFCreateAttributes(&attrs, 10));
  attrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
  attrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
  attrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
  attrs->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
  std::vector<ComPtr<IMFMediaType>> types;
  const UINT32 sizes[][2] = {{1280, 720}, {1920, 1080}, {640, 360}};
  bool with_yuy2 = cfg_yuy2();
  for (auto& s : sizes) {
    ComPtr<IMFMediaType> t;
    LOGHR(make_type(MFVideoFormat_NV12, s[0], s[1], &t));
    types.push_back(t);
  }
  if (with_yuy2)
    for (auto& s : sizes) {
      ComPtr<IMFMediaType> t;
      LOGHR(make_type(MFVideoFormat_YUY2, s[0], s[1], &t));
      types.push_back(t);
    }
  std::vector<IMFMediaType*> raw;
  for (auto& t : types) raw.push_back(t.Get());
  LOGHR(MFCreateStreamDescriptor(0, (DWORD)raw.size(), raw.data(), &sd_));
  ComPtr<IMFMediaTypeHandler> h;
  LOGHR(sd_->GetMediaTypeHandler(&h));
  LOGHR(h->SetCurrentMediaType(raw[0]));
  LOGHR(attrs->CopyAllItems(sd_.Get()));
  LOGHR(MFCreateEventQueue(&q_));
  stop_ev_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  request_ev_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  thread_ = CreateThread(nullptr, 0, thread_main, this, 0, nullptr);
  logf("stream: Initialize, %zu media types (NV12%s)", types.size(), with_yuy2 ? " + YUY2" : "");
  return S_OK;
}

IFACEMETHODIMP Stream::GetMediaSource(IMFMediaSource** out) {
  std::lock_guard<std::recursive_mutex> g(mu_);
  if (!out) return E_POINTER;
  if (!parent_ || !q_) return MF_E_SHUTDOWN;
  return parent_->QueryInterface(IID_PPV_ARGS(out));
}

void Stream::StartStreaming(IMFMediaType* type) {
  std::lock_guard<std::recursive_mutex> g(mu_);
  GUID sub = GUID_NULL;
  type->GetGUID(MF_MT_SUBTYPE, &sub);
  MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w_, &h_);
  yuy2_ = sub == MFVideoFormat_YUY2;
  alloc_.Reset();
  HRESULT hr = MFCreateVideoSampleAllocatorEx(IID_PPV_ARGS(&alloc_));
  if (SUCCEEDED(hr)) hr = alloc_->InitializeSampleAllocator(10, type);
  if (FAILED(hr)) logf("stream: allocator failed 0x%08lx", (unsigned long)hr);
  tmp_.resize((size_t)w_ * h_ * 3 / 2);
  pending_.clear();
  next_due_us_ = unix_us();
  requests_ = delivered_ = empty_ = 0;
  copy_us_sum_ = 0;
  last_helper_frame_ = -1;
  if (ipc_ && ipc_->hdr) ipc_->hdr->delivered = ipc_->hdr->placeholders = ipc_->hdr->dropped = 0;
  state_ = MF_STREAM_STATE_RUNNING;
  q_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
  publish(true);
}

void Stream::StopStreaming() {
  std::lock_guard<std::recursive_mutex> g(mu_);
  if (state_ == MF_STREAM_STATE_RUNNING) publish(false);
  logf("stream: stats requests=%llu delivered=%llu allocator_empty=%llu copy_avg_us=%.1f", requests_, delivered_, empty_,
       delivered_ ? (double)copy_us_sum_ / delivered_ : 0.0);
  state_ = MF_STREAM_STATE_STOPPED;
  pending_.clear();
  if (q_) q_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void Stream::Shutdown() {
  HANDLE th = nullptr;
  {
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (state_ == MF_STREAM_STATE_RUNNING) {
      publish(false);
      state_ = MF_STREAM_STATE_STOPPED;
    }
    if (q_) {
      q_->Shutdown();
      q_.Reset();
    }
    pending_.clear();
    parent_ = nullptr;
    th = thread_;
    thread_ = nullptr;
    if (stop_ev_) SetEvent(stop_ev_);
  }
  if (th) {
    if (GetCurrentThreadId() != GetThreadId(th)) WaitForSingleObject(th, 2000);
    CloseHandle(th);
  }
  std::lock_guard<std::recursive_mutex> g(mu_);
  alloc_.Reset();
  ipc_ = nullptr;
}

// Fills one sample from the newest helper frame or a generated pattern, and queues it.
bool Stream::deliver(uint8_t origin) {
  ComPtr<IUnknown> token = pending_.front();
  ComPtr<IMFSample> sample;
  HRESULT hr = alloc_ ? alloc_->AllocateSample(&sample) : E_FAIL;
  if (FAILED(hr)) {
    empty_++;
    return false;
  }
  pending_.pop_front();
  int64_t t0 = unix_us();
  ComPtr<IMFMediaBuffer> buf;
  sample->GetBufferByIndex(0, &buf);
  ComPtr<IMF2DBuffer2> b2;
  BYTE *scan0 = nullptr, *start = nullptr;
  LONG pitch = 0;
  DWORD len = 0;
  bool locked = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Write, &scan0, &pitch, &start, &len));
  if (!locked) {
    logf("stream: buffer lock failed");
    return false;
  }
  // NV12 target: write straight into the sample, else into tmp_ and convert
  uint8_t* nv12 = yuy2_ ? tmp_.data() : scan0;
  int nstride = yuy2_ ? (int)w_ : (int)pitch;
  bool filled = false;
  if (origin == kOriginHelper && ipc_ && ipc_->hdr) {
    Header* hd = ipc_->hdr;
    LONG i = hd->latest;
    for (int attempt = 0; attempt < 3 && i >= 0 && i < (LONG)kSlots; attempt++) {
      Slot& s = hd->slots[i];
      LONG64 s1 = s.seq;
      if (s1 & 1) continue;
      MemoryBarrier();
      int sw = (int)s.width, sh = (int)s.height;
      if (sw <= 0 || sh <= 0 || sw > (int)kMaxW || sh > (int)kMaxH) break;
      const uint8_t* src = slot_data(hd, i);
      if (sw == (int)w_ && sh == (int)h_) {
        for (int y = 0; y < sh; y++) memcpy(nv12 + (size_t)y * nstride, src + (size_t)y * sw, sw);
        for (int y = 0; y < sh / 2; y++) memcpy(nv12 + (size_t)nstride * h_ + (size_t)y * nstride, src + (size_t)sw * sh + (size_t)y * sw, sw);
      } else {
        scale_nv12(src, sw, sh, nv12, nstride, (int)w_, (int)h_);
      }
      int64_t frame = s.frame;
      MemoryBarrier();
      if (s.seq == s1) {
        if (last_helper_frame_ >= 0 && frame > last_helper_frame_ + 1) InterlockedAdd64(&hd->dropped, frame - last_helper_frame_ - 1);
        last_helper_frame_ = frame;
        filled = true;
        break;
      }
    }
  }
  if (!filled) {
    if (origin == kOriginHelper) origin = kOriginPlaceholder;
    pattern_nv12(nv12, nstride, (int)w_, (int)h_, counter_, origin == kOriginPlaceholder);
    stamp_nv12(nv12, nstride, (int)w_, (int)h_, counter_, (uint32_t)unix_us(), origin);
  }
  if (yuy2_) nv12_to_yuy2(tmp_.data(), (int)w_, (int)w_, (int)h_, scan0, (int)pitch);
  b2->Unlock2D();
  buf->SetCurrentLength(len);
  copy_us_sum_ += unix_us() - t0;
  counter_++;
  sample->SetSampleTime(MFGetSystemTime());
  sample->SetSampleDuration(333333);
  if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());
  hr = q_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
  if (FAILED(hr)) logf("stream: queue sample failed 0x%08lx", (unsigned long)hr);
  delivered_++;
  if (ipc_ && ipc_->hdr) {
    InterlockedIncrement64(&ipc_->hdr->delivered);
    if (origin != kOriginHelper) InterlockedIncrement64(&ipc_->hdr->placeholders);
  }
  return true;
}

void Stream::run() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // Higher priority than normal: delivery is the latency-critical path.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
  for (;;) {
    HANDLE hs[3] = {stop_ev_, request_ev_, ipc_ && ipc_->frame_ev ? ipc_->frame_ev : request_ev_};
    DWORD timeout = 10;
    {
      std::lock_guard<std::recursive_mutex> g(mu_);
      int64_t now = unix_us();
      if (state_ == MF_STREAM_STATE_RUNNING && next_due_us_ > now) timeout = (DWORD)((next_due_us_ - now) / 1000);
      if (timeout > 33) timeout = 33;
    }
    DWORD r = WaitForMultipleObjects(3, hs, FALSE, timeout);
    if (r == WAIT_OBJECT_0) break;
    std::lock_guard<std::recursive_mutex> g(mu_);
    if (!q_ || state_ != MF_STREAM_STATE_RUNNING) continue;
    int64_t now = unix_us();
    Header* hd = ipc_ ? ipc_->hdr : nullptr;
    if (hd) hd->source_heartbeat_us = now;
    bool helper_live = false;
    int64_t newest = -1;
    if (hd && hd->latest >= 0 && hd->latest < (LONG)kSlots && now - hd->slots[hd->latest].write_us < 500000) {
      newest = hd->slots[hd->latest].frame;
      helper_live = true;
      if (!helper_seen_) logf("stream: helper connected");
      helper_seen_ = true;
    }
    if (pending_.empty()) continue;
    if (helper_live) {
      if (newest != last_helper_frame_) {
        deliver(kOriginHelper);
        next_due_us_ = now + 100000;  // placeholder only after 100 ms without helper frames
      }
    } else if (now >= next_due_us_) {
      deliver(helper_seen_ ? kOriginPlaceholder : kOriginPattern);
      next_due_us_ += 33333;
      if (next_due_us_ < now - 100000) next_due_us_ = now + 33333;
    }
  }
  CoUninitialize();
}

// ---- activate ---------------------------------------------------------------------------------------
#define FWD(name, params, args) \
  IFACEMETHODIMP name params override { return a_->name args; }
class Activate : public RuntimeClass<RuntimeClassFlags<ClassicCom>, ChainInterfaces<IMFActivate, IMFAttributes>, FtmBase> {
  ObjCount count_;

 public:
  HRESULT Initialize() {
    logf("activate: created");
    return MFCreateAttributes(&a_, 4);
  }
  IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override {
    std::lock_guard<std::mutex> g(mu_);
    if (!src_) {
      src_ = Make<Source>();
      if (!src_) return E_OUTOFMEMORY;
      HRESULT hr = src_->Initialize(a_.Get());
      if (FAILED(hr)) {
        src_.Reset();
        return hr;
      }
    }
    logf("activate: ActivateObject");
    return src_.CopyTo(riid, ppv);
  }
  IFACEMETHODIMP ShutdownObject() override {
    std::lock_guard<std::mutex> g(mu_);
    logf("activate: ShutdownObject");
    if (src_) src_->Shutdown();
    src_.Reset();
    return S_OK;
  }
  IFACEMETHODIMP DetachObject() override {
    std::lock_guard<std::mutex> g(mu_);
    logf("activate: DetachObject");
    src_.Reset();
    return S_OK;
  }
  FWD(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
  FWD(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
  FWD(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
  FWD(Compare, (IMFAttributes * o, MF_ATTRIBUTES_MATCH_TYPE t, BOOL* r), (o, t, r))
  FWD(GetUINT32, (REFGUID k, UINT32* v), (k, v))
  FWD(GetUINT64, (REFGUID k, UINT64* v), (k, v))
  FWD(GetDouble, (REFGUID k, double* v), (k, v))
  FWD(GetGUID, (REFGUID k, GUID* v), (k, v))
  FWD(GetStringLength, (REFGUID k, UINT32* v), (k, v))
  FWD(GetString, (REFGUID k, LPWSTR v, UINT32 n, UINT32* l), (k, v, n, l))
  FWD(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* l), (k, v, l))
  FWD(GetBlobSize, (REFGUID k, UINT32* v), (k, v))
  FWD(GetBlob, (REFGUID k, UINT8* v, UINT32 n, UINT32* l), (k, v, n, l))
  FWD(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* l), (k, v, l))
  FWD(GetUnknown, (REFGUID k, REFIID r, LPVOID* v), (k, r, v))
  FWD(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
  FWD(DeleteItem, (REFGUID k), (k))
  FWD(DeleteAllItems, (), ())
  FWD(SetUINT32, (REFGUID k, UINT32 v), (k, v))
  FWD(SetUINT64, (REFGUID k, UINT64 v), (k, v))
  FWD(SetDouble, (REFGUID k, double v), (k, v))
  FWD(SetGUID, (REFGUID k, REFGUID v), (k, v))
  FWD(SetString, (REFGUID k, LPCWSTR v), (k, v))
  FWD(SetBlob, (REFGUID k, const UINT8* v, UINT32 n), (k, v, n))
  FWD(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
  FWD(LockStore, (), ())
  FWD(UnlockStore, (), ())
  FWD(GetCount, (UINT32 * n), (n))
  FWD(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
  FWD(CopyAllItems, (IMFAttributes * d), (d))

 private:
  std::mutex mu_;
  ComPtr<IMFAttributes> a_;
  ComPtr<Source> src_;
};

// ---- class factory + exports ------------------------------------------------------------------------
class Factory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
 public:
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (outer) return CLASS_E_NOAGGREGATION;
    auto a = Make<Activate>();
    if (!a) return E_OUTOFMEMORY;
    HRESULT hr = a->Initialize();
    if (FAILED(hr)) return hr;
    return a.CopyTo(riid, ppv);
  }
  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock)
      InterlockedIncrement(&g_objects);
    else
      InterlockedDecrement(&g_objects);
    return S_OK;
  }
};

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_module = h;
    DisableThreadLibraryCalls(h);
  }
  return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
  if (clsid != CLSID_BeamerCamC2) return CLASS_E_CLASSNOTAVAILABLE;
  wchar_t exe[MAX_PATH] = {0};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  char user[128] = "?";
  DWORD n = sizeof user;
  GetUserNameA(user, &n);
  DWORD sess = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &sess);
  logf("DllGetClassObject in %ls user=%s session=%lu", exe, user, sess);
  auto f = Make<Factory>();
  if (!f) return E_OUTOFMEMORY;
  return f.CopyTo(riid, ppv);
}

STDAPI DllCanUnloadNow() { return g_objects == 0 ? S_OK : S_FALSE; }

// Registration is done by vcamctl.exe (it chooses HKLM or HKCU); these keep regsvr32 working (HKLM).
static HRESULT reg_server(HKEY root, bool add) {
  wchar_t key[128];
  swprintf_s(key, L"Software\\Classes\\CLSID\\%s", kClsidStr);
  if (!add) return HRESULT_FROM_WIN32(RegDeleteTreeW(root, key));
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_module, path, MAX_PATH);
  wchar_t sub[160];
  swprintf_s(sub, L"%s\\InprocServer32", key);
  HKEY k;
  LONG r = RegCreateKeyExW(root, sub, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
  if (r) return HRESULT_FROM_WIN32(r);
  RegSetValueExW(k, nullptr, 0, REG_SZ, (BYTE*)path, (DWORD)(wcslen(path) + 1) * 2);
  RegSetValueExW(k, L"ThreadingModel", 0, REG_SZ, (BYTE*)L"Both", 10);
  RegCloseKey(k);
  return S_OK;
}
STDAPI DllRegisterServer() { return reg_server(HKEY_LOCAL_MACHINE, true); }
STDAPI DllUnregisterServer() { return reg_server(HKEY_LOCAL_MACHINE, false); }
