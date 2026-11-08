// SPDX-License-Identifier: GPL-3.0-only
// macOS backend: "Beamer Microphone" demand (is an app recording it?) and the mic permission.
#import <AVFoundation/AVFoundation.h>

#include "mac_internal.h"

namespace beamer::audio::mac {

std::string mic_permission() {
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
    case AVAuthorizationStatusAuthorized: return "granted";
    case AVAuthorizationStatusDenied:
    case AVAuthorizationStatusRestricted: return "denied";
    case AVAuthorizationStatusNotDetermined: return "undetermined";
  }
  return "unknown";
}

DemandWatcher::DemandWatcher(dispatch_queue_t queue, std::function<void(bool)> on_change)
    : queue_(queue), on_change_(std::move(on_change)) {}

DemandWatcher::~DemandWatcher() {  // on queue_
  *alive_ = false;
  detach();
}

void DemandWatcher::detach() {
  if (dev_ && block_) {
    auto a = addr(kAudioDevicePropertyDeviceIsRunningSomewhere);
    AudioObjectRemovePropertyListenerBlock(dev_, &a, queue_, block_);
  }
  dev_ = 0;
  block_ = nil;
}

void DemandWatcher::refresh() {
  AudioObjectID dev = device_for_uid(kVmicUID);
  if (dev != dev_) {
    detach();
    if (dev) {
      auto a = addr(kAudioDevicePropertyDeviceIsRunningSomewhere);
      std::shared_ptr<bool> alive = alive_;
      block_ = ^(UInt32, const AudioObjectPropertyAddress*) {
        if (*alive) update();
      };
      OSStatus st = AudioObjectAddPropertyListenerBlock(dev, &a, queue_, block_);
      if (st == noErr) {
        dev_ = dev;
      } else {
        log_line("warn", "demand listener on Beamer Microphone failed: %s", os_status(st).c_str());
        block_ = nil;
      }
    }
  }
  update();  // also a poll: covers a missed notification
}

void DemandWatcher::update() {
  uint32_t v = 0;
  bool raw = dev_ && get_u32(dev_, addr(kAudioDevicePropertyDeviceIsRunningSomewhere), &v) && v != 0;
  if (raw == raw_) return;
  raw_ = raw;
  const uint64_t gen = ++off_gen_;
  if (raw) {
    if (!held_.exchange(true)) on_change_(true);
    return;
  }
  std::shared_ptr<bool> alive = alive_;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, int64_t(kOffHoldMs) * NSEC_PER_MSEC), queue_, ^{
    if (!*alive || gen != off_gen_ || raw_) return;
    if (held_.exchange(false)) on_change_(false);
  });
}

}  // namespace beamer::audio::mac
