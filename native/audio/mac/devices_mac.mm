// SPDX-License-Identifier: GPL-3.0-only
// macOS backend: Core Audio property helpers, device inventory, forms and the blocklist.
#include "mac_internal.h"

#include <mach/mach_time.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace beamer::audio::mac {

// ---------------------------------------------------------------------------------------------
// logging

void log_line(const char* level, const char* fmt, ...) {
  static std::mutex mu;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::lock_guard<std::mutex> lock(mu);
  fprintf(stderr, "%s: mac: %s\n", level, buf);
  fflush(stderr);
}

// ---------------------------------------------------------------------------------------------
// property helpers

AudioObjectPropertyAddress addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope,
                                AudioObjectPropertyElement element) {
  return AudioObjectPropertyAddress{sel, scope, element};
}

bool get_u32(AudioObjectID obj, const AudioObjectPropertyAddress& a, uint32_t* out) {
  UInt32 size = sizeof(uint32_t);
  return AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, out) == noErr;
}

static std::string from_cf(CFStringRef s) {
  if (!s) return {};
  char stack[512];
  if (CFStringGetCString(s, stack, sizeof stack, kCFStringEncodingUTF8)) return stack;
  CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<size_t>(n), '\0');
  if (!CFStringGetCString(s, out.data(), n, kCFStringEncodingUTF8)) return {};
  out.resize(strlen(out.c_str()));
  return out;
}

std::string get_string(AudioObjectID obj, const AudioObjectPropertyAddress& a) {
  CFStringRef s = nullptr;
  UInt32 size = sizeof(s);
  if (AudioObjectGetPropertyData(obj, &a, 0, nullptr, &size, &s) != noErr || !s) return {};
  std::string out = from_cf(s);
  CFRelease(s);
  return out;
}

std::string device_uid(AudioObjectID dev) { return get_string(dev, addr(kAudioDevicePropertyDeviceUID)); }
std::string device_name(AudioObjectID dev) { return get_string(dev, addr(kAudioObjectPropertyName)); }
std::string device_manufacturer(AudioObjectID dev) {
  return get_string(dev, addr(kAudioObjectPropertyManufacturer));
}

AudioObjectID device_for_uid(const std::string& uid) {
  if (uid.empty()) return 0;
  CFStringRef cf = CFStringCreateWithCString(nullptr, uid.c_str(), kCFStringEncodingUTF8);
  if (!cf) return 0;
  AudioObjectID dev = 0;
  UInt32 size = sizeof(dev);
  auto a = addr(kAudioHardwarePropertyTranslateUIDToDevice);
  OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(cf), &cf, &size, &dev);
  CFRelease(cf);
  return st == noErr ? dev : 0;
}

std::vector<AudioObjectID> all_devices() {
  auto a = addr(kAudioHardwarePropertyDevices);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) != noErr) return {};
  std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
  if (ids.empty()) return ids;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
  ids.resize(size / sizeof(AudioObjectID));
  // Our own private tap aggregates are visible to this process only: never a device to list.
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [](AudioObjectID d) {
                             return transport_type(d) == kAudioDeviceTransportTypeAggregate &&
                                    device_uid(d).rfind(kTapAggregatePrefix, 0) == 0;
                           }),
            ids.end());
  return ids;
}

AudioObjectID default_device(bool input) {
  uint32_t dev = 0;
  get_u32(kAudioObjectSystemObject,
          addr(input ? kAudioHardwarePropertyDefaultInputDevice : kAudioHardwarePropertyDefaultOutputDevice), &dev);
  return dev;
}

int channel_count(AudioObjectID dev, bool input) {
  auto a = addr(kAudioDevicePropertyStreamConfiguration,
                input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
  std::vector<uint8_t> raw(size);
  auto* abl = reinterpret_cast<AudioBufferList*>(raw.data());
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, abl) != noErr) return 0;
  int n = 0;
  for (UInt32 i = 0; i < abl->mNumberBuffers; ++i) n += static_cast<int>(abl->mBuffers[i].mNumberChannels);
  return n;
}

double nominal_rate(AudioObjectID dev) {
  Float64 r = 0;
  UInt32 size = sizeof(r);
  auto a = addr(kAudioDevicePropertyNominalSampleRate);
  return AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &r) == noErr ? r : 0;
}

bool is_alive(AudioObjectID dev) {
  uint32_t v = 0;
  return get_u32(dev, addr(kAudioDevicePropertyDeviceIsAlive), &v) && v != 0;
}

