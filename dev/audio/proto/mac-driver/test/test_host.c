// SPDX-License-Identifier: GPL-3.0-only
//
// Out-of-process test host for BeamerAudio.driver. Loads the built bundle the way coreaudiod does
// (Info.plist factory lookup, then the factory function from the bundle binary), initialises it
// with a stub AudioServerPlugInHostInterface and drives AudioServerPlugInDriverInterface:
// object/property enumeration, formats, sample-rate change, StartIO/StopIO, GetZeroTimeStamp and
// a real-time loopback (feed WriteMix -> mic ReadInput) on two threads.
//
// Usage: test_host [path/to/BeamerAudio.driver]
//
// Copyright (C) 2026 The Beamer authors. Licensed under the GNU GPL version 3 only.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Mirrors of driver constants (asserted against what the driver reports where it reports them).
enum { kObjPlugIn = 1, kObjMic = 2, kObjMicStream = 3, kObjFeed = 4, kObjFeedStream = 5 };
#define kPeriod        16384u      // kAudioDevicePropertyZeroTimeStampPeriod
#define kRingFrames    16384
#define kRingValid     (16384 - 4096)
#define kFactoryUUID   "D590F59B-06C8-4FA2-B549-9F9A6F1756BD"
#define kTypeUUID      "443ABAB8-E7B3-491A-B985-BEB9187030DB"

static int gFailures = 0, gChecks = 0;
#define CHECK(cond, ...) do { gChecks++; if (!(cond)) { gFailures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define SECTION(name) printf("== %s\n", name)

static AudioServerPlugInDriverRef D;   // driver
#define DRV (*D)

// ---------------------------------------------------------------------------------------------
// Stub host
// ---------------------------------------------------------------------------------------------

static int          gPropsChangedCalls;
static int          gConfigRequests;
static AudioObjectID gConfigDevices[8];
static UInt64       gConfigActions[8];

static OSStatus Host_PropertiesChanged(AudioServerPlugInHostRef h, AudioObjectID obj, UInt32 n, const AudioObjectPropertyAddress* a)
{ (void)h; (void)obj; (void)n; (void)a; gPropsChangedCalls++; return 0; }
static OSStatus Host_CopyFromStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef* out)
{ (void)h; (void)k; if (out) *out = NULL; return 0; }
static OSStatus Host_WriteToStorage(AudioServerPlugInHostRef h, CFStringRef k, CFPropertyListRef d)
{ (void)h; (void)k; (void)d; return 0; }
static OSStatus Host_DeleteFromStorage(AudioServerPlugInHostRef h, CFStringRef k)
{ (void)h; (void)k; return 0; }
static OSStatus Host_RequestConfigChange(AudioServerPlugInHostRef h, AudioObjectID dev, UInt64 action, void* info)
{
    (void)h; (void)info;
    if (gConfigRequests < 8) { gConfigDevices[gConfigRequests] = dev; gConfigActions[gConfigRequests] = action; }
    gConfigRequests++;
    return 0;
}

static AudioServerPlugInHostInterface gHost = {
    Host_PropertiesChanged, Host_CopyFromStorage, Host_WriteToStorage, Host_DeleteFromStorage, Host_RequestConfigChange,
};

// ---------------------------------------------------------------------------------------------
// Property helpers
// ---------------------------------------------------------------------------------------------

static AudioObjectPropertyAddress Addr(AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    return a;
}

static OSStatus GetProp(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope, UInt32 size, void* out, UInt32* outSize)
{
    AudioObjectPropertyAddress a = Addr(sel, scope);
    UInt32 got = 0;
    OSStatus err = DRV->GetPropertyData(D, obj, getpid(), &a, 0, NULL, size, &got, out);
    if (outSize) *outSize = got;
    return err;
}

static UInt32 GetU32(AudioObjectID obj, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope)
{
    UInt32 v = 0xDEADBEEF;
    OSStatus err = GetProp(obj, sel, scope, sizeof(v), &v, NULL);
    CHECK(err == 0, "GetU32 obj %u sel '%.4s' err %d", obj, (char*)&(UInt32){CFSwapInt32HostToBig(sel)}, (int)err);
    return v;
}

static Float64 GetF64(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    Float64 v = -1;
    OSStatus err = GetProp(obj, sel, kAudioObjectPropertyScopeGlobal, sizeof(v), &v, NULL);
    CHECK(err == 0, "GetF64 err %d", (int)err);
    return v;
}

// Returns a malloc'd C string (caller frees) for a CFString property.
static char* GetStr(AudioObjectID obj, AudioObjectPropertySelector sel)
{
    CFStringRef s = NULL;
    OSStatus err = GetProp(obj, sel, kAudioObjectPropertyScopeGlobal, sizeof(s), &s, NULL);
    CHECK(err == 0 && s != NULL, "GetStr err %d", (int)err);
    char buf[256] = "";
    if (s) { CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8); CFRelease(s); }
    return strdup(buf);
}

static void CheckStr(AudioObjectID obj, AudioObjectPropertySelector sel, const char* expected, const char* what)
{
    char* s = GetStr(obj, sel);
    CHECK(strcmp(s, expected) == 0, "%s: got '%s', expected '%s'", what, s, expected);
    printf("  %-28s %s\n", what, s);
    free(s);
}

static AudioObjectID TranslateUID(const char* uid)
{
    CFStringRef s = CFStringCreateWithCString(NULL, uid, kCFStringEncodingUTF8);
    AudioObjectPropertyAddress a = Addr(kAudioPlugInPropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal);
    AudioObjectID dev = 12345;
    UInt32 got = 0;
    OSStatus err = DRV->GetPropertyData(D, kObjPlugIn, getpid(), &a, sizeof(s), &s, sizeof(dev), &got, &dev);
    CHECK(err == 0 && got == sizeof(dev), "TranslateUID '%s' err %d", uid, (int)err);
    CFRelease(s);
    return dev;
}

