// PROTOTYPE: Windows escape hatches: overlay extended styles, monitor friendly names, process start.
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use slint::winit_030::winit;
use windows::Win32::Devices::Display::{
    DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,
    DISPLAYCONFIG_MODE_INFO, DISPLAYCONFIG_PATH_INFO, DISPLAYCONFIG_SOURCE_DEVICE_NAME,
    DISPLAYCONFIG_TARGET_DEVICE_NAME, DisplayConfigGetDeviceInfo, GetDisplayConfigBufferSizes,
    QDC_ONLY_ACTIVE_PATHS, QueryDisplayConfig,
};
use windows::Win32::Foundation::{FILETIME, HWND, LPARAM, LRESULT, RECT, WPARAM};
use windows::Win32::UI::Shell::{DefSubclassProc, SetWindowSubclass};
use windows::Win32::System::Threading::{GetCurrentProcess, GetProcessTimes};
use windows::Win32::UI::WindowsAndMessaging::{
    GWL_EXSTYLE, GetWindowLongPtrW, GetWindowRect, HWND_TOPMOST, SWP_FRAMECHANGED, SWP_NOACTIVATE,
    SWP_NOMOVE, SWP_NOSIZE, STYLESTRUCT, SetWindowLongPtrW, WM_STYLECHANGING, SetWindowPos, WS_EX_LAYERED, WS_EX_NOACTIVATE,
    WS_EX_TOOLWINDOW, WS_EX_TOPMOST, WS_EX_TRANSPARENT,
};

use crate::Display;

pub const RENDERER: &str = "Skia via wgpu 30 (Direct3D 12)";

fn hwnd(w: &winit::window::Window) -> Option<HWND> {
    let RawWindowHandle::Win32(h) = w.window_handle().ok()?.as_raw() else { return None };
    Some(HWND(h.hwnd.get() as *mut _))
}

pub fn overlay_attributes(attrs: winit::window::WindowAttributes) -> winit::window::WindowAttributes {
    use winit::platform::windows::WindowAttributesExtWindows;
    attrs.with_skip_taskbar(true)
}

fn friendly_names() -> Vec<(String, String)> {
    let mut out = Vec::new();
    unsafe {
        let (mut np, mut nm) = (0u32, 0u32);
        if GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut np, &mut nm).is_err() {
            return out;
        }
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); np as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); nm as usize];
        if QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &mut np, paths.as_mut_ptr(), &mut nm, modes.as_mut_ptr(), None)
            .is_err()
        {
            return out;
        }
        for p in &paths[..np as usize] {
            let mut src = DISPLAYCONFIG_SOURCE_DEVICE_NAME::default();
            src.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = std::mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            let mut tgt = DISPLAYCONFIG_TARGET_DEVICE_NAME::default();
            tgt.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = std::mem::size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32;
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if DisplayConfigGetDeviceInfo(&mut src.header) != 0 || DisplayConfigGetDeviceInfo(&mut tgt.header) != 0 {
                continue;
            }
            let s = |w: &[u16]| String::from_utf16_lossy(&w[..w.iter().position(|&c| c == 0).unwrap_or(w.len())]);
            out.push((s(&src.viewGdiDeviceName), s(&tgt.monitorFriendlyDeviceName)));
        }
    }
    out
}

pub fn displays(w: &winit::window::Window) -> Vec<Display> {
    let names = friendly_names();
    let primary = w.primary_monitor();
    w.available_monitors()
        .map(|m| {
            let gdi = m.name().unwrap_or_default();
            let name = names
                .iter()
                .find(|(g, f)| *g == gdi && !f.is_empty())
                .map(|(_, f)| f.clone())
                .unwrap_or_else(|| gdi.clone());
            let (p, s) = (m.position(), m.size());
            Display {
                name,
                id: gdi,
                x: p.x as f64,
                y: p.y as f64,
                w: s.width as f64,
                h: s.height as f64,
                scale: m.scale_factor(),
                refresh_hz: m.refresh_rate_millihertz().unwrap_or(0) as f64 / 1000.0,
                is_main: primary.as_ref() == Some(&m),
            }
        })
        .collect()
}

