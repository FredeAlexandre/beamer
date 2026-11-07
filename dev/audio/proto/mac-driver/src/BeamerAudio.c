// SPDX-License-Identifier: GPL-3.0-only
//
// BeamerAudio.driver: a Core Audio AudioServerPlugIn that publishes two loopback devices.
//
//   "Beamer Microphone"  input only, mono, visible.   Apps (Zoom, Discord...) record from it.
//   "Beamer Mic Feed"    output only, mono, hidden.   Beamer plays the received microphone into it.
//
// Whatever is written to the feed device's output stream comes out of the microphone device's input
// stream. Both devices share one clock (host-time based, like Apple's NullAudio sample) and one ring
// buffer indexed by absolute sample time.
//
// Structure follows Apple's public "NullAudio" AudioServerPlugIn sample (object model, property
// dispatch, timestamp clock); the code itself is original.
//
// Copyright (C) 2026 The Beamer authors.
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU General Public License as published by the Free Software Foundation, version 3.
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// General Public License for more details. You should have received a copy of the GNU General
// Public License along with this program. If not, see <https://www.gnu.org/licenses/>.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------------------------

#define kBundleID           "com.beamer.audio.driver"
#define kManufacturer       "Beamer"
#define kMicName            "Beamer Microphone"
#define kMicUID             "com.beamer.audio.mic"
#define kFeedName           "Beamer Mic Feed"
#define kFeedUID            "com.beamer.audio.micfeed"
#define kModelUID           "com.beamer.audio.model"

// Clock domain shared by both devices: tells the HAL they never drift relative to each other.
#define kClockDomain        0x42454D52u /* 'BEMR' */

// Frames between two zero timestamps (the clock's "period"). Same value as NullAudio.
#define kZeroTimeStampPeriod 16384u

// Loopback ring: power of two, mono Float32. 16384 frames = 341 ms at 48 kHz, 64 KiB.
#define kRingFrames         16384
#define kRingMask           (kRingFrames - 1)
// Frames at the old edge of the ring that the reader never trusts, so a writer running ahead
// cannot tear a slot the reader is copying. Must exceed the largest IO buffer the HAL uses.
#define kRingGuardFrames    4096
#define kRingValidFrames    (kRingFrames - kRingGuardFrames)

#define kChannels           1
#define kBytesPerFrame      (sizeof(Float32) * kChannels)

static const Float64 kSupportedRates[] = { 48000.0, 44100.0 };
#define kNumSupportedRates  (sizeof(kSupportedRates) / sizeof(kSupportedRates[0]))
#define kDefaultRate        48000.0

// Change actions passed through RequestDeviceConfigurationChange.
enum { kChangeAction_SampleRate = 1 };

// Object IDs. kAudioObjectPlugInObject (1) is fixed by the API.
enum {
    kObjectID_PlugIn      = kAudioObjectPlugInObject,
    kObjectID_Mic         = 2,
    kObjectID_MicStream   = 3,
    kObjectID_Feed        = 4,
    kObjectID_FeedStream  = 5,
};

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------

typedef struct {
    AudioObjectID   device;
    AudioObjectID   stream;
    bool            isInput;        // stream direction: true = input (mic), false = output (feed)
    bool            hidden;
    // Mutable, guarded by gState.mutex except where noted.
    UInt64          ioCount;        // number of StartIO without matching StopIO
    _Atomic(Float64) sampleRate;    // read lock-free by GetZeroTimeStamp
} Device;

static struct {
    pthread_mutex_t             mutex;
    _Atomic(UInt32)             refCount;
    AudioServerPlugInHostRef    host;
    Float64                     pendingRate;     // rate requested, applied by PerformDeviceConfigurationChange
    UInt64                      totalIOCount;    // sum over both devices
    _Atomic(UInt64)             anchorHostTime;  // shared clock origin, set when IO starts from idle
    Float64                     hostTicksPerSecond;
    Device                      mic;
    Device                      feed;
} gState = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .refCount = 0,
    .pendingRate = kDefaultRate,
    .mic  = { .device = kObjectID_Mic,  .stream = kObjectID_MicStream,  .isInput = true,  .hidden = false, .sampleRate = kDefaultRate },
    .feed = { .device = kObjectID_Feed, .stream = kObjectID_FeedStream, .isInput = false, .hidden = true,  .sampleRate = kDefaultRate },
};