uint32_t transport_type(AudioObjectID dev) {
  uint32_t t = 0;
  get_u32(dev, addr(kAudioDevicePropertyTransportType), &t);
  return t;
}

static std::vector<AudioObjectID> streams_of(AudioObjectID dev, bool input) {
  auto a = addr(kAudioDevicePropertyStreams, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return {};
  std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
  if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, ids.data()) != noErr) return {};
  ids.resize(size / sizeof(AudioObjectID));
  return ids;
}

bool streams_are_float32(AudioObjectID dev, bool input) {
  for (AudioObjectID s : streams_of(dev, input)) {
    AudioStreamBasicDescription f{};
    UInt32 size = sizeof(f);
    auto a = addr(kAudioStreamPropertyVirtualFormat);
    if (AudioObjectGetPropertyData(s, &a, 0, nullptr, &size, &f) != noErr) continue;
    if (f.mFormatID != kAudioFormatLinearPCM || !(f.mFormatFlags & kAudioFormatFlagIsFloat) ||
        f.mBitsPerChannel != 32)
      return false;
  }
  return true;
}

std::string os_status(OSStatus st) {
  uint32_t v = static_cast<uint32_t>(st);
  char c[4] = {char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
  bool printable = std::all_of(c, c + 4, [](char x) { return x >= 32 && x < 127; });
  if (printable) return std::string("'") + std::string(c, 4) + "'";
  return std::to_string(st);
}

static const mach_timebase_info_data_t& timebase() {
  static const mach_timebase_info_data_t tb = [] {
    mach_timebase_info_data_t t{};
    mach_timebase_info(&t);
    return t;
  }();
  return tb;
}

uint64_t host_to_ns(uint64_t t) {
  const auto& tb = timebase();
  if (tb.numer == tb.denom) return t;
  return static_cast<uint64_t>(static_cast<__uint128_t>(t) * tb.numer / tb.denom);
}

uint64_t now_ns() { return host_to_ns(mach_absolute_time()); }

AudioObjectID process_object(pid_t pid) {
  AudioObjectID obj = 0;
  UInt32 size = sizeof(obj);
  auto a = addr(kAudioHardwarePropertyTranslatePIDToProcessObject);
  OSStatus st = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(pid), &pid, &size, &obj);
  return st == noErr ? obj : 0;
}

// ---------------------------------------------------------------------------------------------
// classification

const char* transport_word(uint32_t t) {
  switch (t) {
    case kAudioDeviceTransportTypeBuiltIn: return "builtin";
    case kAudioDeviceTransportTypeUSB: return "usb";
    case kAudioDeviceTransportTypeBluetooth:
    case kAudioDeviceTransportTypeBluetoothLE: return "bluetooth";
    case kAudioDeviceTransportTypeVirtual: return "virtual";
    case kAudioDeviceTransportTypeHDMI:
    case kAudioDeviceTransportTypeDisplayPort: return "hdmi";
    default: return "other";
  }
}

static std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static bool contains(const std::string& hay, const char* needle) { return lower(hay).find(needle) != std::string::npos; }

static const char* const kBlockedNames[] = {
#include "blocklist.inc"
};

bool is_blocked(const std::string& name, const std::string& manufacturer, uint32_t transport) {
  if (transport != kAudioDeviceTransportTypeVirtual) return false;  // physical devices stay allowed
  if (contains(name, "elgato") || contains(manufacturer, "elgato")) return true;
  for (const char* n : kBlockedNames)
    if (name == n) return true;
  return false;
}

bool is_blocked_device(AudioObjectID dev) {
  return is_blocked(device_name(dev), device_manufacturer(dev), transport_type(dev));
}

static uint32_t terminal_type(AudioObjectID dev, bool input) {
  for (AudioObjectID s : streams_of(dev, input)) {
    uint32_t t = 0;
    if (get_u32(s, addr(kAudioStreamPropertyTerminalType), &t) && t != 0) return t;
  }
  return 0;
}

