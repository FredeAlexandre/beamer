// beamer-audio: standalone WASAPI prototype for Beamer's audio feature (Windows side).
// PCM on stdout/stdin is f32le interleaved; diagnostics on stderr are JSON lines.
// SPDX-License-Identifier: GPL-3.0-or-later
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <objbase.h>
#include <mmsystem.h>
#include <timeapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;
using std::string;
using std::vector;
using std::wstring;

static const double kPi = 3.14159265358979323846;
static std::atomic<bool> g_stop{false};
static std::mutex g_logMutex;

// ---------------------------------------------------------------- strings, JSON, logging

static string utf8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static wstring wide(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static string jsonEscape(const string& s) {
    string o;
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
struct J {
    string s = "{";
    bool first = true;
    J& key(const char* k) { if (!first) s += ","; first = false; s += "\""; s += k; s += "\":"; return *this; }
    J& str(const char* k, const string& v) { key(k); s += "\"" + jsonEscape(v) + "\""; return *this; }
    J& num(const char* k, double v) {
        key(k);
        if (!std::isfinite(v)) { s += "null"; return *this; }
        char b[64]; snprintf(b, sizeof b, "%.6g", v); s += b; return *this;
    }
    J& i(const char* k, int64_t v) { key(k); s += std::to_string(v); return *this; }
    J& b(const char* k, bool v) { key(k); s += v ? "true" : "false"; return *this; }
    J& raw(const char* k, const string& v) { key(k); s += v; return *this; }
    J& hr(const char* k, HRESULT v) { char b[16]; snprintf(b, sizeof b, "0x%08lx", (unsigned long)v); return str(k, b); }
    string done() const { return s + "}"; }
};
static void writeLine(DWORD which, const string& line) {
    std::lock_guard<std::mutex> g(g_logMutex);
    string l = line + "\n";
    DWORD n;
    WriteFile(GetStdHandle(which), l.data(), (DWORD)l.size(), &n, nullptr);
}
static void logj(const J& j) { writeLine(STD_ERROR_HANDLE, j.done()); }
static void outLine(const string& s) { writeLine(STD_OUTPUT_HANDLE, s); }
static void logErr(const char* what, HRESULT hr) { logj(J().str("level", "error").str("what", what).hr("hr", hr)); }

// ---------------------------------------------------------------- args

struct Args {
    std::map<string, string> kv;
    std::map<string, int> count;  // how often each option was given (the last value wins in kv)
    bool has(const char* k) const { return kv.count(k) > 0; }
    string get(const char* k, const string& d = "") const { auto it = kv.find(k); return it == kv.end() ? d : it->second; }
    int geti(const char* k, int d) const { return has(k) ? atoi(get(k).c_str()) : d; }
    double getd(const char* k, double d) const { return has(k) ? atof(get(k).c_str()) : d; }
};
static Args parseArgs(int argc, wchar_t** argv, int start) {
    Args a;
    for (int i = start; i < argc; i++) {
        string k = utf8(argv[i]);
        if (k.rfind("--", 0) != 0) continue;
        k = k.substr(2);
        a.count[k]++;
        if (i + 1 < argc && wcsncmp(argv[i + 1], L"--", 2) != 0) a.kv[k] = utf8(argv[++i]);
        else a.kv[k] = "1";
    }
    return a;
}

// ---------------------------------------------------------------- time, cpu

static int64_t qpcFreq() { static int64_t f = [] { LARGE_INTEGER l; QueryPerformanceFrequency(&l); return l.QuadPart; }(); return f; }
// QPC in 100 ns units, the same unit as IAudioCaptureClient timestamps.
static uint64_t now100ns() {
    LARGE_INTEGER l; QueryPerformanceCounter(&l);
    int64_t f = qpcFreq();
    return (uint64_t)(l.QuadPart / f) * 10000000ull + (uint64_t)((l.QuadPart % f) * 10000000ll / f);
}
static double nowSec() { return now100ns() / 1e7; }

// GetProcessTimes samples on the clock tick and reads 0 for threads that wake for microseconds every
// 10 ms, so this counts cycles instead and scales by the nominal clock.
struct CpuMeter {
    uint64_t c0, w0;
    static uint64_t cycles() { ULONG64 c = 0; QueryProcessCycleTime(GetCurrentProcess(), &c); return c; }
    static double hz() {
        static double f = [] {
            DWORD mhz = 0, n = sizeof mhz;
            RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &n);
            return mhz ? mhz * 1e6 : 3e9;
        }();
        return f;
    }
    CpuMeter() : c0(cycles()), w0(now100ns()) {}
    // percent of one logical core
    double pct() const { uint64_t w = now100ns() - w0; return w ? 100.0 * (cycles() - c0) / (hz() * (w / 1e7)) : 0; }
};
static bool g_forceSrc = false;  // test hook: make WASAPI do all format conversion

struct ProAudioThread {
    HANDLE h = nullptr; DWORD idx = 0;
    ProAudioThread() { h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx); }
    ~ProAudioThread() { if (h) AvRevertMmThreadCharacteristics(h); }
};
struct ComThread {
    ComThread() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComThread() { CoUninitialize(); }
};

// ---------------------------------------------------------------- ring buffer (SPSC, bytes)

class Ring {
    vector<uint8_t> b_;
    std::atomic<size_t> w_{0}, r_{0};
public:
    explicit Ring(size_t cap) : b_(cap) {}
    size_t capacity() const { return b_.size(); }
    size_t size() const { return w_.load() - r_.load(); }
    size_t space() const { return b_.size() - size(); }
    size_t write(const void* p, size_t n) {
        n = std::min(n, space());
        size_t w = w_.load(), off = w % b_.size(), a = std::min(n, b_.size() - off);
        memcpy(&b_[off], p, a);
        memcpy(&b_[0], (const uint8_t*)p + a, n - a);
        w_.store(w + n);
        return n;
    }
    size_t read(void* p, size_t n) {
        n = std::min(n, size());
        size_t r = r_.load(), off = r % b_.size(), a = std::min(n, b_.size() - off);
        memcpy(p, &b_[off], a);
        memcpy((uint8_t*)p + a, &b_[0], n - a);
        r_.store(r + n);
        return n;
    }
    size_t drop(size_t n) { n = std::min(n, size()); r_.store(r_.load() + n); return n; }
};

// ---------------------------------------------------------------- devices

static ComPtr<IMMDeviceEnumerator> enumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    if (FAILED(hr)) logErr("MMDeviceEnumerator", hr);
    return e;
}
static string deviceId(IMMDevice* d) {
    LPWSTR id = nullptr; string s;
    if (SUCCEEDED(d->GetId(&id))) { s = utf8(id); CoTaskMemFree(id); }
    return s;
}
static string deviceName(IMMDevice* d) {
    ComPtr<IPropertyStore> ps; string s;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT v; PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) s = utf8(v.pwszVal);
        PropVariantClear(&v);
    }
    return s;
}
static string lower(string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }

// Exact id, else case-insensitive name substring among active endpoints, else the default.
static ComPtr<IMMDevice> resolveDevice(EDataFlow flow, const string& spec) {
    auto e = enumerator();
    ComPtr<IMMDevice> d;
    if (!e) return d;
    if (spec.empty()) {
        HRESULT hr = e->GetDefaultAudioEndpoint(flow, eConsole, &d);
        if (FAILED(hr)) logErr("GetDefaultAudioEndpoint", hr);
        return d;
    }
    if (SUCCEEDED(e->GetDevice(wide(spec).c_str(), &d))) return d;
    d.Reset();
    ComPtr<IMMDeviceCollection> col;
    if (FAILED(e->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) return d;
    UINT n = 0; col->GetCount(&n);
    string want = lower(spec);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> c;
        if (SUCCEEDED(col->Item(i, &c)) && lower(deviceName(c.Get())).find(want) != string::npos) return c;
    }
    logj(J().str("level", "error").str("what", "device not found").str("spec", spec));
    return d;
}

static bool isFloat32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}
static WAVEFORMATEXTENSIBLE floatFormat(int rate, int channels) {
    WAVEFORMATEXTENSIBLE x{};
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = (WORD)channels;
    x.Format.nSamplesPerSec = rate;
    x.Format.wBitsPerSample = 32;
    x.Format.nBlockAlign = (WORD)(channels * 4);
    x.Format.nAvgBytesPerSec = rate * channels * 4;
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = 32;
    const DWORD stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    const DWORD fiveOne = stereo | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    x.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ? stereo
        : channels == 4 ? (stereo | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
        : channels == 6 ? fiveOne : channels == 8 ? (fiveOne | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT) : 0;
    x.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return x;
}

// ---------------------------------------------------------------- stream

enum class Kind { Render, Capture, Loopback };

struct Stream {
    ComPtr<IAudioClient> c;
    ComPtr<IAudioCaptureClient> cap;
    ComPtr<IAudioRenderClient> ren;
    ComPtr<IAudioClock> clk;
    HANDLE ev = nullptr;
    UINT32 bufferFrames = 0, periodFrames = 0;
    int rate = 0, channels = 0;
    string path;       // which WASAPI init path was granted
    bool converted = false;
    int mixRate = 0, mixChannels = 0, mixBits = 0;
    double defaultPeriodMs = 0, minPeriodMs = 0, latencyMs = 0;
    UINT32 engDefault = 0, engFund = 0, engMin = 0, engMax = 0, engCurrent = 0;
    ~Stream() { if (ev) CloseHandle(ev); }
    string describe() const {
        return J().str("path", path).i("rate", rate).i("channels", channels).i("mix_rate", mixRate).i("mix_channels", mixChannels)
            .i("mix_bits", mixBits).b("wasapi_src", converted).i("buffer_frames", bufferFrames).i("period_frames", periodFrames)
            .num("period_ms", rate ? 1000.0 * periodFrames / rate : 0).num("device_period_default_ms", defaultPeriodMs)
            .num("device_period_min_ms", minPeriodMs).num("stream_latency_ms", latencyMs).i("engine_default_frames", engDefault)
            .i("engine_fundamental_frames", engFund).i("engine_min_frames", engMin).i("engine_max_frames", engMax)
            .i("engine_current_frames", engCurrent).done();
    }
};

static bool finishStream(Stream& s, Kind k) {
    HRESULT hr;
    s.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = s.c->SetEventHandle(s.ev))) { logErr("SetEventHandle", hr); return false; }
    if (FAILED(hr = s.c->GetBufferSize(&s.bufferFrames))) { logErr("GetBufferSize", hr); return false; }
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(s.c->GetStreamLatency(&lat))) s.latencyMs = lat / 10000.0;
    if (k == Kind::Render) hr = s.c->GetService(IID_PPV_ARGS(&s.ren));
    else hr = s.c->GetService(IID_PPV_ARGS(&s.cap));
    if (FAILED(hr)) { logErr("GetService", hr); return false; }
    s.c->GetService(IID_PPV_ARGS(&s.clk));
    return true;
}

