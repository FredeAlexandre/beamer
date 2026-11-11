// SPDX-License-Identifier: GPL-3.0-only
// Windows video sink of the helper (camera PHASE2 §4.6, CW4): internal declarations shared by the video .cpp files
// of native/audio/win. The public surface is make_win_video_backend() and run_vcam_verb_if_requested(); the rest
// is in beamer::audio::win and is used by video_check.cpp (the local harness).
//
//   videodec_win.cpp      MF H.264 decoder: D3D11 + own staging readback, software fallback, display vs coded size
//   vcam_win.cpp          install state, kernel objects, shared-memory client (beat, live/hold/waiting, seqlocked
//                         slots), demand (in_use, want, loaded_version), camera create/Start
//   placeholder_win.cpp   Direct2D/DirectWrite placeholder -> NV12 (slot 3)
//   app_usage_win.cpp     which app uses a camera (CapabilityAccessManager consent store), best effort
//   vcam_install_win.cpp  verbs vcam-install / vcam-uninstall / vcam-remove / vcam-status
//   video_backend_win.cpp VideoBackend + VideoSinkHandle (12-picture pool, decoder worker, device-loss handling)
#pragma once

#include "backend_win.h"

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>

#include "../engine/video_backend.h"
#pragma warning(push)
#pragma warning(disable : 4324)  // alignas(64) padding in the contract header, intended
#include "../vcam-win/shm_protocol.h"
#pragma warning(pop)

namespace beamer::audio {

struct WinVideoOptions {
    bool dev = false;
    std::string decoder = "auto";   // auto | hw | sw (`--video-decoder=sw`, honoured with --dev only)
    // ---- test seams (video_check only; never set by main.cpp) ----
    // Registry base under HKLM used instead of the real keys: "" = SOFTWARE\Beamer\Camera and
    // SOFTWARE\Classes\CLSID\{...}; "X" = X\Camera and X\Classes\CLSID\{...}. Lets the harness exercise install
    // states and the verbs without registering anything (only CW5 registers the camera in wave 1).
    std::wstring reg_base;
    bool create_camera = true;      // false: never call MFCreateVirtualCamera (counted as attempts only)
    std::wstring usage_root;        // HKCU key of the webcam consent store ("" = the real one)
    int hw_retry_ms = 30000;        // device loss: back to hardware after this long (§4.6.1)
    int in_use_off_ms = 300;        // in_use off debounce (§3.3)
    int close_hold_ms = 500;        // last picture held after a sink closes (§4.6.4)
};
// --dev and --video-decoder=sw|hw|auto (a backend-only option: main.cpp keeps it away from the engine).
WinVideoOptions win_video_options_from_args();
std::unique_ptr<VideoBackend> make_win_video_backend(const WinVideoOptions& options);
// vcam-install / vcam-uninstall / vcam-remove / vcam-status (§3.1). Returns true when argv[1] is one of them
// (then *rc is the exit code: 0 ok, 2 bad args, 3 not elevated, 4 failed).
bool run_vcam_verb_if_requested(int* rc);

namespace win {

// ------------------------------------------------------------------ small helpers (vcam_win.cpp)
int64_t qpc_now();                       // raw QueryPerformanceCounter ticks (the beat_qpc / written_qpc clock)
int64_t qpc_freq();
double qpc_ms(int64_t ticks);            // ticks -> milliseconds
std::string json_str(const std::string& s);  // JSON string literal with quotes
std::string vcam_event_json(const VcamState& s);  // the §3.3 `vcam` event line (no newline)
bool media_foundation_available();       // mfplat.dll loads (absent on Windows N without the Media Feature Pack)
bool mf_startup();                       // MFStartup once per process (after media_foundation_available)

// ------------------------------------------------------------------ pure rules (unit-tested by video_check)
// §4.6.1: every size rule applies to the DISPLAY size: even, kMinW..kMaxW x kMinH..kMaxH.
bool display_size_ok(int w, int h);
// Copies a decoded NV12 picture of display size w x h from a buffer whose rows are `pitch` bytes apart and whose
// UV plane starts at `pitch * coded_h` (the coded height, e.g. 1088 for 1080p) into `dst` (stride `dst_stride`,
// UV at dst_stride * h). Only rows 0..h-1 and 0..h/2-1 are read.
void copy_nv12_display(const uint8_t* src, size_t pitch, int coded_h, int w, int h, uint8_t* dst, size_t dst_stride);
// How many bytes from `src` copy_nv12_display reads with these arguments (SIZE_MAX when they make no sense): a
// decoder buffer shorter than that is never copied (R3 #11).
size_t nv12_bytes_read(size_t pitch, int coded_h, int w, int h);
// Annex-B access unit contains an IDR slice (nal_unit_type 5).
bool au_has_idr(const uint8_t* au, size_t len);
// §4.6.3, other direction: the source's want_* is used only when it is one of kOffered, else kOffered[0].
vcam::MediaType validate_want(uint32_t w, uint32_t h, uint32_t fps);
// The InstallGuid is used verbatim in object names: only hex digits, '-', '{', '}' and at most 38 chars.
bool install_guid_ok(const std::wstring& g);

// ------------------------------------------------------------------ decoder (videodec_win.cpp)
enum class DecodeResult { kOk, kNoOutput, kError, kDeviceLost, kSizeRejected };
struct DecodeOut {
    DecodeResult result = DecodeResult::kNoOutput;
    HRESULT hr = S_OK;
    int w = 0, h = 0;          // display size of the output type (after cropping)
    int coded_h = 0;
    bool size_changed = false; // a new output type was set during this call
    double ms = 0;             // ProcessInput .. readback done
};
class H264Decoder {
 public:
    // Creates and configures the MFT (MF_LOW_LATENCY + CODECAPI_AVLowLatencyMode, NV12 out); hw = D3D11 device
    // manager. Slow (D3D11 device creation): call on a worker thread. nullptr on failure (*hr, *info set).
    static std::unique_ptr<H264Decoder> create(bool hw, std::string* info, HRESULT* hr);
    ~H264Decoder();
    bool hw() const { return hw_; }
    const std::string& info() const { return info_; }
    int w() const { return w_; }
    int h() const { return h_; }
    // Feeds one AU (sample time = frame) and drains the output. When a picture for this AU comes out and its display
    // size is allowed, `dst(w, h)` is asked for a destination (stride w, w*h*3/2 bytes) or nullptr (decode and drop).
    DecodeOut decode(const uint8_t* au, size_t len, uint32_t frame, const std::function<uint8_t*(int, int)>& dst);
    void simulate_device_removed() { fake_removed_ = true; }  // test hook: next call reports kDeviceLost
    // Test hook (video_check --negative): read UV at the DISPLAY height, the bug §4.6.1 forbids, to prove the
    // harness's band check catches it.
    static inline std::atomic<bool> test_uv_at_display_height{false};