// Loopback ring, indexed by absolute sample time & kRingMask.
// Single writer (feed IO thread), single reader (mic IO thread); synchronised by gWriteEnd.
// Samples are stored as relaxed 32-bit atomics (the Float32 bit pattern): on arm64 and x86_64 these
// compile to plain loads/stores, but they make the rare reader/writer overlap at the ring's old
// edge well-defined instead of a C data race. The reader never writes to the ring.
static _Atomic(uint32_t)  gRing[kRingFrames];
// One past the newest sample time the feed has written. Release-stored by the writer after the
// samples, acquire-loaded by the reader before them.
static _Atomic(int64_t)   gWriteEnd;

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

static Device* DeviceForID(AudioObjectID inID)
{
    if (inID == kObjectID_Mic)  return &gState.mic;
    if (inID == kObjectID_Feed) return &gState.feed;
    return NULL;
}

static Device* DeviceForStreamID(AudioObjectID inID)
{
    if (inID == kObjectID_MicStream)  return &gState.mic;
    if (inID == kObjectID_FeedStream) return &gState.feed;
    return NULL;
}

static bool IsSupportedRate(Float64 inRate)
{
    for (UInt32 i = 0; i < kNumSupportedRates; i++)
        if (kSupportedRates[i] == inRate) return true;
    return false;
}

static void FillASBD(AudioStreamBasicDescription* outASBD, Float64 inRate)
{
    memset(outASBD, 0, sizeof(*outASBD));
    outASBD->mSampleRate       = inRate;
    outASBD->mFormatID         = kAudioFormatLinearPCM;
    outASBD->mFormatFlags      = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    outASBD->mBytesPerPacket   = (UInt32)kBytesPerFrame;
    outASBD->mFramesPerPacket  = 1;
    outASBD->mBytesPerFrame    = (UInt32)kBytesPerFrame;
    outASBD->mChannelsPerFrame = kChannels;
    outASBD->mBitsPerChannel   = 32;
}

// Scope a device's stream lives in.
static AudioObjectPropertyScope DeviceStreamScope(const Device* inDev)
{
    return inDev->isInput ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput;
}

static bool ScopeMatchesDevice(const Device* inDev, AudioObjectPropertyScope inScope)
{
    return inScope == kAudioObjectPropertyScopeGlobal || inScope == kAudioObjectPropertyScopeWildcard ||
           inScope == DeviceStreamScope(inDev);
}

static CFStringRef CopyCFString(const char* inStr)
{
    return CFStringCreateWithCString(NULL, inStr, kCFStringEncodingUTF8);
}

#define REQUIRE_SIZE(type) do { if (inDataSize < sizeof(type)) return kAudioHardwareBadPropertySizeError; } while (0)
#define RETURN_VALUE(type, value) do { REQUIRE_SIZE(type); *(type*)outData = (value); *outDataSize = sizeof(type); return kAudioHardwareNoError; } while (0)

// ---------------------------------------------------------------------------------------------
// Prototypes
// ---------------------------------------------------------------------------------------------

static HRESULT  BA_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    BA_AddRef(void* inDriver);
static ULONG    BA_Release(void* inDriver);
static OSStatus BA_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus BA_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus BA_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus BA_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus BA_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus BA_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus BA_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean  BA_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus BA_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus BA_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus BA_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus BA_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus BA_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus BA_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus BA_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus BA_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus BA_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus BA_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus BA_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    BA_QueryInterface,
    BA_AddRef,
    BA_Release,
    BA_Initialize,
    BA_CreateDevice,
    BA_DestroyDevice,
    BA_AddDeviceClient,
    BA_RemoveDeviceClient,
    BA_PerformDeviceConfigurationChange,
    BA_AbortDeviceConfigurationChange,
    BA_HasProperty,
    BA_IsPropertySettable,
    BA_GetPropertyDataSize,
    BA_GetPropertyData,
    BA_SetPropertyData,
    BA_StartIO,
    BA_StopIO,
    BA_GetZeroTimeStamp,
    BA_WillDoIOOperation,
    BA_BeginIOOperation,
    BA_DoIOOperation,
    BA_EndIOOperation,
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef    = &gInterfacePtr;