// reqPeriodMs: 0 = lowest the engine offers, <0 = engine default, >0 = that many ms if possible.
// When the mix rate matches we stream the mix format and convert channels ourselves, which keeps
// IAudioClient3 low-latency available; otherwise WASAPI's own SRC does the conversion.
static bool openStream(IMMDevice* dev, Kind k, int reqRate, int reqCh, double reqPeriodMs, Stream& s, DWORD extraFlags = 0) {
    HRESULT hr;
    ComPtr<IAudioClient3> c3;
    if (FAILED(hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) { logErr("Activate", hr); return false; }
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = c3->GetMixFormat(&mix))) { logErr("GetMixFormat", hr); return false; }
    s.mixRate = mix->nSamplesPerSec; s.mixChannels = mix->nChannels; s.mixBits = mix->wBitsPerSample;
    REFERENCE_TIME def = 0, mn = 0;
    c3->GetDevicePeriod(&def, &mn);
    s.defaultPeriodMs = def / 10000.0; s.minPeriodMs = mn / 10000.0;
    c3->GetSharedModeEnginePeriod(mix, &s.engDefault, &s.engFund, &s.engMin, &s.engMax);

    // Surround mix: stream all its channels and fold down here. WASAPI's own 7.1->stereo matrix is
    // normalised and costs 13.5 dB on stereo content (measured).
    const int mixCh = mix->nChannels;
    // Rendering is the other way round: stereo written directly into the G733's 8-channel mix comes out
    // 3.98 dB low at any level, stereo through WASAPI's conversion is unity (results/level-test.jsonl).
    bool surroundMix = mixCh > 2 && reqCh != mixCh;
    bool chOk = !g_forceSrc && (reqCh == mixCh || mixCh == 1 || (k == Kind::Render ? !surroundMix : (reqCh == 1 || (reqCh <= 2 && mixCh > 2))));
    bool useMix = (int)mix->nSamplesPerSec == reqRate && isFloat32(mix) && chOk;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extraFlags;
    if (k == Kind::Loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

    int streamCh = mixCh;
    if (useMix && k != Kind::Loopback && reqPeriodMs >= 0 && extraFlags == 0 && s.engMin > 0) {
        UINT32 frames = s.engMin;
        if (reqPeriodMs > 0) {
            frames = (UINT32)(reqPeriodMs * mix->nSamplesPerSec / 1000.0 + 0.5);
            frames = std::clamp(frames, s.engMin, s.engMax);
            if (s.engFund) frames = (frames + s.engFund / 2) / s.engFund * s.engFund;
        }
        hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, frames, mix, nullptr);
        if (SUCCEEDED(hr)) {
            s.path = "IAudioClient3";
            s.c = c3;
            WAVEFORMATEX* cf = nullptr;
            if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cf, &s.engCurrent)) && cf) CoTaskMemFree(cf);
            s.periodFrames = s.engCurrent ? s.engCurrent : frames;
        } else {
            logj(J().str("level", "warn").str("what", "InitializeSharedAudioStream refused, falling back").hr("hr", hr));
            c3.Reset();
            if (FAILED(hr = dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &c3))) { CoTaskMemFree(mix); return false; }
        }
    }
    if (!s.c) {
        REFERENCE_TIME buf = reqPeriodMs > 0 ? (REFERENCE_TIME)(reqPeriodMs * 10000) : 0;
        streamCh = chOk ? mixCh : reqCh;
        WAVEFORMATEXTENSIBLE want = floatFormat(reqRate, streamCh);
        if (chOk && mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
            want.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask;
        const WAVEFORMATEX* fmt = mix;
        if (!useMix) {
            fmt = &want.Format;
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            s.converted = true;
        }
        hr = c3->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buf, 0, fmt, nullptr);
        if (FAILED(hr)) { logErr("IAudioClient::Initialize", hr); CoTaskMemFree(mix); return false; }
        s.path = useMix ? "IAudioClient" : "IAudioClient+AUTOCONVERTPCM";
        s.c = c3;
        WAVEFORMATEX* cf = nullptr;
        if (SUCCEEDED(c3->GetCurrentSharedModeEnginePeriod(&cf, &s.engCurrent)) && cf) CoTaskMemFree(cf);
        s.periodFrames = (UINT32)(reqRate * (s.engCurrent && s.mixRate ? (double)s.engCurrent / s.mixRate : s.defaultPeriodMs / 1000.0) + 0.5);
    }
    s.rate = useMix ? (int)mix->nSamplesPerSec : reqRate;
    s.channels = streamCh;
    CoTaskMemFree(mix);
    return finishStream(s, k);
}

// ---------------------------------------------------------------- process loopback

class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    std::atomic<ULONG> ref_{1};
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~ActivateHandler() { CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *p = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (r == __uuidof(IAgileObject)) *p = static_cast<IAgileObject*>(this);
        else { *p = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --ref_; if (!n) delete this; return n; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation*) override { SetEvent(done); return S_OK; }
};

static bool openProcessLoopback(DWORD pid, bool include, int rate, int channels, Stream& s) {
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        include ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
    auto* h = new ActivateHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
    if (FAILED(hr)) { logErr("ActivateAudioInterfaceAsync", hr); h->Release(); return false; }
    DWORD w = WaitForSingleObject(h->done, 5000);
    h->Release();
    if (w != WAIT_OBJECT_0) { logj(J().str("level", "error").str("what", "process loopback activation timed out")); return false; }
    HRESULT ar = E_FAIL; ComPtr<IUnknown> unk;
    hr = op->GetActivateResult(&ar, &unk);
    if (FAILED(hr) || FAILED(ar)) { logErr("process loopback activate", FAILED(hr) ? hr : ar); return false; }
    unk.As(&s.c);
    WAVEFORMATEXTENSIBLE want = floatFormat(rate, channels);
    hr = s.c->Initialize(AUDCLNT_SHAREMODE_SHARED,
                         AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                         200000, 0, &want.Format, nullptr);
    if (FAILED(hr)) { logErr("process loopback Initialize", hr); return false; }
    s.rate = rate; s.channels = channels; s.mixRate = rate; s.mixChannels = channels; s.mixBits = 32;
    s.path = include ? "ProcessLoopback(include)" : "ProcessLoopback(exclude)";
    s.periodFrames = rate / 100;
    return finishStream(s, Kind::Loopback);
}

// ---------------------------------------------------------------- endpoint state guard

static wstring journalPath() {
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    return wstring(tmp) + L"beamer-audio-endpoint-journal.txt";
}
static ComPtr<IAudioEndpointVolume> endpointVolume(IMMDevice* d) {
    ComPtr<IAudioEndpointVolume> v;
    d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &v);
    return v;
}
// Puts back what a dead run left behind. Journal lines: <device id>\t<mute 0|1>\t<scalar>
static bool restoreFromJournal() {
    std::ifstream f(journalPath());
    if (!f) return false;
    string id; int mute; float vol;
    bool any = false;
    while (f >> id >> mute >> vol) {
        auto e = enumerator(); ComPtr<IMMDevice> d;
        if (e && SUCCEEDED(e->GetDevice(wide(id).c_str(), &d))) {
            if (auto v = endpointVolume(d.Get())) {
                v->SetMasterVolumeLevelScalar(vol, nullptr);
                v->SetMute(mute, nullptr);
                any = true;
                logj(J().str("event", "endpoint_restored").str("device", id).i("mute", mute).num("scalar", vol));
            }
        }
    }
    f.close();
    DeleteFileW(journalPath().c_str());
    return any;
}
// Saves mute and volume, restores them on scope exit. A guard process and a journal cover the cases
// the destructor cannot: taskkill /F of this process, and a kill of both (next run restores).
class EndpointStateGuard {
    ComPtr<IAudioEndpointVolume> v_;
    BOOL mute_ = FALSE; float vol_ = 0; bool saved_ = false;
    HANDLE guard_ = nullptr; DWORD guardPid_ = 0;
public:
    explicit EndpointStateGuard(IMMDevice* d) {
        restoreFromJournal();
        v_ = endpointVolume(d);
        if (!v_) return;
        if (FAILED(v_->GetMute(&mute_)) || FAILED(v_->GetMasterVolumeLevelScalar(&vol_))) return;
        saved_ = true;
        { std::ofstream f(journalPath()); char b[64]; snprintf(b, sizeof b, "\t%d\t%.9g\n", mute_ ? 1 : 0, vol_); f << deviceId(d) << b; }
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wstring cmd = L"\"" + wstring(exe) + L"\" _guard --pid " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            guard_ = pi.hProcess; guardPid_ = pi.dwProcessId; CloseHandle(pi.hThread);
        }
    }
    bool ok() const { return saved_; }
    DWORD guardPid() const { return guardPid_; }
    IAudioEndpointVolume* vol() const { return v_.Get(); }
    void restore() {
        if (!saved_) return;
        v_->SetMasterVolumeLevelScalar(vol_, nullptr);
        v_->SetMute(mute_, nullptr);
        DeleteFileW(journalPath().c_str());
        saved_ = false;
        BOOL m = FALSE; float s = 0; v_->GetMute(&m); v_->GetMasterVolumeLevelScalar(&s);
        logj(J().str("event", "endpoint_restored").b("mute", m != FALSE).num("scalar", s));
    }
    ~EndpointStateGuard() { restore(); if (guard_) CloseHandle(guard_); }
};

static int cmdGuard(const Args& a) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)a.geti("pid", 0));
    if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    restoreFromJournal();
    return 0;
}

// ---------------------------------------------------------------- list / watch

