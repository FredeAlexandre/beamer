# Rust OS bindings for Windows: every OS call mapped to a crate

Research for [#7](https://github.com/FredeAlexandre/beamer/issues/7), part of the Rust port map
([#1](https://github.com/FredeAlexandre/beamer/issues/1)). Written 2026-11-13 against `main` at `fffbede`.

## Answer

**Every Windows touchpoint Beamer has today has a workable Rust path. None blocks the port.**

- Almost everything maps to Microsoft's [`windows`](https://crates.io/crates/windows) crate. The
  published version is 0.62.2. Every function and COM interface Beamer uses was found in its source
  except one: `CreateSyntheticPointerDevice2` (touchpad gestures) and its parameter struct. That one
  is a single hand-written `extern` declaration.
- WASAPI can go through [`wasapi`](https://crates.io/crates/wasapi) 0.27.0, which is built on
  `windows` 0.62. It covers process loopback that excludes a process tree, `IAudioClient3`
  low-latency shared periods, exclusive mode and device notifications.
- **The biggest risk is that windows-rs is changing generation.** The `windows` and `windows-sys`
  crates on crates.io (0.62.2 and 0.61.2) are from October 2025. The project's master branch has
  moved to a 0.100 line built from different metadata, which renames every Cargo feature and
  changes many signatures, and the two umbrella crates have not been published in that line.
  The build spec has to pick a line (Gap 1).
- The other costs are workload, not missing bindings:
  - **Hand-written unsafe COM with no safe Rust wrapper**: Media Foundation H.264 decode, Task
    Scheduler, Windows Firewall and network category, the Explorer de-elevation sequence, and the
    display configuration (CCD) API.
  - **Rules for the hook thread**: low-level hooks need a dedicated thread with a message loop and
    a strict time budget, and a Rust panic in a callback aborts the process.

## How this was checked

- **Inventory.** Every ctypes, pywin32, `winreg` and subprocess call in `win_app/` was read. Linux
  and X11 files (`*_x11.py`, `*portal*.py`, `libei.py`, `linux_*.py`) were skipped. The search also
  covered `win_app/hooks`, the Windows branches of `core/`, the Windows parts of
  `native/audio/engine`, and the WASAPI and Media Foundation code in `native/audio/win`. The
  virtual camera (`native/audio/vcam-win`, `vcam_win.cpp`, `vcam_install_win.cpp`) is left to its
  own ticket.
- **Crate paths.** Each API name was looked up in the source of `windows` 0.62.2 as downloaded from
  crates.io: a `pub fn`, `pub struct`, `pub const` or `define_interface!` declaration. The module
  path shown is the Cargo feature to enable, for example `Win32::UI::WindowsAndMessaging` is
  feature `Win32_UI_WindowsAndMessaging`.
- **Maturity.** Crate versions, dates, download counts and dependencies come from the crates.io API
  on 2026-11-13. Behaviour comes from Microsoft Learn and from crate source.

## Table: touchpoint, where it is used today, Rust option, maturity and risk

The "Today" column names `win_app/` files unless a path says otherwise. "`windows`" means the
`windows` 0.62.2 crate, with its module path.

### Input capture, blocking and injection

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| Low-level keyboard and mouse hooks (`SetWindowsHookExW` with `WH_KEYBOARD_LL`/`WH_MOUSE_LL`, `CallNextHookEx`, `UnhookWindowsHookEx`). **Input blocking** works by returning 1 from the callback to swallow the event. | `capture_win.py`, on its own `Beamer-hooks` thread | `windows` `Win32::UI::WindowsAndMessaging` | API: mature. Risk: **high on design, low on bindings**. The hook runs on the thread that installed it, and that thread needs a message loop. The callback must return within `LowLevelHooksTimeout`, which is capped at 1000 ms since Windows 10 1709. Past that, Windows 7 and later remove the hook silently ([LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc)). A panic escaping an `extern "system"` callback aborts the process ([Rust 1.81](https://blog.rust-lang.org/2024/09/05/Rust-1.81.0/)), so wrap every callback in `catch_unwind`. Do not use `rdev` 0.5.3 or `willhook` 0.6.3: both are on `winapi` 0.3 and were last released in 2023. |
| Raw Input relative mouse counts (`RegisterRawInputDevices` with `RIDEV_INPUTSINK`, `GetRawInputData`) on a message-only window (`RegisterClassW`, `CreateWindowExW(HWND_MESSAGE)`, `DefWindowProcW`, `UnregisterClassW`) | `capture_win.py` | `windows` `Win32::UI::Input` and `Win32::UI::WindowsAndMessaging` | Mature. Lives on the hook thread and shares its message loop. The window class must be unregistered when the thread stops; today's code documents a crash when it is not. |
| Watching for input desktop switches (`SetWinEventHook(EVENT_SYSTEM_DESKTOPSWITCH, WINEVENT_OUTOFCONTEXT)`, `UnhookWinEvent`) | `capture_win.py` | `windows` `Win32::UI::Accessibility` | Mature. Also needs the message loop. |
| Hook thread loop and priority (`GetMessageW`, `TranslateMessage`, `DispatchMessageW`, `PostThreadMessageW` to stop, `SetThreadPriority(THREAD_PRIORITY_HIGHEST)`) | `capture_win.py` | `windows` `Win32::UI::WindowsAndMessaging` and `Win32::System::Threading` | Mature. `windows-window` 0.100 ([newsletter](https://github.com/microsoft/windows-rs/issues/4867)) offers a safe HWND and message loop, but it is three months old and on the new line. Use the raw loop here. |
| Key state and layout translation (`GetKeyState`, `GetAsyncKeyState`, `GetKeyboardLayout`, `ToUnicodeEx`, `VkKeyScanW`, `MapVirtualKeyW`) | `capture_win.py`, `input_injector.py` | `windows` `Win32::UI::Input::KeyboardAndMouse` | Mature. Keep passing `wFlags` bit 2 to `ToUnicodeEx`, as today. That bit leaves the kernel keyboard state alone, so dead keys are not disturbed (Windows 10 1607 and later; [ToUnicodeEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicodeex)). |
| Injecting keys, text (`KEYEVENTF_UNICODE`), mouse, wheel and buttons (`SendInput`), with `INJECTED_MARK` in `dwExtraInfo` so Beamer's own hook ignores them | `input_injector.py` | `windows` `Win32::UI::Input::KeyboardAndMouse::SendInput` | Mature. A direct call keeps control of `dwExtraInfo`; a wrapper such as `enigo` 0.6.1 adds nothing Beamer needs. When UIPI blocks `SendInput`, neither the return value nor `GetLastError` says so ([SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput)). That is why the foreground diagnostics below exist. |
| Diagnosing why input was refused: the foreground window and its process integrity (`GetForegroundWindow`, `GetWindowThreadProcessId`, `GetClassNameW`, `OpenProcess`, `QueryFullProcessImageNameW`, `OpenProcessToken`, `GetTokenInformation` with `TokenIntegrityLevel`/`TokenElevation`, `GetSidSubAuthority`, `IsValidSid`) | `input_blocker.py`, `input_injector.py` | `windows` `Win32::UI::WindowsAndMessaging`, `Win32::System::Threading` and `Win32::Security` | Mature. |
| Recovering from a stuck GameInput window (`schtasks /Run /TN Beamer-GameInputRecovery`) | `input_injector.py` | `ITaskService` → `ITaskFolder::GetTask` → `IRegisteredTask::Run` (`windows` `Win32::System::TaskScheduler`), or keep running `schtasks.exe` through `std::process::Command` with `CREATE_NO_WINDOW` | Mature either way. |
| Touchpad gesture injection (`CreateSyntheticPointerDevice2` with `PT_TOUCHPAD`/`SDCO_PHYSICAL_SIZE`, `InjectSyntheticPointerInput`, `DestroySyntheticPointerDevice`) | `touchpad_injector.py` | `InjectSyntheticPointerInput` is in `windows` `Win32::UI::Input::Pointer`; `DestroySyntheticPointerDevice` is in `Win32::UI::Controls`. **`CreateSyntheticPointerDevice2` and `SYNTHETIC_DEVICE_CREATION_PARAMS` are missing from 0.62.2.** Both are in windows-rs master (`crates/libs/sys/src/Windows/Win32/winuser/mod.rs`, from SDK 10.0.28000). | **Small gap.** Declare the function with `windows_link::link!("user32.dll" "system" fn CreateSyntheticPointerDevice2(...))` and write the `#[repr(C)]` struct by hand, as the Python does. The API is Windows 11 only ([CreateSyntheticPointerDevice2](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createsyntheticpointerdevice2), page dated 2026-07-24). |

### Pointer, monitors, overlays and the desktop

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| Pointer and monitor geometry (`GetCursorPos`, `SetCursorPos`, `EnumDisplayMonitors`, `GetMonitorInfoW`, `MonitorFromWindow`, `GetWindowRect`, `GetSystemMetrics`) | `desktop_win.py`, `capture_win.py` | `windows` `Win32::UI::WindowsAndMessaging` and `Win32::Graphics::Gdi` | Mature. These calls only share one coordinate space if the process is per-monitor-v2 DPI aware. Qt sets that today, so the Rust app must declare it in its manifest or call `SetProcessDpiAwarenessContext` (`Win32::UI::HiDpi`). |
| Hiding and restoring the system pointer (`CreateCursor` with a blank cursor, `SetSystemCursor` for each `OCR_*`, `SystemParametersInfoW(SPI_SETCURSORS)` to restore) | `desktop_win.py` | `windows` `Win32::UI::WindowsAndMessaging` | Mature. Risk: a crash leaves every cursor blank. Today an `atexit` handler restores them; the Rust app also needs a restore at startup and in its panic hook. |
| Cursor overlay for a remotely driven PC with no mouse (`GetCursorInfo`, `GetIconInfo`, `CreateIconIndirect`, `DestroyIcon`, `DeleteObject`, `LoadCursorW`) | `cursor_overlay.py` | `windows` `Win32::UI::WindowsAndMessaging` and `Win32::Graphics::Gdi` | Mature. Drawing the overlay belongs to the UI toolkit. |
| Fullscreen detection (`SHQueryUserNotificationState`) | `desktop_win.py` | `windows` `Win32::UI::Shell` | Mature. |
| Name of the input desktop (`OpenInputDesktop`, `GetUserObjectInformationW(UOI_NAME)`, `CloseDesktop`) | `desktop_win.py` | `windows` `Win32::System::StationsAndDesktops` | Mature. |
| Click-through, always-on-top overlays (`GetWindowLongPtrW`/`SetWindowLongPtrW` with `WS_EX_TRANSPARENT \| WS_EX_LAYERED`, `SetWindowPos(HWND_TOPMOST)`), and the reduce-motion setting (`SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION)`) | `desktop_win.py`, `edge_glow.py`, `effect_overlay.py` | `windows` `Win32::UI::WindowsAndMessaging` | Mature. Needs the HWND from the UI toolkit (`raw-window-handle` 0.6), which is a question for the UI ticket. |
| Title bar colours (`DwmSetWindowAttribute` with 20 `USE_IMMERSIVE_DARK_MODE`, 19 its pre-20H1 alias, 34, 35 and 36) | `titlebar.py` | `windows` `Win32::Graphics::Dwm` | Mature. Attribute 19 is undocumented, so pass it as a raw number. |
| Light or dark theme (registry values `AppsUseLightTheme` and `SystemUsesLightTheme`) | `theme.py`, `tray_glyph.py` | [`windows-registry`](https://crates.io/crates/windows-registry) 0.100.0, published 2026-09-03 and independent of the umbrella crate, or `winreg` 0.56 | Mature. |
| Tray icon (`QSystemTrayIcon` today, which is `Shell_NotifyIconW`) | `kvm_bridge_win.py` | [`tray-icon`](https://crates.io/crates/tray-icon) 0.26.1 (on `windows-sys` 0.60 to 0.61), or `Shell_NotifyIconW` directly (`windows` `Win32::UI::Shell`) | Mature. On Windows, "an event loop must be running on the thread" ([tray-icon README](https://github.com/tauri-apps/tray-icon)). This is for the UI ticket. |
| Error dialog before the UI exists (`MessageBoxW`) | `portable_entry.py` | `windows` `Win32::UI::WindowsAndMessaging` | Mature. |

### Clipboard, displays and brightness

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| Clipboard text and images (`OpenClipboard`, `EmptyClipboard`, `GetClipboardData`/`SetClipboardData` for `CF_UNICODETEXT` and `CF_DIB` with V5 headers, `IsClipboardFormatAvailable`, `GetClipboardSequenceNumber` polling, `GlobalAlloc`/`GlobalLock`/`GlobalUnlock`/`GlobalSize`/`GlobalFree`) | `clipboard_win.py` | `windows` `Win32::System::DataExchange` and `Win32::System::Memory`, about 150 lines. Alternatives are `clipboard-win` 5.4.3 (its own `windows-win` bindings) and `arboard` 3.7.0 (`windows-sys` < 0.62). | Mature. The logic Beamer cares about is its own: DIB header variants, top-down rows, CRLF, retries on open, and sequence-number stamps. Calling the API directly keeps that logic as it is. `AddClipboardFormatListener` is also available if polling gives way to change events. |
| Shared display on and off through CCD (`GetDisplayConfigBufferSizes`, `QueryDisplayConfig`, `SetDisplayConfig`, `DisplayConfigGetDeviceInfo`) | `shared_display_win.py` | `windows` `Win32::Devices::Display` | Mature. The structs are full of unions and are generated, so the Python's hand-written size checks (72 and 64 bytes) go away. |
| DDC/CI through dxva2 (`GetPhysicalMonitorsFromHMONITOR`, `GetVCPFeatureAndVCPFeatureReply`, `SetVCPFeature`) | **Not in shipping code.** Probes only; `dev/display/DESIGN.md` decides "No DDC". | `windows` `Win32::Devices::Display` | Available if ever needed. Avoid `ddc-winapi` 0.2.2 and `ddc-hi` 0.4.1, which are on `winapi` 0.3 and were last released in 2024 and 2021. |
| Built-in display brightness: WMI `root\wmi` `WmiMonitorBrightness` and `WmiMonitorBrightnessMethods.WmiSetBrightness`, through pywin32 `Dispatch` | `display_brightness.py` | [`wmi`](https://crates.io/crates/wmi) 0.18.4 (`windows` 0.59 to 0.62, has `exec_method`), or `IWbemLocator`/`IWbemServices::ExecMethod` (`windows` `Win32::System::Wmi`) | Mature. Each calling thread must initialise COM. |

### Process, session, single instance, unlock

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| Single instance (`CreateMutexW` with `Local\Beamer.Receiver<suffix>` and `Local\Beamer.InputController`, `OpenMutexW`, `ERROR_ALREADY_EXISTS`) | `kvm_bridge_win.py` | `windows` `Win32::System::Threading` | Mature. Do not use `single-instance` 0.3.3: it is on `winapi` 0.3 and was last released in 2021. |
| IPC between instances (`QLocalServer`, a named pipe limited to the current user) | `kvm_bridge_win.py` | `tokio::net::windows::named_pipe` (tokio 1.54.1) or `interprocess` 2.4.8 (`windows-sys` 0.61) | Mature. Limiting the pipe to the current user needs explicit `SECURITY_ATTRIBUTES`, built with `ConvertStringSecurityDescriptorToSecurityDescriptorW` (`windows` `Win32::Security::Authorization`). |
| Priority and power throttling (`SetPriorityClass`, `SetProcessInformation(ProcessPowerThrottling, PROCESS_POWER_THROTTLING_STATE)`) | `kvm_bridge_win.py` | `windows` `Win32::System::Threading` | Mature. |
| Session IDs (`ProcessIdToSessionId`, `WTSGetActiveConsoleSessionId`) | `kvm_bridge_win.py`, `unlock_win.py` | `windows` `Win32::System::RemoteDesktop` | Mature. |
| Unlock request: signal the external credential provider's event (`OpenEventW(EVENT_MODIFY_STATE, "Global\RoomDeckUnlock.<session>")`, `SetEvent`) and detect the lock screen by finding `LogonUI.exe` (`CreateToolhelp32Snapshot`, `Process32FirstW`/`Process32NextW`) | `unlock_win.py` | `windows` `Win32::System::Threading` and `Win32::System::Diagnostics::ToolHelp` | Mature. The provider DLL is outside this repository and stays outside. |
| Autostart: a logon scheduled task registered from XML (`schtasks /create /xml`, `/query /xml`, `/delete`, `/change /disable`), plus the interactive user (`WTSQuerySessionInformationW` for `WTSUserName`/`WTSDomainName`, `GetUserNameExW`) | `autostart_win.py` | Either `ITaskService::Connect` → `GetFolder` → `ITaskFolder::RegisterTask(xml, TASK_CREATE_OR_UPDATE, …, TASK_LOGON_INTERACTIVE_TOKEN)` (`windows` `Win32::System::TaskScheduler`), which can take today's XML unchanged, or keep `schtasks.exe` through `Command` with `CREATE_NO_WINDOW`. User lookup: `windows` `Win32::System::RemoteDesktop` and `Win32::Security::Authentication::Identity`. | API: mature. Awkward: the COM interface is `BSTR`/`VARIANT` heavy. Avoid `planif` 1.0.0 (on `windows` 0.48, released 2023). `auto-launch` 0.6.0 writes the `Run` registry key, which cannot express `HighestAvailable`, so it does not fit. |
| Admin mode: token elevation (`GetTokenInformation` with `TokenElevation`/`TokenElevationType`), a helper started elevated (`ShellExecuteExW` with the `runas` verb and `SEE_MASK_NOCLOSEPROCESS`, then `WaitForSingleObject` and `GetExitCodeProcess`), and the preference stored in HKCU | `admin_mode.py` (pywin32) | `windows` `Win32::Security`, `Win32::UI::Shell` and `Win32::System::Threading`; `windows-registry` for the preference | Mature. |
| Admin mode: relaunching unelevated through Explorer (`IShellWindows::FindWindowSW(CSIDL_DESKTOP)` → `IServiceProvider::QueryService(SID_STopLevelBrowser, IShellBrowser)` → `QueryActiveShellView` → `IShellView::GetItemObject(SVGIO_BACKGROUND)` → `IShellFolderViewDual::Application` → `IShellDispatch2::ShellExecute`) | `admin_mode.py` | All of these are in `windows` `Win32::UI::Shell` and `Win32::System::Com` | API: mature. Awkward: needs an STA thread (`CoInitializeEx(COINIT_APARTMENTTHREADED)`) and `VARIANT` arguments for `FindWindowSW`. About 80 lines of unsafe COM. |
| UIAccess | **Not used.** `core/tests/test_windows_launch_manifest.py` asserts `uac_uiaccess` is false. | Manifest attribute only | Not a binding question. If it is ever wanted, Windows requires a signature that chains to a trusted root and an install under `%ProgramFiles%` or `%WinDir%` ([UAC policy](https://learn.microsoft.com/en-us/windows/security/threat-protection/security-policy-settings/user-account-control-only-elevate-uiaccess-applications-that-are-installed-in-secure-locations)). That is for the packaging work. |
| App manifest and icon: DPI awareness, `asInvoker`, no `uiAccess` (set by PyInstaller `build.spec` today) | `build.spec`, `build_portable.spec` | `embed-resource` 3.0.12 or `winresource` 0.1.34 in `build.rs` | Mature. |

### Firewall, network, locale

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| Firewall rules and network category through PowerShell NetSecurity cmdlets (`Get-NetFirewallApplicationFilter`, `Get-NetFirewallPortFilter`, `New-NetFirewallRule`, `Remove-NetFirewallRule`, `Get-NetConnectionProfile`, `Set-NetConnectionProfile -NetworkCategory Private`), with `IsUserAnAdmin` | `firewall_win.py` | `INetFwPolicy2`/`INetFwRules`/`INetFwRule` (`windows` `Win32::NetworkManagement::WindowsFirewall`) and `INetworkListManager` → `INetwork::SetCategory` (`windows` `Win32::Networking::NetworkListManager`). Or keep PowerShell through `Command`. | APIs present. **Medium effort**: dispatch-style COM with `BSTR`/`VARIANT`, and the rule-matching logic has to be rewritten against COM objects. Writing needs elevation either way. Moving it to the installer is also an option for the packaging work. |
| Interface addresses (`GetAdaptersAddresses`) | `hardware_win.py`, `core/pairing.py` | `windows` `Win32::NetworkManagement::IpHelper`, or `netdev` 0.47.0 / `if-addrs` 0.15.0 | Mature. |
| MAC address for Wake-on-LAN (runs `arp -a` today) | `core/wol.py` | `GetIpNetTable2` or `SendARP` (`windows` `Win32::NetworkManagement::IpHelper`) | Mature, and removes a subprocess. |
| Locale and UI languages (`GetUserDefaultLocaleName`, `GetUserPreferredUILanguages`) | `core/locale.py`, `core/i18n.py` | `windows` `Win32::Globalization`, or `sys-locale` 0.3.2 | Mature. |

### Audio engine (`native/audio/win`, C++ today)

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| COM and threads (`CoInitializeEx` MTA on each worker, `CreateThread`, events, `WaitForMultipleObjects`) | `backend_win.cpp` | `windows` `Win32::System::Com` and `Win32::System::Threading`, plus `std::thread` | Mature. Every audio and Media Foundation thread joins the MTA. `windows-core` 0.100 adds `init_mta()`. |
| Device inventory, default device and notifications (`IMMDeviceEnumerator`, `EnumAudioEndpoints`, `GetDefaultAudioEndpoint`, `IPropertyStore` with `PKEY_AudioEndpoint_FormFactor`, `RegisterEndpointNotificationCallback` with `IMMNotificationClient`) | `devices_win.cpp`, `backend_win.cpp` | [`wasapi`](https://crates.io/crates/wasapi) 0.27.0 (`windows` ^0.62.2, last pushed 2026-11-11, device tests run against VB-CABLE), or `windows` `Win32::Media::Audio` with `#[implement]` | Mature. `cpal` 0.18.2 also sits on `windows` 0.62 but is too high-level for Beamer's routing. |
| Whether a device is physical or virtual, from its devnode chain (`CM_Locate_DevNodeW`, `CM_Get_Parent`, `CM_Get_Device_IDW`, `CM_Get_DevNode_PropertyW`) | `devices_win.cpp` | `windows` `Win32::Devices::DeviceAndDriverInstallation` | Mature. Not covered by `wasapi`. |
| Shared capture and render with a low-latency period (`IAudioClient3::GetSharedModeEnginePeriod`, `InitializeSharedAudioStream` at 128 frames), exclusive render, event-driven buffers | `loopback_win.cpp` | `wasapi`: `get_shared_mode_engine_period`, `initialize_shared_audio_stream`, exclusive mode | Mature ([docs.rs](https://docs.rs/wasapi/0.27.0/wasapi/struct.AudioClient.html)). |
| Endpoint loopback with keep-alive (the echo-cancellation reference) | `loopback_win.cpp` | `wasapi` loopback capture | Mature. |
| **System sound**: process loopback that excludes Beamer's process tree (`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)`, `AUDIOCLIENT_ACTIVATION_PARAMS`, an agile completion handler) | `loopback_win.cpp` | `wasapi::AudioClient::new_application_loopback_client(pid, include_tree)`. **`include_tree = false` selects `PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE`** in the source ([src/api.rs](https://github.com/HEnquist/wasapi-rs/blob/master/src/api.rs)), although the doc comment says "only the target process". | Mature. Needs Windows 10 build 20438 or later. The handler must be agile on versions before Windows 10 ([ActivateAudioInterfaceAsync](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync)); `wasapi` handles that. |
| MMCSS "Pro Audio" (`AvSetMmThreadCharacteristicsW`, `AvRevertMmThreadCharacteristics`) | `devices_win.cpp` | `windows` `Win32::System::Threading` | Mature. |
| Endpoint mute and volume journal (`IAudioEndpointVolume::Get`/`SetMute`, master and per-channel volume, an event-context GUID to tell Beamer's writes from the user's, `RegisterControlChangeNotify` with `IAudioEndpointVolumeCallback`) | `mute_journal_win.cpp` | `windows` `Win32::Media::Audio::Endpoints` with `#[implement]` for the callback (`wasapi` also exposes volume notifications) | Mature. |
| Mute guard process (`CreateProcessW` of `beamer-audio _guard --pid P`, `OpenProcess`, `WaitForSingleObject`, `GetProcessTimes` against PID reuse) | `mute_journal_win.cpp` | `std::process::Command` plus `windows` `Win32::System::Threading` | Mature. |
| Detecting other apps capturing from CABLE Output (`IAudioSessionManager2`, `GetSessionEnumerator`, `IAudioSessionControl2::GetProcessId`, `RegisterSessionNotification` with `IAudioSessionNotification`, `IAudioSessionEvents`) | `sessions_win.cpp` | `windows` `Win32::Media::Audio` with `#[implement]`, or `wasapi` session controls | Mature. |
| Timer resolution and clock (`timeBeginPeriod(1)`, `QueryPerformanceCounter`) | `engine/util.cpp`, `devices_win.cpp` | `windows` `Win32::Media::timeBeginPeriod`; `std::time::Instant` for the clock | Mature. |
| UDP sockets (`WSAStartup`, `SO_RCVBUF`/`SO_SNDBUF`, `WSAIoctl(SIO_UDP_CONNRESET)` off, `WSAPoll`) | `engine/net.cpp` | `std::net::UdpSocket` with `socket2` 0.6.5. One raw `WSAIoctl` on `as_raw_socket()` for `SIO_UDP_CONNRESET` (`windows` `Win32::Networking::WinSock`). Or tokio. | Mature. |
| Random bytes (`BCryptGenRandom`) and SHA-256 (`BCryptHash`, used by the vcam installer) | `engine/util.cpp` | `getrandom` 0.4.3 or `std`; `windows` `Win32::Security::Cryptography` if a hash is still needed | Mature. |
| Registry reads (`RegGetValueW`, `RegOpenKeyExW`, `RegEnumKeyExW`) and the app name of a camera user (`SHLoadIndirectString`, `GetFileVersionInfoW`) | `app_usage_win.cpp`, `backend_win.cpp` | `windows-registry`; `windows` `Win32::UI::Shell` and `Win32::Storage::FileSystem` | Mature. |

### Video (`native/audio/win`, C++ today)

| Touchpoint | Today | Rust option | Maturity / risk |
|---|---|---|---|
| H.264 decode through Media Foundation: the decoder MFT (`CLSID_MSH264DecoderMFT`, `IMFTransform`, `ICodecAPI`), D3D11 (`D3D11CreateDevice`, `ID3D10Multithread`, `MFCreateDXGIDeviceManager`, `IMFDXGIBuffer`, `IMF2DBuffer2`), staging readback, software fallback, device loss | `videodec_win.cpp`, `video_backend_win.cpp` | `windows` `Win32::Media::MediaFoundation` and `Win32::Graphics::Direct3D11`/`Direct3D10`. Every symbol is present. Software alternative: `openh264` 0.9.10. | API coverage complete. **Medium risk**: there is no maintained safe Rust wrapper for Media Foundation, so this is the largest block of hand-written unsafe COM in the port. |
| H.264 encode through Media Foundation | **Not on Windows today.** The PC only decodes; the Mac encodes in `mac/videoenc_mac.mm`. | The same `windows` module, if a PC-to-Mac direction is ever added | n/a |
| Placeholder frame drawn with Direct2D and DirectWrite into a WIC bitmap, converted to NV12 | `placeholder_win.cpp` | `windows` `Win32::Graphics::Direct2D`, `DirectWrite` and `Imaging`; or `windows-canvas` 0.100 (first release 2026-09); or a pure-Rust rasteriser | Mature. Cheap to simplify. |
| Virtual camera client and install (`MFCreateVirtualCamera`, shared memory, registry) | `vcam_win.cpp`, `vcam_install_win.cpp` | Out of scope: the virtual camera ticket. `MFCreateVirtualCamera` is in `windows` `Win32::Media::MediaFoundation`. | — |

## Gaps and awkward parts

### 1. windows-rs is between generations (the main decision)

**What is published.** On crates.io, `windows` 0.62.2 and `windows-sys` 0.61.2 are both dated
2025-10-06. They are generated from the Win32Metadata project and use `Win32_*` features.

**What changed on master.**
- The August 2026 newsletter ([microsoft/windows-rs#4867](https://github.com/microsoft/windows-rs/issues/4867))
  moves every crate to a common 0.100.0 line, built from Windows SDK headers through
  `windows-clang`.
- Every Cargo feature changes. For example, `Win32_Media_Audio_Endpoints` becomes `endpointvolume`.
- Signatures change: COM methods keep raw `HRESULT`, ownership wrappers are removed, and enums
  generated by the old metadata disappear.
- The newsletter says "We may initially publish the release without `windows` and `windows-sys`".
- The smaller crates were published at 0.100.0 on 2026-09-03 (`windows-core`, `windows-registry`,
  `windows-bindgen`, `windows-link`, `windows-window`, …). The two umbrella crates were not, and
  none had been by 2026-11-13.

**What the maintainers recommend.** Reusable libraries should not depend on the umbrella crates
and should generate the APIs they need with `windows-bindgen`
([comment on #4850](https://github.com/microsoft/windows-rs/issues/4850), 2026-09-01).
`#[implement]`, which every COM callback depends on, is also being redesigned
([#5333](https://github.com/microsoft/windows-rs/issues/5333), closed 2026-11).

**Options for the build spec.**

- **A. Stay on the 0.62 line.** Use `windows` 0.62.2 with `windows-core` 0.62.
  - The ecosystem Beamer would use is on this line: `wasapi` 0.27, `cpal` 0.18 and `wmi` 0.18
    use `windows` 0.62, and `interprocess`, `windows-service` and `tray-icon` use `windows-sys`
    0.60 to 0.61.
  - The touchpad function is the only symbol to declare by hand.
  - Cost: a later migration to 0.100 renames every feature and import.
- **B. Generate bindings with `windows-bindgen` 0.100** from its bundled metadata (`windows-default`),
  with `windows-core` 0.100.
  - Pros: the newest metadata, and `CreateSyntheticPointerDevice2` is included.
  - Cons: Beamer's COM types would differ from the ones `wasapi` and `wmi` use. Both can sit in
    one dependency tree, but interface values cannot pass between them. It is also a young code
    path; the bindgen README on master already asks for 0.101.

**Recommendation: A.** Put every Windows FFI call in the platform crate(s) behind the platform
seam, so that moving to 0.100 later is that crate's job alone.

### 2. Missing symbols

Only `CreateSyntheticPointerDevice2` and `SYNTHETIC_DEVICE_CREATION_PARAMS` are missing from
`windows` 0.62.2. Both are in windows-rs master. Until then they are a hand-written `link!`
declaration and a `#[repr(C)]` struct.

### 3. No safe wrapper, so hand-written unsafe COM

The following have no maintained safe crate. They can all be written against `windows`, but each
is a block of unsafe code that needs its own tests:
- Media Foundation H.264 decode with D3D11 (the largest).
- Task Scheduler registration (`ITaskService`).
- Windows Firewall and network category (`INetFwPolicy2`, `INetworkListManager`), or keep the
  PowerShell scripts.
- The Explorer de-elevation sequence (`IShellWindows` … `IShellDispatch2`).
- The CCD display configuration API.

Older wrappers exist for several of these, but they sit on stale bindings and should not be used:
`planif` (task scheduler), `ddc-winapi` and `ddc-hi` (DDC), `rdev` and `willhook` (hooks),
`single-instance`.

### 4. Hook thread, message loops and callbacks

Low-level hooks, Raw Input, the desktop-switch WinEvent hook, the tray icon and any message-only
window are all delivered through a message loop on the thread that created them. The rules
carry over from `capture_win.py` unchanged:
- **One dedicated hook thread** owns the hooks, the Raw Input window and the WinEvent hook. It
  pumps `GetMessageW` and is stopped with `PostThreadMessageW`.
- **The callback never blocks.** It hands work to another thread and returns. If it runs past
  `LowLevelHooksTimeout` (at most 1 s), Windows removes the hook silently, and with it the swallow
  and the shortcut.
- **"Still redirecting?" is checked inside the callback**, so a wedged consumer never keeps the
  user's input.
- **Every `extern "system"` callback is wrapped in `catch_unwind`.** Since Rust 1.81 a panic
  across that boundary aborts the process.

### 5. COM apartments

- **Audio, Media Foundation, WMI and Task Scheduler** work in the MTA. Each worker thread
  initialises COM itself.
- **The Explorer de-elevation call** needs an STA thread.
- **The process-loopback activation handler** is called from an MTA worker and must be agile.
  `wasapi` implements this.
- The rule for the spec: **each COM object lives on the thread or apartment that created it.**
  The Python code already works this way.

### 6. Sessions, desktops, UIPI: platform limits, not Rust gaps

These limits come from Windows, so Rust changes nothing about them:
- **Nothing can inject into the Winlogon secure desktop.** Unlocking stays with the external
  credential provider, and Beamer only signals its event.
- **`SendInput` into a higher-integrity window fails silently.** Admin mode (the task registered
  with `HighestAvailable`) remains the answer, and UIAccess remains unused.
- **A process started over SSH runs in session 0 and has no desktop.** Starting through the
  scheduled task stays the way to launch the app.

### 7. Things Qt did that now need their own owner

Qt has been doing the following, and the Rust app has to do them through the crates above:
- **Tray icon**: `tray-icon`.
- **Single-instance IPC** (`QLocalServer`): a named pipe through tokio or `interprocess`.
- **Per-monitor DPI awareness**: the app manifest, through `embed-resource`.
- **HWND access** for click-through overlays and title-bar colours: `raw-window-handle` from
  whichever UI toolkit is chosen. Whether the toolkit allows this is for the UI ticket.

## Not verified

- **Nothing was compiled or run on `lebig`.** Crate paths were checked by reading the source of
  `windows` 0.62.2. Linking, callback behaviour and run-time results were not tested.
- **When the windows-rs 0.100 umbrella crates will be published.** This note does not predict it.
- **Whether `enigo`, `arboard` or `clipboard-win` could replace the direct calls with Beamer's
  exact semantics** (the `dwExtraInfo` mark, sequence-number stamps, DIB V5 handling). They were
  set aside without reading their source in detail.
- **Thread-affinity details of `IAudioSessionNotification` registration** beyond what the current
  C++ does.

## Sources

- windows-rs: [August 2026 newsletter #4867](https://github.com/microsoft/windows-rs/issues/4867),
  [#4850 comment](https://github.com/microsoft/windows-rs/issues/4850),
  [#5333](https://github.com/microsoft/windows-rs/issues/5333),
  [release 74](https://github.com/microsoft/windows-rs/releases/tag/74), the
  [windows-bindgen README](https://github.com/microsoft/windows-rs/tree/master/crates/libs/bindgen),
  and the master `winuser` sys bindings (searched for `CreateSyntheticPointerDevice2`).
- The crate source of `windows` 0.62.2 from crates.io, used for every module path in the tables.
- The crates.io API (2026-11-13) for versions, dates and dependencies of `windows`, `windows-sys`,
  `windows-core`, `windows-registry`, `windows-bindgen`, `windows-window`, `wasapi`, `cpal`,
  `clipboard-win`, `arboard`, `tray-icon`, `single-instance`, `ddc-winapi`, `ddc-hi`, `rdev`,
  `willhook`, `enigo`, `planif`, `auto-launch`, `wmi`, `interprocess`, `windows-service`, `tokio`,
  `socket2`, `getrandom`, `openh264`, `netdev`, `if-addrs`, `sys-locale`, `embed-resource`,
  `winresource` and `raw-window-handle`.
- The [wasapi-rs README](https://github.com/HEnquist/wasapi-rs),
  [`AudioClient` docs](https://docs.rs/wasapi/0.27.0/wasapi/struct.AudioClient.html) and
  `src/api.rs`, and the [tray-icon README](https://github.com/tauri-apps/tray-icon).
- Microsoft Learn: [LowLevelKeyboardProc](https://learn.microsoft.com/en-us/windows/win32/winmsg/lowlevelkeyboardproc),
  [SendInput](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput),
  [ToUnicodeEx](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-tounicodeex),
  [CreateSyntheticPointerDevice2](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createsyntheticpointerdevice2),
  [ActivateAudioInterfaceAsync](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync),
  and [UIAccess in secure locations](https://learn.microsoft.com/en-us/windows/security/threat-protection/security-policy-settings/user-account-control-only-elevate-uiaccess-applications-that-are-installed-in-secure-locations).
- [Rust 1.81.0 release notes](https://blog.rust-lang.org/2024/09/05/Rust-1.81.0/), on aborting
  when a panic unwinds out of an `extern "C"` function.
