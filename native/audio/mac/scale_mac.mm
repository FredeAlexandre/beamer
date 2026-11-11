// SPDX-License-Identifier: GPL-3.0-only
// macOS video backend: capture format choice, the centre 16:9 crop and the VTPixelTransferSession scaler
// (camera PHASE2 §4.11). The crop and format rules are pure functions (checked by video_check selftest).
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cmath>

#include "video_backend_mac.h"

namespace beamer::audio::macvideo {

namespace {
std::mutex g_times_mu;
std::vector<double> g_scale_times;
constexpr uint32_t k420v = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
constexpr uint32_t k420f = kCVPixelFormatType_420YpCbCr8BiPlanarFullRange;
int even_down(int v) { return v & ~1; }
}  // namespace

std::vector<double> take_scale_times() {
  std::lock_guard<std::mutex> lk(g_times_mu);
  std::vector<double> v;
  v.swap(g_scale_times);
  return v;
}

CropRect crop_16x9(int w, int h) {
  CropRect r;
  if (w <= 0 || h <= 0) return r;
  // Wc = min(W, floor(H * 16 / 9)), Hc = floor(Wc * 9 / 16), both rounded down to even
  int wc = std::min(w, (int)((int64_t)h * 16 / 9));
  wc = even_down(wc);
  int hc = even_down((int)((int64_t)wc * 9 / 16));
  r.w = wc;
  r.h = hc;
  r.x = even_down((w - wc) / 2);
  r.y = even_down((h - hc) / 2);
  return r;
}

int choose_format(const std::vector<FormatCand>& cands, int wt, int ht) {
  if (cands.empty()) return -1;
  std::vector<int> pool;
  for (int i = 0; i < (int)cands.size(); ++i)
    if (cands[i].subtype == k420v || cands[i].subtype == k420f) pool.push_back(i);
  if (pool.empty())  // no NV12 format at all: the data output converts; same rules over every format
    for (int i = 0; i < (int)cands.size(); ++i) pool.push_back(i);
  std::vector<int> fast;
  for (int i : pool)
    if (cands[i].max_fps >= 29.5) fast.push_back(i);
  if (fast.empty()) {  // none reaches 30 fps: the highest-rate ones
    double best = 0;
    for (int i : pool) best = std::max(best, cands[i].max_fps);
    for (int i : pool)
      if (cands[i].max_fps >= best - 0.01) fast.push_back(i);
  }
  // 420v before 420f before other, then the earlier format
  auto rank = [&](int i) { return cands[i].subtype == k420v ? 0 : cands[i].subtype == k420f ? 1 : 2; };
  auto better_tie = [&](int a, int b) { return rank(a) != rank(b) ? rank(a) < rank(b) : a < b; };
  auto crop_area = [&](int i) {
    CropRect c = crop_16x9(cands[i].w, cands[i].h);
    return (int64_t)c.w * c.h;
  };
  // 1. exactly Wt x Ht
  int pick = -1;
  for (int i : fast)
    if (cands[i].w == wt && cands[i].h == ht && (pick < 0 || better_tie(i, pick))) pick = i;
  if (pick >= 0) return pick;
  // 2. landscape, smallest area whose crop covers Wt x Ht
  for (int i : fast) {
    if (cands[i].w < cands[i].h) continue;
    CropRect c = crop_16x9(cands[i].w, cands[i].h);
    if (c.w < wt || c.h < ht) continue;
    int64_t a = (int64_t)cands[i].w * cands[i].h;
    if (pick < 0) { pick = i; continue; }
    int64_t pa = (int64_t)cands[pick].w * cands[pick].h;
    if (a < pa || (a == pa && better_tie(i, pick))) pick = i;
  }
  if (pick >= 0) return pick;
  // 3. landscape with the largest crop; 4. any with the largest crop
  for (int pass = 0; pass < 2 && pick < 0; ++pass) {
    for (int i : fast) {
      if (pass == 0 && cands[i].w < cands[i].h) continue;
      if (pick < 0) { pick = i; continue; }
      int64_t a = crop_area(i), pa = crop_area(pick);
      if (a > pa || (a == pa && better_tie(i, pick))) pick = i;
    }
  }
  return pick;
}

CVPixelBufferPoolRef make_nv12_pool(int w, int h) {
  NSDictionary* attrs = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(k420v),
    (id)kCVPixelBufferWidthKey : @(w),
    (id)kCVPixelBufferHeightKey : @(h),
    (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
  };
  CVPixelBufferPoolRef pool = nullptr;
  if (CVPixelBufferPoolCreate(nullptr, nullptr, (__bridge CFDictionaryRef)attrs, &pool) != kCVReturnSuccess) return nullptr;
  return pool;
}

Scaler::Scaler(int dw, int dh) : dw_(dw), dh_(dh) {}

Scaler::~Scaler() {
  if (session_) {
    VTPixelTransferSessionInvalidate((VTPixelTransferSessionRef)session_);
    CFRelease((CFTypeRef)session_);
  }
  if (pool_) CVPixelBufferPoolRelease(pool_);
}

bool Scaler::ensure(std::string* err) {
  if (session_ && pool_) return true;
  if (!session_) {
    VTPixelTransferSessionRef s = nullptr;
    OSStatus st = VTPixelTransferSessionCreate(nullptr, &s);
    if (st != noErr) {
      if (err) *err = "internal: VTPixelTransferSessionCreate " + std::to_string((int)st);
      return false;
    }
    // Crop to the source's clean aperture (set per frame to the §4.11 rectangle), then scale to fill the
    // destination: with a 16:9 aperture and a 16:9 destination this never letterboxes nor stretches.
    VTSessionSetProperty(s, kVTPixelTransferPropertyKey_ScalingMode, kVTScalingMode_CropSourceToCleanAperture);
    session_ = s;
  }
  if (!pool_) {
    pool_ = make_nv12_pool(dw_, dh_);
    if (!pool_) {
      if (err) *err = "internal: scaler pixel buffer pool";
      return false;
    }
  }
  return true;
}

CVPixelBufferRef Scaler::convert(CVPixelBufferRef src, const CropRect& crop, std::string* err) {
  if (!ensure(err)) return nullptr;
  const int64_t t0 = now_ns();
  const double sw = (double)CVPixelBufferGetWidth(src), sh = (double)CVPixelBufferGetHeight(src);
  // Clean aperture offsets are from the picture centre to the aperture centre.
  NSDictionary* aperture = @{
    (id)kCVImageBufferCleanApertureWidthKey : @(crop.w),
    (id)kCVImageBufferCleanApertureHeightKey : @(crop.h),
    (id)kCVImageBufferCleanApertureHorizontalOffsetKey : @((crop.x + crop.w / 2.0) - sw / 2.0),
    (id)kCVImageBufferCleanApertureVerticalOffsetKey : @((crop.y + crop.h / 2.0) - sh / 2.0),
  };
  CFTypeRef saved = CVBufferCopyAttachment(src, kCVImageBufferCleanApertureKey, nullptr);
  CVBufferSetAttachment(src, kCVImageBufferCleanApertureKey, (__bridge CFDictionaryRef)aperture, kCVAttachmentMode_ShouldPropagate);
  CVPixelBufferRef dst = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pool_, &dst) != kCVReturnSuccess || !dst) {
    if (saved) { CVBufferSetAttachment(src, kCVImageBufferCleanApertureKey, saved, kCVAttachmentMode_ShouldPropagate); CFRelease(saved); }
    else CVBufferRemoveAttachment(src, kCVImageBufferCleanApertureKey);
    if (err) *err = "internal: scaler buffer";
    return nullptr;
  }
  OSStatus st = VTPixelTransferSessionTransferImage((VTPixelTransferSessionRef)session_, src, dst);
  if (saved) {
    CVBufferSetAttachment(src, kCVImageBufferCleanApertureKey, saved, kCVAttachmentMode_ShouldPropagate);
    CFRelease(saved);
  } else {
    CVBufferRemoveAttachment(src, kCVImageBufferCleanApertureKey);
  }
  if (st != noErr) {
    CVPixelBufferRelease(dst);
    if (err) *err = "internal: VTPixelTransferSessionTransferImage " + std::to_string((int)st);
    return nullptr;
  }
  // The output is the whole picture: no aperture may reach the encoder (it would end up in the SPS crop).
  CVBufferRemoveAttachment(dst, kCVImageBufferCleanApertureKey);
  last_ms_ = (now_ns() - t0) / 1e6;
  {
    std::lock_guard<std::mutex> lk(g_times_mu);
    if (g_scale_times.size() < 100000) g_scale_times.push_back(last_ms_);
  }
  return dst;
}

}  // namespace beamer::audio::macvideo