// ---------------------------------------------------------------------------------------------
// Factory (named in Info.plist CFPlugInFactories)
// ---------------------------------------------------------------------------------------------

__attribute__((visibility("default")))
void* BeamerAudio_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (inRequestedTypeUUID != NULL && CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID)) {
        atomic_fetch_add(&gState.refCount, 1);
        return gDriverRef;
    }
    return NULL;
}

// ---------------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------------

static HRESULT BA_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriverRef || outInterface == NULL) return kAudioHardwareBadObjectError;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) return kAudioHardwareIllegalOperationError;
    HRESULT result = E_NOINTERFACE;
    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        atomic_fetch_add(&gState.refCount, 1);
        *outInterface = gDriverRef;
        result = S_OK;
    }
    CFRelease(requested);
    return result;
}

static ULONG BA_AddRef(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    return atomic_fetch_add(&gState.refCount, 1) + 1;
}

static ULONG BA_Release(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    UInt32 cur = atomic_load(&gState.refCount);
    while (cur > 0 && !atomic_compare_exchange_weak(&gState.refCount, &cur, cur - 1)) {}
    // The driver is a static singleton; nothing to free when the count reaches 0.
    return cur > 0 ? cur - 1 : 0;
}

// ---------------------------------------------------------------------------------------------
// Basic operations
// ---------------------------------------------------------------------------------------------

static OSStatus BA_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    pthread_mutex_lock(&gState.mutex);
    gState.host = inHost;
    gState.hostTicksPerSecond = 1.0e9 * (Float64)tb.denom / (Float64)tb.numer;
    pthread_mutex_unlock(&gState.mutex);
    return kAudioHardwareNoError;
}

static OSStatus BA_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{
    (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus BA_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDeviceObjectID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus BA_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (DeviceForID(inDeviceObjectID) == NULL) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

static OSStatus BA_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (DeviceForID(inDeviceObjectID) == NULL) return kAudioHardwareBadObjectError;
    return kAudioHardwareNoError;
}

static OSStatus BA_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;
    if (inChangeAction != kChangeAction_SampleRate) return kAudioHardwareUnsupportedOperationError;
    pthread_mutex_lock(&gState.mutex);
    atomic_store(&dev->sampleRate, gState.pendingRate);
    pthread_mutex_unlock(&gState.mutex);
    return kAudioHardwareNoError;
}

static OSStatus BA_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inChangeAction; (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (DeviceForID(inDeviceObjectID) == NULL) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------------------------
// Properties: plug-in object
// ---------------------------------------------------------------------------------------------

static Boolean PlugIn_HasProperty(const AudioObjectPropertyAddress* inAddress)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyBoxList:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
            return true;
    }
    return false;
}

static OSStatus PlugIn_GetPropertyDataSize(const AudioObjectPropertyAddress* inAddress, UInt32* outDataSize)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:              *outDataSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:              *outDataSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyManufacturer:       *outDataSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:         *outDataSize = 2 * sizeof(AudioObjectID); break;
        case kAudioPlugInPropertyBoxList:            *outDataSize = 0; break;
        case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); break;
        case kAudioPlugInPropertyResourceBundle:     *outDataSize = sizeof(CFStringRef); break;
        default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

static UInt32 FillObjectList(AudioObjectID* outList, UInt32 inCapacity, const AudioObjectID* inItems, UInt32 inCount)
{
    UInt32 n = inCount < inCapacity ? inCount : inCapacity;
    memcpy(outList, inItems, n * sizeof(AudioObjectID));
    return n;
}