static uint32_t data_source(AudioObjectID dev, bool input) {
  uint32_t v = 0;
  auto a = addr(kAudioDevicePropertyDataSource, input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
  if (!AudioObjectHasProperty(dev, &a)) return 0;
  return get_u32(dev, a, &v) ? v : 0;
}

// The device's form for one direction (PHASE2 §7 "Headset detection").
static Form form_of(AudioObjectID dev, bool input, uint32_t transport, const std::string& name, bool duplex) {
  const bool says_speaker = contains(name, "speaker");
  const bool says_headset = contains(name, "headset") || contains(name, "airpods") || contains(name, "buds");
  const bool says_headphones = contains(name, "headphone");
  if (transport == kAudioDeviceTransportTypeBuiltIn) {
    uint32_t ds = data_source(dev, input);
    if (input) return ds == 'emic' ? Form::Headset : Form::Microphone;
    if (ds == 'hdpn') return Form::Headphones;
    return Form::Speakers;
  }
  if (transport == kAudioDeviceTransportTypeBluetooth || transport == kAudioDeviceTransportTypeBluetoothLE) {
    if (says_speaker) return input ? Form::Microphone : Form::Speakers;
    if (input) return Form::Headset;
    return (duplex || says_headset) ? Form::Headset : Form::Headphones;
  }
  switch (terminal_type(dev, input)) {
    case kAudioStreamTerminalTypeHeadphones: return Form::Headphones;
    case kAudioStreamTerminalTypeHeadsetMicrophone: return Form::Headset;
    case kAudioStreamTerminalTypeSpeaker:
    case kAudioStreamTerminalTypeLFESpeaker:
    case kAudioStreamTerminalTypeReceiverSpeaker:
      if (!(transport == kAudioDeviceTransportTypeUSB && duplex)) return Form::Speakers;
      break;
    case kAudioStreamTerminalTypeMicrophone:
    case kAudioStreamTerminalTypeReceiverMicrophone:
      if (!(transport == kAudioDeviceTransportTypeUSB && duplex)) return Form::Microphone;
      break;
    default: break;
  }
  if (transport == kAudioDeviceTransportTypeUSB) {
    if (says_speaker) return input ? Form::Microphone : Form::Speakers;
    if (duplex || says_headset) return Form::Headset;
    if (says_headphones) return Form::Headphones;
    return input ? Form::Microphone : Form::Unknown;
  }
  if (transport == kAudioDeviceTransportTypeHDMI || transport == kAudioDeviceTransportTypeDisplayPort)
    return input ? Form::Unknown : Form::Speakers;
  if (says_headset) return Form::Headset;
  if (says_headphones && !input) return Form::Headphones;
  return Form::Unknown;
}

void fill_inventory(DeviceSnapshot& snap) {
  for (AudioObjectID dev : all_devices()) {
    const std::string uid = device_uid(dev);
    if (uid.empty()) continue;
    const std::string name = device_name(dev), manufacturer = device_manufacturer(dev);
    const uint32_t transport = transport_type(dev);
    if (is_blocked(name, manufacturer, transport)) continue;
    const int ins = channel_count(dev, true), outs = channel_count(dev, false);
    if (ins == 0 && outs == 0) continue;
    const Role role = uid == kVmicUID ? Role::VirtualMic : uid == kFeedUID ? Role::VirtualMicFeed : Role::Normal;
    // A duplex device (USB headset) becomes two entries with the same key, one per direction, so
    // channels and form are unambiguous for each list.
    for (int dir = 0; dir < 2; ++dir) {
      const bool input = dir == 0;
      const int ch = input ? ins : outs;
      if (ch == 0) continue;
      DeviceInfo d;
      d.key = uid;
      d.name = name;
      d.manufacturer = manufacturer;
      d.transport = transport_word(transport);
      d.input = input;
      d.output = !input;
      d.channels = ch;
      d.nominal_rate = nominal_rate(dev);
      d.form = role == Role::Normal ? form_of(dev, input, transport, name, ins > 0 && outs > 0) : Form::Unknown;
      d.role = role;
      snap.devices.push_back(std::move(d));
    }
  }
  // The feed is hidden from the device list; it is reported (role VirtualMicFeed) when present.
  if (AudioObjectID feed = device_for_uid(kFeedUID)) {
    bool listed = std::any_of(snap.devices.begin(), snap.devices.end(),
                              [](const DeviceInfo& d) { return d.key == kFeedUID; });
    int outs = channel_count(feed, false);
    if (!listed && outs > 0) {
      DeviceInfo d;
      d.key = kFeedUID;
      d.name = device_name(feed);
      d.manufacturer = device_manufacturer(feed);
      d.transport = "virtual";
      d.output = true;
      d.channels = outs;
      d.nominal_rate = nominal_rate(feed);
      d.role = Role::VirtualMicFeed;
      snap.devices.push_back(std::move(d));
    }
  }
  auto default_key = [](bool input) -> std::string {
    AudioObjectID dev = default_device(input);
    if (!dev || is_blocked_device(dev)) return {};
    return device_uid(dev);
  };
  snap.default_input = default_key(true);
  snap.default_output = default_key(false);
}

}  // namespace beamer::audio::mac
