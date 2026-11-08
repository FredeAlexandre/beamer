// SPDX-License-Identifier: GPL-3.0-only
// E2E "unplug" stand-in on the Mac: a public aggregate output device over the built-in speakers that
// lives as long as this process. It appears in Beamer's device list like a USB device; quitting
// (stdin EOF, SIGTERM or SIGINT) destroys it, which Beamer sees as the device going away.
//
//   clang -framework CoreAudio -framework CoreFoundation agg.c -o agg
//   agg [UID [NAME]]      default UID com.beamer.e2e.unplug, NAME "E2E Unplug"
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static CFStringRef default_output_uid(void) {
  AudioObjectPropertyAddress a = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                  kAudioObjectPropertyElementMain};
  AudioDeviceID dev = 0;
  UInt32 size = sizeof dev;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, NULL, &size, &dev) != noErr) return NULL;
  CFStringRef uid = NULL;
  a.mSelector = kAudioDevicePropertyDeviceUID;
  size = sizeof uid;
  if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &uid) != noErr) return NULL;
  return uid;
}

int main(int argc, char** argv) {
  const char* uid_c = argc > 1 ? argv[1] : "com.beamer.e2e.unplug";
  const char* name_c = argc > 2 ? argv[2] : "E2E Unplug";
  CFStringRef sub = default_output_uid();
  if (!sub) {
    fprintf(stderr, "{\"event\":\"error\",\"what\":\"no default output\"}\n");
    return 1;
  }
  CFStringRef uid = CFStringCreateWithCString(NULL, uid_c, kCFStringEncodingUTF8);
  CFStringRef name = CFStringCreateWithCString(NULL, name_c, kCFStringEncodingUTF8);
  CFMutableDictionaryRef subdev = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                            &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(subdev, CFSTR(kAudioSubDeviceUIDKey), sub);
  CFArrayRef subs = CFArrayCreate(NULL, (const void**)&subdev, 1, &kCFTypeArrayCallBacks);
  CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceUIDKey), uid);
  CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceNameKey), name);
  CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subs);
  CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), sub);
  int zero = 0;
  CFNumberRef no = CFNumberCreate(NULL, kCFNumberIntType, &zero);
  CFDictionarySetValue(desc, CFSTR(kAudioAggregateDeviceIsPrivateKey), no);
  AudioObjectID agg = 0;
  OSStatus st = AudioHardwareCreateAggregateDevice(desc, &agg);
  if (st != noErr) {
    fprintf(stderr, "{\"event\":\"error\",\"what\":\"create\",\"status\":%d}\n", (int)st);
    return 1;
  }
  printf("{\"event\":\"created\",\"id\":%u,\"uid\":\"%s\"}\n", agg, uid_c);
  fflush(stdout);
  struct sigaction sa = {0};
  sa.sa_handler = on_signal;  // no SA_RESTART: the signal ends the blocking read below
  sigaction(SIGTERM, &sa, NULL);
  sigaction(SIGINT, &sa, NULL);
  char buf[64];
  while (!g_stop) {
    ssize_t n = read(0, buf, sizeof buf);
    if (n <= 0) break;
  }
  st = AudioHardwareDestroyAggregateDevice(agg);
  printf("{\"event\":\"destroyed\",\"status\":%d}\n", (int)st);
  return st == noErr ? 0 : 1;
}