static OSStatus PlugIn_GetPropertyData(const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    static const AudioObjectID kDevices[] = { kObjectID_Mic, kObjectID_Feed };
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:  RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:      RETURN_VALUE(AudioClassID, kAudioPlugInClassID);
        case kAudioObjectPropertyOwner:      RETURN_VALUE(AudioObjectID, kAudioObjectUnknown);
        case kAudioObjectPropertyManufacturer: RETURN_VALUE(CFStringRef, CopyCFString(kManufacturer));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            UInt32 n = FillObjectList((AudioObjectID*)outData, inDataSize / sizeof(AudioObjectID), kDevices, 2);
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioPlugInPropertyBoxList:
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            REQUIRE_SIZE(AudioObjectID);
            if (inQualifierDataSize != sizeof(CFStringRef) || inQualifierData == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *(const CFStringRef*)inQualifierData;
            AudioObjectID found = kAudioObjectUnknown;
            if (uid != NULL) {
                if (CFStringCompare(uid, CFSTR(kMicUID), 0) == kCFCompareEqualTo)       found = kObjectID_Mic;
                else if (CFStringCompare(uid, CFSTR(kFeedUID), 0) == kCFCompareEqualTo) found = kObjectID_Feed;
            }
            *(AudioObjectID*)outData = found;
            *outDataSize = sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioPlugInPropertyResourceBundle:
            // Path relative to the bundle; empty means the bundle itself.
            RETURN_VALUE(CFStringRef, CopyCFString(""));
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------------------------
// Properties: devices
// ---------------------------------------------------------------------------------------------

static Boolean Device_HasProperty(const Device* inDev, const AudioObjectPropertyAddress* inAddress)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyControlList:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:
            return true;
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            return ScopeMatchesDevice(inDev, inAddress->mScope) && inAddress->mScope != kAudioObjectPropertyScopeGlobal;
    }
    return false;
}

static OSStatus Device_IsPropertySettable(const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    *outIsSettable = inAddress->mSelector == kAudioDevicePropertyNominalSampleRate;
    return kAudioHardwareNoError;
}

static UInt32 Device_StreamCount(const Device* inDev, AudioObjectPropertyScope inScope)
{
    return ScopeMatchesDevice(inDev, inScope) ? 1 : 0;
}

#define kMonoLayoutSize ((UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + kChannels * sizeof(AudioChannelDescription)))

static OSStatus Device_GetPropertyDataSize(const Device* inDev, const AudioObjectPropertyAddress* inAddress, UInt32* outDataSize)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:                   *outDataSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:                   *outDataSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:                *outDataSize = sizeof(CFStringRef); break;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:                 *outDataSize = Device_StreamCount(inDev, inAddress->mScope) * (UInt32)sizeof(AudioObjectID); break;
        case kAudioObjectPropertyControlList:             *outDataSize = 0; break;
        case kAudioDevicePropertyRelatedDevices:          *outDataSize = sizeof(AudioObjectID); break;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:           *outDataSize = sizeof(UInt32); break;
        case kAudioDevicePropertyNominalSampleRate:       *outDataSize = sizeof(Float64); break;
        case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = (UInt32)(kNumSupportedRates * sizeof(AudioValueRange)); break;
        case kAudioDevicePropertyPreferredChannelsForStereo: *outDataSize = 2 * sizeof(UInt32); break;
        case kAudioDevicePropertyPreferredChannelLayout:  *outDataSize = kMonoLayoutSize; break;
        default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