static string deviceJson(IMMDevice* d, EDataFlow flow, const string& defC, const string& defM, const string& defK) {
    DWORD st = 0; d->GetState(&st);
    string id = deviceId(d);
    const char* state = st == DEVICE_STATE_ACTIVE ? "active" : st == DEVICE_STATE_DISABLED ? "disabled" : st == DEVICE_STATE_UNPLUGGED ? "unplugged" : "notpresent";
    J j;
    j.str("id", id).str("name", deviceName(d)).str("flow", flow == eRender ? "render" : "capture").str("state", state)
        .b("default_console", id == defC).b("default_multimedia", id == defM).b("default_communications", id == defK);
    if (st == DEVICE_STATE_ACTIVE) {
        ComPtr<IAudioClient> c;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) {
            WAVEFORMATEX* m = nullptr;
            if (SUCCEEDED(c->GetMixFormat(&m))) {
                j.raw("mix", J().i("rate", m->nSamplesPerSec).i("channels", m->nChannels).i("bits", m->wBitsPerSample).b("float", isFloat32(m)).done());
                CoTaskMemFree(m);
            }
            REFERENCE_TIME def = 0, mn = 0;
            if (SUCCEEDED(c->GetDevicePeriod(&def, &mn))) j.num("period_default_ms", def / 10000.0).num("period_min_ms", mn / 10000.0);
        }
        if (auto v = endpointVolume(d)) {
            BOOL mute = FALSE; float s = 0; DWORD hw = 0;
            v->GetMute(&mute); v->GetMasterVolumeLevelScalar(&s); v->QueryHardwareSupport(&hw);
            j.b("mute", mute != FALSE).num("volume", s).b("hw_volume", hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME).b("hw_mute", hw & ENDPOINT_HARDWARE_SUPPORT_MUTE)
                .b("hw_meter", hw & ENDPOINT_HARDWARE_SUPPORT_METER);
        }
    }
    return j.done();
}

static string defaultId(IMMDeviceEnumerator* e, EDataFlow f, ERole r) {
    ComPtr<IMMDevice> d;
    return SUCCEEDED(e->GetDefaultAudioEndpoint(f, r, &d)) ? deviceId(d.Get()) : string();
}

static int cmdList(const Args& a) {
    auto e = enumerator();
    if (!e) return 1;
    DWORD mask = a.has("all") ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE;
    string out = "[";
    bool first = true;
    for (EDataFlow flow : {eCapture, eRender}) {
        string dc = defaultId(e.Get(), flow, eConsole), dm = defaultId(e.Get(), flow, eMultimedia), dk = defaultId(e.Get(), flow, eCommunications);
        ComPtr<IMMDeviceCollection> col;
        if (FAILED(e->EnumAudioEndpoints(flow, mask, &col))) continue;
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            ComPtr<IMMDevice> d;
            if (FAILED(col->Item(i, &d))) continue;
            string js = deviceJson(d.Get(), flow, dc, dm, dk);
            if (a.has("json")) { out += (first ? "" : ",") + js; first = false; }
            else {
                string id = deviceId(d.Get());
                string marks = string(id == dc ? "C" : "-") + (id == dm ? "M" : "-") + (id == dk ? "K" : "-");
                outLine(string(flow == eRender ? "out " : "in  ") + marks + "  " + deviceName(d.Get()) + "\n          " + id + "\n          " + js);
            }
        }
    }
    if (a.has("json")) outLine(out + "]");
    return 0;
}

struct DevEvent { string kind, id; int flow = -1, role = -1; DWORD state = 0; string prop; };
class NotifyClient : public IMMNotificationClient {
    std::atomic<ULONG> ref_{1};
public:
    std::mutex m; std::deque<DevEvent> q; HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void push(DevEvent e) { { std::lock_guard<std::mutex> g(m); q.push_back(std::move(e)); } SetEvent(ev); }
    STDMETHODIMP QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMMNotificationClient)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --ref_; if (!n) delete this; return n; }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR id, DWORD st) override { DevEvent e; e.kind = "state_changed"; e.id = utf8(id); e.state = st; push(e); return S_OK; }
    STDMETHODIMP OnDeviceAdded(LPCWSTR id) override { DevEvent e; e.kind = "added"; e.id = utf8(id); push(e); return S_OK; }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR id) override { DevEvent e; e.kind = "removed"; e.id = utf8(id); push(e); return S_OK; }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow f, ERole r, LPCWSTR id) override {
        DevEvent e; e.kind = "default_changed"; e.flow = f; e.role = r; e.id = id ? utf8(id) : ""; push(e); return S_OK;
    }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY k) override {
        DevEvent e; e.kind = "property_changed"; e.id = utf8(id); char b[80];
        snprintf(b, sizeof b, "{%08lX-%04X-%04X}/%lu", k.fmtid.Data1, k.fmtid.Data2, k.fmtid.Data3, k.pid); e.prop = b;
        push(e); return S_OK;
    }
};

static BOOL WINAPI ctrlHandler(DWORD) { g_stop = true; return TRUE; }

static int cmdWatch(const Args& a) {
    auto e = enumerator();
    if (!e) return 1;
    auto* n = new NotifyClient();
    HRESULT hr = e->RegisterEndpointNotificationCallback(n);
    if (FAILED(hr)) { logErr("RegisterEndpointNotificationCallback", hr); return 1; }
    outLine(J().str("event", "watching").num("t", nowSec()).done());
    double deadline = a.has("seconds") ? nowSec() + a.getd("seconds", 0) : 0;
    const char* roles[] = {"console", "multimedia", "communications"};
    while (!g_stop && (!deadline || nowSec() < deadline)) {
        WaitForSingleObject(n->ev, 200);
        std::deque<DevEvent> q;
        { std::lock_guard<std::mutex> g(n->m); q.swap(n->q); }
        for (auto& ev : q) {
            J j; j.str("event", ev.kind).num("t", nowSec()).str("id", ev.id);
            ComPtr<IMMDevice> d;
            if (!ev.id.empty() && SUCCEEDED(e->GetDevice(wide(ev.id).c_str(), &d))) j.str("name", deviceName(d.Get()));
            if (ev.flow >= 0) j.str("flow", ev.flow == eRender ? "render" : "capture").str("role", roles[ev.role]);
            if (ev.kind == "state_changed") j.i("state", ev.state);
            if (!ev.prop.empty()) j.str("property", ev.prop);
            outLine(j.done());
        }
    }
    e->UnregisterEndpointNotificationCallback(n);
    n->Release();
    return 0;
}

// ---------------------------------------------------------------- pump helpers

static void convertFrames(const float* in, int ic, float* out, int oc, size_t frames) {
    if (ic == oc) { memcpy(out, in, frames * ic * sizeof(float)); return; }
    if (ic > 2 && oc <= 2) {
        // Layouts: 4 = FL FR BL BR, 6 = FL FR FC LFE SL SR, 8 = FL FR FC LFE BL BR SL SR. LFE is dropped.
        const float k = 0.7071f;
        for (size_t f = 0; f < frames; f++) {
            const float* x = in + f * ic;
            float l = x[0], r = x[1];
            if (ic >= 6) { l += k * x[2]; r += k * x[2]; }
            for (int c = ic >= 6 ? 4 : 2; c + 1 < ic; c += 2) { l += k * x[c]; r += k * x[c + 1]; }
            l = std::clamp(l, -1.f, 1.f); r = std::clamp(r, -1.f, 1.f);
            if (oc == 1) out[f] = 0.5f * (l + r);
            else { out[f * 2] = l; out[f * 2 + 1] = r; }
        }
        return;
    }
    if (oc == 1) {
        for (size_t f = 0; f < frames; f++) { float s = 0; for (int c = 0; c < ic; c++) s += in[f * ic + c]; out[f] = s / ic; }
        return;
    }
    if (ic == 1) {
        for (size_t f = 0; f < frames; f++) for (int c = 0; c < oc; c++) out[f * oc + c] = in[f];
        return;
    }
    for (size_t f = 0; f < frames; f++) for (int c = 0; c < oc; c++) out[f * oc + c] = c < ic ? in[f * ic + c] : 0.f;
}

// Silent render stream. A loopback stream only delivers packets while something renders to the endpoint,
// so without this the PCM clock on stdout would stop whenever the machine is quiet.
class KeepAlive {
    std::thread t_; std::atomic<bool> stop_{false};
    string id_;
public:
    explicit KeepAlive(const string& id) : id_(id) {
        t_ = std::thread([this] {
            ComThread com; ProAudioThread pro;
            auto d = resolveDevice(eRender, id_);
            Stream s;
            if (!d || !openStream(d.Get(), Kind::Render, 48000, 2, -1, s)) return;
            s.c->Start();
            while (!stop_) {
                WaitForSingleObject(s.ev, 100);
                UINT32 pad = 0;
                if (FAILED(s.c->GetCurrentPadding(&pad))) break;
                UINT32 n = s.bufferFrames - pad; BYTE* p;
                if (n && SUCCEEDED(s.ren->GetBuffer(n, &p))) s.ren->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            s.c->Stop();
        });
    }
    ~KeepAlive() { stop_ = true; t_.join(); }
};

struct RunStats {
    uint64_t frames = 0, packets = 0, silentPackets = 0, discontinuities = 0, overrunBytes = 0, underruns = 0, droppedBytes = 0;
    double ageSum = 0; double ageMax = -1e9;
};

static void applyMuteLocal(IAudioEndpointVolume* v) { v->SetMute(TRUE, nullptr); }

// ---------------------------------------------------------------- capture commands

static int pumpCapture(Stream& s, int outCh, double seconds) {
    const size_t outBpf = outCh * sizeof(float);
    Ring ring((size_t)(s.rate * outBpf));  // one second
    HANDLE dataEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> writerDone{false};
    std::thread writer([&] {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        vector<uint8_t> tmp(65536);
        while (true) {
            size_t n = ring.read(tmp.data(), tmp.size());
            if (!n) {
                if (writerDone) break;
                WaitForSingleObject(dataEv, 50);
                continue;
            }
            size_t off = 0;
            while (off < n) {
                DWORD w = 0;
                if (!WriteFile(out, tmp.data() + off, (DWORD)(n - off), &w, nullptr)) { g_stop = true; return; }
                off += w;
            }
        }
    });
    ProAudioThread pro;
    CpuMeter cpu;
    RunStats st;
    vector<float> conv;
    double start = nowSec(), nextReport = start + 5, deadline = seconds > 0 ? start + seconds : 0;
    HRESULT hr = s.c->Start();
    if (FAILED(hr)) logErr("Start", hr);
    int rc = FAILED(hr) ? 1 : 0;
    while (!rc && !g_stop && (!deadline || nowSec() < deadline)) {
        WaitForSingleObject(s.ev, 100);
        UINT32 pk = 0;
        while (SUCCEEDED(hr = s.cap->GetNextPacketSize(&pk)) && pk) {
            BYTE* data; UINT32 n; DWORD flags; UINT64 pos, qpc;
            hr = s.cap->GetBuffer(&data, &n, &flags, &pos, &qpc);
            if (FAILED(hr)) break;
            uint64_t t = now100ns();
            double age = ((double)t - (double)qpc) / 1e4;
            st.ageSum += age; st.ageMax = std::max(st.ageMax, age);
            st.packets++; st.frames += n;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) st.discontinuities++;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) st.silentPackets++;
            conv.resize(n * outCh);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::fill(conv.begin(), conv.end(), 0.f);
            else convertFrames(reinterpret_cast<float*>(data), s.channels, conv.data(), outCh, n);
            s.cap->ReleaseBuffer(n);
            size_t bytes = n * outBpf;
            if (ring.space() >= bytes) ring.write(conv.data(), bytes);
            else st.overrunBytes += bytes;  // whole packets only, so an overrun cannot misalign frames
            SetEvent(dataEv);
        }
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { logj(J().str("event", "device_invalidated")); rc = 3; break; }
        if (FAILED(hr)) { logErr("capture loop", hr); rc = 1; break; }
        double t = nowSec();
        if (t >= nextReport) {
            nextReport = t + 5;
            logj(J().str("event", "stats").num("elapsed_s", t - start).i("frames", st.frames).num("rate_hz", st.frames / (t - start)).i("packets", st.packets)
                .i("silent_packets", st.silentPackets).i("discontinuities", st.discontinuities).i("overrun_bytes", st.overrunBytes)
                .num("packet_age_mean_ms", st.packets ? st.ageSum / st.packets : 0).num("cpu_pct_of_core", cpu.pct()));
        }
    }
    s.c->Stop();
    double el = nowSec() - start;
    writerDone = true;
    SetEvent(dataEv);
    writer.join();
    CloseHandle(dataEv);
    logj(J().str("event", "end").num("elapsed_s", el).i("frames", st.frames).num("rate_hz", st.frames / el).i("packets", st.packets)
        .i("silent_packets", st.silentPackets).i("discontinuities", st.discontinuities).i("overrun_bytes", st.overrunBytes)
        .num("packet_age_mean_ms", st.packets ? st.ageSum / st.packets : 0).num("packet_age_max_ms", st.packets ? st.ageMax : 0)
        .num("cpu_pct_of_core", cpu.pct()));
    return rc;
}