 private:
    H264Decoder() = default;
    HRESULT set_output();
    bool device_lost(HRESULT hr);
    ComPtr<IMFTransform> t_;
    ComPtr<ID3D11Device> dev_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IMFDXGIDeviceManager> mgr_;
    ComPtr<ID3D11Texture2D> staging_;
    bool hw_ = false, provides_ = false, fake_removed_ = false;
    DWORD out_size_ = 0;
    int w_ = 0, h_ = 0, coded_w_ = 0, coded_h_ = 0;
    size_t linear_pitch_ = 0;   // row pitch of a linear output buffer: MF_MT_DEFAULT_STRIDE or the coded width
    uint64_t short_buffers_ = 0;  // output buffers too short for the picture: not copied
    std::string info_;
};

// ------------------------------------------------------------------ install state (vcam_win.cpp)
struct RegPaths {
    std::wstring camera;   // HKLM: InstallGuid, OwnerSid, DllVersion, DllPath
    std::wstring clsid;    // HKLM: ...\CLSID\{19FC...}
};
RegPaths reg_paths(const std::wstring& reg_base);
struct InstallInfo {
    std::string install = "missing";   // ok | missing | outdated | broken | unsupported
    int dll_version = 0;
    std::wstring guid, owner_sid, dll_path, clsid_path;
    std::string detail;                 // why broken/unsupported (diagnostics)
};
InstallInfo read_install(const std::wstring& reg_base);
bool virtual_camera_supported();        // MFIsVirtualCameraTypeSupported (resolved at run time)
// Creates (or re-creates, idempotent) and Starts the System-lifetime, CurrentUser-access camera; or Remove()s it.
HRESULT create_and_start_camera();
HRESULT remove_camera();
bool camera_device_present();           // an SWD\VCAMDEVAPI node named "Beamer Camera" is present and started

// ------------------------------------------------------------------ placeholder (placeholder_win.cpp)
// Renders the slot-3 picture (Beamer dark surface, mark, title, detail line) at w x h into NV12 (stride w).
bool render_placeholder(const Placeholder& p, int w, int h, std::vector<uint8_t>* nv12, std::string* err);

// ------------------------------------------------------------------ app usage (app_usage_win.cpp)
// Display name of the single app using a camera now, "" when none or several (§4.6.6). `root` "" = the real
// HKCU consent store key; otherwise an HKCU subkey laid out the same way (tests).
std::string camera_app_in_use(const std::wstring& root, int* in_use_count = nullptr);

// ------------------------------------------------------------------ shared-memory client + picture state (vcam_win.cpp)
class Vcam {
 public:
    explicit Vcam(const WinVideoOptions& o);
    ~Vcam();
    void start(VideoBackendListener* l);   // idle thread; camera creation at ready (§4.6.5)
    void stop();
    void detach_listener();                // the engine is going away: no more callbacks after this returns
    VcamState state(bool trigger_create);  // re-reads the install state; may start creation (see .cpp)
    bool enable(bool on, std::string* err);
    void set_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle);

