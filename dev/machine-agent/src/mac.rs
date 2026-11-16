//! The macOS desktop: Quartz events posted at the HID event tap, where real hardware input
//! enters, so Beamer's capture path sees them as it sees a person's.
//!
//! Posting needs the Accessibility grant, which macOS gives to the process that macOS holds
//! responsible: the agent's own `.app` only when launchd starts it (`open`), never when a terminal
//! runs its binary. Without the grant, macOS drops posted events silently, so the agent checks
//! [`Desktop::trusted`] first.

use std::collections::BTreeSet;
use std::ffi::c_void;

use crate::{Button, Desktop, Display, Input, Key, Point};

#[repr(C)]
#[derive(Clone, Copy)]
struct CGPoint {
    x: f64,
    y: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CGSize {
    width: f64,
    height: f64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct CGRect {
    origin: CGPoint,
    size: CGSize,
}

type CGEventRef = *mut c_void;
type CGEventSourceRef = *mut c_void;

// CGEventType
const LEFT_DOWN: u32 = 1;
const LEFT_UP: u32 = 2;
const RIGHT_DOWN: u32 = 3;
const RIGHT_UP: u32 = 4;
const MOUSE_MOVED: u32 = 5;
const LEFT_DRAGGED: u32 = 6;
const RIGHT_DRAGGED: u32 = 7;
const FLAGS_CHANGED: u32 = 12;
const OTHER_DOWN: u32 = 25;
const OTHER_UP: u32 = 26;
const OTHER_DRAGGED: u32 = 27;
// CGEventField
const CLICK_STATE: u32 = 1;
const DELTA_X: u32 = 4;
const DELTA_Y: u32 = 5;
// CGEventTapLocation: kCGHIDEventTap
const HID_TAP: u32 = 0;
// CGEventSourceStateID
const COMBINED_SESSION: i32 = 0;
const HID_SYSTEM: i32 = 1;
// CGScrollEventUnit: kCGScrollEventUnitLine
const LINE: u32 = 1;
// clockid_t: CLOCK_UPTIME_RAW, mach_absolute_time in ns
const CLOCK_UPTIME_RAW: u32 = 8;

#[link(name = "CoreGraphics", kind = "framework")]
unsafe extern "C" {
    fn CGEventSourceCreate(state: i32) -> CGEventSourceRef;
    fn CGEventSourceKeyState(state: i32, key: u16) -> bool;
    fn CGEventCreate(source: CGEventSourceRef) -> CGEventRef;
    fn CGEventCreateMouseEvent(
        source: CGEventSourceRef,
        kind: u32,
        at: CGPoint,
        button: u32,
    ) -> CGEventRef;
    fn CGEventCreateKeyboardEvent(source: CGEventSourceRef, key: u16, down: bool) -> CGEventRef;
    fn CGEventCreateScrollWheelEvent2(
        source: CGEventSourceRef,
        units: u32,
        wheels: u32,
        wheel1: i32,
        wheel2: i32,
        wheel3: i32,
    ) -> CGEventRef;
    fn CGEventGetLocation(event: CGEventRef) -> CGPoint;
    fn CGEventSetType(event: CGEventRef, kind: u32);
    fn CGEventSetFlags(event: CGEventRef, flags: u64);
    fn CGEventSetIntegerValueField(event: CGEventRef, field: u32, value: i64);
    fn CGEventPost(tap: u32, event: CGEventRef);
    fn CGGetActiveDisplayList(max: u32, displays: *mut u32, count: *mut u32) -> i32;
    fn CGDisplayBounds(display: u32) -> CGRect;
    fn CGMainDisplayID() -> u32;
}

#[link(name = "ApplicationServices", kind = "framework")]
unsafe extern "C" {
    fn AXIsProcessTrusted() -> bool;
    fn AXIsProcessTrustedWithOptions(options: *const c_void) -> bool;
    static kAXTrustedCheckOptionPrompt: *const c_void;
}

#[link(name = "CoreFoundation", kind = "framework")]
unsafe extern "C" {
    fn CFRelease(object: *const c_void);
    fn CFDictionaryCreate(
        allocator: *const c_void,
        keys: *const *const c_void,
        values: *const *const c_void,
        count: isize,
        key_callbacks: *const c_void,
        value_callbacks: *const c_void,
    ) -> *const c_void;
    static kCFBooleanTrue: *const c_void;
    static kCFTypeDictionaryKeyCallBacks: u8;
    static kCFTypeDictionaryValueCallBacks: u8;
}

/// Asks macOS to list this app under Accessibility (switched off) and to show the prompt that
/// leads a person to the switch. Returns whether the app is trusted already.
pub fn ask_for_access() -> bool {
    // SAFETY: a one-entry CFDictionary of CoreFoundation constants, released once.
    unsafe {
        let keys = [kAXTrustedCheckOptionPrompt];
        let values = [kCFBooleanTrue];
        let options = CFDictionaryCreate(
            std::ptr::null(),
            keys.as_ptr(),
            values.as_ptr(),
            1,
            (&raw const kCFTypeDictionaryKeyCallBacks).cast(),
            (&raw const kCFTypeDictionaryValueCallBacks).cast(),
        );
        let trusted = AXIsProcessTrustedWithOptions(options);
        if !options.is_null() {
            CFRelease(options);
        }
        trusted
    }
}

unsafe extern "C" {
    fn clock_gettime_nsec_np(clock: u32) -> u64;
}

/// The Mac's desktop. It remembers which buttons and modifiers it holds down, so a move while a
/// button is held is a drag, and every event carries the held modifiers.
pub struct MacDesktop {
    source: CGEventSourceRef,
    buttons: BTreeSet<u8>,
    modifiers: BTreeSet<&'static str>,
}

// The event source is a CoreFoundation object, used by one thread at a time behind the agent's
// mutex.
unsafe impl Send for MacDesktop {}

impl MacDesktop {
    pub fn new() -> MacDesktop {
        // SAFETY: a plain CoreGraphics constructor; a null source is allowed everywhere below.
        let source = unsafe { CGEventSourceCreate(HID_SYSTEM) };
        MacDesktop {
            source,
            buttons: BTreeSet::new(),
            modifiers: BTreeSet::new(),
        }
    }

    fn flags(&self) -> u64 {
        self.modifiers
            .iter()
            .filter_map(|code| modifier_flags(code))
            .fold(0, |a, b| a | b)
    }

    /// Sets the held modifiers on `event`, posts it at the HID tap and releases it.
    fn send(&self, event: CGEventRef) -> Result<(), String> {
        if event.is_null() {
            return Err("CoreGraphics could not make the event".into());
        }
        // SAFETY: `event` is a live CGEvent this function owns and releases once.
        unsafe {
            CGEventSetFlags(event, self.flags());
            CGEventPost(HID_TAP, event);
            CFRelease(event);
        }
        Ok(())
    }
}

impl Default for MacDesktop {
    fn default() -> Self {
        MacDesktop::new()
    }
}

impl Drop for MacDesktop {
    fn drop(&mut self) {
        if !self.source.is_null() {
            // SAFETY: the source was created in `new` and is released once.
            unsafe { CFRelease(self.source) };
        }
    }
}

/// The CGEventFlags a held modifier key sets: the device-independent bit and its side's bit.
fn modifier_flags(code: &str) -> Option<u64> {
    Some(match code {
        "ShiftLeft" => 0x0002_0000 | 0x0002,
        "ShiftRight" => 0x0002_0000 | 0x0004,
        "ControlLeft" => 0x0004_0000 | 0x0001,
        "ControlRight" => 0x0004_0000 | 0x2000,
        "AltLeft" => 0x0008_0000 | 0x0020,
        "AltRight" => 0x0008_0000 | 0x0040,
        "MetaLeft" => 0x0010_0000 | 0x0008,
        "MetaRight" => 0x0010_0000 | 0x0010,
        _ => return None,
    })
}

fn button_number(button: Button) -> u8 {
    match button {
        Button::Left => 0,
        Button::Right => 1,
        Button::Middle => 2,
    }
}

impl Desktop for MacDesktop {
    fn name(&self) -> &'static str {
        "mac"
    }

    fn trusted(&self) -> bool {
        // SAFETY: no arguments, no state.
        unsafe { AXIsProcessTrusted() }
    }

    fn displays(&self) -> Vec<Display> {
        let mut ids = [0u32; 16];
        let mut count = 0u32;
        // SAFETY: `ids` holds 16 entries and CoreGraphics writes at most `max` of them.
        let main = unsafe {
            if CGGetActiveDisplayList(16, ids.as_mut_ptr(), &mut count) != 0 {
                return Vec::new();
            }
            CGMainDisplayID()
        };
        ids[..count as usize]
            .iter()
            .map(|&id| {
                // SAFETY: an id CoreGraphics just listed.
                let r = unsafe { CGDisplayBounds(id) };
                Display {
                    x: r.origin.x,
                    y: r.origin.y,
                    width: r.size.width,
                    height: r.size.height,
                    main: id == main,
                }
            })
            .collect()
    }

    fn pointer(&self) -> Point {
        // SAFETY: an empty event reads the current pointer location; it is released once.
        unsafe {
            let event = CGEventCreate(std::ptr::null_mut());
            if event.is_null() {
                return Point { x: 0.0, y: 0.0 };
            }
            let at = CGEventGetLocation(event);
            CFRelease(event);
            Point { x: at.x, y: at.y }
        }
    }

    fn mono_ns(&self) -> u64 {
        // SAFETY: a clock read.
        unsafe { clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }
    }

    fn post(&mut self, input: Input) -> Result<(), String> {
        // SAFETY: each constructor gets this desktop's source (or null) and plain values; `send`
        // checks the result for null before touching it.
        match input {
            Input::Move { to, delta } => {
                let (kind, button) = if self.buttons.contains(&0) {
                    (LEFT_DRAGGED, 0)
                } else if self.buttons.contains(&1) {
                    (RIGHT_DRAGGED, 1)
                } else if let Some(&other) = self.buttons.iter().next() {
                    (OTHER_DRAGGED, other)
                } else {
                    (MOUSE_MOVED, 0)
                };
                let at = CGPoint { x: to.x, y: to.y };
                let event =
                    unsafe { CGEventCreateMouseEvent(self.source, kind, at, u32::from(button)) };
                if !event.is_null() {
                    unsafe {
                        CGEventSetIntegerValueField(event, DELTA_X, delta.0.round() as i64);
                        CGEventSetIntegerValueField(event, DELTA_Y, delta.1.round() as i64);
                    }
                }
                self.send(event)
            }
            Input::Button { button, down } => {
                let number = button_number(button);
                let kind = match (button, down) {
                    (Button::Left, true) => LEFT_DOWN,
                    (Button::Left, false) => LEFT_UP,
                    (Button::Right, true) => RIGHT_DOWN,
                    (Button::Right, false) => RIGHT_UP,
                    (Button::Middle, true) => OTHER_DOWN,
                    (Button::Middle, false) => OTHER_UP,
                };
                let at = self.pointer();
                let at = CGPoint { x: at.x, y: at.y };
                let event =
                    unsafe { CGEventCreateMouseEvent(self.source, kind, at, u32::from(number)) };
                if !event.is_null() {
                    unsafe { CGEventSetIntegerValueField(event, CLICK_STATE, 1) };
                }
                if down {
                    self.buttons.insert(number);
                } else {
                    self.buttons.remove(&number);
                }
                self.send(event)
            }
            Input::Key { key, down } => self.post_key(key, down),
            Input::Scroll { dx, dy } => {
                let event =
                    unsafe { CGEventCreateScrollWheelEvent2(self.source, LINE, 2, dy, dx, 0) };
                self.send(event)
            }
        }
    }

    fn key_down(&self, key: &str) -> bool {
        // SAFETY: a state read with a code from the key table.
        Key::named(key).is_some_and(|k| unsafe { CGEventSourceKeyState(COMBINED_SESSION, k.mac) })
    }
}

impl MacDesktop {
    fn post_key(&mut self, key: Key, down: bool) -> Result<(), String> {
        let modifier = modifier_flags(key.code).is_some();
        if modifier {
            // A modifier's own event already carries its new state, as a real one does.
            if down {
                self.modifiers.insert(key.code);
            } else {
                self.modifiers.remove(key.code);
            }
        }
        // SAFETY: as in `post`.
        let event = unsafe { CGEventCreateKeyboardEvent(self.source, key.mac, down) };
        if modifier && !event.is_null() {
            unsafe { CGEventSetType(event, FLAGS_CHANGED) };
        }
        self.send(event)
    }
}