static OSStatus Device_GetPropertyData(Device* inDev, const AudioObjectPropertyAddress* inAddress, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:   RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:       RETURN_VALUE(AudioClassID, kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:       RETURN_VALUE(AudioObjectID, kObjectID_PlugIn);
        case kAudioObjectPropertyName:        RETURN_VALUE(CFStringRef, CopyCFString(inDev->isInput ? kMicName : kFeedName));
        case kAudioObjectPropertyManufacturer: RETURN_VALUE(CFStringRef, CopyCFString(kManufacturer));
        case kAudioDevicePropertyDeviceUID:   RETURN_VALUE(CFStringRef, CopyCFString(inDev->isInput ? kMicUID : kFeedUID));
        case kAudioDevicePropertyModelUID:    RETURN_VALUE(CFStringRef, CopyCFString(kModelUID));
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            UInt32 n = Device_StreamCount(inDev, inAddress->mScope);
            n = FillObjectList((AudioObjectID*)outData, inDataSize / sizeof(AudioObjectID), &inDev->stream, n);
            *outDataSize = n * (UInt32)sizeof(AudioObjectID);
            return kAudioHardwareNoError;
        }
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioDevicePropertyRelatedDevices: RETURN_VALUE(AudioObjectID, inDev->device);
        case kAudioDevicePropertyTransportType:  RETURN_VALUE(UInt32, kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyClockDomain:    RETURN_VALUE(UInt32, kClockDomain);
        case kAudioDevicePropertyDeviceIsAlive:  RETURN_VALUE(UInt32, 1);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&gState.mutex);
            UInt32 running = inDev->ioCount > 0;
            pthread_mutex_unlock(&gState.mutex);
            RETURN_VALUE(UInt32, running);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            // The microphone can be the default input; the hidden feed is never a default output.
            RETURN_VALUE(UInt32, inDev->isInput ? 1 : 0);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertyLatency:        RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertySafetyOffset:   RETURN_VALUE(UInt32, 0);
        case kAudioDevicePropertyIsHidden:       RETURN_VALUE(UInt32, inDev->hidden ? 1 : 0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_VALUE(UInt32, kZeroTimeStampPeriod);
        case kAudioDevicePropertyClockIsStable:  RETURN_VALUE(UInt32, 1);
        case kAudioDevicePropertyNominalSampleRate: RETURN_VALUE(Float64, atomic_load(&inDev->sampleRate));
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inDataSize / (UInt32)sizeof(AudioValueRange);
            if (n > kNumSupportedRates) n = kNumSupportedRates;
            AudioValueRange* ranges = (AudioValueRange*)outData;
            for (UInt32 i = 0; i < n; i++) { ranges[i].mMinimum = kSupportedRates[i]; ranges[i].mMaximum = kSupportedRates[i]; }
            *outDataSize = n * (UInt32)sizeof(AudioValueRange);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo: {
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            UInt32* ch = (UInt32*)outData;
            ch[0] = 1; ch[1] = 1;   // mono: both "stereo" sides map to channel 1
            *outDataSize = 2 * sizeof(UInt32);
            return kAudioHardwareNoError;
        }
        case kAudioDevicePropertyPreferredChannelLayout: {
            if (inDataSize < kMonoLayoutSize) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout* layout = (AudioChannelLayout*)outData;
            memset(layout, 0, kMonoLayoutSize);
            layout->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            layout->mNumberChannelDescriptions = kChannels;
            layout->mChannelDescriptions[0].mChannelLabel = kAudioChannelLabel_Mono;
            *outDataSize = kMonoLayoutSize;
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus Device_SetPropertyData(Device* inDev, const AudioObjectPropertyAddress* inAddress, UInt32 inDataSize, const void* inData)
{
    (void)inDev;
    if (inAddress->mSelector != kAudioDevicePropertyNominalSampleRate) return kAudioHardwareUnknownPropertyError;
    if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
    Float64 rate = *(const Float64*)inData;
    if (!IsSupportedRate(rate)) return kAudioHardwareIllegalOperationError;

    pthread_mutex_lock(&gState.mutex);
    bool change = rate != atomic_load(&gState.mic.sampleRate) || rate != atomic_load(&gState.feed.sampleRate);
    if (change) gState.pendingRate = rate;
    AudioServerPlugInHostRef host = gState.host;
    pthread_mutex_unlock(&gState.mutex);

    // Both devices share the ring, so they always change rate together. The HAL stops IO and calls
    // PerformDeviceConfigurationChange for each device asynchronously.
    if (change && host != NULL) {
        host->RequestDeviceConfigurationChange(host, kObjectID_Mic,  kChangeAction_SampleRate, NULL);
        host->RequestDeviceConfigurationChange(host, kObjectID_Feed, kChangeAction_SampleRate, NULL);
    }
    return kAudioHardwareNoError;
}

// ---------------------------------------------------------------------------------------------
// Properties: streams
// ---------------------------------------------------------------------------------------------

static Boolean Stream_HasProperty(const AudioObjectPropertyAddress* inAddress)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

static OSStatus Stream_IsPropertySettable(const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    switch (inAddress->mSelector) {
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
            *outIsSettable = true; break;
        default:
            *outIsSettable = false; break;
    }
    return kAudioHardwareNoError;
}

static OSStatus Stream_GetPropertyDataSize(const AudioObjectPropertyAddress* inAddress, UInt32* outDataSize)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:            *outDataSize = sizeof(AudioClassID); break;
        case kAudioObjectPropertyOwner:            *outDataSize = sizeof(AudioObjectID); break;
        case kAudioObjectPropertyOwnedObjects:     *outDataSize = 0; break;
        case kAudioObjectPropertyName:             *outDataSize = sizeof(CFStringRef); break;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:          *outDataSize = sizeof(UInt32); break;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:   *outDataSize = sizeof(AudioStreamBasicDescription); break;
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = (UInt32)(kNumSupportedRates * sizeof(AudioStreamRangedDescription)); break;
        default: return kAudioHardwareUnknownPropertyError;
    }
    return kAudioHardwareNoError;
}

static OSStatus Stream_GetPropertyData(Device* inDev, const AudioObjectPropertyAddress* inAddress, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    switch (inAddress->mSelector) {
        case kAudioObjectPropertyBaseClass:   RETURN_VALUE(AudioClassID, kAudioObjectClassID);
        case kAudioObjectPropertyClass:       RETURN_VALUE(AudioClassID, kAudioStreamClassID);
        case kAudioObjectPropertyOwner:       RETURN_VALUE(AudioObjectID, inDev->device);
        case kAudioObjectPropertyOwnedObjects:
            *outDataSize = 0;
            return kAudioHardwareNoError;
        case kAudioObjectPropertyName:        RETURN_VALUE(CFStringRef, CopyCFString(inDev->isInput ? "Beamer Microphone Input" : "Beamer Mic Feed Output"));
        case kAudioStreamPropertyIsActive:    RETURN_VALUE(UInt32, 1);
        case kAudioStreamPropertyDirection:   RETURN_VALUE(UInt32, inDev->isInput ? 1 : 0);  // 1 = input
        case kAudioStreamPropertyTerminalType:
            RETURN_VALUE(UInt32, inDev->isInput ? kAudioStreamTerminalTypeMicrophone : kAudioStreamTerminalTypeSpeaker);
        case kAudioStreamPropertyStartingChannel: RETURN_VALUE(UInt32, 1);
        case kAudioStreamPropertyLatency:     RETURN_VALUE(UInt32, 0);
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            REQUIRE_SIZE(AudioStreamBasicDescription);
            FillASBD((AudioStreamBasicDescription*)outData, atomic_load(&inDev->sampleRate));
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return kAudioHardwareNoError;
        }
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = inDataSize / (UInt32)sizeof(AudioStreamRangedDescription);
            if (n > kNumSupportedRates) n = kNumSupportedRates;
            AudioStreamRangedDescription* d = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < n; i++) {
                FillASBD(&d[i].mFormat, kSupportedRates[i]);
                d[i].mSampleRateRange.mMinimum = kSupportedRates[i];
                d[i].mSampleRateRange.mMaximum = kSupportedRates[i];
            }
            *outDataSize = n * (UInt32)sizeof(AudioStreamRangedDescription);
            return kAudioHardwareNoError;
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus Stream_SetPropertyData(Device* inDev, const AudioObjectPropertyAddress* inAddress, UInt32 inDataSize, const void* inData)
{
    switch (inAddress->mSelector) {
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat: {
            if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            const AudioStreamBasicDescription* f = (const AudioStreamBasicDescription*)inData;
            AudioStreamBasicDescription ours;
            FillASBD(&ours, f->mSampleRate);
            if (f->mFormatID != ours.mFormatID || f->mFormatFlags != ours.mFormatFlags ||
                f->mBytesPerFrame != ours.mBytesPerFrame || f->mChannelsPerFrame != ours.mChannelsPerFrame ||
                f->mBitsPerChannel != ours.mBitsPerChannel || f->mFramesPerPacket != 1)
                return kAudioDeviceUnsupportedFormatError;
            if (!IsSupportedRate(f->mSampleRate)) return kAudioDeviceUnsupportedFormatError;
            AudioObjectPropertyAddress rateAddr = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
            return Device_SetPropertyData(inDev, &rateAddr, sizeof(Float64), &f->mSampleRate);
        }
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------------------------
// Property dispatch
// ---------------------------------------------------------------------------------------------

static Boolean BA_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
    (void)inClientProcessID;
    if (inDriver != gDriverRef || inAddress == NULL) return false;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_HasProperty(inAddress);
    Device* dev;
    if ((dev = DeviceForID(inObjectID)) != NULL) return Device_HasProperty(dev, inAddress);
    if ((dev = DeviceForStreamID(inObjectID)) != NULL) return Stream_HasProperty(inAddress);
    return false;
}

static OSStatus BA_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outIsSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!BA_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress)) {
        if (inObjectID == kObjectID_PlugIn || DeviceForID(inObjectID) || DeviceForStreamID(inObjectID)) return kAudioHardwareUnknownPropertyError;
        return kAudioHardwareBadObjectError;
    }
    if (inObjectID == kObjectID_PlugIn) { *outIsSettable = false; return kAudioHardwareNoError; }
    if (DeviceForID(inObjectID)) return Device_IsPropertySettable(inAddress, outIsSettable);
    return Stream_IsPropertySettable(inAddress, outIsSettable);
}