static int cmdCaptureMic(const Args& a) {
    int rate = a.geti("rate", 48000), ch = a.geti("channels", 1);
    auto d = resolveDevice(eCapture, a.get("device"));
    if (!d) return 2;
    Stream s;
    if (!openStream(d.Get(), Kind::Capture, rate, ch, a.getd("period-ms", 0), s)) return 1;
    logj(J().str("event", "capture_mic").str("device", deviceName(d.Get())).str("id", deviceId(d.Get())).raw("stream", s.describe()));
    return pumpCapture(s, ch, a.getd("seconds", 0));
}

static int mixChannels(IMMDevice* d, int fallback) {
    ComPtr<IAudioClient> c; int n = fallback;
    if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c))) {
        WAVEFORMATEX* m = nullptr;
        if (SUCCEEDED(c->GetMixFormat(&m))) { n = m->nChannels; CoTaskMemFree(m); }
    }
    return n;
}

static int cmdCaptureSystem(const Args& a) {
    int rate = a.geti("rate", 48000), ch = a.geti("channels", 2);
    string spec = a.get("device");
    auto d = resolveDevice(eRender, spec);
    if (!d) return 2;
    Stream s;
    string proc = a.get("process-loopback");
    if (a.has("exclude-pid")) {
        // Process loopback can exclude exactly one process tree. Beamer arranges for its player(s)
        // and this capture to run as children of one parent (Beamer.exe) and passes that parent's PID.
        if (a.count.at("exclude-pid") > 1 || !proc.empty()) {
            logj(J().str("level", "error").str("what", "Windows can exclude one process tree only: pass a single --exclude-pid "
                                                    "(the common parent of the processes to leave out), without --process-loopback"));
            return 2;
        }
        DWORD pid = (DWORD)a.geti("exclude-pid", 0);
        HANDLE ph = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr;
        if (!ph) { logj(J().str("level", "error").str("what", "--exclude-pid: no such process").i("pid", pid)); return 2; }
        wchar_t img[MAX_PATH]; DWORD n = MAX_PATH;
        string image = QueryFullProcessImageNameW(ph, 0, img, &n) ? utf8(wstring(img, n)) : "";
        CloseHandle(ph);
        logj(J().str("event", "exclude_tree").i("pid", pid).str("image", image));
        if (!openProcessLoopback(pid, false, rate, mixChannels(d.Get(), ch), s)) return 1;
    } else if (!proc.empty()) {
        DWORD pid = (DWORD)a.geti("pid", (int)GetCurrentProcessId());
        if (!openProcessLoopback(pid, proc == "include", rate, mixChannels(d.Get(), ch), s)) return 1;
    } else if (!openStream(d.Get(), Kind::Loopback, rate, ch, -1, s)) return 1;
    logj(J().str("event", "capture_system").str("device", deviceName(d.Get())).str("id", deviceId(d.Get())).b("mute_local", a.has("mute-local")).raw("stream", s.describe()));
    std::unique_ptr<EndpointStateGuard> guard;
    if (a.has("mute-local")) {
        guard = std::make_unique<EndpointStateGuard>(d.Get());
        if (!guard->ok()) { logj(J().str("level", "error").str("what", "cannot read endpoint volume")); return 1; }
        DWORD hw = 0; guard->vol()->QueryHardwareSupport(&hw);
        if (!(hw & ENDPOINT_HARDWARE_SUPPORT_MUTE))
            logj(J().str("level", "warn").str("what", "endpoint mute is not in hardware: loopback under software mute is unverified"));
        applyMuteLocal(guard->vol());
    }
    std::unique_ptr<KeepAlive> ka;
    if (!a.has("no-keepalive")) ka = std::make_unique<KeepAlive>(deviceId(d.Get()));
    return pumpCapture(s, ch, a.getd("seconds", 0));
}

// ---------------------------------------------------------------- play

static int cmdPlay(const Args& a) {
    int rate = a.geti("rate", 48000), ch = a.geti("channels", 2);
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    Stream s;
    if (!openStream(d.Get(), Kind::Render, rate, ch, a.getd("period-ms", 0), s)) return 1;
    logj(J().str("event", "play").str("device", deviceName(d.Get())).str("id", deviceId(d.Get())).raw("stream", s.describe()));

    const size_t inBpf = ch * sizeof(float);
    const size_t bytesPerMs = (size_t)(rate * inBpf / 1000);
    // Bounded queue: a producer faster than real time blocks on the pipe. --live-drop-ms sets the depth
    // past which a live producer's backlog is discarded down to --queue-ms instead.
    const size_t queueMs = (size_t)a.geti("queue-ms", 40);
    const size_t liveDropMs = (size_t)a.geti("live-drop-ms", 0);
    Ring ring(std::max<size_t>(liveDropMs, queueMs) * 2 * bytesPerMs + 65536);
    size_t highWater = (liveDropMs ? liveDropMs : queueMs) * bytesPerMs;
    std::atomic<bool> eof{false};
    std::atomic<uint64_t> dropped{0}, received{0};
    std::thread reader([&] {
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
        vector<uint8_t> tmp(8192);
        while (!g_stop) {
            DWORD n = 0;
            if (!ReadFile(in, tmp.data(), (DWORD)tmp.size(), &n, nullptr) || n == 0) break;
            received += n;
            if (liveDropMs && ring.size() + n > highWater) {
                size_t excess = ring.size() + n - queueMs * bytesPerMs;
                excess -= excess % inBpf;
                dropped += ring.drop(excess);
            }
            size_t off = 0;
            while (off < n && !g_stop) {
                if (ring.size() >= highWater + bytesPerMs) { Sleep(1); continue; }
                off += ring.write(tmp.data() + off, n - off);
            }
        }
        eof = true;
    });
    reader.detach();

    ProAudioThread pro;
    CpuMeter cpu;
    double start = nowSec(), nextReport = start + 5;
    double seconds = a.getd("seconds", 0);
    uint64_t frames = 0, silentFrames = 0, underrunEvents = 0, wakes = 0;
    UINT32 targetPad = std::min(s.bufferFrames, (UINT32)(s.periodFrames * a.getd("pad-periods", 2)));
    vector<float> in, out;
    size_t startBytes = (size_t)(a.getd("start-ms", 1000.0 * s.periodFrames / s.rate) * bytesPerMs);
    bool started = false, wasStarving = false;
    HRESULT hr = s.c->Start();
    int rc = 0;
    if (FAILED(hr)) { logErr("Start", hr); rc = 1; }
    while (!rc && !g_stop && (!seconds || nowSec() - start < seconds)) {
        WaitForSingleObject(s.ev, 100);
        wakes++;
        UINT32 pad = 0;
        hr = s.c->GetCurrentPadding(&pad);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { logj(J().str("event", "device_invalidated")); rc = 3; break; }
        if (FAILED(hr)) { logErr("GetCurrentPadding", hr); rc = 1; break; }
        if (!started && (ring.size() >= startBytes || eof)) started = true;
        if (pad < targetPad) {
            UINT32 n = targetPad - pad;
            BYTE* p;
            hr = s.ren->GetBuffer(n, &p);
            if (hr == AUDCLNT_E_DEVICE_INVALIDATED) { rc = 3; break; }
            if (SUCCEEDED(hr)) {
                size_t have = started ? ring.size() / inBpf : 0;
                size_t take = std::min<size_t>(have, n);
                if (take) {
                    in.resize(take * ch);
                    ring.read(in.data(), take * inBpf);
                    convertFrames(in.data(), ch, reinterpret_cast<float*>(p), s.channels, take);
                }
                if (take < n) {
                    memset(p + take * s.channels * sizeof(float), 0, (n - take) * s.channels * sizeof(float));
                    silentFrames += n - take;
                    if (started && !eof && !wasStarving) underrunEvents++;
                    wasStarving = true;
                } else wasStarving = false;
                frames += take;
                s.ren->ReleaseBuffer(n, take ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
            }
        }
        if (eof && ring.size() < inBpf) {
            // let what is queued in the device buffer play out
            UINT32 left = 0;
            if (FAILED(s.c->GetCurrentPadding(&left)) || left == 0) break;
            Sleep((DWORD)(1000.0 * left / s.rate) + 1);
            break;
        }
        double t = nowSec();
        if (t >= nextReport) {
            nextReport = t + 5;
            logj(J().str("event", "stats").num("elapsed_s", t - start).i("frames", frames).i("silent_frames", silentFrames).i("underruns", underrunEvents)
                .i("dropped_bytes", dropped.load()).num("queue_ms", (double)ring.size() / bytesPerMs).i("pad_frames", pad).num("cpu_pct_of_core", cpu.pct()));
        }
    }
    s.c->Stop();
    logj(J().str("event", "end").num("elapsed_s", nowSec() - start).i("frames", frames).i("silent_frames", silentFrames).i("underruns", underrunEvents)
        .i("received_bytes", received.load()).i("dropped_bytes", dropped.load()).i("wakes", wakes).num("cpu_pct_of_core", cpu.pct()));
    // The stdin reader may be parked in ReadFile: leave without joining it.
    ExitProcess(rc);
}

// ---------------------------------------------------------------- measure: helpers

struct Ana {
    bool opened = false;
    uint64_t frames = 0, packets = 0, silent = 0, disc = 0;
    double sumsq = 0, peak = 0, tone = 0;
    double rms() const { return frames ? std::sqrt(sumsq / frames) : 0; }
};
static double dbfs(double x) { return x > 1e-10 ? 20 * std::log10(x) : -200.0; }
static string anaJson(const Ana& a) {
    return J().b("opened", a.opened).i("frames", a.frames).i("packets", a.packets).i("silent_packets", a.silent).i("discontinuities", a.disc)
        .num("rms_dbfs", dbfs(a.rms())).num("peak_dbfs", dbfs(a.peak)).num("tone_dbfs", dbfs(a.tone)).done();
}
// Captures for `seconds`, measures level and a Goertzel of `freq` on channel 0.
static void analyze(Stream& s, double seconds, double freq, Ana& a) {
    a.opened = true;
    double w = 2 * kPi * freq / s.rate, coeff = 2 * std::cos(w), s1 = 0, s2 = 0;
    uint64_t skip = (uint64_t)(0.3 * s.rate), seen = 0, n = 0;
    ProAudioThread pro;
    s.c->Start();
    double t0 = nowSec();
    while (nowSec() - t0 < seconds + 0.3) {
        WaitForSingleObject(s.ev, 50);
        UINT32 pk = 0;
        while (SUCCEEDED(s.cap->GetNextPacketSize(&pk)) && pk) {
            BYTE* data; UINT32 fr; DWORD flags;
            if (FAILED(s.cap->GetBuffer(&data, &fr, &flags, nullptr, nullptr))) break;
            a.packets++;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) a.silent++;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) a.disc++;
            const float* x = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < fr; i++, seen++) {
                if (seen < skip) continue;
                double v = (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : x[i * s.channels];
                a.sumsq += v * v; a.peak = std::max(a.peak, std::abs(v));
                double s0 = v + coeff * s1 - s2; s2 = s1; s1 = s0;
                n++;
            }
            s.cap->ReleaseBuffer(fr);
        }
    }
    s.c->Stop();
    a.frames = n;
    if (n) a.tone = std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) * 2 / n;
}

