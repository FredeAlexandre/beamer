// SPDX-License-Identifier: GPL-3.0-only
// Placeholder picture for the Beamer Camera's slot 3 (camera PHASE2 §4.6.4, CW4): Direct2D + DirectWrite into a
// premultiplied BGRA WIC bitmap at the app's size, converted to NV12 (BT.709, limited range). Beamer's dark surface
// and accent colours (win_app/tokens.py PALETTE, copied as constants), a simple camera mark in `signal`, a title line
// (24 px at 720p) and a detail line (16 px at 720p), scaled with the height. The texts come from Python
// (`vcam placeholders`); the helper never invents wording except the default title "Beamer Camera".
// The DLL draws its own moving dot over slot 3, so nothing here animates.
#include "video_backend_win.h"

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace beamer::audio::win {

namespace {

// win_app/tokens.py PALETTE (dark)
constexpr uint32_t kGround = 0x0e0f0d, kPanel = 0x161714, kRule = 0x2a2b26, kInk = 0xefeee6, kInk2 = 0xb3b2a7,
                   kSignal = 0x5fd4f4, kSignalDim = 0x1f4a55;

D2D1_COLOR_F rgb(uint32_t c, float a = 1.f) {
    return D2D1::ColorF(float((c >> 16) & 255) / 255.f, float((c >> 8) & 255) / 255.f, float(c & 255) / 255.f, a);
}

struct Factories {
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = E_FAIL;
};

Factories& factories() {
    static Factories f;
    static std::once_flag once;
    std::call_once(once, [] {
        ensure_com();
        D2D1_FACTORY_OPTIONS o{};
        f.hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory), &o, reinterpret_cast<void**>(f.d2d.GetAddressOf()));
        if (SUCCEEDED(f.hr))
            f.hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(f.dw.GetAddressOf()));
        if (SUCCEEDED(f.hr)) f.hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.wic));
    });
    return f;
}

void bgra_to_nv12(const uint8_t* bgra, UINT stride, int w, int h, uint8_t* out) {
    // BT.709 limited range. Premultiplied alpha is opaque everywhere (the background fills the bitmap).
    auto Y = [](int r, int g, int b) { return uint8_t(16.5 + (0.2126 * r + 0.7152 * g + 0.0722 * b) * 219.0 / 255.0); };
    for (int y = 0; y < h; y++) {
        const uint8_t* s = bgra + size_t(y) * stride;
        uint8_t* d = out + size_t(y) * w;
        for (int x = 0; x < w; x++) d[x] = Y(s[4 * x + 2], s[4 * x + 1], s[4 * x]);
    }
    uint8_t* uv = out + size_t(w) * h;
    for (int y = 0; y < h / 2; y++) {
        const uint8_t* s0 = bgra + size_t(2 * y) * stride;
        const uint8_t* s1 = s0 + stride;
        uint8_t* d = uv + size_t(y) * w;
        for (int x = 0; x < w / 2; x++) {
            double r = 0, g = 0, b = 0;
            for (const uint8_t* s : {s0 + 8 * x, s0 + 8 * x + 4, s1 + 8 * x, s1 + 8 * x + 4}) {
                b += s[0];
                g += s[1];
                r += s[2];
            }
            r /= 4 * 255.0;
            g /= 4 * 255.0;
            b /= 4 * 255.0;
            double yy = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            double cb = (b - yy) / 1.8556, cr = (r - yy) / 1.5748;
            d[2 * x] = uint8_t(std::clamp(128.5 + cb * 224.0, 16.0, 240.0));
            d[2 * x + 1] = uint8_t(std::clamp(128.5 + cr * 224.0, 16.0, 240.0));
        }
    }
}

}  // namespace