// ---------------------------------------------------------------------------------------------
// Loading (what coreaudiod does)
// ---------------------------------------------------------------------------------------------

static void LoadDriver(const char* bundlePath)
{
    SECTION("load bundle");
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)bundlePath, (CFIndex)strlen(bundlePath), true);
    CFBundleRef bundle = CFBundleCreate(NULL, url);
    if (!bundle) { printf("  FAIL cannot open bundle %s\n", bundlePath); exit(2); }

    CFStringRef bid = CFBundleGetIdentifier(bundle);
    CHECK(bid && CFStringCompare(bid, CFSTR("com.beamer.audio.driver"), 0) == 0, "CFBundleIdentifier");

    // CFPlugInTypes[kAudioServerPlugInTypeUUID] must list our factory, and CFPlugInFactories must
    // map it to a function name.
    CFDictionaryRef info = CFBundleGetInfoDictionary(bundle);
    CFDictionaryRef types = CFDictionaryGetValue(info, CFSTR("CFPlugInTypes"));
    CFDictionaryRef factories = CFDictionaryGetValue(info, CFSTR("CFPlugInFactories"));
    CFArrayRef list = types ? CFDictionaryGetValue(types, CFSTR(kTypeUUID)) : NULL;
    CHECK(list && CFArrayGetCount(list) == 1 && CFStringCompare(CFArrayGetValueAtIndex(list, 0), CFSTR(kFactoryUUID), 0) == 0,
          "CFPlugInTypes does not map kAudioServerPlugInTypeUUID to the factory");
    CFStringRef fn = factories ? CFDictionaryGetValue(factories, CFSTR(kFactoryUUID)) : NULL;
    char fname[128] = "";
    if (fn) CFStringGetCString(fn, fname, sizeof(fname), kCFStringEncodingUTF8);
    CHECK(fname[0] != 0, "CFPlugInFactories has no function for the factory UUID");
    printf("  factory %s -> %s\n", kFactoryUUID, fname);

    CFURLRef exeURL = CFBundleCopyExecutableURL(bundle);
    char exe[1024];
    CFURLGetFileSystemRepresentation(exeURL, true, (UInt8*)exe, sizeof(exe));
    CFRelease(exeURL);
    void* lib = dlopen(exe, RTLD_NOW | RTLD_LOCAL);
    if (!lib) { printf("  FAIL dlopen: %s\n", dlerror()); exit(2); }
    printf("  dlopen %s\n", exe);

    typedef void* (*FactoryFn)(CFAllocatorRef, CFUUIDRef);
    FactoryFn factory = (FactoryFn)dlsym(lib, fname);
    if (!factory) { printf("  FAIL dlsym %s\n", fname); exit(2); }

    CHECK(factory(NULL, kAudioServerPlugInDriverInterfaceUUID) == NULL, "factory must reject a non-type UUID");
    void* obj = factory(NULL, kAudioServerPlugInTypeUUID);
    if (!obj) { printf("  FAIL factory returned NULL\n"); exit(2); }

    // QueryInterface for the driver interface, as the CFPlugIn machinery does.
    AudioServerPlugInDriverInterface** iunk = obj;
    void* drv = NULL;
    HRESULT hr = (*iunk)->QueryInterface(obj, CFUUIDGetUUIDBytes(kAudioServerPlugInDriverInterfaceUUID), &drv);
    CHECK(hr == S_OK && drv == obj, "QueryInterface(driver interface)");
    void* none = (void*)1;
    hr = (*iunk)->QueryInterface(obj, CFUUIDGetUUIDBytes(kAudioServerPlugInTypeUUID), &none);
    CHECK(hr == E_NOINTERFACE, "QueryInterface(unknown) must fail");
    ULONG r1 = (*iunk)->AddRef(obj), r2 = (*iunk)->Release(obj);
    CHECK(r2 == r1 - 1, "AddRef/Release");
    (*iunk)->Release(obj);   // balance QueryInterface
    D = drv;

    CHECK(DRV->Initialize(D, &gHost) == 0, "Initialize");
    CFRelease(bundle);
    CFRelease(url);
}

// ---------------------------------------------------------------------------------------------
// Object model
// ---------------------------------------------------------------------------------------------