static HANDLE spawnSelf(const wstring& args, DWORD* pidOut = nullptr) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wstring cmd = L"\"" + wstring(exe) + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        logj(J().str("level", "error").str("what", "CreateProcess").i("gle", GetLastError()));
        return nullptr;
    }
    CloseHandle(pi.hThread);
    if (pidOut) *pidOut = pi.dwProcessId;
    return pi.hProcess;
}

// Child process playing a sine, so that it has its own PID and audio session.
// Renders a sine at a given queue depth and looks for holes in it on the loopback: a render stream
// that misses the engine's deadline shows up as a run of silence or a jump in the waveform.
static int cmdGlitch(const Args& a) {
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    double pads = a.getd("pad-periods", 2), seconds = a.getd("seconds", 60), amp = a.getd("amp", 0.1), freq = 997;
    Stream r, l;
    if (!openStream(d.Get(), Kind::Render, 48000, 2, a.getd("period-ms", 0), r)) return 1;
    if (!openStream(d.Get(), Kind::Loopback, 48000, 2, -1, l)) return 1;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> lateWakes{0};
    std::thread rt([&] {
        ComThread com; ProAudioThread pro;
        UINT32 target = std::min(r.bufferFrames, (UINT32)(r.periodFrames * pads));
        double ph = 0, dph = 2 * kPi * freq / r.rate;
        // Negative control: --stall-every-s S sleeps 60 ms every S seconds, which must register as holes.
        double stallEvery = a.getd("stall-every-s", 0), nextStall = nowSec() + stallEvery;
        r.c->Start();
        while (!done) {
            WaitForSingleObject(r.ev, 100);
            if (stallEvery && nowSec() >= nextStall) { nextStall += stallEvery; Sleep(60); }
            UINT32 pad = 0;
            if (FAILED(r.c->GetCurrentPadding(&pad))) break;
            if (pad == 0) lateWakes++;
            if (pad >= target) continue;
            UINT32 n = target - pad; BYTE* p;
            if (FAILED(r.ren->GetBuffer(n, &p))) continue;
            float* x = reinterpret_cast<float*>(p);
            for (UINT32 i = 0; i < n; i++) {
                float v = (float)(amp * std::sin(ph));
                ph += dph; if (ph > 2 * kPi) ph -= 2 * kPi;
                for (int c = 0; c < r.channels; c++) x[i * r.channels + c] = c < 2 ? v : 0.f;
            }
            r.ren->ReleaseBuffer(n, 0);
        }
        r.c->Stop();
    });
    ProAudioThread pro;
    Sleep(500);
    l.c->Start();
    uint64_t frames = 0, holes = 0, jumps = 0, disc = 0, run = 0, skip = 24000;
    double prev = 0, maxStep = 2 * amp * std::sin(kPi * freq / r.rate) * 1.5;
    bool havePrev = false, inHole = false;
    double t0 = nowSec();
    while (nowSec() - t0 < seconds) {
        WaitForSingleObject(l.ev, 50);
        UINT32 pk = 0;
        while (SUCCEEDED(l.cap->GetNextPacketSize(&pk)) && pk) {
            BYTE* data; UINT32 fr; DWORD fl;
            if (FAILED(l.cap->GetBuffer(&data, &fr, &fl, nullptr, nullptr))) break;
            if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) { if (frames > skip) disc++; havePrev = false; }
            const float* x = reinterpret_cast<const float*>(data);
            for (UINT32 i = 0; i < fr; i++, frames++) {
                if (frames < skip) continue;
                double v = (fl & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0 : x[i * l.channels];
                if (std::abs(v) < amp * 1e-3) {
                    if (++run >= 8 && !inHole) { holes++; inHole = true; }
                } else { run = 0; inHole = false; }
                if (havePrev && std::abs(v - prev) > maxStep) jumps++;
                prev = v; havePrev = true;
            }
            l.cap->ReleaseBuffer(fr);
        }
    }
    done = true;
    rt.join();
    l.c->Stop();
    outLine(J().str("event", "glitch_result").num("pad_periods", pads).num("seconds", seconds).i("frames_checked", frames > skip ? frames - skip : 0)
        .i("holes", holes).i("jumps", jumps).i("discontinuities", disc).i("render_wakes_with_empty_buffer", lateWakes.load()).done());
    return 0;
}

// Spins on every logical core at normal priority, to test the audio threads under load.
static int cmdBurn(const Args& a) {
    double seconds = a.getd("seconds", 30);
    vector<std::thread> ts;
    for (unsigned i = 0; i < std::thread::hardware_concurrency(); i++)
        ts.emplace_back([seconds] { double t0 = nowSec(); volatile double x = 1; while (nowSec() - t0 < seconds) x = std::sqrt(x + 1.5); });
    for (auto& t : ts) t.join();
    return 0;
}

// f32le sine on stdout, unpaced: `play` must apply backpressure through the pipe.
static int cmdGen(const Args& a) {
    int rate = a.geti("rate", 48000), ch = a.geti("channels", 2);
    double freq = a.getd("freq", 997), amp = a.getd("amp", 0.1), seconds = a.getd("seconds", 5), ph = 0, dph = 2 * kPi * freq / rate;
    size_t total = (size_t)(seconds * rate);
    vector<float> buf(4096 * ch);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    for (size_t done = 0; done < total;) {
        size_t n = std::min<size_t>(4096, total - done);
        for (size_t i = 0; i < n; i++) {
            float v = (float)(amp * std::sin(ph));
            ph += dph; if (ph > 2 * kPi) ph -= 2 * kPi;
            for (int c = 0; c < ch; c++) buf[i * ch + c] = v;
        }
        DWORD w;
        if (!WriteFile(out, buf.data(), (DWORD)(n * ch * sizeof(float)), &w, nullptr)) return 1;
        done += n;
    }
    return 0;
}

// Per-channel level and tone of f32le on stdin, to check what the tools actually emit.
static int cmdPcmStats(const Args& a) {
    int rate = a.geti("rate", 48000), ch = a.geti("channels", 2);
    double freq = a.getd("freq", 997), skipS = a.getd("skip-s", 0.5);
    double coeff = 2 * std::cos(2 * kPi * freq / rate);
    vector<double> s1(ch), s2(ch), sq(ch), pk(ch);
    vector<float> buf(4096 * ch);
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    uint64_t frames = 0, used = 0, skip = (uint64_t)(skipS * rate);
    size_t carry = 0;
    while (true) {
        DWORD n = 0;
        if (!ReadFile(in, (uint8_t*)buf.data() + carry, (DWORD)(buf.size() * sizeof(float) - carry), &n, nullptr) || !n) break;
        size_t bytes = carry + n, fr = bytes / (ch * sizeof(float));
        for (size_t i = 0; i < fr; i++, frames++) {
            if (frames < skip) continue;
            used++;
            for (int c = 0; c < ch; c++) {
                double v = buf[i * ch + c];
                sq[c] += v * v; pk[c] = std::max(pk[c], std::abs(v));
                double s0 = v + coeff * s1[c] - s2[c]; s2[c] = s1[c]; s1[c] = s0;
            }
        }
        carry = bytes - fr * ch * sizeof(float);
        if (carry) memmove(buf.data(), (uint8_t*)buf.data() + fr * ch * sizeof(float), carry);
    }
    string chans = "[";
    for (int c = 0; c < ch; c++) {
        double tone = used ? std::sqrt(std::max(0.0, s1[c] * s1[c] + s2[c] * s2[c] - coeff * s1[c] * s2[c])) * 2 / used : 0;
        chans += (c ? "," : "") + J().num("rms_dbfs", dbfs(used ? std::sqrt(sq[c] / used) : 0)).num("peak_dbfs", dbfs(pk[c])).num("tone_dbfs", dbfs(tone)).done();
    }
    outLine(J().str("label", a.get("label")).i("frames", frames).num("seconds", (double)frames / rate).i("trailing_bytes", carry).raw("channels", chans + "]").done());
    return 0;
}