const OVERLAY_EX: u32 = WS_EX_LAYERED.0 | WS_EX_TRANSPARENT.0 | WS_EX_TOOLWINDOW.0 | WS_EX_NOACTIVATE.0 | WS_EX_TOPMOST.0;

/// winit recomputes WS_EX_* from its own flags whenever it touches the window (seen: TOOLWINDOW and
/// NOACTIVATE gone a few seconds in), so a subclass pins our bits on every WM_STYLECHANGING.
unsafe extern "system" fn pin_ex_style(h: HWND, msg: u32, wp: WPARAM, lp: LPARAM, _id: usize, _data: usize) -> LRESULT {
    if msg == WM_STYLECHANGING && wp.0 as i32 == GWL_EXSTYLE.0 {
        let ss = unsafe { &mut *(lp.0 as *mut STYLESTRUCT) };
        ss.styleNew |= OVERLAY_EX;
    }
    unsafe { DefSubclassProc(h, msg, wp, lp) }
}

/// winit's set_cursor_hittest(false) already added LAYERED|TRANSPARENT; add the rest.
pub fn shape_overlay(w: &winit::window::Window) -> String {
    let Some(h) = hwnd(w) else { return "no HWND".into() };
    unsafe {
        let _ = SetWindowSubclass(h, Some(pin_ex_style), 0xBEA3, 0);
        let ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | OVERLAY_EX as isize);
        let _ = SetWindowPos(h, Some(HWND_TOPMOST), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    report(w)
}

pub fn report(w: &winit::window::Window) -> String {
    let Some(h) = hwnd(w) else { return "no HWND".into() };
    let ex = unsafe { GetWindowLongPtrW(h, GWL_EXSTYLE) } as u32;
    let has = |f: windows::Win32::UI::WindowsAndMessaging::WINDOW_EX_STYLE| if ex & f.0 != 0 { "1" } else { "0" };
    format!(
        "{} (WS_EX {ex:#010x}: LAYERED={} TRANSPARENT={} TOOLWINDOW={} NOACTIVATE={} TOPMOST={})",
        if ex & WS_EX_TRANSPARENT.0 != 0 && ex & WS_EX_LAYERED.0 != 0 { "on" } else { "OFF" },
        has(WS_EX_LAYERED),
        has(WS_EX_TRANSPARENT),
        has(WS_EX_TOOLWINDOW),
        has(WS_EX_NOACTIVATE),
        has(WS_EX_TOPMOST),
    )
}

pub fn place_overlay(w: &winit::window::Window, d: &Display) -> String {
    let Some(h) = hwnd(w) else { return "no HWND".into() };
    let mut r = RECT::default();
    unsafe {
        // Twice: the first move may cross a DPI boundary, and winit then applies the suggested rect.
        for _ in 0..2 {
            let _ = SetWindowPos(h, Some(HWND_TOPMOST), d.x as i32, d.y as i32, d.w as i32, d.h as i32, SWP_NOACTIVATE);
        }
        let _ = GetWindowRect(h, &mut r);
    }
    format!("HWND rect ({}, {}) {}x{} px", r.left, r.top, r.right - r.left, r.bottom - r.top)
}

pub fn activate_app() {}

pub fn process_start_time() -> Option<std::time::SystemTime> {
    let (mut c, mut e, mut k, mut u) = (FILETIME::default(), FILETIME::default(), FILETIME::default(), FILETIME::default());
    unsafe { GetProcessTimes(GetCurrentProcess(), &mut c, &mut e, &mut k, &mut u) }.ok()?;
    let t = ((c.dwHighDateTime as u64) << 32) | c.dwLowDateTime as u64;
    Some(std::time::UNIX_EPOCH + std::time::Duration::from_nanos((t - 116_444_736_000_000_000) * 100))
}
