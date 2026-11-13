# Rust OS bindings for macOS: every OS call mapped to a crate

Research for [#6](https://github.com/FredeAlexandre/beamer/issues/6), part of the
[Rust port map](https://github.com/FredeAlexandre/beamer/issues/1). Checked on 2026-11-13 against
`main` at `fffbede`.

## Answer

Every macOS call Beamer makes today has a workable Rust path, and none of them needs an
Objective-C or Swift shim. The `objc2` family of framework crates covers almost all of it with
generated bindings: CoreGraphics (event taps, posting, warp, cursor, displays), AppKit, Foundation,
ApplicationServices (Accessibility), IOKit, ServiceManagement, UserNotifications, CoreAudio
(including process taps), AudioToolbox, AVFoundation, CoreMedia, CoreVideo and VideoToolbox.
`define_class!` replaces the PyObjC delegate and subclass classes, and `block2` and `dispatch2`
cover the blocks and queues.

The gaps are small and all have a known fix:

1. **Keyboard layouts** (`TISCopyCurrentKeyboardLayoutInputSource`, `TISGetInputSourceProperty`,
   `UCKeyTranslate`, `LMGetKbdType`). The released `objc2-carbon` 0.3.2 is empty, and the released
   `objc2-core-services` 0.3.2 has no CarbonCore. Both are bound on objc2's `main` branch, which is
   not released yet. Until it is, the fix is about 40 lines of hand-written `extern "C"`, or a pin
   on an objc2 `main` commit.
2. **Private APIs**, used on purpose today: `CGSSetConnectionProperty` and `_CGSDefaultConnection`
   to hide the pointer while another app is frontmost, `CGSConfigureDisplayEnabled` (or SkyLight's
   `SLSConfigureDisplayEnabled`) for shared displays, `CoreDisplay_DisplayCreateInfoDictionary` for
   a display's name, the undocumented CGEvent gesture fields (110, 113, 123, 124, 129, 130, 132),
   and the event types 14, 29 and 30 that have no public constant. No crate binds the private
   functions. They are plain C symbols, so `extern "C"` (or `libloading` for SkyLight) works the
   same as `ctypes` does today. The display name can use public `NSScreen.localizedName` instead.
3. **The objc2 release lag.** The framework crates were last released as 0.3.2 on 2025-10-04.
   `main` already has the Xcode 27.1 headers (commit of 2026-10-16), but no release date is set:
   [objc2#892](https://github.com/madsmtm/objc2/issues/892) asks for one and has no answer yet.
   Everything in the table below except item 1 is in the released 0.3.2.

There is no macOS touchpoint without a Rust path. The real risks are elsewhere: private APIs that
can break with any macOS update (unchanged from today), and TCC grants that are bound to the code
signature, which the Rust dev loop has to keep stable (see [Gaps and risks](#gaps-and-risks)).

## What Beamer touches today

The inventory comes from reading `mac_app/` (PyObjC and `ctypes`), the macOS parts of `core/`, and
`native/audio/mac/` (Objective-C++). The HAL driver (`native/audio/mac-driver/`) is out of scope;
another ticket covers it.

Three facts from the inventory correct the ticket's starting list:

- **The Mac UI is not Qt.** `mac_app` draws its UI with AppKit and Core Animation through PyObjC,
  and its menu bar item with `rumps` (`mac_app/requirements-mac.txt`). Only `win_app` uses Qt.
- **No Bonjour, no DDC, no brightness control on the Mac.** Discovery is UDP beacons on the pairing
  port (`core/pairing.py`). Shared displays turn an output off with a private CoreGraphics call, not
  DDC, because DDC through the GN10's cable hangs in the kernel. Brightness keys are forwarded as
  `NSSystemDefined` media keys, not applied through DDC.
- **There is no virtual camera on macOS.** `video_backend_mac.mm` answers
  "unsupported: no virtual camera on macOS". The Mac only captures, encodes and decodes.

## Mapping

Crate versions are the latest on crates.io on 2026-11-13. "objc2 0.3.2" means the framework crates
released 2025-10-04 (`objc2` itself is at 0.6.5). Maturity:

- **Solid**: released binding in a crate with millions of recent downloads, or a direct
  replacement in `std`.
- **OK**: released, generated binding in a less used crate, so there are fewer users to have hit
  its mistakes.
- **Raw**: no crate. Hand-written `extern "C"` against a public, documented symbol.
- **Private**: no crate. Hand-written `extern "C"` against an undocumented symbol that macOS can
  change or remove.

### Input capture and injection

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Event tap: `CGEventTapCreate` at `kCGSessionEventTap`, head insert, run on a `CFRunLoop` thread, re-enable after `kCGEventTapDisabledBy*` | `mac_app/bridge.py` | `objc2-core-graphics` `CGEvent::tap_create` (the free function is deprecated in favour of it). The callback type is `unsafe extern "C-unwind" fn(CGEventTapProxy, CGEventType, NonNull<CGEvent>, *mut c_void) -> *mut CGEvent`. `objc2-core-foundation` `CFMachPort::new_run_loop_source`, `CFRunLoop::current`/`add_source`/`run` | Solid. Wrap the callback body in `catch_unwind` so a panic never takes the tap down |
| Tap mask bits with no public constant: `NSSystemDefined` (14), gesture types 29 to 32 | `bridge.py`, `media_keys.py`, `gestures.py` | `CGEventMask` is a `u64` and `CGEventType` is `pub struct CGEventType(pub u32)`, so raw values go in directly | OK. Undocumented values, same risk as today |
| Reading events: `CGEventGetIntegerValueField`, `CGEventGetDoubleValueField`, `CGEventGetFlags`, `CGEventGetLocation`, `CGEventKeyboardGetUnicodeString` | `bridge.py` | `objc2-core-graphics` `CGEvent` methods | Solid |
| Private gesture fields 110, 113, 123, 124, 129, 130, 132 | `gestures.py` | `CGEventField` is `pub struct CGEventField(pub u32)`, so any number works with the same getter | Private: fields measured on macOS 26.6.2; can change with any macOS release, as today |
| Media keys in: `NSEvent.eventWithCGEvent_`, then `subtype()` and `data1()` | `bridge.py`, `widgets.py` | `objc2-app-kit` `NSEvent::eventWithCGEvent`, `subtype`, `data1`. `NSEvent` is `AnyThread` in objc2, so the tap thread can call it | Solid |
| Injection: `CGEventCreateKeyboardEvent`, `CGEventCreateMouseEvent`, `CGEventCreateScrollWheelEvent`, `CGEventSetFlags`, `CGEventSetIntegerValueField` (click state, deltas, autorepeat, `kCGEventSourceUserData` mark), `CGEventKeyboardSetUnicodeString`, `CGEventPost(kCGHIDEventTap)` | `mac_app/input_injector_mac.py` | `objc2-core-graphics` `CGEvent::new_keyboard_event`, `set_integer_value_field`, `keyboard_set_unicode_string`, `post`; `CGEventCreateScrollWheelEvent2` (the non-variadic form) | Solid |
| Media keys out: `NSEvent.otherEventWithType_…subtype_data1_data2_` then `.CGEvent()` and post | `input_injector_mac.py` | `objc2-app-kit` `NSEvent::otherEventWithType_location_modifierFlags_timestamp_windowNumber_context_subtype_data1_data2`, `NSEvent::CGEvent` | Solid |
| Key and button state: `CGEventSourceKeyState`, `CGEventSourceButtonState`, `CGEventSourceSecondsSinceLastEventType` | `bridge.py` | `objc2-core-graphics` | Solid |
| Wake the display on remote input: `IOPMAssertionDeclareUserActivity` | `input_injector_mac.py` | `objc2-io-kit` `IOPMAssertionDeclareUserActivity` | Solid |
| Trackpad gestures on an overlay panel (`magnifyWithEvent:`, `swipeWithEvent:`) | `mac_app/kvm_bridge_app.py` | `objc2` `define_class!` subclass of `NSView`/`NSPanel` from `objc2-app-kit` | Solid. Needs a native panel even if the UI toolkit owns the other windows |

### Pointer and displays

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Pointer warp and detach: `CGWarpMouseCursorPosition`, `CGAssociateMouseAndMouseCursorPosition` | `mac_app/desktop_mac.py`, `bridge.py` | `objc2-core-graphics` | Solid |
| Pointer hide: `CGDisplayHideCursor`, `CGDisplayShowCursor`, `CGMainDisplayID` | `mac_app/pointer_hide.py` | `objc2-core-graphics` | Solid |
| Hide while another app is frontmost: `_CGSDefaultConnection`, `CGSSetConnectionProperty(…, "SetsCursorInBackground", true)` | `pointer_hide.py` | `extern "C"` with `#[link(name = "CoreGraphics", kind = "framework")]`; `CFString` and `kCFBooleanTrue` from `objc2-core-foundation` | Private |
| Display geometry: `CGGetActiveDisplayList`, `CGGetOnlineDisplayList`, `CGDisplayBounds`, `CGDisplayIsBuiltin`, `CGDisplayVendorNumber`, `CGDisplayModelNumber` | `desktop_mac.py`, `mac_app/shared_display_mac.py`, `kvm_bridge_app.py` | `objc2-core-graphics` | Solid |
| Shared display off and on: `CGBeginDisplayConfiguration`, `CGSConfigureDisplayEnabled` (fallback `SLSConfigureDisplayEnabled` from SkyLight), `CGCompleteDisplayConfiguration(…, kCGConfigureForSession)`, `CGCancelDisplayConfiguration` | `shared_display_mac.py` | The `CGBegin/Complete/Cancel…` calls are in `objc2-core-graphics`. The enable call is `extern "C"` from CoreGraphics; SkyLight is a private framework, so load it with `libloading` (`/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight`) rather than link it | Private. The core of the shared-display feature, so test it on every macOS update |
| Display product name: `CoreDisplay_DisplayCreateInfoDictionary` | `shared_display_mac.py` | Drop the private call: `NSScreen.localizedName` (public since macOS 10.15) matched to the display id through `deviceDescription["NSScreenNumber"]`. It only answers while the display is on, which is when the code reads the name today | Solid via `objc2-app-kit` (or Private, if the CoreDisplay call is kept) |
| Screens for overlays: `NSScreen.screens`, `frame`, `safeAreaInsets` (notch), `maximumFramesPerSecond` | `kvm_bridge_app.py`, `mac_app/notch_island.py`, `mac_app/frame_clock.py`, `mac_app/effects_overlay.py` | `objc2-app-kit` `NSScreen` (`MainThreadOnly` in objc2, which matches AppKit's rule) | Solid |
| Full-screen app detection: `NSWorkspace.frontmostApplication`, `AXUIElementCreateApplication`, `AXUIElementCopyAttributeValue("AXFocusedWindow"/"AXFullScreen")`, `CGWindowListCopyWindowInfo` | `kvm_bridge_app.py` | `objc2-app-kit` `NSWorkspace`; `objc2-application-services` `AXUIElement…`; `objc2-core-graphics` `CGWindowListCopyWindowInfo` | Solid (OK for `objc2-application-services`, about 300k recent downloads). Reads only bounds, layer and pid, so no Screen Recording grant is needed, as today |
| DDC/CI (`IOAVServiceCreateWithService`, `IOAVServiceWriteI2C`) | Not used. Measured to hang through the GN10 cable | If ever needed: `objc2-io-kit` for the registry walk, and `extern "C"` for the private `IOAVService*` exports | Private. Not needed for parity |

### Keyboard layout

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Layout data: `TISCopyCurrentKeyboardLayoutInputSource`, `TISGetInputSourceProperty(kTISPropertyUnicodeKeyLayoutData)`, `TISCreateInputSourceList` (tests), `LMGetKbdType` (Carbon, HIToolbox) | `mac_app/keyboard_layout.py` | Released `objc2-carbon` 0.3.2 is empty ("This is currently empty"). On objc2 `main`, `objc2-carbon` has a `TextInputSources` feature. Today: `extern "C"` with `#[link(name = "Carbon", kind = "framework")]` | Raw (gap 1) |
| Translation: `UCKeyTranslate` (CoreServices, CarbonCore `UnicodeUtilities.h`) with dead-key state | `keyboard_layout.py` | Released `objc2-core-services` 0.3.2 has no CarbonCore. objc2 `main` added it on 2026-06-28 (commit `c8b8bdc05`, feature `UnicodeUtilities`). Today: `extern "C"` against CoreServices; `CFData::byte_ptr` from `objc2-core-foundation` for the layout bytes | Raw (gap 1) |
| Layout change notice: `NSDistributedNotificationCenter` observing `com.apple.Carbon.TISNotifySelectedKeyboardInputSourceChanged` | `keyboard_layout.py` | `objc2-foundation` `NSDistributedNotificationCenter::defaultCenter`, observer from `define_class!` (or the block-based `addObserverForName…usingBlock` with `block2`) | Solid |

### Clipboard

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| `NSPasteboard.generalPasteboard`, `changeCount`, `stringForType`, `dataForType` (PNG, TIFF), `clearContents`, `setString_forType`, `setData_forType` | `mac_app/clipboard_mac.py` | `objc2-app-kit` `NSPasteboard` (marked `AnyThread` in objc2; keep today's lock, since Apple does not document it as thread-safe) | Solid |
| TIFF to PNG: `NSBitmapImageRep.imageRepWithData_`, `representationUsingType_properties_` | `clipboard_mac.py` | `objc2-app-kit` `NSBitmapImageRep`, or the `image` crate off the main thread | Solid |

### App lifecycle and system

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Dock icon on or off: `NSApplication.setActivationPolicy_` (regular or accessory) | `kvm_bridge_app.py`, `mac_app/dock_visibility.py` | `objc2-app-kit` `NSApplication::setActivationPolicy` | Solid |
| Menu bar item: `NSStatusItem` through `rumps` | `kvm_bridge_app.py` | `objc2-app-kit` `NSStatusBar`/`NSStatusItem`/`NSMenu`, or `tray-icon` 0.26.1 with `muda` 0.21.2 (Tauri). The UI toolkit ticket decides | Solid |
| Click-through overlays: `NSPanel` borderless and non-activating, `setIgnoresMouseEvents_`, `setLevel_` (status, pop-up menu), `setCollectionBehavior_` (all spaces, full-screen auxiliary) | `effects_overlay.py`, `mac_app/identify_overlay.py`, `notch_island.py`, `kvm_bridge_app.py` | `objc2-app-kit` `NSPanel`/`NSWindow` | Solid. The escape hatch the map asks for: any toolkit that exposes its `NSView` through `raw-window-handle` can be reconfigured this way |
| Login item: `SMAppService.mainAppService`, `status`, `registerAndReturnError`, `unregisterAndReturnError`, `openSystemSettingsLoginItems` | `mac_app/login_item.py` | `objc2-service-management` `SMAppService` (or `smappservice-rs` 0.1.3, a thin wrapper) | Solid. Needs a signed `.app` bundle in `/Applications`, as today |
| App Nap off: `NSProcessInfo.beginActivityWithOptions_reason_` (`UserInitiatedAllowingIdleSystemSleep` and `LatencyCritical`) | `kvm_bridge_app.py` | `objc2-foundation` `NSProcessInfo::beginActivityWithOptions_reason` | Solid |
| Notifications: `UNUserNotificationCenter` plus a delegate | `mac_app/notices.py` | `objc2-user-notifications`; delegate with `define_class!`; completion handlers with `block2` | Solid. Needs a bundled, signed app |
| Haptics: `NSHapticFeedbackManager.defaultPerformer` | `kvm_bridge_app.py` | `objc2-app-kit` | Solid |
| Reduce motion, dark mode: `NSWorkspace.accessibilityDisplayShouldReduceMotion`, `NSUserDefaults "AppleInterfaceStyle"` | `mac_app/motion.py`, `notch_beam.py` | `objc2-app-kit`, `objc2-foundation` (or `NSApp.effectiveAppearance`) | Solid |
| Open a URL or System Settings pane (`x-apple.systempreferences:`): `NSWorkspace.openURL_` | `mac_app/audio_page.py`, `kvm_bridge_app.py` | `objc2-app-kit` `NSWorkspace::openURL` | Solid |
| Relaunch after a grant: `/bin/sh` waiting on the pid, then `/usr/bin/open` | `kvm_bridge_app.py` | `std::process::Command` | Solid |
| Single instance: Unix socket `~/Library/Application Support/Beamer/Beamer.sock`, "activate" line | `mac_app/instance_signal.py` | `std::os::unix::net::UnixListener` and `UnixStream` | Solid |
| Locale and languages: `NSLocale.currentLocale` country code, `NSLocale.preferredLanguages` | `core/locale.py`, `core/i18n.py` | `objc2-foundation` `NSLocale` (or `sys-locale` 0.3.2 for the language list) | Solid |
| Update check over HTTPS with the system trust store: `NSURLConnection` | `core/updates.py` | `ureq` or `reqwest` with `rustls-platform-verifier` 0.7.1 (verifies through Security.framework) | Solid. Only if updates are kept (an open map question) |
| Hardware address for wake-on-LAN: `getifaddrs` with `AF_LINK` | `mac_app/hardware_mac.py`, `core/pairing.py` | `nix` 0.31.3 `ifaddrs` (gives the link-layer address), or `libc` directly | Solid |
| Wake-on-LAN packet and ARP lookup (`arp -n`) | `mac_app/wake.py`, `core/wol.py` | `socket2` 0.6.5 for the UDP broadcast; `std::process::Command` for `arp`, or a `sysctl(NET_RT_FLAGS)` read with `libc` | Solid |
| HAL driver install: `osascript … with administrator privileges` and a `sudo` helper | `mac_app/audio_driver.py` | `std::process::Command` | Solid. The driver itself is another ticket |
| Lock screen | `mac_app/no_unlock.py` answers "unknown"; no OS call | None needed | N/A |

### Permissions (TCC)

| Permission | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Accessibility (to post events and read AX): `AXIsProcessTrustedWithOptions` with `kAXTrustedCheckOptionPrompt` | `kvm_bridge_app.py` | `objc2-application-services` `AXIsProcessTrustedWithOptions`. `CGPreflightPostEventAccess` and `CGRequestPostEventAccess` (macOS 10.15+) in `objc2-core-graphics` are a narrower check | Solid |
| Input Monitoring (for the tap): `CGPreflightListenEventAccess`, `CGRequestListenEventAccess` | `kvm_bridge_app.py` | `objc2-core-graphics` | Solid |
| Camera and Microphone: `AVCaptureDevice authorizationStatusForMediaType` (and the request) | `native/audio/mac/camera_mac.mm`, `vmic_mac.mm` | `objc2-av-foundation` `AVCaptureDevice::authorizationStatusForMediaType`, `requestAccessForMediaType_completionHandler` (`block2`) | Solid. Info.plist keys `NSCameraUsageDescription` and `NSMicrophoneUsageDescription` |
| System audio recording (process taps) | `native/audio/mac/tap_mac.mm` | No call: Apple says the prompt comes the first time an aggregate device holding a tap starts recording, and the app needs `NSAudioCaptureUsageDescription` | Same as today: no preflight API |
| Local Network (macOS 15+) | `NSLocalNetworkUsageDescription` in `mac_app/setup.py` | No call: an Info.plist key, prompted on the first LAN connection | Same as today |
| Screen Recording | Not needed today (`CGWindowListCopyWindowInfo` reads no window titles) | `CGPreflightScreenCaptureAccess` exists in `objc2-core-graphics` if ever needed | N/A |

### Audio engine (`native/audio/mac`)

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Device list and properties: `AudioObjectGetPropertyData`/`DataSize`/`SetPropertyData`/`HasProperty` (devices, defaults, UID, name, transport type, streams, terminal type, nominal rate, buffer size, data source, is-alive, running-somewhere, translate UID or PID) | `devices_mac.mm`, `backend_mac.mm`, `vmic_mac.mm` | `objc2-core-audio` 0.3.2 | Solid. `coreaudio-rs` 0.14.2 and `cpal` 0.18.2 are now built on these crates |
| Property listeners: `AudioObjectAddPropertyListenerBlock`, `…RemovePropertyListenerBlock`, `kAudioHardwarePropertyServiceRestarted` | `backend_mac.mm`, `vmic_mac.mm` | `objc2-core-audio` plus `block2` and `dispatch2` | Solid |
| Device I/O: `AudioDeviceCreateIOProcID`, `AudioDeviceCreateIOProcIDWithBlock`, `AudioDeviceStart`/`Stop`/`DestroyIOProcID`, `kAudioDevicePropertyIOProcStreamUsage` | `backend_mac.mm` | `objc2-core-audio` `AudioDeviceCreateIOProcID` (C function pointer, no allocation on the real-time thread) | Solid |
| Process taps: `CATapDescription`, `AudioHardwareCreateProcessTap`, `AudioHardwareDestroyProcessTap`, `AudioHardwareCreateAggregateDevice` with the tap list, `kAudioTapPropertyUID` (macOS 14.2+) | `tap_mac.mm` | `objc2-core-audio` `AudioHardwareCreateProcessTap`, `CATapDescription`. `cpal`'s macOS loopback (`src/host/coreaudio/macos/loopback.rs`) does the same sequence with these crates, which proves the path | Solid. Gate on macOS 14.2 at run time (`LSMinimumSystemVersion` is 13.0 today) with objc2's `available!` |
| Sample-rate conversion: `AudioConverterNew`, `AudioConverterFillComplexBuffer`, `AudioConverterSetProperty` | `backend_mac.mm` | `objc2-audio-toolbox` (or a Rust resampler, which the engine port may prefer) | Solid |
| Queues and timers: `dispatch_queue_create` (with QoS), `dispatch_async`/`sync`/`after`, `dispatch_source` timers | `backend_mac.mm`, `camera_mac.mm`, `video_backend_mac.mm` | `dispatch2` 0.3.1 (`DispatchQueue`, `DispatchQueueAttr`, `DispatchQoS`, `DispatchSource`, `DispatchTime`), or plain Rust threads | Solid |
| Host time: `mach_absolute_time` | several | `mach2` 0.7.0 or `libc` | Solid |
| Process list and spawn (test tool only): `proc_listpids`, `proc_pidpath`, `posix_spawn` | `video_check.mm` | `libc`, `std::process` | Solid |

### Camera and video (`native/audio/mac`)

| Touchpoint | Where used today | Rust option | Maturity / risk |
|---|---|---|---|
| Capture: `AVCaptureDeviceDiscoverySession` (built-in, external, Continuity Camera), `AVCaptureSession`, `AVCaptureDeviceInput`, `AVCaptureVideoDataOutput` with a sample buffer delegate on a queue, format selection, connect, disconnect and interruption notices | `camera_mac.mm`, `video_backend_mac.mm` | `objc2-av-foundation` 0.3.2 (`setSampleBufferDelegate_queue`, delegate via `define_class!` implementing `AVCaptureVideoDataOutputSampleBufferDelegate`) | OK. About 1.7M recent downloads; delegate and dispatch-queue plumbing is the fiddly part |
| Sample buffers and clocks: `CMSampleBuffer…`, `CMClockGetHostTimeClock`, `CMSyncConvertTime`, `CMTime…`, `CMBlockBuffer…`, `CMVideoFormatDescription…` | `camera_mac.mm`, `videoenc_mac.mm`, `video_check.mm` | `objc2-core-media` 0.3.2 | OK |
| Pixel buffers: `CVPixelBufferCreate`, pools, lock and plane access, IOSurface-backed attachments | `synth_mac.mm`, `scale_mac.mm`, `videoenc_mac.mm`, `video_backend_mac.mm` | `objc2-core-video` 0.3.2 (`objc2-io-surface` for the IOSurface key) | Solid |
| Encode: `VTCompressionSessionCreate`, `…EncodeFrame`, `…CompleteFrames`, `…PrepareToEncodeFrames`, `VTSessionSetProperty` | `videoenc_mac.mm`, `video_backend_mac.mm` | `objc2-video-toolbox` 0.3.2 | OK. About 258k recent downloads, the least used crate on this list. `cidre` 0.30.1 is the fallback (see below) |
| Scale and convert: `VTPixelTransferSessionCreate`, `…TransferImage` | `scale_mac.mm` | `objc2-video-toolbox` | OK |
| Decode (test tool today): `VTDecompressionSessionCreate`, `…DecodeFrame` | `video_check.mm` | `objc2-video-toolbox` | OK. Decoding moves into the product if the Mac ever shows the PC's camera |

### UI drawing (for the UI toolkit ticket)

The Mac UI draws with AppKit views, Auto Layout, Core Animation layers (`CALayer`,
`CAShapeLayer`, `CAGradientLayer`, `CATextLayer`, animations), Core Graphics contexts and paths,
Core Text glyph paths and Core Image (`CIQRCodeGenerator`). All of it is bound by
`objc2-app-kit`, `objc2-quartz-core`, `objc2-core-graphics`, `objc2-core-text` and
`objc2-core-image`, but a Rust UI toolkit replaces most of it. Two items move to plain Rust: the
pairing QR (`qrcode` 0.14.1) and accessibility announcements, which belong to whatever toolkit is
chosen.

## Gaps and risks

1. **Keyboard layout bindings are not released.** `TIS*`, `LMGetKbdType` and `UCKeyTranslate` are
   missing from the released objc2 crates and present on objc2 `main`. Hand-write about six
   `extern "C"` declarations and the two property-key statics in a small `macos-sys` module, and
   swap to `objc2-carbon`/`objc2-core-services` when they ship. No shim is needed.
2. **Private APIs stay private.** Pointer hiding in the background, the shared-display switch,
   the gesture fields and the undocumented event types keep today's risk: any macOS update can
   break them. Keep each one behind a function that reports "unavailable" and degrades the way the
   Python code does (gestures already fail soft, shared displays already log and switch off). One
   private call can go: the display name, through `NSScreen.localizedName`.
3. **objc2 framework crates lag the SDK.** 0.3.2 predates the macOS 27 SDK. Nothing Beamer uses is
   new in macOS 27, so 0.3.2 is enough today. If a newer API is wanted, pin an objc2 `main` commit
   (the request in objc2#892 does this) and plan for one breaking upgrade: `main` already changes
   the `define_class!` ivars syntax.
4. **Almost every call is `unsafe`.** The generated bindings mark most functions `unsafe`, and
   CoreFoundation ownership (`CFRetained`) and `Retained` replace PyObjC's automatic reference
   counting. Keep the `unsafe` in one platform crate behind a safe trait, which is also the seam
   the Linux backend needs.
5. **Main-thread rules are enforced.** objc2 marks `NSScreen`, `NSWindow`, `NSView` and the rest
   of AppKit's UI classes `MainThreadOnly` and asks for a `MainThreadMarker`. The tap stays on its
   own `CFRunLoop` thread, and only `AnyThread` classes (`NSEvent`, `NSPasteboard`, `NSWorkspace`)
   may be touched from it.
6. **TCC follows the code signature.** Accessibility, Input Monitoring, Camera, Microphone and
   system audio grants belong to the signed app. A `cargo run` binary is ad-hoc signed with a new
   code hash on every build, and a binary started from Terminal is attributed to Terminal. The
   Rust dev loop must build a `.app` bundle and sign it with the stable "Beamer Dev" identity on
   every build, as `dev/deploy.sh` does today. Without that, every rebuild asks for the grants
   again. The packaging ticket owns this.
7. **Process taps need macOS 14.2.** `LSMinimumSystemVersion` is 13.0. Decide whether to raise
   the minimum or gate system-audio sending at run time.

## Alternatives considered

- **`core-graphics` 0.25.0 and `core-foundation` 0.10.1 (servo).** Widely used, still maintained
  (last push 2026-11-01), but hand-written, older in coverage and not linked to the AppKit,
  AVFoundation or CoreAudio crates. Mixing them with objc2 means two sets of CF types. Use objc2
  throughout.
- **`cidre` 0.30.1** (MIT, released 2026-11-13, requires Rust 1.100). Hand-curated Rust APIs for
  AVFoundation, CoreMedia, VideoToolbox, Core Audio (including `hardware_tapping.rs`) and TIS
  (`tis.rs`), built for screen and audio recording apps. It is ahead of objc2 on media ergonomics
  and already covers TIS. The cost: one maintainer, frequent breaking 0.x releases, and its own
  type system that does not interoperate with objc2's. Keep it as the fallback for the video path
  if `objc2-video-toolbox` turns out rough.
- **`coreaudio-rs` 0.14.2 and `cpal` 0.18.2.** Both are now built on `objc2-core-audio`.
  `cpal` covers device I/O and loopback, but Beamer's engine needs per-process tap exclusion,
  the virtual microphone device from the HAL driver and IOProc stream usage, so bind CoreAudio
  directly and read `cpal` as the reference implementation.
- **`rdev`, `enigo` and similar input crates.** Too high level: Beamer needs the private fields,
  the injected-event mark, the tap-disabled events and the raw event types.

## Not verified

- No Rust code was compiled for this research. Symbol presence was checked on docs.rs item pages
  and crate sources, not by building against the macOS 27 SDK.
- Whether the private symbols (`CGSSetConnectionProperty`, `_CGSDefaultConnection`,
  `CGSConfigureDisplayEnabled`, `SLSConfigureDisplayEnabled`) still export on the Mac's current
  macOS build was not re-checked here. The Python app calls them today, which is the evidence.
- Whether `objc2-video-toolbox` handles every property key and callback the encoder uses was not
  exercised. Its low download count is the reason it is rated OK, not Solid.

## Sources

- objc2 repository and framework crates: <https://github.com/madsmtm/objc2>;
  `framework-crates/objc2-carbon/translation-config.toml` and `Cargo.toml` (feature
  `TextInputSources` on `main`); commit `c8b8bdc05` "Add CoreServices module CarbonCore"
  (2026-06-28); release request <https://github.com/madsmtm/objc2/issues/892>.
- docs.rs pages (latest = 0.3.2 for the framework crates):
  [`objc2-carbon`](https://docs.rs/objc2-carbon/latest/objc2_carbon/) ("This is currently empty"),
  [`CGEventTapCreate`](https://docs.rs/objc2-core-graphics/latest/objc2_core_graphics/fn.CGEventTapCreate.html)
  (deprecated, renamed `CGEvent::tap_create`),
  [`CGEventTapCallBack`](https://docs.rs/objc2-core-graphics/latest/objc2_core_graphics/type.CGEventTapCallBack.html),
  [`CGEventField`](https://docs.rs/objc2-core-graphics/latest/objc2_core_graphics/struct.CGEventField.html),
  [`NSEvent`](https://docs.rs/objc2-app-kit/latest/objc2_app_kit/struct.NSEvent.html),
  [`NSPasteboard`](https://docs.rs/objc2-app-kit/latest/objc2_app_kit/struct.NSPasteboard.html),
  [`SMAppService`](https://docs.rs/objc2-service-management/latest/objc2_service_management/struct.SMAppService.html),
  [`AudioHardwareCreateProcessTap`](https://docs.rs/objc2-core-audio/latest/objc2_core_audio/fn.AudioHardwareCreateProcessTap.html),
  [`AVCaptureVideoDataOutput`](https://docs.rs/objc2-av-foundation/latest/objc2_av_foundation/struct.AVCaptureVideoDataOutput.html),
  [`VTCompressionSessionCreate`](https://docs.rs/objc2-video-toolbox/latest/objc2_video_toolbox/fn.VTCompressionSessionCreate.html),
  [`IOPMAssertionDeclareUserActivity`](https://docs.rs/objc2-io-kit/latest/objc2_io_kit/fn.IOPMAssertionDeclareUserActivity.html),
  [`AXIsProcessTrustedWithOptions`](https://docs.rs/objc2-application-services/latest/objc2_application_services/fn.AXIsProcessTrustedWithOptions.html),
  [`dispatch2`](https://docs.rs/dispatch2/latest/dispatch2/).
- crates.io API (`/api/v1/crates/<name>`) for versions, dates and recent downloads, read
  2026-11-13; feature lists of `objc2-carbon` 0.3.2 and `objc2-core-services` 0.3.2.
- Apple documentation:
  [`AudioHardwareCreateProcessTap`](https://developer.apple.com/documentation/coreaudio/audiohardwarecreateprocesstap(_:_:))
  (macOS 14.2),
  [Capturing system audio with Core Audio taps](https://developer.apple.com/documentation/coreaudio/capturing-system-audio-with-core-audio-taps)
  (`NSAudioCaptureUsageDescription`, prompt on first recording),
  [`CGPreflightListenEventAccess`](https://developer.apple.com/documentation/coregraphics/cgpreflightlisteneventaccess())
  and [`CGPreflightPostEventAccess`](https://developer.apple.com/documentation/coregraphics/cgpreflightposteventaccess())
  (macOS 10.15),
  [`UCKeyTranslate`](https://developer.apple.com/documentation/coreservices/1390584-uckeytranslate)
  (Core Services),
  [`SMAppService`](https://developer.apple.com/documentation/servicemanagement/smappservice)
  (macOS 13).
- `cpal` loopback over `objc2-core-audio`:
  <https://github.com/RustAudio/cpal/blob/master/src/host/coreaudio/macos/loopback.rs>.
- `cidre`: <https://github.com/yury/cidre> (README, `cidre/Cargo.toml`, `cidre/src/tis.rs`,
  `cidre/src/core_audio/hardware_tapping.rs`).
- `core-foundation-rs`: <https://github.com/servo/core-foundation-rs>.
- Beamer sources as listed in the tables, at `fffbede`.