static int cmdLevel(const Args& a) {
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    Stream s;
    if (!openStream(d.Get(), Kind::Loopback, 48000, 2, -1, s)) return 1;
    Ana x;
    analyze(s, a.getd("seconds", 2), a.getd("freq", 997), x);
    outLine(J().str("label", a.get("label")).i("stream_channels", s.channels).raw("loopback", anaJson(x)).done());
    return 0;
}

static int cmdTone(const Args& a) {
    g_forceSrc = a.has("force-src");
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    Stream s;
    if (!openStream(d.Get(), Kind::Render, 48000, 2, -1, s, AUDCLNT_STREAMFLAGS_NOPERSIST)) return 1;
    ComPtr<ISimpleAudioVolume> sv;
    s.c->GetService(IID_PPV_ARGS(&sv));
    if (sv && a.has("session-mute")) sv->SetMute(TRUE, nullptr);
    if (sv && a.has("session-volume")) sv->SetMasterVolume((float)a.getd("session-volume", 1), nullptr);
    double freq = a.getd("freq", 997), amp = a.getd("amp", 0.1), seconds = a.getd("seconds", 10), ph = 0, dph = 2 * kPi * freq / s.rate;
    uint64_t fr = 0, fade = s.rate / 20;
    ProAudioThread pro;
    s.c->Start();
    double t0 = nowSec();
    while (nowSec() - t0 < seconds) {
        WaitForSingleObject(s.ev, 100);
        UINT32 pad = 0;
        if (FAILED(s.c->GetCurrentPadding(&pad))) break;
        UINT32 n = s.bufferFrames - pad; BYTE* p;
        if (!n || FAILED(s.ren->GetBuffer(n, &p))) continue;
        float* x = reinterpret_cast<float*>(p);
        for (UINT32 i = 0; i < n; i++, fr++) {
            float v = (float)(amp * std::min(1.0, (double)fr / fade) * std::sin(ph));
            ph += dph; if (ph > 2 * kPi) ph -= 2 * kPi;
            for (int c = 0; c < s.channels; c++) x[i * s.channels + c] = c < 2 ? v : 0.f;
        }
        s.ren->ReleaseBuffer(n, 0);
    }
    s.c->Stop();
    return 0;
}

// ---------------------------------------------------------------- measure: mute matrix

struct Scenario {
    const char* name; const char* capture;
    bool endpointMute; double endpointVol; bool sessionMute; double sessionVol;
};

static int cmdMuteMatrix(const Args& a) {
    auto d = resolveDevice(eRender, a.get("device"));
    auto mic = resolveDevice(eCapture, a.get("mic"));
    if (!d) return 2;
    string id = deviceId(d.Get());
    double amp = a.getd("amp", 0.2), freq = 997, secs = a.getd("seconds", 3);
    static const Scenario all[] = {
        {"baseline",               "endpoint",          false, -1, false, -1},
        {"endpoint_mute",          "endpoint",          true,  -1, false, -1},
        {"endpoint_vol50",         "endpoint",          false, .5, false, -1},
        {"endpoint_vol0",          "endpoint",          false, 0., false, -1},
        {"session_mute",           "endpoint",          false, -1, true,  -1},
        {"session_vol0",           "endpoint",          false, -1, false, 0.},
        {"proc_incl_baseline",     "proc_include",      false, -1, false, -1},
        {"proc_incl_endpoint_mute","proc_include",      true,  -1, false, -1},
        {"proc_incl_endpoint_vol0","proc_include",      false, 0., false, -1},
        {"proc_incl_session_mute", "proc_include",      false, -1, true,  -1},
        {"proc_incl_session_vol0", "proc_include",      false, -1, false, 0.},
        {"proc_excl_other_baseline","proc_exclude_other", false, -1, false, -1},
        {"proc_excl_other_mute",   "proc_exclude_other", true,  -1, false, -1},
        {"proc_excl_other_vol0",   "proc_exclude_other", false, 0., false, -1},
        {"proc_excl_other_sess_mute","proc_exclude_other",false, -1, true,  -1},
        {"proc_excl_tone",         "proc_exclude_tone", false, -1, false, -1},
    };
    int fails = 0;
    const int chs = mixChannels(d.Get(), 2);
    {
        EndpointStateGuard guard(d.Get());
        if (!guard.ok()) { logj(J().str("level", "error").str("what", "cannot read endpoint volume")); return 1; }
        BOOL mute0 = FALSE; float vol0 = 0;
        guard.vol()->GetMute(&mute0); guard.vol()->GetMasterVolumeLevelScalar(&vol0);
        DWORD hw = 0; guard.vol()->QueryHardwareSupport(&hw);
        outLine(J().str("event", "matrix_start").str("device", deviceName(d.Get())).b("mute", mute0 != FALSE).num("scalar", vol0).i("hw_support", hw).num("tone_dbfs_sent", dbfs(amp)).done());
        ComPtr<IAudioMeterInformation> meter;
        d->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, &meter);
        for (const auto& sc : all) {
            if (sc.endpointMute) guard.vol()->SetMute(TRUE, nullptr);
            if (sc.endpointVol >= 0) guard.vol()->SetMasterVolumeLevelScalar((float)sc.endpointVol, nullptr);
            wstring args = L"measure tone --device \"" + wide(id) + L"\" --amp " + std::to_wstring(amp) + L" --seconds " + std::to_wstring((int)secs + 4);
            if (sc.sessionMute) args += L" --session-mute";
            if (sc.sessionVol >= 0) args += L" --session-volume " + std::to_wstring(sc.sessionVol);
            DWORD tonePid = 0;
            HANDLE tone = spawnSelf(args, &tonePid);
            Sleep(500);
            Ana cap, micA;
            string capPath;
            std::atomic<bool> capDone{false};
            std::thread t1([&] {
                ComThread com;
                Stream s; bool ok;
                string c = sc.capture;
                if (c == "endpoint") ok = openStream(d.Get(), Kind::Loopback, 48000, 2, -1, s);
                else if (c == "proc_include") ok = openProcessLoopback(tonePid, true, 48000, chs, s);
                // The tone is our child, so excluding ourselves would exclude it: exclude the guard instead.
                else if (c == "proc_exclude_other") ok = openProcessLoopback(guard.guardPid(), false, 48000, chs, s);
                else ok = openProcessLoopback(tonePid, false, 48000, chs, s);
                if (ok) { capPath = s.path; analyze(s, secs, freq, cap); }
                capDone = true;
            });
            std::thread t2([&] {
                ComThread com;
                Stream s;
                if (mic && openStream(mic.Get(), Kind::Capture, 48000, 1, -1, s)) analyze(s, secs, freq, micA);
            });
            double meterPeak = 0;
            while (!capDone) {
                float p = 0;
                if (meter && SUCCEEDED(meter->GetPeakValue(&p))) meterPeak = std::max(meterPeak, (double)p);
                Sleep(20);
            }
            t1.join(); t2.join();
            if (tone) { TerminateProcess(tone, 0); CloseHandle(tone); }
            guard.vol()->SetMute(mute0, nullptr);
            guard.vol()->SetMasterVolumeLevelScalar(vol0, nullptr);
            if (!cap.opened) fails++;
            outLine(J().str("scenario", sc.name).str("capture_path", capPath).raw("capture", anaJson(cap)).raw("mic", anaJson(micA))
                .num("endpoint_meter_peak_dbfs", dbfs(meterPeak)).done());
            Sleep(200);
        }
    }
    return fails ? 1 : 0;
}

// ---------------------------------------------------------------- measure: latency

static void percentileReport(const char* label, vector<double> v, J& j) {
    if (v.empty()) { j.raw(label, "null"); return; }
    std::sort(v.begin(), v.end());
    auto pct = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * v.size()))]; };
    j.raw(label, J().i("n", v.size()).num("min", v.front()).num("median", pct(0.5)).num("p95", pct(0.95)).num("max", v.back()).done());
}