static OSStatus BA_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outDataSize == NULL) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_GetPropertyDataSize(inAddress, outDataSize);
    Device* dev;
    if ((dev = DeviceForID(inObjectID)) != NULL) {
        if (!Device_HasProperty(dev, inAddress)) return kAudioHardwareUnknownPropertyError;
        return Device_GetPropertyDataSize(dev, inAddress, outDataSize);
    }
    if (DeviceForStreamID(inObjectID) != NULL) return Stream_GetPropertyDataSize(inAddress, outDataSize);
    return kAudioHardwareBadObjectError;
}

static OSStatus BA_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    (void)inClientProcessID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outDataSize == NULL || (outData == NULL && inDataSize > 0)) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return PlugIn_GetPropertyData(inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
    Device* dev;
    if ((dev = DeviceForID(inObjectID)) != NULL) {
        if (!Device_HasProperty(dev, inAddress)) return kAudioHardwareUnknownPropertyError;
        return Device_GetPropertyData(dev, inAddress, inDataSize, outDataSize, outData);
    }
    if ((dev = DeviceForStreamID(inObjectID)) != NULL) return Stream_GetPropertyData(dev, inAddress, inDataSize, outDataSize, outData);
    return kAudioHardwareBadObjectError;
}

static OSStatus BA_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || (inData == NULL && inDataSize > 0)) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObjectID_PlugIn) return kAudioHardwareUnknownPropertyError;
    Device* dev;
    if ((dev = DeviceForID(inObjectID)) != NULL) return Device_SetPropertyData(dev, inAddress, inDataSize, inData);
    if ((dev = DeviceForStreamID(inObjectID)) != NULL) return Stream_SetPropertyData(dev, inAddress, inDataSize, inData);
    return kAudioHardwareBadObjectError;
}

