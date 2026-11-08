// SPDX-License-Identifier: GPL-3.0-only
// Internal declarations shared by the macOS backend files (not part of the engine contract).
#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <dispatch/dispatch.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "backend_mac.h"

namespace beamer::audio::mac {

// The Beamer virtual microphone (BeamerAudio.driver): apps record kVmicUID, Beamer renders into the
// hidden kFeedUID. Both are found with TranslateUIDToDevice (the feed is not in the device list).
constexpr const char* kVmicUID = "com.beamer.audio.mic";
constexpr const char* kFeedUID = "com.beamer.audio.micfeed";
constexpr const char* kTapAggregatePrefix = "com.beamer.audio.tap.";  // our private tap aggregates

// ---- logging (stderr, "level: message"; never from an IO thread) ----
void log_line(const char* level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- Core Audio property helpers (devices_mac.mm) ----
AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
                                AudioObjectPropertyElement element = kAudioObjectPropertyElementMain);
bool get_u32(AudioObjectID obj, const AudioObjectPropertyAddress& a, uint32_t* out);
std::string get_string(AudioObjectID obj, const AudioObjectPropertyAddress& a);
std::string device_uid(AudioObjectID dev);
std::string device_name(AudioObjectID dev);
std::string device_manufacturer(AudioObjectID dev);
AudioObjectID device_for_uid(const std::string& uid);  // hidden devices too; 0 = unknown
std::vector<AudioObjectID> all_devices();
AudioObjectID default_device(bool input);
int channel_count(AudioObjectID dev, bool input);
double nominal_rate(AudioObjectID dev);
bool is_alive(AudioObjectID dev);
uint32_t transport_type(AudioObjectID dev);
bool streams_are_float32(AudioObjectID dev, bool input);
std::string os_status(OSStatus st);  // "-50" or "'who?'"
uint64_t host_to_ns(uint64_t host_ticks);
uint64_t now_ns();
AudioObjectID process_object(pid_t pid);  // Core Audio process object of a pid, 0 = none yet

// ---- device inventory (devices_mac.mm) ----
const char* transport_word(uint32_t transport);  // contract words: builtin usb bluetooth virtual hdmi other
bool is_blocked(const std::string& name, const std::string& manufacturer, uint32_t transport);
bool is_blocked_device(AudioObjectID dev);
void fill_inventory(DeviceSnapshot& snap);  // devices (split per direction), defaults

// ---- process taps (tap_mac.mm) ----
bool taps_supported();  // macOS 14.2+
struct TapSpec {
  bool global = true;       // stereo global tap; false = tap of `device_uid` stream 0
  std::string device_uid;
  bool mute_local = false;  // CATapMutedWhenTapped
  std::vector<pid_t> exclude;  // processes left out of the tap
  bool exclude_self = true;    // also leave this process out
};
struct Tap {
  AudioObjectID tap = 0, aggregate = 0;
  std::vector<pid_t> pending;  // pids without a Core Audio process object yet
  void* desc = nullptr;        // retained CATapDescription (CFBridgingRetain), for late exclusions
};
bool create_tap(const TapSpec& spec, Tap* out, std::string* err);
void destroy_tap(Tap* t);
int add_late_exclusions(Tap* t);  // resolves pending pids; returns how many were added to the tap

// ---- virtual mic demand and permission (vmic_mac.mm) ----
std::string mic_permission();  // granted | denied | undetermined | unknown

// Watches kAudioDevicePropertyDeviceIsRunningSomewhere of "Beamer Microphone" (PHASE2 §6.3). The
// driver counts IO per device, so Beamer's own feed does not set it. Changes are reported at once in
// both directions: the engine's helper loop holds "off" for 3 s (§3.3), one hold for every backend.
// All methods (and the destructor) run on `queue`.
class DemandWatcher {
 public:
  DemandWatcher(dispatch_queue_t queue, std::function<void(bool)> on_change);
  ~DemandWatcher();
  void refresh();  // (re)attach to the current device (after a device change or coreaudiod restart)
  void detach();
  bool in_use() const { return held_.load(std::memory_order_relaxed); }

 private:
  void update();
  dispatch_queue_t queue_;
  std::function<void(bool)> on_change_;
  AudioObjectID dev_ = 0;
  AudioObjectPropertyListenerBlock block_ = nil;
  std::atomic<bool> held_{false};
  bool raw_ = false;
  std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);  // queued listener blocks check it
};

}  // namespace beamer::audio::mac