// In-process: writes clicks to a render stream and times their appearance in a loopback stream.
static int cmdLatency(const Args& a) {
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    int clicks = a.geti("clicks", 40);
    double periodMs = a.getd("period-ms", 0), interval = a.getd("interval-ms", 250), amp = a.getd("amp", 0.15);
    Stream r, l;
    if (!openStream(d.Get(), Kind::Render, 48000, 2, periodMs, r)) return 1;
    if (!openStream(d.Get(), Kind::Loopback, 48000, 2, -1, l)) return 1;
    outLine(J().str("event", "latency_start").str("device", deviceName(d.Get())).raw("render", r.describe()).raw("loopback", l.describe()).done());

    std::mutex m; std::deque<uint64_t> pending;
    vector<double> toApp, toTs, age;
    std::atomic<bool> done{false};
    CpuMeter cpu;
    std::thread rt([&] {
        ComThread com; ProAudioThread pro;
        UINT32 target = std::min(r.bufferFrames, (UINT32)(r.periodFrames * a.getd("pad-periods", 2)));
        uint64_t pos = 0, next = (uint64_t)r.rate / 2, step = (uint64_t)(interval * r.rate / 1000);
        int sent = 0;
        r.c->Start();
        while (!done) {
            WaitForSingleObject(r.ev, 100);
            UINT32 pad = 0;
            if (FAILED(r.c->GetCurrentPadding(&pad)) || pad >= target) continue;
            UINT32 n = target - pad; BYTE* p;
            if (FAILED(r.ren->GetBuffer(n, &p))) continue;
            float* x = reinterpret_cast<float*>(p);
            memset(x, 0, n * r.channels * sizeof(float));
            bool clicked = false;
            if (sent < clicks && next >= pos && next < pos + n) {
                UINT32 off = (UINT32)(next - pos);
                UINT32 len = std::min<UINT32>(24, n - off);
                for (UINT32 i = 0; i < len; i++) for (int c = 0; c < std::min(2, r.channels); c++) x[(off + i) * r.channels + c] = (float)amp;
                clicked = true; sent++; next += step;
            }
            uint64_t t = now100ns();
            if (clicked) { std::lock_guard<std::mutex> g(m); pending.push_back(t); }
            r.ren->ReleaseBuffer(n, 0);
            pos += n;
        }
        r.c->Stop();
    });
    ProAudioThread pro;
    l.c->Start();
    double t0 = nowSec(), limit = 1.0 + (clicks + 4) * interval / 1000;
    uint64_t lastHit = 0;
    double thr = amp / 3;
    while (nowSec() - t0 < limit && (int)toApp.size() < clicks) {
        WaitForSingleObject(l.ev, 50);
        UINT32 pk = 0;
        while (SUCCEEDED(l.cap->GetNextPacketSize(&pk)) && pk) {
            BYTE* data; UINT32 fr; DWORD flags; UINT64 devpos, qpc;
            if (FAILED(l.cap->GetBuffer(&data, &fr, &flags, &devpos, &qpc))) break;
            uint64_t tApp = now100ns();
            age.push_back(((double)tApp - (double)qpc) / 1e4);
            if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* x = reinterpret_cast<const float*>(data);
                for (UINT32 i = 0; i < fr; i++) {
                    if (std::abs(x[i * l.channels]) < thr) continue;
                    uint64_t ts = qpc + (uint64_t)(i * 1e7 / l.rate);
                    if (lastHit && ts - lastHit < (uint64_t)(interval * 5000)) continue;  // same click
                    lastHit = ts;
                    uint64_t sent = 0;
                    { std::lock_guard<std::mutex> g(m); if (!pending.empty()) { sent = pending.front(); pending.pop_front(); } }
                    if (sent) { toApp.push_back((tApp - (double)sent) / 1e4); toTs.push_back(((double)ts - (double)sent) / 1e4); }
                    break;
                }
            }
            l.cap->ReleaseBuffer(fr);
        }
    }
    done = true;
    rt.join();
    l.c->Stop();
    J j; j.str("event", "latency_result").i("clicks_sent", clicks).i("clicks_seen", toApp.size());
    percentileReport("write_to_app_ms", toApp, j);
    percentileReport("write_to_loopback_timestamp_ms", toTs, j);
    percentileReport("loopback_packet_age_ms", age, j);
    j.num("cpu_pct_of_core", cpu.pct());
    outLine(j.done());
    return toApp.empty() ? 1 : 0;
}

// Through the real tools: f32le into `play` on a pipe, out of `capture-system` on a pipe.
static HANDLE spawnPiped(const wstring& args, HANDLE in, HANDLE out) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wstring cmd = L"\"" + wstring(exe) + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in ? in : GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out ? out : CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    SetHandleInformation(si.hStdInput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdOutput, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(si.hStdError, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        logj(J().str("level", "error").str("what", "CreateProcess").i("gle", GetLastError()));
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

static int cmdLatencyStdio(const Args& a) {
    auto d = resolveDevice(eRender, a.get("device"));
    if (!d) return 2;
    wstring id = wide(deviceId(d.Get()));
    int clicks = a.geti("clicks", 40), rate = 48000, ch = 2;
    double interval = a.getd("interval-ms", 250), amp = a.getd("amp", 0.15), chunkMs = a.getd("chunk-ms", 5);
    int queue = a.geti("queue-ms", 40);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, FALSE};
    HANDLE playR, playW, capR, capW;
    CreatePipe(&playR, &playW, &sa, 4096);
    CreatePipe(&capR, &capW, &sa, 4096);
    HANDLE hPlay = spawnPiped(L"play --device \"" + id + L"\" --rate 48000 --channels 2 --queue-ms " + std::to_wstring(queue) + L" --live-drop-ms " + std::to_wstring(queue) + L" --pad-periods " + std::to_wstring(a.getd("pad-periods", 2)), playR, nullptr);
    HANDLE hCap = spawnPiped(L"capture-system --device \"" + id + L"\" --rate 48000 --channels 2", nullptr, capW);
    CloseHandle(playR); CloseHandle(capW);
    if (!hPlay || !hCap) return 1;
    std::mutex m; std::deque<uint64_t> pending;
    vector<double> deltas;
    std::atomic<bool> done{false};
    std::thread reader([&] {
        vector<float> buf(4800 * ch);
        uint64_t lastHit = 0;
        double thr = amp / 3;
        while (!done) {
            DWORD n = 0;
            if (!ReadFile(capR, buf.data(), (DWORD)(buf.size() * sizeof(float)), &n, nullptr) || !n) break;
            uint64_t t = now100ns();
            size_t frames = n / (ch * sizeof(float));
            for (size_t i = 0; i < frames; i++) {
                if (std::abs(buf[i * ch]) < thr) continue;
                if (lastHit && t - lastHit < (uint64_t)(interval * 5000)) break;
                lastHit = t;
                uint64_t sent = 0;
                { std::lock_guard<std::mutex> g(m); if (!pending.empty()) { sent = pending.front(); pending.pop_front(); } }
                if (sent) deltas.push_back(((double)t - (double)sent) / 1e4);
                break;
            }
        }
    });
    Sleep(1000);  // let both children come up
    {
        std::lock_guard<std::mutex> g(m); pending.clear();
    }
    timeBeginPeriod(1);
    size_t chunkFrames = (size_t)(chunkMs * rate / 1000);
    vector<float> chunk(chunkFrames * ch);
    uint64_t pos = 0, next = rate / 2, step = (uint64_t)(interval * rate / 1000);
    int sent = 0;
    double t0 = nowSec();
    while (sent < clicks || nowSec() - t0 < (clicks * interval / 1000 + 1.0)) {
        std::fill(chunk.begin(), chunk.end(), 0.f);
        bool clicked = false;
        if (sent < clicks && next >= pos && next < pos + chunkFrames) {
            size_t off = (size_t)(next - pos), len = std::min<size_t>(24, chunkFrames - off);
            for (size_t i = 0; i < len; i++) for (int c = 0; c < ch; c++) chunk[(off + i) * ch + c] = (float)amp;
            clicked = true; sent++; next += step;
        }
        if (clicked) { std::lock_guard<std::mutex> g(m); pending.push_back(now100ns()); }
        DWORD w;
        if (!WriteFile(playW, chunk.data(), (DWORD)(chunk.size() * sizeof(float)), &w, nullptr)) break;
        pos += chunkFrames;
        double due = t0 + (double)pos / rate;
        while (nowSec() < due) Sleep(1);
        if (nowSec() - t0 > 2 * clicks * interval / 1000 + 10) break;
    }
    timeEndPeriod(1);
    Sleep(500);
    done = true;
    CloseHandle(playW);                 // play sees EOF and drains
    WaitForSingleObject(hPlay, 3000);
    CancelSynchronousIo(reader.native_handle());
    CloseHandle(capR);                  // capture-system's next write fails: it stops by itself
    bool capExited = WaitForSingleObject(hCap, 3000) == WAIT_OBJECT_0;
    if (!capExited) TerminateProcess(hCap, 9);
    reader.join();
    J j; j.str("event", "latency_stdio_result").i("clicks_sent", sent).i("clicks_seen", deltas.size()).num("chunk_ms", chunkMs).i("queue_ms", queue).b("capture_stopped_on_broken_pipe", capExited);
    percentileReport("stdin_to_stdout_ms", deltas, j);
    outLine(j.done());
    return deltas.empty() ? 1 : 0;
}

// ---------------------------------------------------------------- measure: periods

static int cmdPeriods(const Args& a) {
    auto devRender = resolveDevice(eRender, a.get("render"));
    auto devMic = resolveDevice(eCapture, a.get("mic"));
    double seconds = a.getd("seconds", 3);
    struct T { const char* label; IMMDevice* d; Kind k; double p; };
    vector<T> tests = {
        {"render_default", devRender.Get(), Kind::Render, -1},
        {"render_lowest", devRender.Get(), Kind::Render, 0},
        {"mic_default", devMic.Get(), Kind::Capture, -1},
        {"mic_lowest", devMic.Get(), Kind::Capture, 0},
        {"loopback_default", devRender.Get(), Kind::Loopback, -1},
    };
    for (auto& t : tests) {
        if (!t.d) continue;
        Stream s;
        // Mic is measured at its own mix format so that no SRC hides the period granted.
        ComPtr<IAudioClient> probe; int r = 48000, c = 2;
        if (SUCCEEDED(t.d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe))) {
            WAVEFORMATEX* m = nullptr;
            if (SUCCEEDED(probe->GetMixFormat(&m))) { r = m->nSamplesPerSec; c = m->nChannels; CoTaskMemFree(m); }
        }
        if (!openStream(t.d, t.k, r, c, t.p, s)) { outLine(J().str("test", t.label).b("opened", false).done()); continue; }
        std::unique_ptr<KeepAlive> ka;
        if (t.k == Kind::Loopback) ka = std::make_unique<KeepAlive>(deviceId(t.d));
        ProAudioThread pro;
        CpuMeter cpu;
        s.c->Start();
        double t0 = nowSec(), last = 0, sum = 0, sum2 = 0, mx = 0, ageSum = 0, ageMax = -1e9, drained = 0; uint64_t n = 0, pkts = 0, frames = 0;
        while (nowSec() - t0 < seconds) {
            if (WaitForSingleObject(s.ev, 200) != WAIT_OBJECT_0) continue;
            double t1 = nowSec();
            if (last && t1 - t0 > 0.3) { double d = (t1 - last) * 1000; sum += d; sum2 += d * d; mx = std::max(mx, d); n++; }
            last = t1;
            if (t.k == Kind::Render) {
                UINT32 pad = 0; s.c->GetCurrentPadding(&pad);
                UINT32 fr = s.bufferFrames - pad; BYTE* p;
                if (fr && SUCCEEDED(s.ren->GetBuffer(fr, &p))) { s.ren->ReleaseBuffer(fr, AUDCLNT_BUFFERFLAGS_SILENT); frames += fr; }
            } else {
                UINT32 pk = 0;
                while (SUCCEEDED(s.cap->GetNextPacketSize(&pk)) && pk) {
                    BYTE* data; UINT32 fr; DWORD fl; UINT64 pos, qpc;
                    if (FAILED(s.cap->GetBuffer(&data, &fr, &fl, &pos, &qpc))) break;
                    uint64_t tn = now100ns();
                    double age = ((double)tn - (double)qpc) / 1e4;
                    ageSum += age; ageMax = std::max(ageMax, age); pkts++; frames += fr; drained += fr;
                    s.cap->ReleaseBuffer(fr);
                }
            }
        }
        s.c->Stop();
        double mean = n ? sum / n : 0, sd = n ? std::sqrt(std::max(0.0, sum2 / n - mean * mean)) : 0;
        J j;
        j.str("test", t.label).str("device", deviceName(t.d)).b("opened", true).raw("stream", s.describe())
            .num("event_interval_mean_ms", mean).num("event_interval_sd_ms", sd).num("event_interval_max_ms", mx)
            .num("frames_per_s", frames / seconds).num("cpu_pct_of_core", cpu.pct());
        if (pkts) j.num("packet_age_mean_ms", ageSum / pkts).num("packet_age_max_ms", ageMax).num("packet_frames_mean", drained / pkts);
        outLine(j.done());
    }
    return 0;
}

