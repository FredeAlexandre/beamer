//! The Windows desktop: SendInput events, which Windows hands to low-level hooks and Raw Input as
//! it hands them a person's, so Beamer's capture path sees them.
//!
//! Windows lets a process post input only to the desktop it is in, and only while that desktop
//! takes input. SSH lands in session 0, which has none, so the agent runs in the signed-in
//! session, started by its scheduled task (README.md); [`Desktop::trusted`] says whether it is
//! there and the desktop is not locked.

use std::ffi::c_void;

use super::{Motion, VirtualScreen, motion};
use crate::{Button, Desktop, Display, Input, Key, Point};

type Handle = *mut c_void;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct WinPoint {
    x: i32,
    y: i32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Rect {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
}

#[repr(C)]
#[derive(Default)]
struct MonitorInfo {
    size: u32,
    monitor: Rect,
    work: Rect,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MouseInput {
    dx: i32,
    dy: i32,
    data: u32,
    flags: u32,
    time: u32,
    extra: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct KeyboardInput {
    vk: u16,
    scan: u16,
    flags: u32,
    time: u32,
    extra: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
union InputData {
    mouse: MouseInput,
    keyboard: KeyboardInput,
}

/// `INPUT`: its union is as large as its largest member, `MOUSEINPUT`.
#[repr(C)]
#[derive(Clone, Copy)]
struct WinInput {
    kind: u32,
    data: InputData,
}

const _: () = assert!(
    size_of::<WinInput>()
        == if cfg!(target_pointer_width = "64") {
            40
        } else {
            28
        }
);

const INPUT_MOUSE: u32 = 0;
const INPUT_KEYBOARD: u32 = 1;
const MOUSEEVENTF_MOVE: u32 = 0x0001;
const MOUSEEVENTF_LEFTDOWN: u32 = 0x0002;
const MOUSEEVENTF_LEFTUP: u32 = 0x0004;
const MOUSEEVENTF_RIGHTDOWN: u32 = 0x0008;
const MOUSEEVENTF_RIGHTUP: u32 = 0x0010;
const MOUSEEVENTF_MIDDLEDOWN: u32 = 0x0020;
const MOUSEEVENTF_MIDDLEUP: u32 = 0x0040;
const MOUSEEVENTF_WHEEL: u32 = 0x0800;
const MOUSEEVENTF_HWHEEL: u32 = 0x1000;
const MOUSEEVENTF_MOVE_NOCOALESCE: u32 = 0x2000;
const MOUSEEVENTF_VIRTUALDESK: u32 = 0x4000;
const MOUSEEVENTF_ABSOLUTE: u32 = 0x8000;
const KEYEVENTF_EXTENDEDKEY: u32 = 0x0001;
const KEYEVENTF_KEYUP: u32 = 0x0002;
const KEYEVENTF_SCANCODE: u32 = 0x0008;
const WHEEL_DELTA: i32 = 120;
const SM_XVIRTUALSCREEN: i32 = 76;
const SM_YVIRTUALSCREEN: i32 = 77;
const SM_CXVIRTUALSCREEN: i32 = 78;
const SM_CYVIRTUALSCREEN: i32 = 79;
const MONITORINFOF_PRIMARY: u32 = 1;
const MAPVK_VSC_TO_VK_EX: u32 = 3;
const DESKTOP_SWITCHDESKTOP: u32 = 0x0100;
const DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2: isize = -4;

type MonitorEnum = unsafe extern "system" fn(Handle, Handle, *mut Rect, isize) -> i32;

#[link(name = "user32")]
unsafe extern "system" {
    fn SendInput(count: u32, inputs: *const WinInput, size: i32) -> u32;
    fn GetCursorPos(point: *mut WinPoint) -> i32;
    fn GetSystemMetrics(index: i32) -> i32;
    fn EnumDisplayMonitors(dc: Handle, clip: *const Rect, f: MonitorEnum, data: isize) -> i32;
    fn GetMonitorInfoW(monitor: Handle, info: *mut MonitorInfo) -> i32;
    fn GetAsyncKeyState(vk: i32) -> i16;
    fn MapVirtualKeyW(code: u32, map: u32) -> u32;
    fn SetProcessDpiAwarenessContext(context: isize) -> i32;
    fn OpenInputDesktop(flags: u32, inherit: i32, access: u32) -> Handle;
    fn CloseDesktop(desktop: Handle) -> i32;
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn QueryPerformanceCounter(count: *mut i64) -> i32;
    fn QueryPerformanceFrequency(frequency: *mut i64) -> i32;
    fn ProcessIdToSessionId(process: u32, session: *mut u32) -> i32;
    fn GetCurrentProcessId() -> u32;
}

/// The PC's desktop. Windows itself tracks held buttons and keys, so a move while a button is
/// held is a drag and every key carries the held modifiers, with nothing remembered here.
pub struct PcDesktop {
    /// QueryPerformanceCounter ticks per second.
    frequency: u64,
}

impl PcDesktop {
    /// Makes the process per-monitor DPI aware first, so every coordinate it reads and posts is
    /// in physical pixels (`PROTOCOL.md`, Coordinates).
    pub fn new() -> PcDesktop {
        let mut frequency = 0i64;
        // SAFETY: plain calls; the frequency is written to a live local.
        unsafe {
            SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            QueryPerformanceFrequency(&mut frequency);
        }
        PcDesktop {
            frequency: u64::try_from(frequency).unwrap_or(1).max(1),
        }
    }

    fn screen() -> VirtualScreen {
        // SAFETY: metric reads.
        unsafe {
            VirtualScreen {
                x: GetSystemMetrics(SM_XVIRTUALSCREEN),
                y: GetSystemMetrics(SM_YVIRTUALSCREEN),
                width: GetSystemMetrics(SM_CXVIRTUALSCREEN),
                height: GetSystemMetrics(SM_CYVIRTUALSCREEN),
            }
        }
    }

    fn send(inputs: &[WinInput]) -> Result<(), String> {
        let count = u32::try_from(inputs.len()).unwrap_or(u32::MAX);
        // SAFETY: `inputs` is a live slice of `count` INPUTs of the size passed.
        let sent = unsafe { SendInput(count, inputs.as_ptr(), size_of::<WinInput>() as i32) };
        if sent == count {
            Ok(())
        } else {
            Err(format!(
                "SendInput posted {sent} of {count} events: {}",
                std::io::Error::last_os_error()
            ))
        }
    }
}

impl Default for PcDesktop {
    fn default() -> Self {
        PcDesktop::new()
    }
}

fn mouse(dx: i32, dy: i32, data: i32, flags: u32) -> WinInput {
    WinInput {
        kind: INPUT_MOUSE,
        data: InputData {
            mouse: MouseInput {
                dx,
                dy,
                data: data as u32,
                flags,
                time: 0,
                extra: 0,
            },
        },
    }
}

/// The virtual key Windows reads `key`'s scan code as, under this thread's keyboard layout.
fn virtual_key(key: Key) -> u32 {
    // SAFETY: a table lookup; the 0xE0 high byte marks an extended scan code.
    unsafe { MapVirtualKeyW(u32::from(key.pc), MAPVK_VSC_TO_VK_EX) }
}

unsafe extern "system" fn add_display(
    monitor: Handle,
    _: Handle,
    _: *mut Rect,
    data: isize,
) -> i32 {
    // SAFETY: `data` is the `Vec` `displays` passed, alive for the whole enumeration.
    let displays = unsafe { &mut *(data as *mut Vec<Display>) };
    let mut info = MonitorInfo {
        size: size_of::<MonitorInfo>() as u32,
        ..MonitorInfo::default()
    };
    // SAFETY: a monitor Windows is enumerating, and an info struct of the size it states.
    if unsafe { GetMonitorInfoW(monitor, &mut info) } != 0 {
        let r = info.monitor;
        displays.push(Display {
            x: f64::from(r.left),
            y: f64::from(r.top),
            width: f64::from(r.right - r.left),
            height: f64::from(r.bottom - r.top),
            main: info.flags & MONITORINFOF_PRIMARY != 0,
        });
    }
    1
}

impl Desktop for PcDesktop {
    fn name(&self) -> &'static str {
        "pc"
    }

    /// In a session other than 0, and on a desktop that takes input: not locked, no UAC prompt.
    fn trusted(&self) -> bool {
        let mut session = 0u32;
        // SAFETY: plain calls; the desktop handle is closed once.
        unsafe {
            if ProcessIdToSessionId(GetCurrentProcessId(), &mut session) == 0 || session == 0 {
                return false;
            }
            let desktop = OpenInputDesktop(0, 0, DESKTOP_SWITCHDESKTOP);
            if desktop.is_null() {
                return false;
            }
            CloseDesktop(desktop);
        }
        true
    }

    fn displays(&self) -> Vec<Display> {
        let mut displays: Vec<Display> = Vec::new();
        // SAFETY: the callback only touches `displays`, which outlives the call.
        unsafe {
            EnumDisplayMonitors(
                std::ptr::null_mut(),
                std::ptr::null(),
                add_display,
                (&raw mut displays) as isize,
            );
        }
        displays
    }

    fn pointer(&self) -> Point {
        let mut at = WinPoint::default();
        // SAFETY: writes one POINT to a live local.
        unsafe { GetCursorPos(&mut at) };
        Point {
            x: f64::from(at.x),
            y: f64::from(at.y),
        }
    }

    fn mono_ns(&self) -> u64 {
        let mut count = 0i64;
        // SAFETY: writes the counter to a live local.
        unsafe { QueryPerformanceCounter(&mut count) };
        let ns = u128::from(count.max(0) as u64) * 1_000_000_000 / u128::from(self.frequency);
        u64::try_from(ns).unwrap_or(u64::MAX)
    }

    fn post(&mut self, input: Input) -> Result<(), String> {
        match input {
            Input::Move { to, delta } => {
                let event = match motion(self.pointer(), to, delta, PcDesktop::screen()) {
                    Motion::Absolute { x, y } => mouse(
                        x,
                        y,
                        0,
                        MOUSEEVENTF_MOVE
                            | MOUSEEVENTF_ABSOLUTE
                            | MOUSEEVENTF_VIRTUALDESK
                            | MOUSEEVENTF_MOVE_NOCOALESCE,
                    ),
                    Motion::Relative { dx, dy } => {
                        mouse(dx, dy, 0, MOUSEEVENTF_MOVE | MOUSEEVENTF_MOVE_NOCOALESCE)
                    }
                };
                PcDesktop::send(&[event])
            }
            Input::Button { button, down } => {
                let flags = match (button, down) {
                    (Button::Left, true) => MOUSEEVENTF_LEFTDOWN,
                    (Button::Left, false) => MOUSEEVENTF_LEFTUP,
                    (Button::Right, true) => MOUSEEVENTF_RIGHTDOWN,
                    (Button::Right, false) => MOUSEEVENTF_RIGHTUP,
                    (Button::Middle, true) => MOUSEEVENTF_MIDDLEDOWN,
                    (Button::Middle, false) => MOUSEEVENTF_MIDDLEUP,
                };
                PcDesktop::send(&[mouse(0, 0, 0, flags)])
            }
            Input::Key { key, down } => {
                let mut flags = KEYEVENTF_SCANCODE;
                if key.pc > 0xFF {
                    flags |= KEYEVENTF_EXTENDEDKEY;
                }
                if !down {
                    flags |= KEYEVENTF_KEYUP;
                }
                let event = WinInput {
                    kind: INPUT_KEYBOARD,
                    data: InputData {
                        keyboard: KeyboardInput {
                            vk: 0,
                            scan: key.pc & 0xFF,
                            flags,
                            time: 0,
                            extra: 0,
                        },
                    },
                };
                PcDesktop::send(&[event])
            }
            Input::Scroll { dx, dy } => {
                // Windows' wheel already counts positive away from the user, and tilt right.
                let mut events = Vec::new();
                if dy != 0 {
                    events.push(mouse(0, 0, dy * WHEEL_DELTA, MOUSEEVENTF_WHEEL));
                }
                if dx != 0 {
                    events.push(mouse(0, 0, dx * WHEEL_DELTA, MOUSEEVENTF_HWHEEL));
                }
                PcDesktop::send(&events)
            }
        }
    }

    fn key_down(&self, key: &str) -> bool {
        let Some(vk) = Key::named(key).map(virtual_key).filter(|&vk| vk != 0) else {
            return false;
        };
        // SAFETY: a state read with a virtual key Windows just mapped.
        unsafe { GetAsyncKeyState(vk as i32) as u16 & 0x8000 != 0 }
    }
}