bool render_placeholder(const Placeholder& p, int w, int h, std::vector<uint8_t>* nv12, std::string* err) {
    if (!display_size_ok(w, h)) {
        if (err) *err = "bad size";
        return false;
    }
    Factories& f = factories();
    if (FAILED(f.hr)) {
        if (err) *err = "factories " + hr_text(f.hr);
        return false;
    }
    ComPtr<IWICBitmap> bmp;
    HRESULT hr = f.wic->CreateBitmap(UINT(w), UINT(h), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp);
    ComPtr<ID2D1RenderTarget> rt;
    if (SUCCEEDED(hr)) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.f, 96.f);
        hr = f.d2d->CreateWicBitmapRenderTarget(bmp.Get(), props, &rt);
    }
    if (FAILED(hr)) {
        if (err) *err = "render target " + hr_text(hr);
        return false;
    }
    const float s = float(h) / 720.f;  // 24 px title, 16 px line at 720p
    const float W = float(w), H = float(h);
    ComPtr<ID2D1SolidColorBrush> panel, rule, ink, ink2, signal, signal_dim;
    rt->CreateSolidColorBrush(rgb(kPanel), &panel);
    rt->CreateSolidColorBrush(rgb(kRule), &rule);
    rt->CreateSolidColorBrush(rgb(kInk), &ink);
    rt->CreateSolidColorBrush(rgb(kInk2), &ink2);
    rt->CreateSolidColorBrush(rgb(kSignal), &signal);
    rt->CreateSolidColorBrush(rgb(kSignalDim), &signal_dim);
    rt->BeginDraw();
    rt->Clear(rgb(kGround));
    // A panel card in the middle, as the app's modules sit on its ground.
    const float cw = std::min(W * 0.72f, 760.f * s), ch = 300.f * s;
    D2D1_RECT_F card = D2D1::RectF((W - cw) / 2, (H - ch) / 2, (W + cw) / 2, (H + ch) / 2);
    rt->FillRoundedRectangle(D2D1::RoundedRect(card, 14.f * s, 14.f * s), panel.Get());
    rt->DrawRoundedRectangle(D2D1::RoundedRect(card, 14.f * s, 14.f * s), rule.Get(), 1.5f * s);
    // The mark: a camera body with a lens, in signal on signal_dim.
    const float mx = W / 2, my = card.top + 78.f * s;
    D2D1_RECT_F body = D2D1::RectF(mx - 38.f * s, my - 26.f * s, mx + 38.f * s, my + 26.f * s);
    rt->FillRoundedRectangle(D2D1::RoundedRect(body, 9.f * s, 9.f * s), signal_dim.Get());
    rt->DrawRoundedRectangle(D2D1::RoundedRect(body, 9.f * s, 9.f * s), signal.Get(), 3.f * s);
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), 14.f * s, 14.f * s), signal.Get(), 3.f * s);
    rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(mx, my), 5.f * s, 5.f * s), signal.Get());
    // Texts.
    auto text = [&](const std::string& t, float size, DWRITE_FONT_WEIGHT weight, ID2D1Brush* brush, float top) {
        if (t.empty()) return;
        ComPtr<IDWriteTextFormat> fmt;
        if (FAILED(f.dw->CreateTextFormat(L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          size, L"", &fmt)))
            return;
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        std::wstring wt = wide(t);
        D2D1_RECT_F r = D2D1::RectF(card.left + 24.f * s, top, card.right - 24.f * s, top + size * 2.6f);
        rt->DrawText(wt.c_str(), UINT32(wt.size()), fmt.Get(), r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    text(p.title, 24.f * s, DWRITE_FONT_WEIGHT_SEMI_BOLD, ink.Get(), card.top + 140.f * s);
    text(p.line, 16.f * s, DWRITE_FONT_WEIGHT_NORMAL, ink2.Get(), card.top + 210.f * s);
    hr = rt->EndDraw();
    if (FAILED(hr)) {
        if (err) *err = "EndDraw " + hr_text(hr);
        return false;
    }
    rt.Reset();
    WICRect rect{0, 0, w, h};
    ComPtr<IWICBitmapLock> lock;
    hr = bmp->Lock(&rect, WICBitmapLockRead, &lock);
    UINT stride = 0, size = 0;
    BYTE* px = nullptr;
    if (SUCCEEDED(hr)) hr = lock->GetStride(&stride);
    if (SUCCEEDED(hr)) hr = lock->GetDataPointer(&size, &px);
    if (FAILED(hr)) {
        if (err) *err = "lock " + hr_text(hr);
        return false;
    }
    nv12->resize(size_t(w) * h * 3 / 2);
    bgra_to_nv12(px, stride, w, h, nv12->data());
    return true;
}

}  // namespace beamer::audio::win