    // ---- called by the sink, on the engine's video thread; every call is a heartbeat (§4.6.4) ----
    void sink_opened();
    void sink_closed();
    void sink_present(const uint8_t* nv12, int w, int h);   // stride w
    void sink_hold();
    void sink_placeholder(const std::string& state);

    // ---- diagnostics / test hooks ----
    std::string shown();                    // "live", "hold", "ph:<state>", "none"
    int create_attempts() const { return create_attempts_.load(); }
    uint64_t placeholder_renders() const { return renders_.load(); }
    void post_demand_check();               // wake the idle loop (tests)
    void poll();                            // one demand/state read now (vcam-status)

 private:
    struct Shm;
    void run();
    void open_objects_locked();
    void close_objects_locked();
    void read_source_locked(int64_t now, bool* changed);
    void tick_locked(int64_t now, bool idle_beat);
    void beat_locked(int64_t now);
    void write_live_locked(const uint8_t* nv12, int w, int h);
    void show_placeholder_locked(const std::string& state, bool force);
    void kick_create(bool force);
    void notify();

    WinVideoOptions opt_;
    VideoBackendListener* listener_ = nullptr;
    std::mutex lmu_;                        // guards listener_ and every call through it
    std::mutex mu_;
    std::thread thread_, create_thread_;
    std::atomic<bool> quit_{false};
    HANDLE wake_ = nullptr;                 // auto-reset
    std::unique_ptr<Shm> shm_;
    InstallInfo install_;
    // source state (validated)
    bool in_use_ = false, streaming_ = false;
    vcam::MediaType want_ = vcam::kOffered[0];
    int loaded_version_ = 0;
    uint32_t seen_state_seq_ = 0xFFFFFFFFu;
    int64_t off_since_ = 0;
    std::string app_;
    int64_t app_read_at_ = 0;
    std::string shm_error_, demand_error_, create_error_;
    // camera creation
    std::atomic<bool> creating_{false};
    std::atomic<int> create_attempts_{0};
    bool present_ = false;
    int64_t last_create_try_ = 0;
    std::string created_for_;               // install signature the last automatic attempt was for
    // pictures
    std::map<std::string, Placeholder> texts_;
    std::string idle_ = "waiting";
    bool sink_open_ = false;
    bool have_live_ = false;                // the live slots hold a picture of the current/last stream
    int64_t close_hold_until_ = 0;          // after a sink closed: hold the last picture until then
    std::string pending_ph_;                // placeholder to show when the close hold ends ("" = idle)
    enum class Show { kNone, kLive, kHold, kPlaceholder } show_ = Show::kNone;
    std::string ph_state_;                  // placeholder shown (kPlaceholder)
    std::string ph_key_;                    // what slot 3 holds: state|w|h|texts version
    uint64_t texts_version_ = 0;
    std::vector<uint8_t> ph_nv12_;
    uint32_t picture_ = 0;
    std::atomic<uint64_t> renders_{0};
};

// ------------------------------------------------------------------ backend + sink (video_backend_win.cpp)
class WinVideoSink;
class WinVideoBackend : public VideoBackend {
 public:
    explicit WinVideoBackend(const WinVideoOptions& o);
    ~WinVideoBackend() override;
    void set_listener(VideoBackendListener* l) override;
    std::vector<std::string> features() override { return {"virtual_camera"}; }
    CameraSnapshot cameras() override { CameraSnapshot s; s.permission = "unknown"; return s; }
    void request_permission() override {}
    std::unique_ptr<CameraHandle> open_camera(const CameraSpec&, EncoderSink*, std::string* err) override {
        if (err) *err = "unsupported: no camera capture on Windows";
        return nullptr;
    }
    void set_encoder_standby(const EncoderStandby& s) override { std::lock_guard<std::mutex> g(mu_); standby_ = s; }
    VcamState vcam() override { return vcam_->state(true); }
    bool vcam_enable(bool on, std::string* err) override { return vcam_->enable(on, err); }
    void vcam_placeholders(const std::map<std::string, Placeholder>& texts, const std::string& idle) override {
        vcam_->set_placeholders(texts, idle);
    }
    std::unique_ptr<VideoSinkHandle> open_sink(VideoSinkListener* l, std::string* err) override;

