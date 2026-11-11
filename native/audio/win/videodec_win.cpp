// SPDX-License-Identifier: GPL-3.0-only
// MF H.264 decoder for the Windows video sink (camera PHASE2 §4.6.1, CW4): CLSID_CMSH264DecoderMFT with
// MF_LOW_LATENCY + CODECAPI_AVLowLatencyMode (mandatory: without it the MFT buffers 15-30 frames, C2 Q5), NV12
// out; hardware = a D3D11 video device given through MFT_MESSAGE_SET_D3D_MANAGER and read back through an own
// staging texture (C2: the fastest path in the user session), software = the same MFT without a device manager.
//
// Display vs coded size (R2 #6): the output type gives the coded size (MF_MT_FRAME_SIZE, e.g. 1920x1088) and the
// display size (MF_MT_MINIMUM_DISPLAY_APERTURE, 1920x1080; the coded size when absent). Every size rule uses the
// display size; the coded height is used only to find the UV plane. Only display rows are ever copied.
#include "video_backend_win.h"

#include <codecapi.h>
#include <mferror.h>
#include <wmcodecdsp.h>

#include <chrono>

namespace beamer::audio::win {

namespace {
constexpr int64_t kFrameTime = 333333;  // sample time per frame number (100 ns units, 30 fps)
}

bool display_size_ok(int w, int h) {
    if (w <= 0 || h <= 0 || (w & 1) || (h & 1)) return false;
    return uint32_t(w) >= vcam::kMinW && uint32_t(w) <= vcam::kMaxW && uint32_t(h) >= vcam::kMinH &&
           uint32_t(h) <= vcam::kMaxH;
}

size_t nv12_bytes_read(size_t pitch, int coded_h, int w, int h) {
    if (w <= 0 || h <= 1 || coded_h < h || pitch < size_t(w)) return SIZE_MAX;
    // last byte copy_nv12_display reads: UV row h/2-1, column w-1
    return pitch * size_t(coded_h) + pitch * size_t(h / 2 - 1) + size_t(w);
}

void copy_nv12_display(const uint8_t* src, size_t pitch, int coded_h, int w, int h, uint8_t* dst, size_t dst_stride) {
    for (int y = 0; y < h; y++) memcpy(dst + size_t(y) * dst_stride, src + size_t(y) * pitch, size_t(w));
    const uint8_t* uv = src + pitch * size_t(coded_h);
    uint8_t* duv = dst + dst_stride * size_t(h);
    for (int y = 0; y < h / 2; y++) memcpy(duv + size_t(y) * dst_stride, uv + size_t(y) * pitch, size_t(w));
}

bool au_has_idr(const uint8_t* p, size_t n) {
    for (size_t i = 0; i + 3 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if ((p[i + 3] & 0x1F) == 5) return true;
            i += 2;
        }
    }
    return false;
}

static double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

std::unique_ptr<H264Decoder> H264Decoder::create(bool hw, std::string* info, HRESULT* out_hr) {
    std::unique_ptr<H264Decoder> d(new H264Decoder());
    auto fail = [&](const char* what, HRESULT hr) -> std::unique_ptr<H264Decoder> {
        if (out_hr) *out_hr = hr;
        if (info) *info = std::string(what) + " " + hr_text(hr);
        return nullptr;
    };
    if (!mf_startup()) return fail("MFStartup", E_FAIL);
    HRESULT hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d->t_));
    if (FAILED(hr)) return fail("CoCreateInstance(H264 MFT)", hr);
    ComPtr<IMFAttributes> a;
    d->t_->GetAttributes(&a);
    if (a) a->SetUINT32(MF_LOW_LATENCY, TRUE);
    ComPtr<ICodecAPI> api;
    hr = d->t_.As(&api);
    if (SUCCEEDED(hr)) {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 1;
        hr = api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
    }
    if (FAILED(hr)) return fail("AVLowLatencyMode", hr);  // mandatory (C2 Q5)
    if (hw) {
        UINT32 aware = 0;
        if (a) a->GetUINT32(MF_SA_D3D11_AWARE, &aware);
        if (!aware) return fail("MF_SA_D3D11_AWARE", E_NOTIMPL);
        D3D_FEATURE_LEVEL fl;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, &d->dev_, &fl, &d->ctx_);
        if (FAILED(hr)) return fail("D3D11CreateDevice", hr);
        ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(d->dev_.As(&mt))) mt->SetMultithreadProtected(TRUE);
        UINT token = 0;
        hr = MFCreateDXGIDeviceManager(&token, &d->mgr_);
        if (FAILED(hr)) return fail("MFCreateDXGIDeviceManager", hr);
        hr = d->mgr_->ResetDevice(d->dev_.Get(), token);
        if (FAILED(hr)) return fail("ResetDevice", hr);
        hr = d->t_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(d->mgr_.Get()));
        if (FAILED(hr)) return fail("SET_D3D_MANAGER", hr);
        d->hw_ = true;
    }
    ComPtr<IMFMediaType> in;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, 1280, 720);  // the SPS decides; a stream change follows
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, 30, 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    hr = d->t_->SetInputType(0, in.Get(), 0);
    if (FAILED(hr)) return fail("SetInputType", hr);
    hr = d->set_output();
    if (FAILED(hr)) return fail("SetOutputType", hr);
    d->t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    d->t_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    d->info_ = hw ? "hw d3d11 lowlatency" : "sw lowlatency";
    if (info) *info = d->info_;
    if (out_hr) *out_hr = S_OK;
    return d;
}

