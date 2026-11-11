// beamer-camera: synthetic NV12 test pattern with an embedded frame counter and timestamp.
//
// Content is meant to cost an encoder roughly what a webcam does: a moving gradient, a moving "head"
// (textured disc), a scrolling fine-detail panel (text-like) and per-pixel temporal noise (sensor
// noise, amplitude --noise luma steps). Everything but the code strip is a pure function of the
// frame number, so a decoder can regenerate the source for PSNR.
#import "common.h"
#include <cmath>

static uint8_t g_noise[65536];
static bool g_noise_init = false;
static void init_noise() {
  if (g_noise_init) return;
  uint32_t s = 0x12345678;
  for (int i = 0; i < 65536; i++) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    g_noise[i] = (uint8_t)(s >> 24);
  }
  g_noise_init = true;
}

static inline uint8_t clamp8(int v) { return v < 16 ? 16 : (v > 235 ? 235 : (uint8_t)v); }

void synth_fill(CVPixelBufferRef pb, uint32_t f, int noise) {
  init_noise();
  CVPixelBufferLockBaseAddress(pb, 0);
  const int w = (int)CVPixelBufferGetWidth(pb), h = (int)CVPixelBufferGetHeight(pb);
  uint8_t *Y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  uint8_t *UV = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
  const int B = w / 80;
  // disc ("head"): moves on a Lissajous path, 4 s and 6 s periods at 30 fps
  const double t = f / 30.0;
  const int cx = (int)(w * (0.5 + 0.22 * std::sin(2 * M_PI * t / 4.0)));
  const int cy = (int)(h * (0.48 + 0.10 * std::sin(2 * M_PI * t / 6.0)));
  const int r = h / 5;
  const int panel_y0 = h * 3 / 4, panel_y1 = h - h / 20;
  const int scroll = (int)(f * 3);
  const int amp = noise * 2 + 1;
  for (int y = B; y < h; y++) {
    uint8_t *row = Y + y * ys;
    const bool in_panel = y >= panel_y0 && y < panel_y1;
    const int dy = y - cy;
    for (int x = 0; x < w; x++) {
      int v;
      const int dx = x - cx;
      const int d2 = dx * dx + dy * dy;
      if (d2 < r * r) {
        // textured disc: concentric rings + a highlight
        int d = (int)std::sqrt((double)d2);
        v = 120 + ((d / 6) & 1) * 30 + (dx > 0 ? 20 : 0);
      } else if (in_panel) {
        // text-like fine detail: 6x10 px glyph cells with pseudo-random strokes, scrolling left
        int xs = x + scroll;
        int cell = (xs / 6) * 31 + ((y - panel_y0) / 12) * 977;
        int bits = g_noise[cell & 0xFFFF];
        int gx = xs % 6, gy = (y - panel_y0) % 12;
        bool on = gx < 5 && gy < 10 && ((bits >> ((gx + gy * 3) & 7)) & 1);
        v = on ? 40 : 200;
      } else {
        // smooth moving gradient background
        v = 50 + (((x + 2 * y + (int)f * 4) % 1024) * 140) / 1024;
      }
      if (noise > 0) v += (int)(g_noise[(uint32_t)(x * 7 + y * 1031 + f * 40503u) & 0xFFFF] % amp) - noise;
      row[x] = clamp8(v);
    }
  }
  // chroma (half resolution): gradients + skin tone in the disc
  for (int y = 0; y < h / 2; y++) {
    uint8_t *row = UV + y * uvs;
    for (int x = 0; x < w / 2; x++) {
      int dx = 2 * x - cx, dy = 2 * y - cy;
      uint8_t u, v;
      if (2 * y < B) { u = 128; v = 128; }
      else if (dx * dx + dy * dy < r * r) { u = 110; v = 150; }
      else { u = (uint8_t)(96 + (x * 64) / (w / 2)); v = (uint8_t)(96 + ((y + f) % (h / 2)) * 64 / (h / 2)); }
      row[2 * x] = u;
      row[2 * x + 1] = v;
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
}

void synth_stamp(CVPixelBufferRef pb, uint32_t f, uint64_t host_ns) {
  CVPixelBufferLockBaseAddress(pb, 0);
  const int w = (int)CVPixelBufferGetWidth(pb);
  uint8_t *Y = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
  const int B = w / 80;
  uint32_t ts = (uint32_t)(host_ns / 100000ULL);
  uint8_t bytes[10] = {0xA5, (uint8_t)(f >> 24), (uint8_t)(f >> 16), (uint8_t)(f >> 8), (uint8_t)f,
                       (uint8_t)(ts >> 24), (uint8_t)(ts >> 16), (uint8_t)(ts >> 8), (uint8_t)ts, 0};
  for (int i = 1; i < 9; i++) bytes[9] ^= bytes[i];
  for (int bit = 0; bit < 80; bit++) {
    bool on = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
    for (int y = 0; y < B; y++) memset(Y + y * ys + bit * B, on ? 235 : 16, B);
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);
}

bool synth_read(CVPixelBufferRef pb, uint32_t *f, uint64_t *ts100us) {
  CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  const int w = (int)CVPixelBufferGetWidth(pb);
  const uint8_t *Y = (const uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
  const size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
  const int B = w / 80;
  uint8_t bytes[10] = {0};
  for (int bit = 0; bit < 80; bit++) {
    bool on = Y[(B / 2) * ys + bit * B + B / 2] > 128;
    if (on) bytes[bit / 8] |= 1 << (7 - bit % 8);
  }
  CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  uint8_t x = 0;
  for (int i = 1; i < 9; i++) x ^= bytes[i];
  if (bytes[0] != 0xA5 || x != bytes[9]) return false;
  *f = (uint32_t)bytes[1] << 24 | bytes[2] << 16 | bytes[3] << 8 | bytes[4];
  *ts100us = (uint32_t)bytes[5] << 24 | bytes[6] << 16 | bytes[7] << 8 | bytes[8];
  return true;
}