// ---------------------------------------------------------------- measure: drift

struct Fit {
    double sx = 0, sy = 0, sxx = 0, sxy = 0, n = 0, x0 = 0, y0 = 0; bool init = false;
    void add(double x, double y) {
        if (!init) { x0 = x; y0 = y; init = true; }
        x -= x0; y -= y0;
        sx += x; sy += y; sxx += x * x; sxy += x * y; n += 1;
    }
    double slope() const { double d = n * sxx - sx * sx; return d > 0 ? (n * sxy - sx * sy) / d : 0; }
};
struct DriftTrack {
    std::mutex m; Fit fit; double lastX = 0, lastY = 0, winX = 0, winY = 0;
    uint64_t frames = 0, disc = 0;
    void add(double x, double y) {
        std::lock_guard<std::mutex> g(m);
        fit.add(x, y); lastX = x; lastY = y;
        if (!winX) { winX = x; winY = y; }
    }
    // Seconds of device time per second of QPC, minus one, in ppm.
    double ppm() { std::lock_guard<std::mutex> g(m); return (fit.slope() - 1.0) * 1e6; }
    double windowPpm() {
        std::lock_guard<std::mutex> g(m);
        double r = lastX > winX ? ((lastY - winY) / (lastX - winX) - 1.0) * 1e6 : 0;
        winX = lastX; winY = lastY;
        return r;
    }
};

static int cmdDrift(const Args& a) {
    auto devRender = resolveDevice(eRender, a.get("render"));
    auto devMic = resolveDevice(eCapture, a.get("mic"));
    if (!devRender || !devMic) return 2;
    double seconds = a.getd("seconds", 300), report = a.getd("report-s", 30);
    DriftTrack micT, renT, lbT;
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};
    auto captureThread = [&](IMMDevice* d, Kind k, DriftTrack& tr) {
        ComThread com; ProAudioThread pro;
        Stream s;
        ComPtr<IAudioClient> probe; int r = 48000, c = 2;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe))) {
            WAVEFORMATEX* m = nullptr;
            if (SUCCEEDED(probe->GetMixFormat(&m))) { r = m->nSamplesPerSec; c = m->nChannels; CoTaskMemFree(m); }
        }
        if (!openStream(d, k, r, c, -1, s)) { errors++; return; }
        s.c->Start();
        while (!done) {
            WaitForSingleObject(s.ev, 100);
            UINT32 pk = 0;
            while (SUCCEEDED(s.cap->GetNextPacketSize(&pk)) && pk) {
                BYTE* data; UINT32 fr; DWORD fl; UINT64 pos, qpc;
                if (FAILED(s.cap->GetBuffer(&data, &fr, &fl, &pos, &qpc))) break;
                tr.frames += fr;
                if (fl & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) tr.disc++;
                tr.add(qpc / 1e7, (double)pos / r);
                s.cap->ReleaseBuffer(fr);
            }
        }
        s.c->Stop();
    };
    std::thread tMic(captureThread, devMic.Get(), Kind::Capture, std::ref(micT));
    std::thread tLb(captureThread, devRender.Get(), Kind::Loopback, std::ref(lbT));
    std::thread tRen([&] {
        ComThread com; ProAudioThread pro;
        Stream s;
        if (!openStream(devRender.Get(), Kind::Render, 48000, 2, -1, s) || !s.clk) { errors++; return; }
        UINT64 freq = 0; s.clk->GetFrequency(&freq);
        s.c->Start();
        double nextPos = nowSec();
        while (!done) {
            WaitForSingleObject(s.ev, 100);
            UINT32 pad = 0;
            if (FAILED(s.c->GetCurrentPadding(&pad))) break;
            UINT32 n = s.bufferFrames - pad; BYTE* p;
            if (n && SUCCEEDED(s.ren->GetBuffer(n, &p))) s.ren->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
            double t = nowSec();
            if (t >= nextPos) {
                nextPos = t + 0.1;
                UINT64 pos, qpc;
                if (SUCCEEDED(s.clk->GetPosition(&pos, &qpc)) && freq) { renT.add(qpc / 1e7, (double)pos / freq); renT.frames = pos; }
            }
        }
        s.c->Stop();
    });
    CpuMeter cpu;
    double t0 = nowSec(), nextReport = t0 + report;
    outLine(J().str("event", "drift_start").str("mic", deviceName(devMic.Get())).str("render", deviceName(devRender.Get())).num("seconds", seconds).done());
    while (!g_stop && nowSec() - t0 < seconds) {
        Sleep(200);
        if (nowSec() >= nextReport) {
            nextReport += report;
            double m = micT.ppm(), r = renT.ppm(), l = lbT.ppm();
            outLine(J().str("event", "drift").num("elapsed_s", nowSec() - t0).num("mic_ppm", m).num("render_ppm", r).num("loopback_ppm", l)
                .num("mic_minus_render_ppm", m - r).num("mic_window_ppm", micT.windowPpm()).num("render_window_ppm", renT.windowPpm()).num("loopback_window_ppm", lbT.windowPpm())
                .i("mic_discontinuities", micT.disc).i("loopback_discontinuities", lbT.disc).done());
        }
    }
    done = true;
    tMic.join(); tLb.join(); tRen.join();
    double el = nowSec() - t0, m = micT.ppm(), r = renT.ppm(), l = lbT.ppm();
    outLine(J().str("event", "drift_result").num("elapsed_s", el).num("mic_ppm", m).num("render_ppm", r).num("loopback_ppm", l).num("mic_minus_render_ppm", m - r)
        .num("mic_vs_render_ms_per_hour", (m - r) * 3.6).i("mic_frames", micT.frames).i("mic_discontinuities", micT.disc).i("loopback_discontinuities", lbT.disc)
        .num("cpu_pct_of_core", cpu.pct()).i("stream_errors", errors.load()).done());
    return errors ? 1 : 0;
}

// ---------------------------------------------------------------- measure: default swap
// Undocumented IPolicyConfig, used only to exercise `watch`: flips one role and puts it back.

MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID CLSID_PolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

static int cmdDefaultSwap(const Args& a) {
    auto e = enumerator();
    if (!e) return 1;
    wstring to = wide(a.get("to"));
    string from = defaultId(e.Get(), eRender, eCommunications);
    if (to.empty() || from.empty()) { logj(J().str("level", "error").str("what", "need --to ID and a current default")); return 2; }
    ComPtr<IPolicyConfig> pc;
    HRESULT hr = CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), &pc);
    if (FAILED(hr)) { logErr("PolicyConfigClient", hr); return 1; }
    hr = pc->SetDefaultEndpoint(to.c_str(), eCommunications);
    logj(J().str("event", "default_set").str("to", utf8(to)).hr("hr", hr));
    Sleep(1500);
    HRESULT back = pc->SetDefaultEndpoint(wide(from).c_str(), eCommunications);
    string now = defaultId(e.Get(), eRender, eCommunications);
    logj(J().str("event", "default_restored").str("to", from).hr("hr", back).b("verified", now == from));
    return now == from ? 0 : 1;
}

// ---------------------------------------------------------------- main

static void usage() {
    writeLine(STD_ERROR_HANDLE,
              "beamer-audio list [--json] [--all]\n"
              "beamer-audio watch [--seconds S]\n"
              "beamer-audio capture-mic [--device ID|NAME] [--rate 48000] [--channels 1] [--period-ms P] [--seconds S]\n"
              "beamer-audio capture-system [--device ID|NAME] [--mute-local] [--rate 48000] [--channels 2] [--seconds S] [--no-keepalive]\n"
              "                            [--process-loopback include|exclude --pid PID] [--exclude-pid PID]\n"
              "beamer-audio play [--device ID|NAME] [--rate 48000] [--channels 2] [--period-ms P] [--queue-ms 40] [--live-drop-ms N] [--seconds S]\n"
              "beamer-audio measure periods|mute-matrix|latency|latency-stdio|glitch|drift|level|tone|gen|pcm-stats|burn|default-swap [...]\n"
              "beamer-audio restore-endpoint");
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 2; }
    string cmd = utf8(argv[1]);
    string sub = (cmd == "measure" && argc >= 3) ? utf8(argv[2]) : "";
    Args a = parseArgs(argc, argv, 2);
    ComThread com;
    SetConsoleCtrlHandler(ctrlHandler, TRUE);
    if (cmd == "list") return cmdList(a);
    if (cmd == "watch") return cmdWatch(a);
    if (cmd == "capture-mic") return cmdCaptureMic(a);
    if (cmd == "capture-system") return cmdCaptureSystem(a);
    if (cmd == "play") return cmdPlay(a);
    if (cmd == "_guard") return cmdGuard(a);
    if (cmd == "restore-endpoint") return restoreFromJournal() ? 0 : 1;
    if (cmd == "measure") {
        if (sub == "periods") return cmdPeriods(a);
        if (sub == "mute-matrix") return cmdMuteMatrix(a);
        if (sub == "latency") return cmdLatency(a);
        if (sub == "latency-stdio") return cmdLatencyStdio(a);
        if (sub == "drift") return cmdDrift(a);
        if (sub == "tone") return cmdTone(a);
        if (sub == "level") return cmdLevel(a);
        if (sub == "glitch") return cmdGlitch(a);
        if (sub == "gen") return cmdGen(a);
        if (sub == "burn") return cmdBurn(a);
        if (sub == "pcm-stats") return cmdPcmStats(a);
        if (sub == "default-swap") return cmdDefaultSwap(a);
    }
    usage();
    return 2;
}