static void TestPlugInObject(void)
{
    SECTION("plug-in object");
    CHECK(GetU32(kObjPlugIn, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioPlugInClassID, "class");
    CheckStr(kObjPlugIn, kAudioObjectPropertyManufacturer, "Beamer", "manufacturer");

    AudioObjectID devs[4] = {0};
    UInt32 sz = 0;
    CHECK(GetProp(kObjPlugIn, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, sizeof(devs), devs, &sz) == 0, "device list");
    CHECK(sz == 2 * sizeof(AudioObjectID) && devs[0] == kObjMic && devs[1] == kObjFeed, "device list = [mic, feed]");
    printf("  devices                      %u, %u\n", devs[0], devs[1]);

    // Truncated buffer: the driver must clip, not overrun.
    AudioObjectID one[2] = { 0, 0xAAAAAAAA };
    CHECK(GetProp(kObjPlugIn, kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, sizeof(AudioObjectID), one, &sz) == 0 &&
          sz == sizeof(AudioObjectID) && one[1] == 0xAAAAAAAA, "device list clips to buffer size");

    AudioObjectID mic = TranslateUID("com.beamer.audio.mic");
    AudioObjectID feed = TranslateUID("com.beamer.audio.micfeed");
    AudioObjectID bogus = TranslateUID("com.example.nope");
    CHECK(mic == kObjMic, "UID mic -> %u", mic);
    CHECK(feed == kObjFeed, "UID micfeed -> %u", feed);
    CHECK(bogus == kAudioObjectUnknown, "unknown UID -> %u", bogus);
    printf("  TranslateUIDToDevice         mic=%u micfeed=%u unknown=%u\n", mic, feed, bogus);
}

static void CheckFormat(const AudioStreamBasicDescription* f, Float64 rate, const char* what)
{
    CHECK(f->mSampleRate == rate && f->mFormatID == kAudioFormatLinearPCM &&
          (f->mFormatFlags & kAudioFormatFlagIsFloat) && (f->mFormatFlags & kAudioFormatFlagIsPacked) &&
          f->mChannelsPerFrame == 1 && f->mBitsPerChannel == 32 && f->mBytesPerFrame == 4 && f->mFramesPerPacket == 1,
          "%s: unexpected format %.0f Hz %u ch %u bit", what, f->mSampleRate, f->mChannelsPerFrame, f->mBitsPerChannel);
}

static void TestDevice(AudioObjectID dev, AudioObjectID stream, int isInput)
{
    const char* name = isInput ? "Beamer Microphone" : "Beamer Mic Feed";
    const char* uid  = isInput ? "com.beamer.audio.mic" : "com.beamer.audio.micfeed";
    printf("== device %u (%s)\n", dev, name);
    CHECK(GetU32(dev, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioDeviceClassID, "device class");
    CHECK(GetU32(dev, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == kObjPlugIn, "owner");
    CheckStr(dev, kAudioObjectPropertyName, name, "name");
    CheckStr(dev, kAudioObjectPropertyManufacturer, "Beamer", "manufacturer");
    CheckStr(dev, kAudioDevicePropertyDeviceUID, uid, "UID");
    CheckStr(dev, kAudioDevicePropertyModelUID, "com.beamer.audio.model", "model UID");

    UInt32 hidden = GetU32(dev, kAudioDevicePropertyIsHidden, kAudioObjectPropertyScopeGlobal);
    CHECK(hidden == (isInput ? 0u : 1u), "IsHidden = %u", hidden);
    UInt32 transport = GetU32(dev, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal);
    CHECK(transport == kAudioDeviceTransportTypeVirtual, "transport");
    UInt32 canDefault = GetU32(dev, kAudioDevicePropertyDeviceCanBeDefaultDevice, isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput);
    CHECK(canDefault == (isInput ? 1u : 0u), "CanBeDefaultDevice = %u", canDefault);
    CHECK(GetU32(dev, kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioObjectPropertyScopeGlobal) == 0, "CanBeDefaultSystemDevice");
    CHECK(GetU32(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal) == 1, "alive");
    CHECK(GetU32(dev, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 0, "not running before StartIO");
    CHECK(GetU32(dev, kAudioDevicePropertyZeroTimeStampPeriod, kAudioObjectPropertyScopeGlobal) == kPeriod, "ZTS period");
    UInt32 domain = GetU32(dev, kAudioDevicePropertyClockDomain, kAudioObjectPropertyScopeGlobal);
    printf("  %-28s %u  hidden=%u canBeDefault=%u transport='%.4s'\n", "clock domain", domain, hidden, canDefault,
           (char*)&(UInt32){CFSwapInt32HostToBig(transport)});

    // Streams: exactly one, in the device's direction only.
    AudioObjectPropertyScope mine = isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
    AudioObjectPropertyScope other = isInput ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput;
    AudioObjectID streams[4]; UInt32 sz = 0;
    CHECK(GetProp(dev, kAudioDevicePropertyStreams, mine, sizeof(streams), streams, &sz) == 0 && sz == sizeof(AudioObjectID) && streams[0] == stream,
          "streams in own scope");
    CHECK(GetProp(dev, kAudioDevicePropertyStreams, other, sizeof(streams), streams, &sz) == 0 && sz == 0, "no streams in other scope");
    CHECK(GetProp(dev, kAudioObjectPropertyControlList, kAudioObjectPropertyScopeGlobal, sizeof(streams), streams, &sz) == 0 && sz == 0, "no controls");

    UInt32 stereo[2] = {0, 0};
    CHECK(GetProp(dev, kAudioDevicePropertyPreferredChannelsForStereo, mine, sizeof(stereo), stereo, &sz) == 0 && stereo[0] == 1 && stereo[1] == 1, "stereo pair = 1,1");
    UInt32 latency = GetU32(dev, kAudioDevicePropertyLatency, mine);
    UInt32 safety = GetU32(dev, kAudioDevicePropertySafetyOffset, mine);
    printf("  %-28s latency=%u safety=%u\n", "timing", latency, safety);

    CHECK(GetF64(dev, kAudioDevicePropertyNominalSampleRate) == 48000.0, "nominal rate 48 kHz");
    AudioValueRange rates[4]; sz = 0;
    CHECK(GetProp(dev, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, sizeof(rates), rates, &sz) == 0, "available rates");
    UInt32 nr = sz / sizeof(AudioValueRange);
    CHECK(nr == 2 && rates[0].mMinimum == 48000.0 && rates[1].mMinimum == 44100.0, "rates = {48000, 44100}");
    printf("  %-28s", "available rates"); for (UInt32 i = 0; i < nr; i++) printf(" %.0f", rates[i].mMinimum); printf("\n");

    Boolean settable = false;
    AudioObjectPropertyAddress a = Addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    CHECK(DRV->IsPropertySettable(D, dev, getpid(), &a, &settable) == 0 && settable, "nominal rate settable");
    a = Addr(kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal);
    CHECK(DRV->IsPropertySettable(D, dev, getpid(), &a, &settable) == 0 && !settable, "name not settable");

    // Stream object.
    CHECK(GetU32(stream, kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal) == kAudioStreamClassID, "stream class");
    CHECK(GetU32(stream, kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal) == dev, "stream owner");
    CHECK(GetU32(stream, kAudioStreamPropertyDirection, kAudioObjectPropertyScopeGlobal) == (isInput ? 1u : 0u), "stream direction");
    CHECK(GetU32(stream, kAudioStreamPropertyStartingChannel, kAudioObjectPropertyScopeGlobal) == 1, "starting channel");
    CHECK(GetU32(stream, kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal) == 1, "stream active");
    AudioStreamBasicDescription f;
    CHECK(GetProp(stream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(f), &f, &sz) == 0, "virtual format");
    CheckFormat(&f, 48000.0, "virtual format");
    CHECK(GetProp(stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(f), &f, &sz) == 0, "physical format");
    CheckFormat(&f, 48000.0, "physical format");
    AudioStreamRangedDescription fmts[4];
    CHECK(GetProp(stream, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, sizeof(fmts), fmts, &sz) == 0 &&
          sz == 2 * sizeof(AudioStreamRangedDescription), "2 available formats");
    CheckFormat(&fmts[0].mFormat, 48000.0, "available[0]");
    CheckFormat(&fmts[1].mFormat, 44100.0, "available[1]");
    printf("  %-28s Float32 mono %.0f Hz (also %.0f Hz)\n", "stream format", f.mSampleRate, fmts[1].mFormat.mSampleRate);
}

// Sweep: every selector on every object/scope must be self-consistent (HasProperty <=> size and
// data succeed, data never exceeds the announced size, buffers are not overrun).
static void TestPropertySweep(void)
{
    SECTION("property sweep");
    static const AudioObjectPropertySelector sels[] = {
        kAudioObjectPropertyBaseClass, kAudioObjectPropertyClass, kAudioObjectPropertyOwner, kAudioObjectPropertyName,
        kAudioObjectPropertyModelName, kAudioObjectPropertyManufacturer, kAudioObjectPropertyElementName,
        kAudioObjectPropertyOwnedObjects, kAudioObjectPropertyIdentify, kAudioObjectPropertySerialNumber,
        kAudioObjectPropertyFirmwareVersion, kAudioObjectPropertyControlList, kAudioObjectPropertyCustomPropertyInfoList,
        kAudioPlugInPropertyBundleID, kAudioPlugInPropertyDeviceList, kAudioPlugInPropertyBoxList,
        kAudioPlugInPropertyClockDeviceList, kAudioPlugInPropertyResourceBundle,
        kAudioDevicePropertyConfigurationApplication, kAudioDevicePropertyDeviceUID, kAudioDevicePropertyModelUID,
        kAudioDevicePropertyTransportType, kAudioDevicePropertyRelatedDevices, kAudioDevicePropertyClockDomain,
        kAudioDevicePropertyDeviceIsAlive, kAudioDevicePropertyDeviceIsRunning, kAudioDevicePropertyDeviceCanBeDefaultDevice,
        kAudioDevicePropertyDeviceCanBeDefaultSystemDevice, kAudioDevicePropertyLatency, kAudioDevicePropertyStreams,
        kAudioDevicePropertySafetyOffset, kAudioDevicePropertyNominalSampleRate, kAudioDevicePropertyAvailableNominalSampleRates,
        kAudioDevicePropertyIcon, kAudioDevicePropertyIsHidden, kAudioDevicePropertyPreferredChannelsForStereo,
        kAudioDevicePropertyPreferredChannelLayout, kAudioDevicePropertyZeroTimeStampPeriod, kAudioDevicePropertyClockIsStable,
        kAudioDevicePropertyClockAlgorithm, (AudioObjectPropertySelector)'volm', (AudioObjectPropertySelector)'mute',
        kAudioStreamPropertyIsActive, kAudioStreamPropertyDirection, kAudioStreamPropertyTerminalType,
        kAudioStreamPropertyStartingChannel, kAudioStreamPropertyLatency, kAudioStreamPropertyVirtualFormat,
        kAudioStreamPropertyAvailableVirtualFormats, kAudioStreamPropertyPhysicalFormat, kAudioStreamPropertyAvailablePhysicalFormats,
        'zzzz',
    };
    static const AudioObjectPropertyScope scopes[] = { kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput };
    // Properties that hand out a CFTypeRef the caller must release.
    static const AudioObjectPropertySelector cfSels[] = {
        kAudioObjectPropertyName, kAudioObjectPropertyManufacturer, kAudioDevicePropertyDeviceUID,
        kAudioDevicePropertyModelUID, kAudioPlugInPropertyResourceBundle,
    };
    int supported = 0;
    for (AudioObjectID obj = kObjPlugIn; obj <= kObjFeedStream; obj++) {
        for (size_t si = 0; si < sizeof(scopes) / sizeof(scopes[0]); si++) {
            for (size_t i = 0; i < sizeof(sels) / sizeof(sels[0]); i++) {
                AudioObjectPropertyAddress a = Addr(sels[i], scopes[si]);
                Boolean has = DRV->HasProperty(D, obj, getpid(), &a);
                UInt32 size = 0;
                OSStatus e1 = DRV->GetPropertyDataSize(D, obj, getpid(), &a, 0, NULL, &size);
                if (!has) {
                    CHECK(e1 != 0, "obj %u sel '%.4s' not advertised but size succeeds", obj, (char*)&(UInt32){CFSwapInt32HostToBig(sels[i])});
                    continue;
                }
                if (sels[i] == kAudioPlugInPropertyTranslateUIDToDevice) continue;   // needs a qualifier
                supported++;
                CHECK(e1 == 0, "obj %u sel '%.4s' advertised but size fails (%d)", obj, (char*)&(UInt32){CFSwapInt32HostToBig(sels[i])}, (int)e1);
                unsigned char buf[512 + 16];
                memset(buf, 0xA5, sizeof(buf));
                UInt32 got = 0xFFFF;
                OSStatus e2 = DRV->GetPropertyData(D, obj, getpid(), &a, 0, NULL, size, &got, size ? buf : buf);
                CHECK(e2 == 0 && got <= size, "obj %u sel '%.4s' data err %d got %u size %u", obj, (char*)&(UInt32){CFSwapInt32HostToBig(sels[i])}, (int)e2, got, size);
                CHECK(buf[size] == 0xA5, "obj %u sel '%.4s' overran its buffer", obj, (char*)&(UInt32){CFSwapInt32HostToBig(sels[i])});
                for (size_t c = 0; c < sizeof(cfSels) / sizeof(cfSels[0]); c++)
                    if (cfSels[c] == sels[i] && e2 == 0) CFRelease(*(CFTypeRef*)buf);
            }
        }
    }
    printf("  %d (object, scope, selector) combinations answered consistently\n", supported);

    // Bad object IDs.
    AudioObjectPropertyAddress a = Addr(kAudioObjectPropertyClass, kAudioObjectPropertyScopeGlobal);
    UInt32 v, got;
    CHECK(!DRV->HasProperty(D, 99, getpid(), &a), "bad object HasProperty");
    CHECK(DRV->GetPropertyData(D, 99, getpid(), &a, 0, NULL, sizeof(v), &got, &v) == kAudioHardwareBadObjectError, "bad object GetPropertyData");
    CHECK(DRV->StartIO(D, kObjMicStream, 1) != 0, "StartIO on a stream must fail");
    CHECK(DRV->StopIO(D, kObjMic, 1) != 0, "StopIO without StartIO must fail");
    // Too-small buffer.
    Float64 small;
    a = Addr(kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal);
    CHECK(DRV->GetPropertyData(D, kObjMicStream, getpid(), &a, 0, NULL, sizeof(small), &got, &small) == kAudioHardwareBadPropertySizeError, "short buffer rejected");
}

// ---------------------------------------------------------------------------------------------
// IO helpers
// ---------------------------------------------------------------------------------------------

static Float64 gTicksPerSec;
static Float64 gRate = 48000.0;

static void Cycle(AudioServerPlugInIOCycleInfo* c, Float64 inputTime, Float64 outputTime, UInt32 frames)
{
    memset(c, 0, sizeof(*c));
    c->mNominalIOBufferFrameSize = frames;
    c->mInputTime.mSampleTime  = inputTime;
    c->mOutputTime.mSampleTime = outputTime;
    c->mInputTime.mFlags = c->mOutputTime.mFlags = kAudioTimeStampSampleTimeValid;
}

static OSStatus FeedWrite(int64_t t, const Float32* buf, UInt32 n)
{
    AudioServerPlugInIOCycleInfo c; Cycle(&c, 0, (Float64)t, n);
    Boolean will, inPlace;
    DRV->WillDoIOOperation(D, kObjFeed, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace);
    if (!will) return -1;
    DRV->BeginIOOperation(D, kObjFeed, 1, kAudioServerPlugInIOOperationWriteMix, n, &c);
    OSStatus e = DRV->DoIOOperation(D, kObjFeed, kObjFeedStream, 1, kAudioServerPlugInIOOperationWriteMix, n, &c, (void*)buf, NULL);
    DRV->EndIOOperation(D, kObjFeed, 1, kAudioServerPlugInIOOperationWriteMix, n, &c);
    return e;
}

static OSStatus MicRead(int64_t t, Float32* buf, UInt32 n)
{
    AudioServerPlugInIOCycleInfo c; Cycle(&c, (Float64)t, 0, n);
    Boolean will, inPlace;
    DRV->WillDoIOOperation(D, kObjMic, 2, kAudioServerPlugInIOOperationReadInput, &will, &inPlace);
    if (!will) return -1;
    for (UInt32 i = 0; i < n; i++) buf[i] = 12345.0f;   // poison: the driver must overwrite every frame
    DRV->BeginIOOperation(D, kObjMic, 2, kAudioServerPlugInIOOperationReadInput, n, &c);
    OSStatus e = DRV->DoIOOperation(D, kObjMic, kObjMicStream, 2, kAudioServerPlugInIOOperationReadInput, n, &c, buf, NULL);
    DRV->EndIOOperation(D, kObjMic, 2, kAudioServerPlugInIOOperationReadInput, n, &c);
    return e;
}

// The test signal: a 440 Hz sine indexed by absolute sample time, so any misalignment shows.
static inline Float32 Signal(int64_t t)
{
    Float32 v = (Float32)(0.5 * sin(2.0 * M_PI * 440.0 * (double)t / gRate));
    return v == 0.0f ? 1e-6f : v;   // never exactly 0, so "silence" and "signal" are distinguishable
}

// Current device sample time from the driver's zero timestamp (what the HAL computes).
static Float64 NowSampleTime(AudioObjectID dev)
{
    Float64 st; UInt64 ht, seed;
    DRV->GetZeroTimeStamp(D, dev, 1, &st, &ht, &seed);
    UInt64 now = mach_absolute_time();
    // Signed: a broken driver could hand out a zero timestamp in the future.
    return st + (Float64)((int64_t)(now - ht)) * gRate / gTicksPerSec;
}

static void SleepUntilSample(AudioObjectID dev, Float64 target)
{
    UInt64 start = mach_absolute_time();
    for (;;) {
        Float64 now = NowSampleTime(dev);
        if (now >= target) return;
        if ((Float64)(mach_absolute_time() - start) / gTicksPerSec > 2.0) {
            gChecks++; gFailures++;
            printf("  FAIL clock stalled: waited 2 s for sample time %.0f (now %.0f)\n", target, now);
            return;
        }
        Float64 us = (target - now) * 1e6 / gRate;
        usleep(us > 2000 ? 1000 : (useconds_t)(us > 50 ? us / 2 : 20));
    }
}

static void StartBoth(void)
{
    CHECK(DRV->StartIO(D, kObjMic, 2) == 0, "StartIO mic");
    CHECK(DRV->StartIO(D, kObjFeed, 1) == 0, "StartIO feed");
}

static void StopBoth(void)
{
    CHECK(DRV->StopIO(D, kObjFeed, 1) == 0, "StopIO feed");
    CHECK(DRV->StopIO(D, kObjMic, 2) == 0, "StopIO mic");
}

// ---------------------------------------------------------------------------------------------
// IO tests
// ---------------------------------------------------------------------------------------------

static void TestIOOperations(void)
{
    SECTION("IO operation routing");
    Boolean will = 9, inPlace = 9;
    DRV->WillDoIOOperation(D, kObjMic, 1, kAudioServerPlugInIOOperationReadInput, &will, &inPlace);
    CHECK(will && inPlace, "mic does ReadInput in place");
    DRV->WillDoIOOperation(D, kObjMic, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace);
    CHECK(!will, "mic does not WriteMix");
    DRV->WillDoIOOperation(D, kObjFeed, 1, kAudioServerPlugInIOOperationWriteMix, &will, &inPlace);
    CHECK(will && inPlace, "feed does WriteMix in place");
    DRV->WillDoIOOperation(D, kObjFeed, 1, kAudioServerPlugInIOOperationReadInput, &will, &inPlace);
    CHECK(!will, "feed does not ReadInput");
    static const UInt32 others[] = { kAudioServerPlugInIOOperationThread, kAudioServerPlugInIOOperationCycle,
        kAudioServerPlugInIOOperationConvertInput, kAudioServerPlugInIOOperationProcessInput,
        kAudioServerPlugInIOOperationProcessOutput, kAudioServerPlugInIOOperationMixOutput,
        kAudioServerPlugInIOOperationProcessMix, kAudioServerPlugInIOOperationConvertMix };
    int anyOther = 0;
    for (size_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        DRV->WillDoIOOperation(D, kObjMic, 1, others[i], &will, &inPlace);  anyOther |= will;
        DRV->WillDoIOOperation(D, kObjFeed, 1, others[i], &will, &inPlace); anyOther |= will;
    }
    CHECK(!anyOther, "no other IO operations claimed");
    printf("  mic: ReadInput only; feed: WriteMix only\n");
}

static void TestClock(void)
{
    SECTION("clock (GetZeroTimeStamp)");
    StartBoth();
    CHECK(GetU32(kObjMic, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 1, "mic running");
    CHECK(GetU32(kObjFeed, kAudioDevicePropertyDeviceIsRunning, kAudioObjectPropertyScopeGlobal) == 1, "feed running");

    Float64 s0, s1, firstS = -1, lastS = -1; UInt64 h0, h1, seed, firstH = 0, lastH = 0;
    int periods = 0, sameDomain = 1, sane = 1, monotonic = 1;
    UInt64 startT = mach_absolute_time();
    Float64 ticksPerPeriod = gTicksPerSec * kPeriod / gRate;
    Float64 duration = 3.2 * kPeriod / gRate;   // see at least 3 period boundaries
    while ((Float64)(mach_absolute_time() - startT) / gTicksPerSec < duration) {
        DRV->GetZeroTimeStamp(D, kObjMic, 2, &s0, &h0, &seed);
        DRV->GetZeroTimeStamp(D, kObjFeed, 1, &s1, &h1, &seed);
        UInt64 now = mach_absolute_time();
        if (s0 == s1 && h0 != h1) sameDomain = 0;
        if (h0 > now || (Float64)(now - h0) > ticksPerPeriod * 1.001) sane = 0;
        if (fmod(s0, kPeriod) != 0) sane = 0;
        if (lastS >= 0 && (s0 < lastS || h0 < lastH)) monotonic = 0;
        if (lastS >= 0 && s0 != lastS) {
            CHECK(s0 - lastS == kPeriod, "sample time advanced by %.0f", s0 - lastS);
            periods++;
            if (firstS < 0) { firstS = s0; firstH = h0; }
        }
        lastS = s0; lastH = h0;
        usleep(1000);
    }
    CHECK(sameDomain, "mic and feed report identical zero timestamps");
    CHECK(sane, "zero timestamp is recent, in the past, and on the period grid");
    CHECK(monotonic, "zero timestamps monotonic");
    CHECK(periods >= 3, "observed %d period boundaries", periods);
    if (periods >= 2) {
        Float64 measuredRate = (lastS - firstS) / ((Float64)(lastH - firstH) / gTicksPerSec);
        CHECK(fabs(measuredRate - gRate) < 0.01, "clock rate %.4f Hz", measuredRate);
        printf("  %d periods of %u frames, measured rate %.4f Hz, mic/feed timestamps identical\n", periods, kPeriod, measuredRate);
    }
    StopBoth();
}

// Deterministic ring semantics (IO ops take their sample times from the cycle info, so we can
// place writes and reads anywhere on the timeline).
static void TestRingSemantics(void)
{
    SECTION("loopback semantics (synthetic timeline)");
    StartBoth();   // fresh timeline: ring cleared, write position 0
    enum { N = 512 };
    Float32 w[N], r[N];
    int ok;

    MicRead(0, r, N);
    ok = 1; for (int i = 0; i < N; i++) ok &= r[i] == 0.0f;
    CHECK(ok, "silence when nothing was ever fed");

    for (int i = 0; i < N; i++) w[i] = Signal(1000 + i);
    CHECK(FeedWrite(1000, w, N) == 0, "WriteMix");
    MicRead(1000, r, N);
    ok = 1; for (int i = 0; i < N; i++) ok &= r[i] == w[i];
    CHECK(ok, "read back exactly what was written at the same sample time");
    MicRead(1000 - 100, r, N);
    ok = 1; for (int i = 0; i < 100; i++) ok &= r[i] == 0.0f;
    for (int i = 100; i < N; i++) ok &= r[i] == w[i - 100];
    CHECK(ok, "misaligned read: hole before the write is silent, rest aligned");
    MicRead(1000 + N, r, N);
    ok = 1; for (int i = 0; i < N; i++) ok &= r[i] == 0.0f;
    CHECK(ok, "silence past the write position (feed not ahead)");

    // Gap: next write jumps forward; the hole must be silent, not old ring content.
    for (int i = 0; i < N; i++) w[i] = Signal(1000 + kRingFrames + 2000 + i);
    FeedWrite(1000 + kRingFrames + 2000, w, N);
    MicRead(1000 + kRingFrames, r, N);   // same ring slots as the first write, one lap later
    ok = 1; for (int i = 0; i < N; i++) ok &= r[i] == 0.0f;
    CHECK(ok, "one lap later: slots of the old write read as silence (no stale replay)");
    MicRead(1000, r, N);
    ok = 1; for (int i = 0; i < N; i++) ok &= r[i] == 0.0f;
    CHECK(ok, "times older than the valid window read as silence");

    // Feed stops (no more writes): reading on past the write position stays silent forever.
    int64_t end = 1000 + kRingFrames + 2000 + N;
    int nonzero = 0;
    for (int64_t t = end; t < end + 3 * kRingFrames; t += N) { MicRead(t, r, N); for (int i = 0; i < N; i++) nonzero += r[i] != 0.0f; }
    CHECK(nonzero == 0, "feed stopped: %d non-silent frames over 3 laps", nonzero);

    // Randomised model check: random forward jumps, contiguous runs, small rewinds, random reads.
    // Model: value at time s = last value written for exactly s; a read returns it iff s is in
    // [writeEnd - valid, writeEnd), otherwise silence.
    enum { SPAN = 1 << 21 };
    Float32* model = calloc(SPAN, sizeof(Float32));
    int64_t base = end + 4 * kRingFrames, wend = base, mismatches = 0, compared = 0, withData = 0;
    srand(1234);
    // Start from a clean, known state: after a jump > one lap the driver clears the whole ring.
    for (int iter = 0; iter < 20000 && wend < base + SPAN - 2 * kRingFrames; iter++) {
        int op = rand() % 10;
        if (op < 6) {
            int64_t t;
            int r10 = rand() % 10;
            if (r10 < 6) t = wend;                                    // contiguous
            else if (r10 < 9) t = wend + rand() % (kRingFrames + 4000); // forward jump (some > 1 lap)
            else t = wend - rand() % 256;                             // small rewind/overlap
            if (t < base) t = base;
            UInt32 n = 1 + (UInt32)(rand() % 1024);
            Float32 buf[1024];
            for (UInt32 i = 0; i < n; i++) { buf[i] = (Float32)(1 + ((t + i) * 7 + iter * 13) % 4093); model[t + i - base] = buf[i]; }
            if (t > wend) for (int64_t s = wend; s < t; s++) model[s - base] = 0.0f;   // hole
            FeedWrite(t, buf, n);
            if (t + n > wend) wend = t + n;
        } else {
            int64_t t = wend - (kRingFrames + 2000) + rand() % (kRingFrames + 4000);
            UInt32 n = 1 + (UInt32)(rand() % 1024);
            Float32 buf[1024];
            MicRead(t, buf, n);
            for (UInt32 i = 0; i < n; i++) {
                int64_t s = t + i;
                Float32 expect = (s >= wend - kRingValid && s < wend && s >= base) ? model[s - base] : 0.0f;
                compared++; withData += expect != 0.0f;
                if (buf[i] != expect) mismatches++;
            }
        }
    }
    free(model);
    CHECK(mismatches == 0, "model check: %lld mismatches", (long long)mismatches);
    printf("  model check: %lld frames compared (%lld carrying data), %lld mismatches\n", (long long)compared, (long long)withData, (long long)mismatches);
    StopBoth();
}

// Real-time loopback on two threads, like coreaudiod's two device IO threads.
typedef struct {
    UInt32  frames;
    Float64 seconds;
    _Atomic int stop;
    _Atomic int64_t firstWrite, writeEnd;
    int64_t cycles;
} FeedArgs;

static void* FeedThread(void* p)
{
    FeedArgs* a = p;
    Float32 buf[4096];
    Float64 next = ceil(NowSampleTime(kObjFeed) / a->frames) * a->frames;
    a->firstWrite = -1;
    while (!atomic_load(&a->stop)) {
        SleepUntilSample(kObjFeed, next);
        // HAL-style output time: one buffer ahead of "now".
        int64_t out = (int64_t)next + a->frames;
        for (UInt32 i = 0; i < a->frames; i++) buf[i] = Signal(out + i);
        FeedWrite(out, buf, a->frames);
        if (a->firstWrite < 0) a->firstWrite = out;
        a->writeEnd = out + a->frames;
        a->cycles++;
        next += a->frames;
    }
    return NULL;
}

static void TestRealtimeLoopback(Float64 rate)
{
    char title[96];
    snprintf(title, sizeof(title), "real-time loopback, 2 threads, %.0f Hz, 512-frame cycles", rate);
    SECTION(title);
    StartBoth();
    const UInt32 F = 512;
    Float32 r[4096];

    // Mic alone for a few cycles: silence.
    int64_t in = (int64_t)(ceil(NowSampleTime(kObjMic) / F) * F);
    int nonzero = 0;
    for (int k = 0; k < 10; k++) {
        SleepUntilSample(kObjMic, (Float64)in + F);
        MicRead(in - F, r, F);   // HAL-style input time: one buffer behind "now"
        for (UInt32 i = 0; i < F; i++) nonzero += r[i] != 0.0f;
        in += F;
    }
    CHECK(nonzero == 0, "mic running, feed silent: %d non-zero frames", nonzero);

    FeedArgs fa = { .frames = F, .seconds = 2.0 };
    pthread_t th;
    pthread_create(&th, NULL, FeedThread, &fa);

    int64_t matched = 0, dropouts = 0, corrupt = 0, readFrames = 0, firstSignalAt = -1;
    UInt64 t0 = mach_absolute_time();
    while ((Float64)(mach_absolute_time() - t0) / gTicksPerSec < 2.0) {
        SleepUntilSample(kObjMic, (Float64)in + F);
        int64_t t = in - F;
        MicRead(t, r, F);
        int64_t fw = fa.firstWrite;
        for (UInt32 i = 0; i < F; i++) {
            int64_t s = t + i;
            readFrames++;
            if (r[i] == 0.0f) { if (fw >= 0 && s >= fw + F) dropouts++; continue; }
            if (r[i] == Signal(s)) { matched++; if (firstSignalAt < 0) firstSignalAt = s; }
            else corrupt++;
        }
        in += F;
    }
    atomic_store(&fa.stop, 1);
    pthread_join(th, NULL);
    CHECK(DRV->StopIO(D, kObjFeed, 1) == 0, "StopIO feed while mic keeps running");

    // After the feed stops: the frames it had already written ahead still play, then silence.
    int64_t afterEnd = 0, tailMatched = 0;
    UInt64 t1 = mach_absolute_time();
    while ((Float64)(mach_absolute_time() - t1) / gTicksPerSec < 0.6) {   // > one ring lap at 48 kHz
        SleepUntilSample(kObjMic, (Float64)in + F);
        int64_t t = in - F;
        MicRead(t, r, F);
        for (UInt32 i = 0; i < F; i++) {
            int64_t s = t + i;
            if (s >= fa.writeEnd) afterEnd += r[i] != 0.0f;
            else if (r[i] == Signal(s)) tailMatched++;
        }
        in += F;
    }
    CHECK(DRV->StopIO(D, kObjMic, 2) == 0, "StopIO mic");

    Float64 expected = 2.0 * rate;
    CHECK(corrupt == 0, "%lld corrupted/misaligned frames", (long long)corrupt);
    CHECK(matched > expected * 0.9, "only %lld of ~%.0f frames looped back", (long long)matched, expected);
    CHECK(dropouts <= (int64_t)(expected * 0.01), "%lld dropout frames (scheduler hiccups?)", (long long)dropouts);
    CHECK(afterEnd == 0, "%lld non-silent frames after the feed stopped", (long long)afterEnd);
    printf("  feed cycles %lld; mic read %lld frames: %lld bit-exact signal, %lld corrupt, %lld dropouts\n",
           (long long)fa.cycles, (long long)readFrames, (long long)matched, (long long)corrupt, (long long)dropouts);
    printf("  loopback latency in this harness: %d frames (feed writes 1 buffer ahead, mic reads 1 behind)\n", 2 * (int)F);
    printf("  after feed stop: %lld written-ahead frames drained, then %lld non-silent frames over 0.6 s\n",
           (long long)tailMatched, (long long)afterEnd);
    (void)firstSignalAt;
}

static void TestSampleRateChange(Float64 to)
{
    char title[64];
    snprintf(title, sizeof(title), "sample-rate change to %.0f Hz", to);
    SECTION(title);
    gConfigRequests = 0;
    AudioObjectPropertyAddress a = Addr(kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal);
    Float64 bad = 22050.0;
    CHECK(DRV->SetPropertyData(D, kObjMic, getpid(), &a, 0, NULL, sizeof(bad), &bad) != 0, "22050 Hz rejected");
    CHECK(gConfigRequests == 0, "no config request for a rejected rate");
    CHECK(DRV->SetPropertyData(D, kObjMic, getpid(), &a, 0, NULL, sizeof(to), &to) == 0, "set rate on mic");
    CHECK(gConfigRequests == 2 && gConfigDevices[0] == kObjMic && gConfigDevices[1] == kObjFeed,
          "host asked to reconfigure both devices (%d requests)", gConfigRequests);
    CHECK(GetF64(kObjMic, kAudioDevicePropertyNominalSampleRate) != to, "rate not applied before PerformDeviceConfigurationChange");
    // The HAL calls Perform once IO is quiesced.
    for (int i = 0; i < gConfigRequests && i < 8; i++)
        CHECK(DRV->PerformDeviceConfigurationChange(D, gConfigDevices[i], gConfigActions[i], NULL) == 0, "Perform");
    CHECK(GetF64(kObjMic, kAudioDevicePropertyNominalSampleRate) == to, "mic rate applied");
    CHECK(GetF64(kObjFeed, kAudioDevicePropertyNominalSampleRate) == to, "feed rate applied");
    AudioStreamBasicDescription f; UInt32 sz;
    GetProp(kObjMicStream, kAudioStreamPropertyVirtualFormat, kAudioObjectPropertyScopeGlobal, sizeof(f), &f, &sz);
    CheckFormat(&f, to, "mic stream after change");
    GetProp(kObjFeedStream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, sizeof(f), &f, &sz);
    CheckFormat(&f, to, "feed stream after change");
    gRate = to;
    printf("  both devices now %.0f Hz (2 config-change requests, applied via Perform)\n", to);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char* path = argc > 1 ? argv[1] : "build/BeamerAudio.driver";
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    gTicksPerSec = 1e9 * tb.denom / tb.numer;
#if defined(__arm64__)
    printf("BeamerAudio test host (arm64)\n");
#else
    printf("BeamerAudio test host (x86_64)\n");
#endif

    LoadDriver(path);
    TestPlugInObject();
    TestDevice(kObjMic, kObjMicStream, 1);
    TestDevice(kObjFeed, kObjFeedStream, 0);
    TestPropertySweep();
    TestIOOperations();
    TestClock();
    TestRingSemantics();
    TestRealtimeLoopback(48000.0);
    TestSampleRateChange(44100.0);
    TestClock();
    TestRealtimeLoopback(44100.0);
    TestSampleRateChange(48000.0);

    printf("\n%s: %d checks, %d failures\n", gFailures ? "FAILED" : "PASSED", gChecks, gFailures);
    return gFailures ? 1 : 0;
}
