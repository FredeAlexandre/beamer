// SPDX-License-Identifier: GPL-3.0-only
// Windows backend: WASAPI client setup (endpoint render/capture/loopback, process loopback), keep-alive,
// channel conversion. Behaviour from dev/audio/proto/win (Phase 1): fold-down of surround mixes is ours
// (WASAPI's 7.1->stereo matrix costs 13.5 dB), render conversion is WASAPI's (direct stereo into the G733's
// 8-channel mix came out 3.98 dB low).
#include "backend_win.h"

#include <audioclientactivationparams.h>
#include <ksmedia.h>
#include <mmreg.h>

#include <algorithm>

#pragma comment(lib, "mmdevapi.lib")

namespace beamer::audio::win {

Client::~Client() {
    if (client) client->Stop();
    capture.Reset();
    render.Reset();
    client.Reset();
    if (event) CloseHandle(event);
}

static bool is_float32(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return false;
}

static WAVEFORMATEXTENSIBLE float_format(int channels, DWORD mask) {
    WAVEFORMATEXTENSIBLE x{};
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = (WORD)channels;
    x.Format.nSamplesPerSec = kRate;
    x.Format.wBitsPerSample = 32;
    x.Format.nBlockAlign = (WORD)(channels * 4);
    x.Format.nAvgBytesPerSec = kRate * channels * 4;
    x.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    x.Samples.wValidBitsPerSample = 32;
    const DWORD stereo = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    const DWORD five1 = stereo | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    x.dwChannelMask = mask ? mask
                    : channels == 1 ? SPEAKER_FRONT_CENTER
                    : channels == 2 ? stereo
                    : channels == 4 ? (stereo | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                    : channels == 6 ? five1
                    : channels == 8 ? (five1 | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT) : 0;
    x.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return x;
}

static HRESULT finish(Client* c, ClientKind kind) {
    HRESULT hr;
    c->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr = c->client->SetEventHandle(c->event))) return hr;
    if (FAILED(hr = c->client->GetBufferSize(&c->buffer_frames))) return hr;
    if (kind == ClientKind::Render) hr = c->client->GetService(IID_PPV_ARGS(&c->render));
    else hr = c->client->GetService(IID_PPV_ARGS(&c->capture));
    return hr;
}

HRESULT open_endpoint_client(IMMDevice* d, ClientKind kind, int channels, Client* c) {
    HRESULT hr;
    ComPtr<IAudioClient> ac;
    if (FAILED(hr = d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac))) return hr;
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = ac->GetMixFormat(&mix))) return hr;
    const int mix_ch = mix->nChannels;
    const DWORD mix_mask = mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE ? reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->dwChannelMask : 0;
    const bool mix_ok = (int)mix->nSamplesPerSec == kRate && is_float32(mix);
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (kind == ClientKind::EndpointLoopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    WAVEFORMATEXTENSIBLE want{};
    const WAVEFORMATEX* fmt = mix;
    int stream_ch = mix_ch;
    if (kind == ClientKind::Render) {
        if (!(mix_ok && mix_ch == channels)) {
            want = float_format(channels, 0);
            fmt = &want.Format;
            stream_ch = channels;
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            c->wasapi_convert = true;
        }
    } else if (!mix_ok) {  // keep the mix's channels (our fold-down), let WASAPI convert rate/sample format only
        want = float_format(mix_ch, mix_mask);
        fmt = &want.Format;
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        c->wasapi_convert = true;
    }
    hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, fmt, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) return hr;
    c->client = ac;
    c->channels = stream_ch;
    c->path = std::string(kind == ClientKind::Render ? "render" : kind == ClientKind::Capture ? "capture" : "endpoint-loopback") +
              (c->wasapi_convert ? "+autoconvert" : "");
    return finish(c, kind);
}

// ------------------------------------------------------------------ process loopback

namespace {
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
}  // namespace

