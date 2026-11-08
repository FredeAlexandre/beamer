// SPDX-License-Identifier: GPL-3.0-only
// Windows (WASAPI) implementation of beamer::audio::Backend (PHASE2 §2.4).
//
// Threads: one worker (device notifications debounced 300 ms and de-duplicated against the last snapshot, default
// tracking, virtual-mic demand, stream_lost delivery) and one IO thread per stream (MMCSS "Pro Audio"), which is the
// only thread that calls the stream's sink/source. Streams whose device is "" follow the Windows default (eConsole).
#include "backend_win.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

namespace beamer::audio {
namespace win {

class WinBackend;

enum class StreamKind { Mic, System, Reference, Render };
static const char* kind_name(StreamKind k) {
    return k == StreamKind::Mic ? "mic" : k == StreamKind::System ? "system" : k == StreamKind::Reference ? "reference" : "render";
}

// ------------------------------------------------------------------ stream

class WinStream : public StreamHandle {
 public:
    WinStream(WinBackend* b, StreamKind kind) : b_(b), kind_(kind) {
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        opened_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~WinStream() override;
    std::string device_in_use() const override {
        std::lock_guard<std::mutex> g(m_);
        return device_;
    }
    bool start(std::string* err);
    void inject_invalidation() { inject_ = true; SetEvent(wake_); }
    void wake() { SetEvent(wake_); }
    CaptureStats stats() const {
        std::lock_guard<std::mutex> g(m_);
        return stats_;
    }

    // configuration (set before start)
    std::string want_device;       // "" = follow the default
    bool virtual_mic = false;      // render into CABLE Input
    int channels = 2;              // channels delivered to / pulled from the engine
    bool mute_local = false;       // System: mute the endpoint while capturing
    DWORD exclude_pid = 0;         // System: process tree left out of the capture
    CaptureSink* sink = nullptr;
    RenderSource* source = nullptr;

 private:
    enum class End { Stop, Invalidated, DefaultChanged, Failed };
    static DWORD WINAPI thread_main(void* p) { static_cast<WinStream*>(p)->run(); return 0; }
    void run();
    bool resolve(IMMDeviceEnumerator* e, std::string* id, std::string* err);
    bool open(IMMDeviceEnumerator* e, const std::string& id, std::string* err);
    End pump_capture();
    End pump_render();
    void close_device();
    bool follows_default() const { return want_device.empty() && !virtual_mic; }
    EDataFlow flow() const { return kind_ == StreamKind::Mic ? eCapture : eRender; }

    WinBackend* b_;
    StreamKind kind_;
    mutable std::mutex m_;
    std::string device_;           // endpoint in use (System: the endpoint muted / followed)
    std::string muted_;            // endpoint this stream holds muted
    std::unique_ptr<Client> client_;
    std::unique_ptr<KeepAlive> keepalive_;
    std::vector<float> scratch_;
    uint64_t gen_seen_ = 0;
    uint64_t starved_ = 0;         // render wakes that found the device buffer empty
    CaptureStats stats_;           // capture streams: counters (under m_; WP14)
    HANDLE thread_ = nullptr, wake_ = nullptr, opened_ = nullptr;
    std::atomic<bool> stop_{false}, inject_{false};
    std::string open_err_;
    bool open_ok_ = false;
};

// ------------------------------------------------------------------ notification client

class NotifyClient : public IMMNotificationClient {
    std::atomic<ULONG> ref_{1};
    std::function<void()> fn_;
 public:
    explicit NotifyClient(std::function<void()> fn) : fn_(std::move(fn)) {}
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMMNotificationClient)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --ref_; if (!n) delete this; return n; }
    // Never call MMDevice APIs from these callbacks: just wake the worker.
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { fn_(); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { fn_(); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { fn_(); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { fn_(); return S_OK; }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { fn_(); return S_OK; }
};

// ------------------------------------------------------------------ backend

class WinBackend : public Backend {
 public:
    explicit WinBackend(const WinOptions& o) : opt_(o), mutes_(o.state_dir), vmic_([this](bool) { post_vmic(); }) {
        set_log_level(o.log_level);
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        worker_ = std::thread([this] { worker(); });
        WaitForSingleObject(ready_, 5000);  // first defaults known before any stream opens
    }
    ~WinBackend() override {
        {
            std::lock_guard<std::mutex> g(qm_);
            quit_ = true;
        }
        qcv_.notify_all();
        if (worker_.joinable()) worker_.join();
        CloseHandle(ready_);
        // Streams the engine did not destroy: stop them before the mute manager restores.
        std::vector<WinStream*> left;
        {
            std::lock_guard<std::mutex> g(sm_);
            left = streams_;
        }
        if (!left.empty()) BLOG_W("%u stream(s) still open at backend shutdown", (unsigned)left.size());
    }

    void set_listener(BackendListener* l) override { listener_ = l; }

    DeviceSnapshot snapshot() override {
        ensure_com();
        auto e = make_enumerator();
        return build_snapshot(e.Get(), vmic_in_use());
    }

    std::unique_ptr<StreamHandle> open_capture(const CaptureSpec& spec, CaptureSink* sink, std::string* err) override {
        ensure_com();
        if (!sink || spec.channels < 1 || spec.channels > 8) { if (err) *err = "bad_args: sink and 1..8 channels required"; return nullptr; }
        StreamKind k = spec.kind == CaptureKind::Mic ? StreamKind::Mic : spec.kind == CaptureKind::System ? StreamKind::System : StreamKind::Reference;
        auto s = std::make_unique<WinStream>(this, k);
        s->want_device = spec.device;
        s->channels = spec.channels;
        s->sink = sink;
        if (k == StreamKind::System) {
            if (!has_process_loopback()) { if (err) *err = "unsupported: process loopback needs Windows 10 2004 or later"; return nullptr; }
            s->mute_local = spec.mute_local;
            DWORD pid = spec.exclude_pids.empty() ? GetCurrentProcessId() : (DWORD)spec.exclude_pids.front();
            if (spec.exclude_pids.size() > 1)
                BLOG_W("Windows excludes one process tree: using pid %lu, ignoring %u other(s)", (unsigned long)pid, (unsigned)spec.exclude_pids.size() - 1);
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) { if (err) *err = "bad_args: exclude pid " + std::to_string(pid) + " is not a running process"; return nullptr; }
            CloseHandle(h);
            s->exclude_pid = pid;
        } else if (spec.mute_local) {
            BLOG_W("mute_local ignored for a %s capture", kind_name(k));
        }
        if (!s->start(err)) return nullptr;
        return s;
    }

    std::unique_ptr<StreamHandle> open_render(const RenderSpec& spec, RenderSource* source, std::string* err) override {
        ensure_com();
        if (!source || spec.channels < 1 || spec.channels > 8) { if (err) *err = "bad_args: source and 1..8 channels required"; return nullptr; }
        auto s = std::make_unique<WinStream>(this, StreamKind::Render);
        s->want_device = spec.virtual_mic ? std::string() : spec.device;
        s->virtual_mic = spec.virtual_mic;
        s->channels = spec.channels;
        s->source = source;
        if (!s->start(err)) return nullptr;
        return s;
    }

    int restore_journal() override {
        ensure_com();
        return mutes_.restore_journal();
    }

    std::vector<std::string> features() override {
        std::vector<std::string> f;
        if (has_process_loopback()) { f.push_back("system_capture"); f.push_back("local_mute_endpoint"); }
        f.push_back("mic_demand");
        f.push_back("virtual_mic");
        f.push_back("aec");  // the backend provides CaptureKind::Reference (endpoint loopback); the canceller is the engine's
        if (opt_.vmic_exclusive) f.push_back("vmic_exclusive");  // started with --vmic-exclusive (falls back to shared if refused)
        return f;
    }

    std::string permission(const std::string& what) override {
        if (what == "mic") return mic_permission();
        if (what == "system_audio") return "granted";  // no consent gate for loopback on Windows
        return "unknown";
    }

    // ---- used by streams
    MuteManager& mutes() { return mutes_; }
    uint64_t default_gen(EDataFlow f) const { return gen_[f == eRender ? 0 : 1].load(); }
    void add_stream(WinStream* s) { std::lock_guard<std::mutex> g(sm_); streams_.push_back(s); }
    void remove_stream(WinStream* s) {
        std::unique_lock<std::mutex> g(sm_);
        streams_.erase(std::remove(streams_.begin(), streams_.end(), s), streams_.end());
        while (lost_in_flight_ == s && GetCurrentThreadId() != worker_tid_) {
            // stream_lost is being delivered for this stream on the worker: wait for it before the handle goes away
            g.unlock();
            Sleep(1);
            g.lock();
        }
    }
    void post_lost(WinStream* s, const std::string& why) {
        {
            std::lock_guard<std::mutex> g(qm_);
            lost_.push_back({s, why});
        }
        qcv_.notify_all();
    }
    bool vmic_in_use() const { return vmic_.in_use(); }
    bool has_process_loopback() const { return windows_build() >= 19041; }

 private:
    void post_device_event() {
        {
            std::lock_guard<std::mutex> g(qm_);
            if (!dirty_) deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            dirty_ = true;
        }
        qcv_.notify_all();
    }
    void post_vmic() {
        {
            std::lock_guard<std::mutex> g(qm_);
            vmic_changed_ = true;
        }
        qcv_.notify_all();
    }

    std::string fingerprint(const DeviceSnapshot& s) {
        std::string f = s.default_input + "|" + s.default_output + "|" + s.vmic_kind + (s.vmic_present ? "P" : "-") + (s.vmic_in_use ? "U" : "-");
        for (auto& d : s.devices)
            f += "|" + d.key + "\x1f" + d.name + "\x1f" + d.transport + "\x1f" + std::to_string(d.channels) + "\x1f" + std::to_string((int)d.form) +
                 "\x1f" + std::to_string((int)d.role) + "\x1f" + std::to_string((int)d.nominal_rate);
        return f;
    }

    // Re-reads defaults, bumps the generation of a flow whose default moved, re-attaches the demand watcher.
    void refresh(IMMDeviceEnumerator* e, bool device_change) {
        for (EDataFlow f : {eRender, eCapture}) {
            std::string id = default_endpoint_id(e, f);
            int i = f == eRender ? 0 : 1;
            if (id != defaults_[i]) {
                BLOG_I("default %s endpoint: %s", f == eRender ? "output" : "input", id.empty() ? "(none)" : id.c_str());
                defaults_[i] = id;
                gen_[i]++;
                std::lock_guard<std::mutex> g(sm_);
                for (auto* s : streams_) s->wake();
            }
        }
        // An endpoint unplugged while muted left an orphan journal entry (no owner); put it back when it returns,
        // without waiting for the next helper start.
        // (Not on the first refresh: the startup replay is the engine's restore_journal() call, whose count goes in `ready`.)
        if (device_change)
            if (int n = mutes_.restore_journal()) BLOG_I("restored %d endpoint(s) from the journal after a device change", n);
        std::vector<Endpoint> all = enumerate_endpoints(e, DEVICE_STATE_ACTIVE, false);
        std::string cable;
        for (auto& ep : all)
            if (!ep.blocked && ep.info.role == Role::VirtualMic) cable = ep.id;
        if (cable != vmic_.attached_id()) {
            if (cable.empty()) {
                vmic_.detach();
                BLOG_I("virtual mic (CABLE Output) absent");
            } else if (auto d = device_by_id(e, cable)) {
                bool ok = vmic_.attach(d.Get(), cable);
                BLOG_I("virtual mic demand watcher on CABLE Output: %s", ok ? "on" : "failed");
            }
        }
    }

    void worker() {
        ComScope com;
        worker_tid_ = GetCurrentThreadId();
        auto e = make_enumerator();
        auto* nc = new NotifyClient([this] { post_device_event(); });
        if (e) {
            HRESULT hr = e->RegisterEndpointNotificationCallback(nc);
            if (FAILED(hr)) BLOG_E("RegisterEndpointNotificationCallback: %s", hr_text(hr).c_str());
        }
        refresh(e.Get(), false);
        std::string last = fingerprint(build_snapshot(e.Get(), vmic_.in_use()));
        SetEvent(ready_);
        std::unique_lock<std::mutex> lk(qm_);
        while (!quit_) {
            if (dirty_) qcv_.wait_until(lk, deadline_, [this] { return quit_ || vmic_changed_ || !lost_.empty(); });
            else qcv_.wait(lk, [this] { return quit_ || dirty_ || vmic_changed_ || !lost_.empty(); });
            if (quit_) break;
            auto lost = std::move(lost_);
            lost_.clear();
            bool vmic = vmic_changed_;
            vmic_changed_ = false;
            bool due = dirty_ && std::chrono::steady_clock::now() >= deadline_;
            if (due) dirty_ = false;
            lk.unlock();
            for (auto& [s, why] : lost) {
                {
                    std::lock_guard<std::mutex> g(sm_);
                    if (std::find(streams_.begin(), streams_.end(), s) == streams_.end()) continue;  // already destroyed
                    lost_in_flight_ = s;
                }
                BLOG_W("stream lost (%s): %s", s->device_in_use().c_str(), why.c_str());
                if (auto* l = listener_.load()) l->stream_lost(s, why);
                std::lock_guard<std::mutex> g(sm_);
                lost_in_flight_ = nullptr;
            }
            if (due) refresh(e.Get(), true);
            if (due || vmic) {
                // Device changes are debounced; the virtual mic's in_use is reported at once (demand latency).
                std::string fp = fingerprint(build_snapshot(e.Get(), vmic_.in_use()));
                if (fp != last) {
                    last = fp;
                    if (auto* l = listener_.load()) l->devices_changed();
                }
            }
            lk.lock();
        }
        lk.unlock();
        if (e) e->UnregisterEndpointNotificationCallback(nc);
        nc->Release();
        vmic_.detach();
    }

    static std::string consent(HKEY root, const wchar_t* path) {
        wchar_t buf[32];
        DWORD n = sizeof buf;
        if (RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return "";
        return lower(utf8(buf));
    }
    static std::string mic_permission() {
        const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
        std::wstring nonpackaged = std::wstring(base) + L"\\NonPackaged";
        std::string machine = consent(HKEY_LOCAL_MACHINE, base), user = consent(HKEY_CURRENT_USER, base),
                    desktop = consent(HKEY_CURRENT_USER, nonpackaged.c_str());
        if (machine == "deny" || user == "deny" || desktop == "deny") return "denied";
        if (user == "allow" || desktop == "allow" || (user.empty() && desktop.empty())) return "granted";
        return "unknown";
    }

    WinOptions opt_;
    MuteManager mutes_;
    SessionWatcher vmic_;
    std::atomic<BackendListener*> listener_{nullptr};
    std::thread worker_;
    DWORD worker_tid_ = 0;
    HANDLE ready_ = nullptr;
    std::mutex qm_;
    std::condition_variable qcv_;
    bool quit_ = false, dirty_ = false, vmic_changed_ = false;
    std::chrono::steady_clock::time_point deadline_;
    std::vector<std::pair<WinStream*, std::string>> lost_;
    std::string defaults_[2];
    std::atomic<uint64_t> gen_[2] = {0, 0};
    std::mutex sm_;
    std::vector<WinStream*> streams_;
    WinStream* lost_in_flight_ = nullptr;

    friend class WinStream;
};

// ------------------------------------------------------------------ stream implementation

WinStream::~WinStream() {
    stop_ = true;
    SetEvent(wake_);
    if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); }
    b_->remove_stream(this);
    CloseHandle(wake_);
    CloseHandle(opened_);
}

bool WinStream::start(std::string* err) {
    b_->add_stream(this);
    thread_ = CreateThread(nullptr, 0, &WinStream::thread_main, this, 0, nullptr);
    if (!thread_) { if (err) *err = "internal: CreateThread failed"; return false; }
    WaitForSingleObject(opened_, INFINITE);
    if (!open_ok_) {
        if (err) *err = open_err_;
        return false;  // the destructor joins the (finished) thread and unregisters
    }
    return true;
}

// Picks the endpoint for this stream now. Errors use the §3.3 codes as "code: message".
bool WinStream::resolve(IMMDeviceEnumerator* e, std::string* id, std::string* err) {
    std::vector<Endpoint> all = enumerate_endpoints(e, DEVICE_STATE_ACTIVE, false);
    const bool render_side = flow() == eRender;
    if (virtual_mic) {
        for (auto& ep : all)
            if (!ep.blocked && ep.info.role == Role::VirtualMicFeed) { *id = ep.id; return true; }
        *err = "no_virtual_mic: VB-CABLE (CABLE Input) is not installed or not active";
        return false;
    }
    std::string want = want_device.empty() ? default_endpoint_id(e, flow()) : want_device;
    if (want.empty()) { *err = std::string("no_device: no default ") + (render_side ? "output" : "input"); return false; }
    for (auto& ep : all) {
        if (ep.id != want) continue;
        if (ep.blocked) { *err = "blocked_device: " + ep.name + " is never used by Beamer"; return false; }
        if (ep.render != render_side) { *err = "bad_args: " + ep.name + (render_side ? " is not an output" : " is not an input"); return false; }
        if (ep.hidden || ep.info.role != Role::Normal) { *err = "blocked_device: " + ep.name + " is not offered as a source or output"; return false; }
        *id = ep.id;
        return true;
    }
    *err = "no_device: " + want + " is not present";
    return false;
}

void WinStream::close_device() {
    keepalive_.reset();
    client_.reset();
}

bool WinStream::open(IMMDeviceEnumerator* e, const std::string& id, std::string* err) {
    close_device();
    auto c = std::make_unique<Client>();
    HRESULT hr = S_OK;
    auto d = device_by_id(e, id);
    if (kind_ == StreamKind::System) {
        // Process loopback is not tied to an endpoint; the endpoint matters for the mute and the channel count.
        int ch = 2;
        if (d) {
            Endpoint ep = describe_endpoint(d.Get(), eRender, true);
            if (ep.mix_channels > 0) ch = std::min(ep.mix_channels, 8);
        }
        hr = open_process_loopback(exclude_pid, false, ch, c.get());
    } else {
        if (!d) { *err = "no_device: " + id + " is not present"; return false; }
        ClientKind ck = kind_ == StreamKind::Mic ? ClientKind::Capture : kind_ == StreamKind::Reference ? ClientKind::EndpointLoopback : ClientKind::Render;
        hr = E_FAIL;
        if (virtual_mic && b_->opt_.vmic_exclusive) {
            hr = open_exclusive_render(d.Get(), channels, c.get());
            if (FAILED(hr)) {
                BLOG_W("exclusive render into CABLE Input refused (%s), using shared mode", hr_text(hr).c_str());
                c = std::make_unique<Client>();
            }
        }
        if (FAILED(hr)) {
            const WinOptions& o = b_->opt_;
            // Endpoint loopback (AEC reference) keeps the default: it follows the render engine's period anyway.
            UINT32 period = kind_ == StreamKind::Mic ? o.mic_period : kind_ != StreamKind::Render ? 0 : virtual_mic ? o.vmic_period : o.render_period;
            hr = open_endpoint_client(d.Get(), ck, channels, c.get(), period);
        }
    }
    if (FAILED(hr)) {
        *err = (hr == E_ACCESSDENIED ? "permission: " : hr == AUDCLNT_E_DEVICE_IN_USE ? "device_busy: " : hr == AUDCLNT_E_DEVICE_INVALIDATED ? "no_device: " : "internal: ") +
               std::string("opening ") + kind_name(kind_) + " on " + id + " failed " + hr_text(hr);
        return false;
    }
    size_t frames = std::max<size_t>(c->buffer_frames, 4800);
    scratch_.assign(frames * std::max(channels, c->channels), 0.f);  // never reallocated on the IO path
    if (kind_ == StreamKind::Reference) keepalive_ = std::make_unique<KeepAlive>(id);
    BLOG_I("%s stream on %s: %s, %d ch from the device, %d ch to the engine, buffer %u frames", kind_name(kind_), id.empty() ? "(process loopback)" : id.c_str(),
           c->path.c_str(), c->channels, channels, c->buffer_frames);
    if (kind_ == StreamKind::Render && !c->exclusive)
        BLOG_I("render queue %.2f periods = %.1f ms", virtual_mic ? b_->opt_.vmic_queue : b_->opt_.render_queue,
               (virtual_mic ? b_->opt_.vmic_queue : b_->opt_.render_queue) * c->period_frames * 1000.0 / kRate);
    client_ = std::move(c);
    return true;
}

WinStream::End WinStream::pump_capture() {
    Client& c = *client_;
    HRESULT hr = c.client->Start();
    if (FAILED(hr)) return hr == AUDCLNT_E_DEVICE_INVALIDATED ? End::Invalidated : End::Failed;
    HANDLE waits[2] = {c.event, wake_};
    const int ic = c.channels, oc = channels;
    const size_t cap_frames = scratch_.size() / std::max(ic, oc);
    const uint64_t buffer_ns = (uint64_t)c.buffer_frames * 1000000000ull / kRate;
    CaptureStats st = stats();
    uint64_t last_wake = 0, prev_wake = 0, last_qpc = 0, next_pos = UINT64_MAX, last_discont_log = 0;
    while (!stop_) {
        WaitForMultipleObjects(2, waits, FALSE, 100);
        if (stop_) break;
        if (inject_.exchange(false)) return End::Invalidated;
        if (follows_default() && b_->default_gen(flow()) != gen_seen_) return End::DefaultChanged;
        const uint64_t wake = now_ns();
        if (last_wake) {
            st.max_wake_gap_us = std::max(st.max_wake_gap_us, (wake - last_wake) / 1000);
            if (wake - last_wake > buffer_ns) st.late_wakes++;
        }
        prev_wake = last_wake ? last_wake : wake;
        last_wake = wake;
        st.wakes++;
        uint64_t got = 0;
        UINT32 pk = 0;
        while (SUCCEEDED(hr = c.capture->GetNextPacketSize(&pk)) && pk) {
            BYTE* data;
            UINT32 n;
            DWORD flags;
            UINT64 pos, qpc;
            hr = c.capture->GetBuffer(&data, &n, &flags, &pos, &qpc);
            if (FAILED(hr)) break;
            if (hr == AUDCLNT_S_BUFFER_EMPTY) { hr = S_OK; break; }
            got++;
            st.packets++;
            st.min_packet = st.min_packet ? std::min<uint64_t>(st.min_packet, n) : n;
            st.max_packet = std::max<uint64_t>(st.max_packet, n);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) st.silent++;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                // WASAPI dropped data before this packet (it is the only loss the capture side can see): leave a
                // trace, at most every 10 s, so live runs in the user's session show it (WP14).
                if (!st.discontinuity || wake - last_discont_log > 10000000000ull) {
                    BLOG_W("%s capture: discontinuity flag (%llu so far, last wake gap %.1f ms)", kind_name(kind_),
                           (unsigned long long)st.discontinuity + 1, st.wakes > 1 ? (wake - prev_wake) / 1e6 : 0.0);
                    last_discont_log = wake;
                }
                st.discontinuity++;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) st.ts_error++;
            if (next_pos != UINT64_MAX && pos != next_pos) {
                if (pos > next_pos) { st.pos_gaps++; st.pos_gap_frames += pos - next_pos; }
                else st.pos_back++;
            }
            next_pos = pos + n;
            if (qpc && last_qpc && qpc > last_qpc) st.max_qpc_gap_us = std::max(st.max_qpc_gap_us, (qpc - last_qpc) / 10);
            if (qpc) last_qpc = qpc;
            st.frames += n;
            uint64_t t0 = qpc ? qpc100_to_ns(qpc) : now_ns();
            const float* in = reinterpret_cast<const float*>(data);
            for (UINT32 done = 0; done < n;) {
                UINT32 chunk = (UINT32)std::min<size_t>(n - done, cap_frames);
                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(scratch_.begin(), scratch_.begin() + (size_t)chunk * oc, 0.f);
                else convert_channels(in + (size_t)done * ic, ic, scratch_.data(), oc, chunk);
                sink->on_capture(scratch_.data(), (int)chunk, oc, t0 + (uint64_t)done * 1000000000ull / kRate);
                done += chunk;
            }
            c.capture->ReleaseBuffer(n);
        }
        if (!got) st.empty_wakes++;
        st.max_packets_per_wake = std::max(st.max_packets_per_wake, got);
        {
            std::lock_guard<std::mutex> g(m_);
            stats_ = st;
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return End::Invalidated;
        if (FAILED(hr)) { BLOG_W("%s capture: %s", kind_name(kind_), hr_text(hr).c_str()); return End::Failed; }
    }
    return End::Stop;
}

WinStream::End WinStream::pump_render() {
    Client& c = *client_;
    // Shared: keep `queue` periods queued (WinOptions: endpoints 1.5, CABLE Input 3 at a 128-frame period; see
    // WP12). Exclusive: the buffer is one period and each event asks for all of it.
    const double queue = virtual_mic ? b_->opt_.vmic_queue : b_->opt_.render_queue;
    const UINT32 target = c.exclusive ? c.buffer_frames
                                      : std::clamp((UINT32)std::lround(c.period_frames * queue), c.period_frames, c.buffer_frames);
    BYTE* p;
    HRESULT hr;
    // The engine always fills float32; integer buffers (exclusive mode) go through scratch_ and are converted here.
    auto fill = [&](BYTE* dst, UINT32 n, uint64_t t) {
        if (c.sample_kind == 0) { source->on_render(reinterpret_cast<float*>(dst), (int)n, c.channels, t); return; }
        n = (UINT32)std::min<size_t>(n, scratch_.size() / c.channels);
        source->on_render(scratch_.data(), (int)n, c.channels, t);
        const size_t count = (size_t)n * c.channels;
        if (c.sample_kind == 1) {
            auto* o = reinterpret_cast<int16_t*>(dst);
            for (size_t i = 0; i < count; i++) o[i] = (int16_t)std::lrint(std::clamp(scratch_[i], -1.f, 1.f) * 32767.f);
        } else {
            auto* o = reinterpret_cast<int32_t*>(dst);
            for (size_t i = 0; i < count; i++) o[i] = (int32_t)std::llrint(std::clamp((double)scratch_[i], -1.0, 1.0) * 2147483647.0);
        }
    };
    if (SUCCEEDED(c.render->GetBuffer(target, &p))) {
        fill(p, target, now_ns());
        c.render->ReleaseBuffer(target, 0);
    }
    hr = c.client->Start();
    if (FAILED(hr)) return hr == AUDCLNT_E_DEVICE_INVALIDATED ? End::Invalidated : End::Failed;
    HANDLE waits[2] = {c.event, wake_};
    while (!stop_) {
        WaitForMultipleObjects(2, waits, FALSE, 100);
        if (stop_) break;
        if (inject_.exchange(false)) return End::Invalidated;
        if (follows_default() && b_->default_gen(flow()) != gen_seen_) return End::DefaultChanged;
        UINT32 pad = 0;
        hr = c.exclusive ? S_OK : c.client->GetCurrentPadding(&pad);
        if (SUCCEEDED(hr) && !c.exclusive && pad == 0) starved_++;  // the device drained everything: a hole, unless at start
        if (SUCCEEDED(hr) && pad < target) {
            UINT32 n = target - pad;
            hr = c.render->GetBuffer(n, &p);
            if (SUCCEEDED(hr)) {
                fill(p, n, now_ns() + (uint64_t)pad * 1000000000ull / kRate);
                hr = c.render->ReleaseBuffer(n, 0);
            }
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return End::Invalidated;
        if (FAILED(hr)) { BLOG_W("render: %s", hr_text(hr).c_str()); return End::Failed; }
    }
    return End::Stop;
}

void WinStream::run() {
    ComScope com;
    MmcssScope mmcss;
    auto e = make_enumerator();
    std::string id, err;
    gen_seen_ = b_->default_gen(flow());
    bool ok = e && resolve(e.Get(), &id, &err) && open(e.Get(), id, &err);
    if (ok && kind_ == StreamKind::System && mute_local) {
        auto d = device_by_id(e.Get(), id);
        Endpoint ep = d ? describe_endpoint(d.Get(), eRender, false) : Endpoint{};
        ok = d && b_->mutes().acquire(d.Get(), ep.name, &err);
        if (ok) muted_ = id;
    }
    {
        std::lock_guard<std::mutex> g(m_);
        device_ = id;
    }
    open_ok_ = ok;
    open_err_ = err;
    SetEvent(opened_);
    if (!ok) { close_device(); return; }

    std::string lost_why;
    while (!stop_) {
        End end = kind_ == StreamKind::Render ? pump_render() : pump_capture();
        if (end == End::Stop || stop_) break;
        if (end == End::DefaultChanged) {
            gen_seen_ = b_->default_gen(flow());
            std::string nid;
            if (!resolve(e.Get(), &nid, &err)) { lost_why = err; break; }
            if (nid == id) continue;
            BLOG_I("%s stream follows the default to %s", kind_name(kind_), nid.c_str());
            if (kind_ == StreamKind::System) {
                // The capture is endpoint-independent: only the mute moves (new first, so the sound never leaks).
                if (mute_local) {
                    auto d = device_by_id(e.Get(), nid);
                    Endpoint ep = d ? describe_endpoint(d.Get(), eRender, false) : Endpoint{};
                    if (!d || !b_->mutes().acquire(d.Get(), ep.name, &err)) { lost_why = err.empty() ? "no_device: new default output" : err; break; }
                    if (!muted_.empty()) b_->mutes().release(muted_);
                    muted_ = nid;
                }
                id = nid;
                std::lock_guard<std::mutex> g(m_);
                device_ = id;
                continue;
            }
            if (!open(e.Get(), nid, &err)) { lost_why = err; break; }
            id = nid;
            std::lock_guard<std::mutex> g(m_);
            device_ = id;
            continue;
        }
        // Invalidated or failed: the endpoint went away, changed format, or a new default is coming. Retry for 3 s
        // (same endpoint, or the current default when following), then give up with stream_lost.
        BLOG_W("%s stream on %s %s, recovering", kind_name(kind_), id.c_str(), end == End::Invalidated ? "invalidated" : "failed");
        close_device();
        bool back = false;
        for (int attempt = 0; attempt < 15 && !stop_; attempt++) {
            // CABLE Input: wait 250 ms first. Another app's exclusive open preempts us (invalidation); one that retries
            // with another format right after must not find the low-latency stream back already (WP12: it got
            // AUDCLNT_E_DEVICE_IN_USE when we reopened after 50 ms; default-period streams did not show this).
            if (WaitForSingleObject(wake_, attempt == 0 ? (virtual_mic ? 250 : 50) : 200) == WAIT_OBJECT_0 && stop_) break;
            gen_seen_ = b_->default_gen(flow());
            std::string nid;
            if (!resolve(e.Get(), &nid, &err)) continue;
            if (!open(e.Get(), nid, &err)) continue;
            if (kind_ == StreamKind::System && mute_local && nid != muted_) {
                auto d = device_by_id(e.Get(), nid);
                Endpoint ep = d ? describe_endpoint(d.Get(), eRender, false) : Endpoint{};
                if (!d || !b_->mutes().acquire(d.Get(), ep.name, &err)) continue;
                if (!muted_.empty()) b_->mutes().release(muted_);
                muted_ = nid;
            }
            id = nid;
            {
                std::lock_guard<std::mutex> g(m_);
                device_ = id;
            }
            BLOG_I("%s stream recovered on %s", kind_name(kind_), id.c_str());
            back = true;
            break;
        }
        if (stop_) break;
        if (!back) { lost_why = end == End::Invalidated ? "device_invalidated" : "device_failed"; if (!err.empty()) lost_why += " (" + err + ")"; break; }
    }
    close_device();
    if (kind_ != StreamKind::Render) BLOG_I("%s stream: %s", kind_name(kind_), capture_stats_text(stats()).c_str());
    if (starved_) BLOG_I("%s stream: %llu render wake(s) found the device buffer empty", kind_name(kind_), (unsigned long long)starved_);
    // The capture is over: give the sound back to the speakers before anything else.
    if (!muted_.empty()) { b_->mutes().release(muted_); muted_.clear(); }
    if (!lost_why.empty() && !stop_) b_->post_lost(this, lost_why);
}

// ------------------------------------------------------------------ test hooks

void inject_invalidation(StreamHandle* stream) {
    if (auto* s = dynamic_cast<WinStream*>(stream)) s->inject_invalidation();
}
CaptureStats capture_stats(StreamHandle* stream) {
    auto* s = dynamic_cast<WinStream*>(stream);
    return s ? s->stats() : CaptureStats{};
}
std::string capture_stats_text(const CaptureStats& s) {
    char b[512];
    snprintf(b, sizeof b,
             "%llu packets (%llu..%llu frames, up to %llu per wake), %llu frames, %llu wakes (%llu empty, %llu late, max gap %.1f ms), "
             "flags: %llu silent %llu discontinuity %llu timestamp-error, position gaps %llu (%llu frames) back %llu, max QPC step %.1f ms",
             (unsigned long long)s.packets, (unsigned long long)s.min_packet, (unsigned long long)s.max_packet, (unsigned long long)s.max_packets_per_wake,
             (unsigned long long)s.frames, (unsigned long long)s.wakes, (unsigned long long)s.empty_wakes, (unsigned long long)s.late_wakes,
             s.max_wake_gap_us / 1000.0, (unsigned long long)s.silent, (unsigned long long)s.discontinuity, (unsigned long long)s.ts_error,
             (unsigned long long)s.pos_gaps, (unsigned long long)s.pos_gap_frames, (unsigned long long)s.pos_back, s.max_qpc_gap_us / 1000.0);
    return b;
}
int held_mutes(Backend* backend) {
    auto* b = dynamic_cast<WinBackend*>(backend);
    return b ? b->mutes().held_count() : -1;
}

// UTF-8 argv from the wide command line (main's char** argv is in the ANSI code page on Windows).
static std::vector<std::string> wide_args() {
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    if (!w) return out;
    for (int i = 0; i < n; i++) out.push_back(utf8(w[i]));
    LocalFree(w);
    return out;
}

}  // namespace win

WinOptions win_options_from_args(int, char**) {
    WinOptions o;
    auto a = win::wide_args();
    for (size_t i = 1; i < a.size(); i++) {
        if (a[i] == "--state-dir" && i + 1 < a.size()) o.state_dir = win::wide(a[++i]);
        else if (a[i] == "--log-level" && i + 1 < a.size()) o.log_level = a[++i];
        else if (a[i] == "--dev") o.dev = true;
        else if (a[i] == "--vmic-exclusive") o.vmic_exclusive = true;
    }
    return o;
}

std::unique_ptr<Backend> make_win_backend(const WinOptions& options) { return std::make_unique<win::WinBackend>(options); }

bool run_guard_if_requested(int argc, char** argv, int* rc) {
    (void)argc; (void)argv;
    auto a = win::wide_args();
    if (a.size() < 2 || a[1] != "_guard") return false;
    DWORD pid = 0;
    std::wstring dir;
    for (size_t i = 2; i < a.size(); i++) {
        if (a[i] == "--pid" && i + 1 < a.size()) pid = (DWORD)strtoul(a[++i].c_str(), nullptr, 10);
        else if (a[i] == "--state-dir" && i + 1 < a.size()) dir = win::wide(a[++i]);
    }
    *rc = 0;
    if (!pid) { *rc = 2; return true; }
    win::ComScope com;
    // Wait for the helper to end, however it ends; then put back whatever it still held muted.
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    win::MuteManager::restore_entries_of(dir, pid);
    return true;
}

}  // namespace beamer::audio
