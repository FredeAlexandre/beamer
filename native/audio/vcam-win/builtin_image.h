// SPDX-License-Identifier: GPL-3.0-only
// The source DLL's built-in image (PHASE2 §4.6.4): placeholder_builtin.inc (a 1920x1080 PNG made by
// tools/make_builtin.py) decoded with WIC, scaled to the app's size and converted to NV12, plus the small
// moving dot drawn over every placeholder frame. Shared by the DLL and camprobe (which recognises it).
// The caller initialises COM on the calling thread.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace beamer::vcam::builtin {
#include "placeholder_builtin.inc"

// Beamer's dark ground (tokens.PALETTE "ground" #0e0f0d) and signal (#5fd4f4), BT.601 limited range: the
// matrix apps assume for an unlabelled NV12 capture type.
struct Yuv { uint8_t y, u, v; };
inline Yuv to_yuv(int r, int g, int b) {
  double yy = 0.299 * r + 0.587 * g + 0.114 * b;
  auto clamp = [](double x) { return (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x + 0.5); };
  return {clamp(16 + yy * 219.0 / 255), clamp(128 + (b - yy) / 1.772 * 224.0 / 255), clamp(128 + (r - yy) / 1.402 * 224.0 / 255)};
}
inline Yuv ground() { return to_yuv(0x0e, 0x0f, 0x0d); }
inline Yuv dot_colour() { return to_yuv(0x5f, 0xd4, 0xf4); }

constexpr double kDotY = 800.0 / 1080.0;   // dot centre, fraction of the height (below the PNG's text)
constexpr double kDotSwing = 0.06;         // half the horizontal travel, fraction of the width
constexpr double kDotPeriodMs = 2000;      // one back-and-forth
inline int dot_radius(uint32_t h) { return h / 72 < 3 ? 3 : (int)(h / 72); }

// Fills `out` (w*h*3/2 bytes, stride w) with the built-in image at w x h. Returns false if WIC failed; `out`
// then holds the plain ground colour (never black, never garbage).
inline bool render_nv12(uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  using Microsoft::WRL::ComPtr;
  out.assign((size_t)w * h * 3 / 2, 0);
  Yuv g = ground();
  memset(out.data(), g.y, (size_t)w * h);
  for (size_t i = (size_t)w * h; i + 1 < out.size(); i += 2) {
    out[i] = g.u;
    out[i + 1] = g.v;
  }
  ComPtr<IWICImagingFactory> f;
  ComPtr<IWICStream> st;
  ComPtr<IWICBitmapDecoder> dec;
  ComPtr<IWICBitmapFrameDecode> fr;
  ComPtr<IWICFormatConverter> conv;
  ComPtr<IWICBitmapScaler> sc;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
  if (FAILED(f->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)kBuiltinPng, kBuiltinPngSize))) return false;
  if (FAILED(f->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
  if (FAILED(dec->GetFrame(0, &fr)) || FAILED(f->CreateFormatConverter(&conv))) return false;
  if (FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
  if (FAILED(f->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(conv.Get(), w, h, WICBitmapInterpolationModeFant))) return false;
  std::vector<uint8_t> bgr((size_t)w * h * 4);
  if (FAILED(sc->CopyPixels(nullptr, w * 4, (UINT)bgr.size(), bgr.data()))) return false;
  uint8_t* yp = out.data();
  uint8_t* uv = out.data() + (size_t)w * h;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      const uint8_t* p = &bgr[((size_t)y * w + x) * 4];
      yp[(size_t)y * w + x] = to_yuv(p[2], p[1], p[0]).y;
    }
  for (uint32_t y = 0; y + 1 < h; y += 2)
    for (uint32_t x = 0; x + 1 < w; x += 2) {
      int r = 0, gg = 0, b = 0;
      for (int k = 0; k < 4; k++) {
        const uint8_t* p = &bgr[((size_t)(y + k / 2) * w + x + k % 2) * 4];
        r += p[2];
        gg += p[1];
        b += p[0];
      }
      Yuv c = to_yuv(r / 4, gg / 4, b / 4);
      uv[(size_t)(y / 2) * w + x] = c.u;
      uv[(size_t)(y / 2) * w + x + 1] = c.v;
    }
  return true;
}

// Dot position at time t_ms (any monotonic millisecond clock).
inline void dot_centre(uint32_t w, uint32_t h, uint64_t t_ms, int* cx, int* cy) {
  double ph = std::sin(6.283185307179586 * (double)(t_ms % (uint64_t)kDotPeriodMs) / kDotPeriodMs);
  *cx = (int)(w / 2.0 + ph * kDotSwing * w);
  *cy = (int)(h * kDotY);
}

// Draws the dot into an NV12 picture (Y at `y`, UV at y + pitch * h).
inline void draw_dot(uint8_t* y, size_t pitch, uint32_t w, uint32_t h, uint64_t t_ms) {
  int cx, cy, r = dot_radius(h);
  dot_centre(w, h, t_ms, &cx, &cy);
  Yuv c = dot_colour();
  uint8_t* uv = y + pitch * h;
  for (int yy = cy - r; yy <= cy + r; yy++) {
    if (yy < 0 || yy >= (int)h) continue;
    for (int xx = cx - r; xx <= cx + r; xx++) {
      if (xx < 0 || xx >= (int)w) continue;
      int dx = xx - cx, dy = yy - cy;
      if (dx * dx + dy * dy > r * r) continue;
      y[(size_t)yy * pitch + xx] = c.y;
      size_t o = (size_t)(yy / 2) * pitch + (size_t)(xx & ~1);
      uv[o] = c.u;
      uv[o + 1] = c.v;
    }
  }
}
}  // namespace beamer::vcam::builtin
