# Can the Mac HAL driver and the Windows virtual camera be written in Rust?

Research for [#9](https://github.com/FredeAlexandre/beamer/issues/9), part of the Rust rewrite map
[#1](https://github.com/FredeAlexandre/beamer/issues/1). Written 2026-11-13 against `main` at `fffbede`.

**Short answer.** Both plug-ins can be written in Rust: other projects have already shipped one of
each. Neither port is cheap enough to do now, and neither is needed for the Rust app. The Mac driver
should stay in C: it shares nothing with the app except Core Audio device UIDs, and Rust would give
up checks that C has today. The Windows camera source should stay in C++ for now. Port it later, and
only when its shared-memory protocol has to change anyway. Its contract with the app is a frozen,
fixed-layout shared-memory section, and the Rust helper can implement that contract directly.

## 1. What the two plug-ins are today

### `native/audio/mac-driver`: BeamerAudio.driver (virtual microphone)

- **Language and size.** One file of **C11** (not C++), `src/BeamerAudio.c`, 1,206 lines. Its
  structure follows Apple's NullAudio sample (`mac-driver/README.md`, "Files"). Built with clang from
  the Command Line Tools into a universal arm64 + x86_64 `-bundle`. `build.sh` checks that the only
  exported symbol is `_BeamerAudio_Create`, then signs the bundle with "Beamer Dev" or ad hoc.
- **What it does.** It exposes two devices that share one 16,384-frame ring buffer: the visible
  input device "Beamer Microphone" (`com.beamer.audio.mic`) and the hidden output device "Beamer Mic
  Feed" (`com.beamer.audio.micfeed`). The microphone also has volume and mute controls. The driver
  is its own clock, based on `mach_absolute_time`. Its IO path takes no locks, makes no allocations
  and makes no syscalls (README, "Real-time safety").
- **Tests.** `test/test_host.c` (925 lines) loads the bundle the way coreaudiod does: it goes
  through `CFBundle`, finds the factory, calls `QueryInterface` and drives the vtable. It runs
  signed, under ASan+UBSan and under TSan, for 2,205 checks. `test/live_check.py` tests the
  installed driver live, including a bit-exact loopback.
- **Install.** The `beamer-hal` root helper copies the bundle to `/Library/Audio/Plug-Ins/HAL` and
  restarts coreaudiod. It checks `codesign --verify --strict` with the leaf certificate pinned.
- **How the app talks to it.** There is no custom IPC and no shared memory. The helper finds the
  hidden feed with `kAudioHardwarePropertyTranslateUIDToDevice` and renders the received microphone
  into it as an ordinary output device (`native/audio/mac/mac_internal.h` `kFeedUID`; README "How
  Beamer feeds it"). It watches demand through `kAudioDevicePropertyDeviceIsRunningSomewhere` on
  the mic (`native/audio/mac/vmic_mac.mm`). Volume and mute are the standard
  `kAudioDevicePropertyVolumeScalar` and `kAudioDevicePropertyMute` properties.

### `native/audio/vcam-win`: BeamerCameraSource.dll (virtual camera)

- **Language and size.** `source.cpp` is 1,272 lines of C++20 using WRL (`RuntimeClass`,
  `ChainInterfaces`, `FtmBase`). Alongside it: `shm_protocol.h` (54 lines, "Frozen"), the
  generated built-in image, and test tools (`camprobe`, `feeder`, `shmfuzz`, 950 lines). It is built
  with MSVC `/MT`, and `build.ps1` refuses a DLL that imports the MSVC runtime. It exports only
  `DllGetClassObject` and `DllCanUnloadNow` (`source.def`).
- **What it does.** It is a Media Foundation custom media source: activate, then
  `IMFMediaSourceEx` + `IMFGetService` + `IKsControl`, then one `IMFMediaStream2` delivering NV12 at
  three fixed sizes. It owns the shared section and frame event, and it treats every byte the
  helper writes as hostile: geometry comes from constants, the slot is copied under a seqlock, and
  the delivery thread is rate-limited. When nobody feeds it, it shows a built-in image. Outside
  Frame Server it is inert (`source.cpp` header comment; `vcam-win/README.md`).
- **Where it runs.** The Windows Camera Frame Server (`svchost -k Camera`) loads it as LocalService
  in session 0.
- **Install and registration.** These are helper code, not part of the DLL.
  `native/audio/win/vcam_install_win.cpp` (733 lines) copies the DLL to
  `%ProgramFiles%\Beamer\Camera\<kDllVersion>\`, checks its SHA-256 there and registers
  `HKLM\...\CLSID\{19FCB9D1-...}\InprocServer32` with `ThreadingModel=Both`. It also writes
  `HKLM\SOFTWARE\Beamer\Camera` (InstallGuid, OwnerSid, DllVersion, DllPath).
  `native/audio/win/vcam_win.cpp` (806 lines) calls `MFCreateVirtualCamera` (System lifetime,
  CurrentUser access) and is the shared-memory writer.
- **How the app talks to it.** Through shared memory and events only. The helper, never the DLL,
  is the app's side. See section 4.

Both helper-side files (`vcam_win.cpp`, `vcam_install_win.cpp`) and the Mac `vmic_mac.mm` belong
to the helper, which moves to Rust with the engine whatever this ticket decides. This ticket covers
only the two binaries that the OS loads into its own processes.

## 2. Mac: an AudioServerPlugIn in Rust

### What the platform requires (primary source: the SDK header)

`CoreAudio.framework/Headers/AudioServerPlugIn.h` (MacOSX.sdk, Command Line Tools, Oct 2026):

- The plug-in is a CFPlugIn whose factory returns an object conforming to
  `kAudioServerPlugInDriverInterfaceUUID`. That object is a C struct of a reserved pointer plus 22 function
  pointers (COM-style `QueryInterface`/`AddRef`/`Release` plus the driver calls). Any language that can
  export a C function and build a `#[repr(C)]` vtable can implement it.
- "An AudioServerPlugIn operates in its own process separate from the system daemon … may not make
  any calls to the client HAL API." That process is the "driver service process" measured in the
  README.
- "The host process is sandboxed": the plug-in can read only its bundle and system files. Mach
  services, custom IOKit user clients and the network must each be declared in Info.plist
  (`AudioServerPlugIn_MachServices`, `_IOKitUserClients`, `_Network`).
- "The IO operations are performed on a real time thread on a deadline. As such the plug-in must
  avoid blocking and return as quickly as possible." `GetZeroTimeStamp`, `BeginIOOperation`,
  `DoIOOperation` and `EndIOOperation` carry `CA_REALTIME_API`. `CoreAudioBaseTypes.h` defines it as
  clang's `[[clang::nonblocking]]` / `__attribute__((nonblocking))`, "A function (type) which is
  guaranteed / required to be realtime safe". In C, clang's function-effect analysis can therefore
  check the IO path at compile time. Rust has no equivalent attribute. The best Rust can do is a
  runtime guard in tests (an allocator that panics, such as `assert_no_alloc`).

Apple's own sample, [Creating an Audio Server Driver Plug-in](https://developer.apple.com/documentation/coreaudio/creating-an-audio-server-driver-plug-in),
is "Written in standard C". Apple ships no Rust or Swift path.

### Rust bindings and real implementations

| Source | What it shows |
|---|---|
| [`objc2-core-audio` 0.3.2](https://docs.rs/objc2-core-audio/0.3.2) `translation-config.toml` | It deliberately **skips** `AudioServerPlugInDriverInterface`, `AudioServerPlugInHostInterface` and their `Ref` typedefs ("Self-referential / recursive structs"). It also skips `AudioServerPlugInIOCycleInfo` because "the union field is not included, which is an ABI mismatch". So the best-maintained Apple bindings do not give you the plug-in interface. |
| [`coreaudio-sys` 0.2.17](https://docs.rs/coreaudio-sys) | Its default feature `audio_server_plugin` runs bindgen over `AudioServerPlugIn.h` at build time (`build.rs`), which needs libclang and the SDK. It gives raw types only. |
| [`tympan-aspl` 0.1.0](https://docs.rs/tympan-aspl) (MIT/Apache, 0 stars) | A safe `Driver` trait, a `plugin_entry!` macro, Info.plist generation and a `RealtimeContext` marker, tested with an `assert_no_alloc` guard. Its own docs say the live `extern "C"` entry points and CoreFoundation calls are "deferred to a later PR". It cannot load into coreaudiod yet. |
| [snapcaster](https://github.com/tsirysndr/snapcaster) `driver/src/lib.rs` (MIT, 2 stars, last pushed 2026-11-10) | A working Rust `cdylib` HAL driver: an output device plus a loopback input with a 16,384-frame ring, "modeled on Apple's NullAudio sample and BlackHole", ad hoc signed, installed with sudo plus a coreaudiod restart. It **hand-writes** all of the FFI (`#[repr(C)]` vtables, roughly 300 of its 1,511 lines) and depends on no crates. Its design is close to ours. |
| [vdev](https://github.com/gqf2008/vdev) (MIT) | Claims to be "all Rust" across macOS and Windows virtual devices, but its README does not say what language the HAL audio driver is written in. Not counted. |

### Feasibility, effort, risks

- **Feasible: yes.** snapcaster shows that coreaudiod loads a Rust `cdylib` bundle and that a
  NullAudio-style loopback ports almost line for line. A port would be about 1.2-1.5k lines of
  Rust: hand-written FFI (neither binding crate gives a usable vtable), the property dispatcher (most
  of the code) and the ring and clock. Packaging stays the same: `cargo build` per target, then
  `lipo`, Info.plist, `codesign`. Signing and the `beamer-hal` installer do not care which language
  built the bundle.
- **The existing tests carry over.** `test_host.c` loads whatever bundle it is given through
  `CFBundle`, so the 2,205 checks and `live_check.py` would validate a Rust build unchanged. This
  is the strongest argument that a port would be low-risk *if* it were wanted.
- **What Rust would cost.**
  1. Clang can check `CA_REALTIME_API` at compile time; Rust has only test-time guards. The C IO
     path is a short stretch of relaxed atomics, exactly where Rust adds little.
  2. Nearly every function is `unsafe extern "C"`: raw pointers in and out, `memcpy` into
     caller-sized buffers, and CFString/CFDictionary ownership. Rust's guarantees stop at the
     boundary, and the boundary is most of this code.
  3. Panics: since [Rust 1.81](https://blog.rust-lang.org/2024/09/05/Rust-1.81.0/), an unwind
     reaching an `extern "C"` function aborts the process. Here that kills the driver service and
     every HAL plug-in in it. A C bug crashes it too, but a Rust port must also avoid every implicit
     panic (indexing, `unwrap`, overflow in debug builds) on paths coreaudiod calls.
  4. There is no ecosystem to lean on: version 0.x crates with no users, so the FFI would be ours to
     maintain against future SDK changes.
- **What Rust would gain.** Very little. The driver is finished, validated live and tested under
  ASan, UBSan and TSan. It parses no untrusted input: the only data it reads comes through the HAL
  from its own feed device. It shares no data structure with the app, so keeping it in C creates no
  ABI to keep in sync with the Rust side.

## 3. Windows: a Frame Server media source in Rust

### What the platform requires (Microsoft Learn)

- [`MFCreateVirtualCamera`](https://learn.microsoft.com/en-us/windows/win32/api/mfvirtualcamera/nf-mfvirtualcamera-mfcreatevirtualcamera):
  `sourceId` is "the unique CLSID of the custom media source to be activated for this virtual
  camera". It requires Windows build 22000, and `MFVirtualCameraAccess_AllUsers` needs an
  administrator. This call is made by the **helper**. `windows-rs` already exposes it as
  [`windows::Win32::Media::MediaFoundation::MFCreateVirtualCamera`](https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/Media/MediaFoundation/fn.MFCreateVirtualCamera.html),
  so the Rust helper needs nothing special here.
- [Frame Server Custom Media Source](https://learn.microsoft.com/en-us/windows-hardware/drivers/stream/frame-server-custom-media-source)
  (updated 2025-05-08):
  - It "is a standard inproc COM server which must implement" `IMFMediaEventGenerator`,
    `IMFMediaSource`, `IMFMediaSourceEx`, `IKsControl` and `IMFGetService`. Each stream implements
    `IMFMediaStream` and `IMFMediaStream2`.
  - It "runs as Local Service … in Session 0 … and can't interact with the user desktop", and "must
    not attempt to access protected parts of the file system nor the registry … read access is
    permitted, but write access isn't".
  - The example registration uses `InprocServer32` with `ThreadingModel Both`.
  - Samples must come from `MFCreateSample` and the standard MF buffer allocators. The docs say
    "new/malloc/VirtualAlloc … must not be used for frame data".
  - Timestamps must be based on QPC.

### `windows-rs` `#[implement]`

The macro ([windows-implement 0.100.0](https://docs.rs/windows-core/latest/windows_core/attr.implement.html),
2026-09-03) "Implements one or more COM interfaces" on a Rust struct and generates `Foo_Impl` plus the
vtables. Its source shows that by default (`agile: true`) `QueryInterface` answers `IAgileObject`
and hands out the free-threaded marshaler for `IMarshal` (`src/gen.rs`, `identity_query` /
`marshal_query`). That is the equivalent of WRL's `FtmBase`, which `source.cpp` uses on every class.
`windows-rs` generates `IMFMediaSourceEx_Impl`, `IMFMediaStream2_Impl` and `IKsControl_Impl` from
the Windows metadata, so the full interface set is available.

### Real Rust implementations

| Source | What it shows |
|---|---|
| [DeskCam (sinnafuls/rust-virtual-cam)](https://github.com/sinnafuls/rust-virtual-cam), crate `deskcam-source` | A Frame Server media source in Rust on `windows` 0.62.2, released with an installer. It uses `#[implement(IMFMediaSourceEx, IMFAttributes, IMFGetService, IKsControl, IMFSampleAllocatorControl)]` and `#[implement(IMFMediaStream2, IMFAttributes, IKsControl)]`, hand-written `DllGetClassObject`/`DllCanUnloadNow`/`DllRegisterServer`, a separate `deskcam-proto` crate for the shared-memory layout used by both producer and source, and `panic = "abort"` in both profiles. It is about 60 KB of Rust and "mirrors smourier/VCamSample". This is direct evidence that our design (activate, source, one stream, a delivery thread, a shared section) ports. |
| [smourier/VCamSample](https://github.com/smourier/VCamSample) (C++, MIT) | The reference that both DeskCam and our `source.cpp` follow. |
| JStream PR #25, skakel PR #24 | Other Rust ports of VCamSample; found by search but not read, and described by their authors as early or compile-checked only. Not counted. |

### Feasibility, effort, risks

- **Feasible: yes.** The bindings are complete and generated by Microsoft, and the agile/FTM
  behaviour matches WRL by default. DeskCam is a working example of the same architecture. A port
  of `source.cpp` would be roughly the same size, about 1.2-1.5k lines.
- **Effort is not small, and most of it is outside the code.** The parts that took work in C++ are
  Windows security details, and Rust does not make them shorter:
  - DACLs on the section and events: `D:P(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;<OwnerSid>)`.
  - Rejecting squatted objects: a section or event that already exists must be owned by
    LocalService or SYSTEM.
  - The LocalService check that keeps the source inert in the helper process.
  - The log's token bucket.
  - DLL update and unload behaviour measured against Frame Server (`dev/camera/reports/cw5.md`).

  All of this needs re-testing on the PC with `camprobe`, `feeder` and `shmfuzz`. A person also has
  to check real apps (Camera, Teams, browsers), because Frame Server behaviour cannot be unit tested.
- **Gains.** This is where Rust helps most. The DLL runs as a service account and reads memory a
  user-session process writes (`source.cpp`: "treats every helper-written byte as hostile").
  Bounds-checked parsing of the seqlocked slots is a genuine safety improvement. One
  `#[repr(C)]` protocol crate shared by the helper and the DLL, as DeskCam's `deskcam-proto` does,
  would also replace the duplicated layout described in section 4.
- **Risks.**
  1. A panic aborts `svchost -k Camera`, which ends every app's camera stream until the service
     restarts (Rust 1.81 semantics, as above; inferred, not tested). A C++ access violation does the same, but
     the Rust code must be written panic-free on every COM entry point.
  2. Matching `/MT`: a Rust `cdylib` on `x86_64-pc-windows-msvc` imports `vcruntime140.dll` unless
     it is built with `-C target-feature=+crt-static`. `build.ps1`'s check would catch that.
  3. `windows-rs` is still 0.x and went from 0.62 to 0.100 by September 2026. Expect API churn on
     upgrades.
  4. `kDllVersion` must be bumped and the DLL reinstalled into a new version folder, which shows up
     as `install: outdated` on existing installs.

## 4. The ABI the Rust app talks to if the plug-ins stay native

### Mac driver: no binary ABI

The Rust helper uses the public Core Audio HAL client API (via `objc2-core-audio` or
`coreaudio-sys`, which both bind `AudioHardware.h`):

| Contract | Value |
|---|---|
| Feed device (render into it) | UID `com.beamer.audio.micfeed`, found with `kAudioHardwarePropertyTranslateUIDToDevice` on `kAudioObjectSystemObject` (hidden, so not in the device list) |
| Microphone (apps record it) | UID `com.beamer.audio.mic`; demand = `kAudioDevicePropertyDeviceIsRunningSomewhere` |
| Format | Float32 mono, 48 kHz or 44.1 kHz (the HAL converts any client rate) |
| Controls | `kAudioDevicePropertyVolumeScalar` / `kAudioDevicePropertyMute`, input scope, on the mic |
| Lifecycle | After a coreaudiod restart, translate the UID again and restart the IO proc (README, live check 6) |
| Install | `beamer-hal install / uninstall / restart / status`, staged bundle under `~/Library/Application Support/Beamer/hal-staging/` |

Nothing is linked and nothing is shared in memory, so the language of either side is irrelevant.

### Windows camera: shared memory, kernel objects, registry

Everything the helper needs is defined in `vcam-win/shm_protocol.h` (layout version `kVersion = 1`,
`kDllVersion = 1`):

| Contract | Value |
|---|---|
| COM class | `{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}`, `InprocServer32` in HKLM, `ThreadingModel=Both` (written by the helper's `vcam-install`) |
| Install record | `HKLM\SOFTWARE\Beamer\Camera`: `InstallGuid`, `OwnerSid`, `DllVersion`, `DllPath` |
| Camera | `MFCreateVirtualCamera(SoftwareCameraSource, System, CurrentUser, "Beamer Camera", CLSID)` from the helper in the user's session |
| Section | `Global\BeamerCamera.<InstallGuid>.Shm`, 12,455,936 bytes, created **by the DLL**; the helper opens it |
| Events | `.Frame` (auto-reset, DLL creates, helper sets after a write); `.Demand` (helper creates, DLL sets on Start/Stop/Shutdown) |
| Security | DACL SYSTEM + LocalService + OwnerSid only; whichever side opens an existing object must check that its owner is LocalService or SYSTEM |
| Pixels | NV12, display size (never the coded 1088), slot `i` at `4096 + i * 3,112,960`; slots 0-2 live, slot 3 the "waiting" picture |
| Media types | 1280x720, 1920x1080 and 640x360 at 30 fps, in that order |

Header and slot layout. These offsets were measured with clang++ C++20 on arm64 macOS. Every field
is a fixed-width integer at its natural alignment, so MSVC x64 should give the same offsets, but
that has not been measured on the PC (see "Not verified" at the end):

```
Slot (64 B, align 64)            Header (448 B, align 64; slots at 192)
  0 seq        atomic u32 seqlock  0 magic 'BCAM'   4 version   8 header_bytes  12 slot_count
  4 w  8 h  12 stride  u32        16 state_seq atomic u32   20 streaming   24/28/32 want_w/h/fps
 16 written_qpc u64               40 state_qpc u64   48 dll_version   52 source_pid
 24 picture    u32                56/64/72/80 delivered/placeholders/repeats/rejected atomic u64
 28 rsv[9]                        88 helper_pid   96 beat_qpc atomic u64   104 mode atomic u32
                                 108 latest atomic u32   112 rsv[16]   192 slots[4]
```

Helper obligations, which the DLL checks:

- Write a slot as: `seq` to odd, then the pixels and `w`/`h`/`stride`/`written_qpc`/`picture`,
  then `seq` to even (release).
- Never reset `seq`, and set `written_qpc` on every write.
- Keep `beat_qpc` fresh: a beat older than 500 ms means the built-in image is shown.
- Set `mode` and `latest`.
- Set `.Frame` after each picture.

In Rust this is a `#[repr(C, align(64))]` mirror using `AtomicU32`/`AtomicU64`, with
`const _: () = assert!(offset_of!(Header, slots) == 192)` and similar checks for every field. The
existing `feeder.exe` and `shmfuzz.exe` remain the cross-language conformance tests.

## 5. Recommendation

| Plug-in | Recommendation | Why |
|---|---|---|
| **Mac HAL driver** (`mac-driver`, C) | **Keep it in C.** Do not port. | Feasible (snapcaster), but it gains almost nothing: the driver is finished, sanitizer-tested and validated live, it reads no untrusted input and shares no ABI with the app. Porting would lose clang's `CA_REALTIME_API` checks and add panic-abort risk inside the driver service. The Rust app talks to it only through public Core Audio calls on two UIDs. If a port is ever wanted, `test_host.c` and `live_check.py` validate a Rust bundle unchanged, so the risk would be contained. |
| **Windows camera source** (`vcam-win`, C++) | **Keep it in C++ for the rewrite. Port it to Rust later, as its own task, when the shared-memory protocol next has to change** (a `kVersion` or `kDllVersion` bump forces a reinstall anyway). | Feasible with `windows-rs` `#[implement]` (DeskCam is a working precedent with the same design), and it is the one place where memory safety pays (a service-account process reading hostile shared memory) and where a shared protocol crate removes ABI duplication. But it costs a full Frame Server re-test with a person at the PC, plus a DLL reinstall for every user. The C++ contract is frozen and small, and the Rust helper can implement it as a `#[repr(C)]` mirror with offset asserts. |

So the Rust rewrite's plan (ticket #16, which this blocks) can treat both plug-ins as external
binaries built by their existing scripts. The Rust helper owns everything the C++ helper owns today:
the `beamer-hal` calls on the Mac; `vcam-install`, `vcam-uninstall`, `vcam-remove`,
`MFCreateVirtualCamera` and the shared-memory writer on Windows.

## Not verified

- No Rust driver or DLL was built or loaded in this research. Feasibility rests on the cited
  working projects (snapcaster, DeskCam), on reading their source, and on the platform docs.
- I did not run snapcaster or DeskCam. I did not check whether either has been tested by anyone
  other than its author. DeskCam's README says its installer is unsigned.
- The `Header`/`Slot` offsets were measured with clang on arm64 macOS, not with MSVC x64. Verify
  them on the PC (or with `static_assert`s added to `shm_protocol.h`) before writing the Rust
  mirror.
- `tympan-aspl`'s claim that it cannot load yet comes from its docs.rs page; I did not read its
  source.
- Line-count estimates for a port are extrapolated from the existing C/C++ and from
  snapcaster/DeskCam, not measured.