H264Decoder::~H264Decoder() {
    if (t_) {
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        if (hw_) t_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
    }
}

HRESULT H264Decoder::set_output() {
    for (DWORD i = 0;; i++) {
        ComPtr<IMFMediaType> o;
        HRESULT hr = t_->GetOutputAvailableType(0, i, &o);
        if (FAILED(hr)) return hr;
        GUID g{};
        o->GetGUID(MF_MT_SUBTYPE, &g);
        if (g != MFVideoFormat_NV12) continue;
        hr = t_->SetOutputType(0, o.Get(), 0);
        if (FAILED(hr)) return hr;
        UINT32 ow = 0, oh = 0;
        MFGetAttributeSize(o.Get(), MF_MT_FRAME_SIZE, &ow, &oh);
        w_ = coded_w_ = int(ow);
        h_ = coded_h_ = int(oh);
        // Rows of a linear (non-2D) output buffer are the coded width apart, or MF_MT_DEFAULT_STRIDE (R3 #11)
        UINT32 stride = 0;
        linear_pitch_ = SUCCEEDED(o->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && int32_t(stride) >= int32_t(ow) ? size_t(int32_t(stride))
                                                                                                                 : size_t(ow);
        MFVideoArea area{};
        if (SUCCEEDED(o->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof area, nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0) {
            // OffsetX/Y are 0 for every stream we produce (VideoToolbox crops at the bottom/right only).
            w_ = std::min<int>(area.Area.cx, int(ow));
            h_ = std::min<int>(area.Area.cy, int(oh));
        }
        MFT_OUTPUT_STREAM_INFO si{};
        t_->GetOutputStreamInfo(0, &si);
        provides_ = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        out_size_ = si.cbSize;
        staging_.Reset();
        return S_OK;
    }
}

bool H264Decoder::device_lost(HRESULT hr) {
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG ||
        hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)
        return true;
    return hw_ && dev_ && dev_->GetDeviceRemovedReason() != S_OK;
}

DecodeOut H264Decoder::decode(const uint8_t* au, size_t len, uint32_t frame,
                              const std::function<uint8_t*(int, int)>& dst) {
    DecodeOut r;
    auto t0 = std::chrono::steady_clock::now();
    if (fake_removed_) {
        r.result = DecodeResult::kDeviceLost;
        r.hr = DXGI_ERROR_DEVICE_REMOVED;
        return r;
    }
    ComPtr<IMFSample> s;
    ComPtr<IMFMediaBuffer> b;
    HRESULT hr = MFCreateSample(&s);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(DWORD(len), &b);
    if (FAILED(hr)) {
        r.result = DecodeResult::kError;
        r.hr = hr;
        return r;
    }
    BYTE* p = nullptr;
    b->Lock(&p, nullptr, nullptr);
    memcpy(p, au, len);
    b->Unlock();
    b->SetCurrentLength(DWORD(len));
    s->AddBuffer(b.Get());
    s->SetSampleTime(int64_t(frame) * kFrameTime);
    s->SetSampleDuration(kFrameTime);

    bool got = false, rejected = false, short_copy = false;
    auto drain = [&]() -> HRESULT {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER db{};
            ComPtr<IMFSample> mine;
            if (!provides_) {
                ComPtr<IMFMediaBuffer> ob;
                MFCreateSample(&mine);
                MFCreateMemoryBuffer(out_size_ ? out_size_ : DWORD(linear_pitch_ * size_t(coded_h_) * 3 / 2), &ob);
                mine->AddBuffer(ob.Get());
                db.pSample = mine.Get();
            }
            DWORD status = 0;
            HRESULT h2 = t_->ProcessOutput(0, 1, &db, &status);
            if (db.pEvents) db.pEvents->Release();
            if (h2 == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_OK;
            if (h2 == MF_E_TRANSFORM_STREAM_CHANGE) {
                if (db.pSample && provides_) db.pSample->Release();
                h2 = set_output();
                if (FAILED(h2)) return h2;
                r.size_changed = true;
                continue;
            }
            if (FAILED(h2)) return h2;
            ComPtr<IMFSample> os;
            if (provides_) os.Attach(db.pSample);
            else os = mine;
            LONGLONG st = 0;
            os->GetSampleTime(&st);
            uint32_t of = uint32_t((st + kFrameTime / 2) / kFrameTime);
            if (!display_size_ok(w_, h_)) {
                rejected = true;  // never copied, never written to a slot
                continue;
            }
            if (of != frame) continue;  // not ours (cannot happen in low-latency mode; ignored)
            got = true;
            uint8_t* out = dst ? dst(w_, h_) : nullptr;
            if (!out) continue;
            ComPtr<IMFMediaBuffer> ob;
            h2 = os->GetBufferByIndex(0, &ob);
            if (FAILED(h2)) return h2;
            ComPtr<IMFDXGIBuffer> dx;
            if (SUCCEEDED(ob.As(&dx))) {
                ComPtr<ID3D11Texture2D> tex;
                UINT sub = 0;
                h2 = dx->GetResource(IID_PPV_ARGS(&tex));
                if (FAILED(h2)) return h2;
                dx->GetSubresourceIndex(&sub);
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                D3D11_TEXTURE2D_DESC cur{};
                if (staging_) staging_->GetDesc(&cur);
                if (!staging_ || cur.Width != td.Width || cur.Height != td.Height || cur.Format != td.Format) {
                    D3D11_TEXTURE2D_DESC sd = td;
                    sd.ArraySize = 1;
                    sd.MipLevels = 1;
                    sd.Usage = D3D11_USAGE_STAGING;
                    sd.BindFlags = 0;
                    sd.MiscFlags = 0;
                    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    staging_.Reset();
                    h2 = dev_->CreateTexture2D(&sd, nullptr, &staging_);
                    if (FAILED(h2)) return h2;
                }
                ctx_->CopySubresourceRegion(staging_.Get(), 0, 0, 0, 0, tex.Get(), sub, nullptr);
                D3D11_MAPPED_SUBRESOURCE m{};
                h2 = ctx_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &m);
                if (FAILED(h2)) return h2;
                // The texture is the coded picture (td.Height >= coded height): its UV plane follows td.Height rows.
                if (int(td.Width) >= w_ && int(td.Height) >= h_ && m.RowPitch >= UINT(w_))
                    copy_nv12_display(static_cast<const uint8_t*>(m.pData), m.RowPitch, test_uv_at_display_height ? h_ : int(td.Height), w_, h_, out, size_t(w_));
                else
                    short_copy = true;
                ctx_->Unmap(staging_.Get(), 0);
                continue;
            }
            ComPtr<IMF2DBuffer2> b2;
            BYTE *bp = nullptr, *start = nullptr;
            LONG pitch = 0;
            DWORD blen = 0;
            if (SUCCEEDED(ob.As(&b2)) && SUCCEEDED(b2->Lock2DSize(MF2DBuffer_LockFlags_Read, &bp, &pitch, &start, &blen))) {
                // Everything read must lie inside the locked buffer (R3 #11): [bp, start + blen).
                const int ch = test_uv_at_display_height ? h_ : coded_h_;
                const size_t need = pitch > 0 ? nv12_bytes_read(size_t(pitch), ch, w_, h_) : SIZE_MAX;
                const size_t avail = bp >= start && size_t(bp - start) <= blen ? blen - size_t(bp - start) : 0;
                if (need <= avail) copy_nv12_display(bp, size_t(pitch), ch, w_, h_, out, size_t(w_));
                else {
                    short_buffers_++;
                    short_copy = true;
                }
                b2->Unlock2D();
            } else {
                DWORD cur_len = 0;
                if (SUCCEEDED(ob->Lock(&bp, nullptr, &cur_len))) {
                    // A linear buffer: rows are the coded width (or the default stride) apart, not the display width.
                    const int ch = test_uv_at_display_height ? h_ : coded_h_;
                    if (nv12_bytes_read(linear_pitch_, ch, w_, h_) <= cur_len) copy_nv12_display(bp, linear_pitch_, ch, w_, h_, out, size_t(w_));
                    else {
                    short_buffers_++;
                    short_copy = true;
                }
                    ob->Unlock();
                }
            }
        }
    };

    hr = t_->ProcessInput(0, s.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {  // output pending (not expected in low-latency mode): drain, then retry once
        HRESULT h2 = drain();
        if (SUCCEEDED(h2)) hr = t_->ProcessInput(0, s.Get(), 0);
        else hr = h2;
    }
    if (SUCCEEDED(hr)) hr = drain();
    r.w = w_;
    r.h = h_;
    r.coded_h = coded_h_;
    r.hr = hr;
    r.ms = ms_since(t0);
    if (FAILED(hr)) r.result = device_lost(hr) ? DecodeResult::kDeviceLost : DecodeResult::kError;
    else if (rejected) r.result = DecodeResult::kSizeRejected;
    else if (short_copy) {  // the picture was not copied: never shown (a decoder error)
        r.result = DecodeResult::kError;
        r.hr = E_UNEXPECTED;
    }
    else r.result = got ? DecodeResult::kOk : DecodeResult::kNoOutput;
    return r;
}

}  // namespace beamer::audio::win
