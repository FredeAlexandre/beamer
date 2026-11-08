// SPDX-License-Identifier: GPL-3.0-only
// macOS backend: Core Audio process taps (macOS 14.2+) wrapped in a private, tap-only aggregate.
//
// Behaviour from the Phase 1 prototype (dev/audio/proto/mac/README.md, "Tap aggregate"):
// - tap-only aggregate (no output device as clock sub-device): the speakers are not kept running
//   by us (coreaudiod ~0.4 % CPU instead of ~11 %), same latency and mute results;
// - kAudioAggregateDeviceTapAutoStartKey = false: with it the IOProc gets no callback at all while
//   the system is silent;
// - mutedWhenTapped mutes the tapped processes locally only while the tap exists; the HAL removes
//   a private tap (and lifts the mute) when its process dies, even on SIGKILL;
// - Core Audio has no process trees: every process to exclude is named; a pid that is not yet a
//   Core Audio client has no process object and is added to the live tap when it appears.
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <unistd.h>

#include "mac_internal.h"

namespace beamer::audio::mac {

bool taps_supported() {
  if (@available(macOS 14.2, *)) return true;
  return false;
}

static NSArray<NSNumber*>* exclusion_list(const std::vector<pid_t>& pids, bool self, std::vector<pid_t>* pending) {
  NSMutableArray<NSNumber*>* objs = [NSMutableArray array];
  AudioObjectID self_obj = self ? process_object(getpid()) : 0;
  if (self_obj) [objs addObject:@(self_obj)];
  for (pid_t pid : pids) {
    if (pid <= 0 || pid == getpid()) continue;
    AudioObjectID o = process_object(pid);
    if (!o) {
      pending->push_back(pid);
      continue;
    }
    if (![objs containsObject:@(o)]) [objs addObject:@(o)];
  }
  return objs;
}

bool create_tap(const TapSpec& spec, Tap* out, std::string* err) {
  if (@available(macOS 14.2, *)) {
    *out = Tap{};
    NSArray<NSNumber*>* exclude = exclusion_list(spec.exclude, spec.exclude_self, &out->pending);
    CATapDescription* desc;
    if (spec.global) {
      desc = [[CATapDescription alloc] initStereoGlobalTapButExcludeProcesses:exclude];
    } else {
      desc = [[CATapDescription alloc] initExcludingProcesses:exclude
                                                 andDeviceUID:[NSString stringWithUTF8String:spec.device_uid.c_str()]
                                                   withStream:0];
    }
    desc.name = @"Beamer audio tap";
    desc.UUID = [NSUUID UUID];
    desc.privateTap = YES;
    desc.muteBehavior = spec.mute_local ? CATapMutedWhenTapped : CATapUnmuted;

    AudioObjectID tap = 0;
    OSStatus st = AudioHardwareCreateProcessTap(desc, &tap);
    if (st != noErr || !tap) {
      if (err) *err = "internal: AudioHardwareCreateProcessTap " + os_status(st);
      return false;
    }
    std::string tap_uid = get_string(tap, addr(kAudioTapPropertyUID));
    if (tap_uid.empty()) tap_uid = desc.UUID.UUIDString.UTF8String;

    NSString* agg_uid = [@(kTapAggregatePrefix) stringByAppendingString:[NSUUID UUID].UUIDString];
    NSDictionary* dict = @{
      @kAudioAggregateDeviceNameKey : @"Beamer audio tap",
      @kAudioAggregateDeviceUIDKey : agg_uid,
      @kAudioAggregateDeviceIsPrivateKey : @YES,
      @kAudioAggregateDeviceIsStackedKey : @NO,
      @kAudioAggregateDeviceTapAutoStartKey : @NO,
      @kAudioAggregateDeviceTapListKey : @[ @{
        @kAudioSubTapDriftCompensationKey : @YES,
        @kAudioSubTapUIDKey : [NSString stringWithUTF8String:tap_uid.c_str()],
      } ],
    };
    AudioObjectID agg = 0;
    st = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)dict, &agg);
    if (st != noErr || !agg) {
      AudioHardwareDestroyProcessTap(tap);
      if (err) *err = "internal: AudioHardwareCreateAggregateDevice " + os_status(st);
      return false;
    }
    // The tap's input stream appears asynchronously on some systems.
    for (int i = 0; i < 50 && channel_count(agg, true) == 0; ++i) usleep(10000);
    out->tap = tap;
    out->aggregate = agg;
    out->desc = (void*)CFBridgingRetain(desc);
    return true;
  }
  if (err) *err = "unsupported: process taps need macOS 14.2";
  return false;
}

void destroy_tap(Tap* t) {
  if (@available(macOS 14.2, *)) {
    if (t->aggregate) AudioHardwareDestroyAggregateDevice(t->aggregate);
    if (t->tap) AudioHardwareDestroyProcessTap(t->tap);
  }
  if (t->desc) CFRelease(t->desc);
  *t = Tap{};
}

int add_late_exclusions(Tap* t) {
  if (t->pending.empty() || !t->tap || !t->desc) return 0;
  if (@available(macOS 14.2, *)) {
    CATapDescription* desc = (__bridge CATapDescription*)t->desc;
    NSMutableArray<NSNumber*>* objs = [desc.processes mutableCopy];
    std::vector<pid_t> still;
    int added = 0;
    for (pid_t pid : t->pending) {
      AudioObjectID o = process_object(pid);
      if (!o) {
        still.push_back(pid);
        continue;
      }
      if (![objs containsObject:@(o)]) [objs addObject:@(o)];
      ++added;
    }
    if (!added) return 0;
    desc.processes = objs;
    // kAudioTapPropertyDescription is settable on a live tap; its data is the object pointer.
    __unsafe_unretained CATapDescription* d = desc;
    auto a = addr(kAudioTapPropertyDescription);
    OSStatus st = AudioObjectSetPropertyData(t->tap, &a, 0, nullptr, sizeof(d), (void*)&d);
    if (st != noErr) {
      log_line("warn", "late tap exclusion failed: %s", os_status(st).c_str());
      return 0;
    }
    t->pending = std::move(still);
    return added;
  }
  return 0;
}

}  // namespace beamer::audio::mac