// ---------------------------------------------------------------------------------------------
// IO
// ---------------------------------------------------------------------------------------------

static OSStatus BA_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;

    pthread_mutex_lock(&gState.mutex);
    if (gState.totalIOCount == 0) {
        // First IO on either device: start a fresh shared timeline. No IO thread is running, so
        // resetting the ring here cannot race with the IO paths.
        for (UInt32 i = 0; i < kRingFrames; i++) atomic_store_explicit(&gRing[i], 0, memory_order_relaxed);
        atomic_store(&gWriteEnd, 0);
        atomic_store(&gState.anchorHostTime, mach_absolute_time());
    }
    dev->ioCount++;
    gState.totalIOCount++;
    pthread_mutex_unlock(&gState.mutex);
    return kAudioHardwareNoError;
}

static OSStatus BA_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;

    OSStatus err = kAudioHardwareNoError;
    pthread_mutex_lock(&gState.mutex);
    if (dev->ioCount == 0) {
        err = kAudioHardwareIllegalOperationError;
    } else {
        dev->ioCount--;
        gState.totalIOCount--;
    }
    pthread_mutex_unlock(&gState.mutex);
    return err;
}

// Lock-free: the zero timestamp is a pure function of the shared anchor and the current host time,
// so both devices report the exact same timeline (same clock domain).
static OSStatus BA_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;

    UInt64  anchor        = atomic_load_explicit(&gState.anchorHostTime, memory_order_relaxed);
    Float64 rate          = atomic_load_explicit(&dev->sampleRate, memory_order_relaxed);
    Float64 ticksPerFrame = gState.hostTicksPerSecond / rate;
    Float64 ticksPerPeriod = ticksPerFrame * kZeroTimeStampPeriod;
    UInt64  now           = mach_absolute_time();
    UInt64  periods       = now > anchor ? (UInt64)((Float64)(now - anchor) / ticksPerPeriod) : 0;

    *outSampleTime = (Float64)(periods * kZeroTimeStampPeriod);
    *outHostTime   = anchor + (UInt64)((Float64)periods * ticksPerPeriod);
    *outSeed       = 1;
    return kAudioHardwareNoError;
}

