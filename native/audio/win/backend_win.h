// SPDX-License-Identifier: GPL-3.0-only
// Windows (WASAPI) backend for the Beamer audio helper: internal declarations shared by the win/*.cpp files.
// The public surface is make_win_backend(); everything else is in beamer::audio::win.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// backend.h is the verbatim contract of PHASE2 §2.4: no includes and no include guard of its own, so it is
// included here exactly once, after the standard headers it needs.
#include "../engine/backend.h"

namespace beamer::audio {

struct WinOptions {
    std::wstring state_dir;       // folder of settings.json; the mute journal lives there
    std::string log_level = "info";
    bool dev = false;
};
// Parses --state-dir, --log-level and --dev out of the helper's argv (other arguments are ignored).
WinOptions win_options_from_args(int argc, char** argv);
std::unique_ptr<Backend> make_win_backend(const WinOptions& options);
// The crash guard is a second copy of the helper: `beamer-audio _guard --pid P --state-dir D`. main() calls this
// first; it returns true (and sets *rc) when argv is a guard invocation, which then has run to completion.
bool run_guard_if_requested(int argc, char** argv, int* rc);

namespace win {

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ util (devices_win.cpp)
enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };
void set_log_level(const std::string& level);
void logf(LogLevel level, const char* fmt, ...);
#define BLOG_D(...) ::beamer::audio::win::logf(::beamer::audio::win::LogLevel::Debug, __VA_ARGS__)
#define BLOG_I(...) ::beamer::audio::win::logf(::beamer::audio::win::LogLevel::Info, __VA_ARGS__)
#define BLOG_W(...) ::beamer::audio::win::logf(::beamer::audio::win::LogLevel::Warn, __VA_ARGS__)
#define BLOG_E(...) ::beamer::audio::win::logf(::beamer::audio::win::LogLevel::Error, __VA_ARGS__)

std::string utf8(const std::wstring& w);
std::string utf8(const wchar_t* w);
std::wstring wide(const std::string& s);
std::string lower(std::string s);
std::string hr_text(HRESULT hr);
uint64_t now_ns();                    // QPC in ns
uint64_t qpc100_to_ns(uint64_t t100); // IAudioCaptureClient positions are QPC in 100 ns units

// Initializes COM (MTA) on the calling thread if it is not initialized yet; harmless when it already is.
void ensure_com();
struct ComScope {  // for threads the backend owns
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct MmcssScope {  // MMCSS "Pro Audio" for an IO thread
    HANDLE h = nullptr; DWORD idx = 0;
    MmcssScope();
    ~MmcssScope();
};

// ------------------------------------------------------------------ endpoints (devices_win.cpp)
struct Endpoint {
    std::string id;               // MMDevice endpoint id = DeviceInfo::key
    std::string name;             // PKEY_Device_FriendlyName, e.g. "Haut-parleurs (Logitech G733 Gaming Headset)"
    std::string description;      // PKEY_Device_DeviceDesc, e.g. "Haut-parleurs"
    std::string adapter;          // PKEY_DeviceInterface_FriendlyName, e.g. "Logitech G733 Gaming Headset"
    std::string enumerator;       // PKEY_Device_EnumeratorName of the endpoint, e.g. "USB", "TUSBAUDIO_ENUM", "ROOT"
    std::string adapter_instance; // devnode instance id of the audio adapter, e.g. "USB\VID_046D&PID_0B1F&MI_00\..."
    std::vector<std::string> ancestors;  // devnode instance ids from the adapter up to the root
    std::string manufacturer;     // first meaningful DEVPKEY_Device_Manufacturer up the devnode chain
    std::string driver_provider;  // DEVPKEY_Device_DriverProvider of the adapter
    bool render = false;
    DWORD state = 0;
    int form_factor = -1;         // PKEY_AudioEndpoint_FormFactor
    int mix_channels = 0; int mix_rate = 0; bool mix_float = false;
    DeviceInfo info;              // what the backend reports
    bool blocked = false;         // never listed, opened, muted or changed
    bool hidden = false;          // not blocked but never offered (CABLE In 16ch, inactive endpoints)
    std::string why;              // reason for blocked/hidden/role (diagnostics)
};

ComPtr<IMMDeviceEnumerator> make_enumerator();
std::string device_id(IMMDevice* d);
// Reads every property and classifies (transport, form, role, blocked) one endpoint.
Endpoint describe_endpoint(IMMDevice* d, EDataFlow flow, bool read_mix_format);
// Every endpoint in `state_mask` (both flows), classified. Blocked ones are included, marked.
std::vector<Endpoint> enumerate_endpoints(IMMDeviceEnumerator* e, DWORD state_mask, bool read_mix_format);
std::string default_endpoint_id(IMMDeviceEnumerator* e, EDataFlow flow);  // eConsole role, "" when none
// Looks up an endpoint by key; returns nullptr when absent or not active.
ComPtr<IMMDevice> device_by_id(IMMDeviceEnumerator* e, const std::string& id);
// Builds the DeviceSnapshot (blocked/hidden endpoints left out; VB-CABLE's endpoints carry their roles).
DeviceSnapshot build_snapshot(IMMDeviceEnumerator* e, bool vmic_in_use, std::vector<Endpoint>* all_out = nullptr);
// VB-CABLE: "CABLE Input" (render, the virtual mic sink) and "CABLE Output" (capture, what apps record).
bool is_vbcable(const Endpoint& ep);
bool is_vbcable_input(const Endpoint& ep);
bool is_vbcable_output(const Endpoint& ep);
std::string form_word(Form f);

// ------------------------------------------------------------------ mute journal (mute_journal_win.cpp)
struct EndpointLevel {  // what an endpoint mute changes and what is put back
    std::string device, name;
    bool mute = false;
    float master = 0;
    std::vector<float> channels;
    DWORD hw = 0;
};
bool read_level(IMMDevice* d, EndpointLevel* out);
bool write_level(IMMDevice* d, const EndpointLevel& level);  // only the values that differ are written

// Endpoint mutes taken by system-sound captures (refcounted per endpoint), journalled to
// <state_dir>/audio-mute-journal.json before the mute so a crash can be undone: by the guard process
// (helper killed) or by restore_journal() on the next start (helper and guard both killed).
class MuteManager {
 public:
    explicit MuteManager(std::wstring state_dir);
    ~MuteManager();  // restores everything still held
    // Mutes `device` (refcounted). False + err when it cannot be read, journalled or muted.
    bool acquire(IMMDevice* device, const std::string& name, std::string* err);
    void release(const std::string& device_id);
    void release_all();
    // Restores the journal entries of dead owners (startup: every entry not owned by this process).
    int restore_journal();
    // Guard side: restores only entries owned by `owner_pid`.
    static int restore_entries_of(const std::wstring& state_dir, DWORD owner_pid);
    std::wstring journal_path() const;
    int held_count();
 private:
    struct Held { EndpointLevel saved; int refs = 0; };
    void write_journal_locked();
    void start_guard_locked();
    std::wstring dir_;
    std::mutex m_;
    std::vector<Held> held_;
    HANDLE guard_ = nullptr;
};

// ------------------------------------------------------------------ demand (sessions_win.cpp)
// Watches the audio sessions of one capture endpoint ("CABLE Output") and reports whether another process
// has an Active session on it, i.e. records the virtual mic.
class SessionWatcher {
 public:
    explicit SessionWatcher(std::function<void(bool)> on_change);
    ~SessionWatcher();
    bool attach(IMMDevice* capture_endpoint, const std::string& id);  // replaces any previous endpoint
    void detach();
    bool in_use() const { return in_use_.load(); }
    std::string attached_id() const;
    int active_sessions() const;
    struct Impl;  // COM objects in sessions_win.cpp
 private:
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> in_use_{false};
    std::function<void(bool)> on_change_;
    friend struct SessionWatcherAccess;
};

// ------------------------------------------------------------------ WASAPI clients (loopback_win.cpp)
enum class ClientKind { Render, Capture, EndpointLoopback, ProcessLoopback };
struct Client {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    HANDLE event = nullptr;
    UINT32 buffer_frames = 0;
    int channels = 0;           // channels of the stream as initialized
    bool wasapi_convert = false;
    std::string path;           // diagnostics
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();
};
// Shared-mode, event-driven, 48 kHz float32. Capture/EndpointLoopback stream the mix's channels (fold-down is ours);
// Render uses `channels` (WASAPI converts when the mix differs: unity gain, unlike direct writes into an 8-ch mix).
HRESULT open_endpoint_client(IMMDevice* d, ClientKind kind, int channels, Client* c);
// Process loopback excluding (or including) the tree of `pid`; `channels` as requested.
HRESULT open_process_loopback(DWORD pid, bool include_tree, int channels, Client* c);
// Fold-down / mix-down / up-mix interleaved float frames (LFE dropped above stereo).
void convert_channels(const float* in, int ic, float* out, int oc, size_t frames);

// Silent render stream that keeps an endpoint's loopback delivering packets while nothing else plays.
class KeepAlive {
 public:
    explicit KeepAlive(std::string device_id);
    ~KeepAlive();
 private:
    std::string id_;
    std::atomic<bool> stop_{false};
    HANDLE thread_ = nullptr;
    static DWORD WINAPI run(void* self);
};

// Windows build number (RtlGetVersion), for process loopback (>= 20348 documented, works on 19041+).
DWORD windows_build();

// ------------------------------------------------------------------ test hooks (backend_check only)
// Makes the stream's IO thread behave as if WASAPI had returned AUDCLNT_E_DEVICE_INVALIDATED.
void inject_invalidation(StreamHandle* stream);
// How many endpoint mutes the backend holds right now.
int held_mutes(Backend* backend);

}  // namespace win
}  // namespace beamer::audio