    // internal
    Vcam& vcamref() { return *vcam_; }
    const WinVideoOptions& options() const { return opt_; }
    void sink_gone(WinVideoSink* s);
    // Runs `job` on the decoder worker thread (COM MTA); jobs run one at a time, in order.
    void post_job(std::function<void()> job);

 private:
    void worker();
    WinVideoOptions opt_;
    std::unique_ptr<Vcam> vcam_;
    std::mutex mu_;
    EncoderStandby standby_;
    WinVideoSink* sink_ = nullptr;
    std::thread worker_;
    std::mutex jmu_;
    std::condition_variable jcv_;
    std::deque<std::function<void()>> jobs_;
    bool jquit_ = false;
    bool started_ = false;
};

struct SinkCounters {
    uint64_t decoded = 0, decode_errors = 0, discarded = 0, size_rejected = 0, skipped = 0, presented = 0;
    uint64_t reopens = 0, device_lost = 0, backlogged = 0;  // backlogged: AUs held while the decoder was made
    int w = 0, h = 0, coded_h = 0;          // last output type: display size and coded height
};

class WinVideoSink : public VideoSinkHandle {
 public:
    WinVideoSink(WinVideoBackend* b, VideoSinkListener* l);
    ~WinVideoSink() override;
    void decode(const uint8_t* au, size_t len, uint32_t frame, bool keep) override;
    void present(uint32_t frame) override;
    void hold() override;
    void drop(uint32_t frame) override;
    void show_placeholder(const std::string& state) override;

    // ---- test hooks / diagnostics (video_check) ----
    void simulate_device_removed();
    std::string decoder_kind();             // "hw", "sw", "" (none yet)
    SinkCounters counters();

 private:
    struct Pic { uint32_t frame = 0; bool used = false, busy = false; std::vector<uint8_t> nv12; };
    struct Job { std::vector<uint8_t> au; uint32_t frame; bool keep; };
    void run_decoder();                     // the sink's decoder thread
    void process(const Job& j);
    struct Shared {                         // decoder hand-off between the worker and the engine thread
        std::mutex mu;
        std::unique_ptr<H264Decoder> pending;   // finished by the worker, adopted on the next engine-thread call
        std::string info, fail;
        bool running = false, dead = false;
    };
    void request_decoder(bool hw, const char* why);
    void adopt_pending();                   // engine thread: install a decoder the worker finished
    void maybe_retry_hw(int64_t now);
    void decoded(uint32_t frame, bool ok, double ms);
    void decode_one(const uint8_t* au, size_t len, uint32_t frame, bool keep, bool idr);
    void flush_backlog(bool decode);
    void on_error(uint32_t frame, HRESULT hr, double ms);
    Pic* pic_for_new(uint32_t frame, int w, int h);
    void set_kind();

    WinVideoBackend* b_;
    VideoSinkListener* l_;
    std::shared_ptr<Shared> sh_;
    // decoder-thread state
    std::unique_ptr<H264Decoder> dec_;
    std::unique_ptr<H264Decoder> standby_hw_;  // hardware decoder waiting for an IDR to replace software
    std::string standby_info_;                 // its description, reported with decoder_hw at the take-over
    bool awaiting_idr_ = true;
    bool sw_after_loss_ = false;
    int64_t hw_retry_at_ = 0;
    int64_t last_hw_reopen_ = 0;
    int64_t retry_create_at_ = 0;
    std::deque<int64_t> errors_;
    std::string rejected_size_;             // display size of the current output type when refused
    std::mutex pmu_;                        // pool: written by the decoder thread, read by the presenter
    std::vector<Pic> pool_;
    int pool_w_ = 0, pool_h_ = 0;
    std::thread dth_;
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<Job> q_;
    bool qquit_ = false;
    // An IDR (and the AUs behind it, <= 1 s) that arrived while the decoder was being made: decoded as soon as it
    // is ready, so the stream's first IDR is never lost to the decoder's creation time.
    struct Held { std::vector<uint8_t> au; uint32_t frame; bool keep; };
    std::vector<Held> backlog_;
    std::atomic<bool> fake_remove_{false};
    std::atomic<int> kind_{0};              // 0 none, 1 hw, 2 sw
    std::mutex cmu_;
    SinkCounters c_;
};

}  // namespace win
}  // namespace beamer::audio