HRESULT open_process_loopback(DWORD pid, bool include_tree, int channels, Client* c) {
    AUDIOCLIENT_ACTIVATION_PARAMS ap{};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode =
        include_tree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);
    auto* h = new ActivateHandler();
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
    if (FAILED(hr)) { h->Release(); return hr; }
    DWORD w = WaitForSingleObject(h->done, 5000);
    h->Release();
    if (w != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    HRESULT ar = E_FAIL;
    ComPtr<IUnknown> unk;
    hr = op->GetActivateResult(&ar, &unk);
    if (FAILED(hr)) return hr;
    if (FAILED(ar)) return ar;
    if (FAILED(hr = unk.As(&c->client))) return hr;
    WAVEFORMATEXTENSIBLE want = float_format(channels, 0);
    hr = c->client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                               200000, 0, &want.Format, nullptr);
    if (FAILED(hr)) return hr;
    c->channels = channels;
    c->path = include_tree ? "process-loopback(include)" : "process-loopback(exclude)";
    return finish(c, ClientKind::ProcessLoopback);
}

// ------------------------------------------------------------------ channels

void convert_channels(const float* in, int ic, float* out, int oc, size_t frames) {
    if (ic == oc) { memcpy(out, in, frames * ic * sizeof(float)); return; }
    if (ic > 2 && oc <= 2) {
        // Layouts: 4 = FL FR BL BR, 6 = FL FR FC LFE SL SR, 8 = FL FR FC LFE BL BR SL SR. LFE is dropped.
        const float k = 0.7071f;
        for (size_t f = 0; f < frames; f++) {
            const float* x = in + f * ic;
            float l = x[0], r = x[1];
            if (ic >= 6) { l += k * x[2]; r += k * x[2]; }
            for (int ch = ic >= 6 ? 4 : 2; ch + 1 < ic; ch += 2) { l += k * x[ch]; r += k * x[ch + 1]; }
            l = std::clamp(l, -1.f, 1.f);
            r = std::clamp(r, -1.f, 1.f);
            if (oc == 1) out[f] = 0.5f * (l + r);
            else { out[f * 2] = l; out[f * 2 + 1] = r; }
        }
        return;
    }
    if (oc == 1) {
        for (size_t f = 0; f < frames; f++) {
            float s = 0;
            for (int ch = 0; ch < ic; ch++) s += in[f * ic + ch];
            out[f] = s / ic;
        }
        return;
    }
    if (ic == 1) {
        for (size_t f = 0; f < frames; f++)
            for (int ch = 0; ch < oc; ch++) out[f * oc + ch] = in[f];
        return;
    }
    for (size_t f = 0; f < frames; f++)
        for (int ch = 0; ch < oc; ch++) out[f * oc + ch] = ch < ic ? in[f * ic + ch] : 0.f;
}

// ------------------------------------------------------------------ keep-alive

KeepAlive::KeepAlive(std::string device_id) : id_(std::move(device_id)) {
    thread_ = CreateThread(nullptr, 0, &KeepAlive::run, this, 0, nullptr);
}
KeepAlive::~KeepAlive() {
    stop_ = true;
    if (thread_) { WaitForSingleObject(thread_, INFINITE); CloseHandle(thread_); }
}
DWORD WINAPI KeepAlive::run(void* p) {
    auto* self = static_cast<KeepAlive*>(p);
    ComScope com;
    MmcssScope mmcss;
    auto e = make_enumerator();
    auto d = device_by_id(e.Get(), self->id_);
    Client c;
    if (!d || FAILED(open_endpoint_client(d.Get(), ClientKind::Render, 2, &c)) || FAILED(c.client->Start())) {
        BLOG_W("keep-alive: cannot open %s", self->id_.c_str());
        return 0;
    }
    while (!self->stop_) {
        WaitForSingleObject(c.event, 50);
        UINT32 pad = 0;
        if (FAILED(c.client->GetCurrentPadding(&pad))) break;  // invalidated: the owning stream sees it too
        UINT32 n = c.buffer_frames - pad;
        BYTE* buf;
        if (n && SUCCEEDED(c.render->GetBuffer(n, &buf))) c.render->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    return 0;
}

DWORD windows_build() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    static DWORD build = [] {
        OSVERSIONINFOW v{};
        v.dwOSVersionInfoSize = sizeof v;
        auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        if (fn && fn(&v) == 0) return v.dwBuildNumber;
        return (DWORD)0;
    }();
    return build;
}

}  // namespace beamer::audio::win