static OSStatus BA_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;
    bool willDo = (dev->isInput && inOperationID == kAudioServerPlugInIOOperationReadInput) ||
                  (!dev->isInput && inOperationID == kAudioServerPlugInIOOperationWriteMix);
    if (outWillDo) *outWillDo = willDo;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return kAudioHardwareNoError;
}

static OSStatus BA_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (DeviceForID(inDeviceObjectID) == NULL) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}

static OSStatus BA_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (DeviceForID(inDeviceObjectID) == NULL) return kAudioHardwareBadDeviceError;
    return kAudioHardwareNoError;
}

// Ring sample access. Relaxed atomics: plain loads/stores on arm64/x86_64, no locks, no allocation.
static inline void RingStore(int64_t t, Float32 v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    atomic_store_explicit(&gRing[t & kRingMask], bits, memory_order_relaxed);
}

static inline Float32 RingLoad(int64_t t)
{
    uint32_t bits = atomic_load_explicit(&gRing[t & kRingMask], memory_order_relaxed);
    Float32 v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

// Feed device: the HAL hands us the mix of everything Beamer played, for output sample time t.
static void FeedWriteMix(int64_t t, const Float32* src, UInt32 n)
{
    if (n > kRingValidFrames) {          // never expected (HAL buffers are far smaller); keep the tail
        src += n - kRingValidFrames;
        t   += n - kRingValidFrames;
        n    = kRingValidFrames;
    }
    int64_t end = atomic_load_explicit(&gWriteEnd, memory_order_relaxed);   // we are the only writer
    // Jumped forward (first write, feed restarted, overload): clear the hole so the reader never
    // sees samples left from an earlier lap of the ring. At most one full lap.
    if (t > end) {
        int64_t gap = t - end;
        if (gap > kRingFrames) gap = kRingFrames;
        for (int64_t i = 0; i < gap; i++) RingStore(end + i, 0.0f);
    }
    for (UInt32 i = 0; i < n; i++) RingStore(t + (int64_t)i, src[i]);
    int64_t newEnd = t + (int64_t)n;
    if (newEnd > end) atomic_store_explicit(&gWriteEnd, newEnd, memory_order_release);
}

// Microphone device: fill n frames for input sample time t. A frame is returned only if the feed
// wrote it in the current lap, i.e. its time lies in [writeEnd - kRingValidFrames, writeEnd).
// Anything else (not written yet, feed stopped, gap, older than a lap) is silence, so stale audio
// can never be replayed. The reader never writes to the ring (single writer).
static void MicReadInput(int64_t t, Float32* dst, UInt32 n)
{
    int64_t end    = atomic_load_explicit(&gWriteEnd, memory_order_acquire);
    int64_t oldest = end - kRingValidFrames;
    if (oldest < 0) oldest = 0;
    for (UInt32 i = 0; i < n; i++) {
        int64_t s = t + (int64_t)i;
        dst[i] = (s >= oldest && s < end) ? RingLoad(s) : 0.0f;
    }
}

static OSStatus BA_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void)inClientID; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    Device* dev = DeviceForID(inDeviceObjectID);
    if (dev == NULL) return kAudioHardwareBadDeviceError;
    if (inStreamObjectID != dev->stream) return kAudioHardwareBadObjectError;
    if (ioMainBuffer == NULL || inIOCycleInfo == NULL) return kAudioHardwareIllegalOperationError;

    if (inOperationID == kAudioServerPlugInIOOperationReadInput && dev->isInput) {
        MicReadInput((int64_t)inIOCycleInfo->mInputTime.mSampleTime, (Float32*)ioMainBuffer, inIOBufferFrameSize);
    } else if (inOperationID == kAudioServerPlugInIOOperationWriteMix && !dev->isInput) {
        FeedWriteMix((int64_t)inIOCycleInfo->mOutputTime.mSampleTime, (const Float32*)ioMainBuffer, inIOBufferFrameSize);
    }
    return kAudioHardwareNoError;
}
