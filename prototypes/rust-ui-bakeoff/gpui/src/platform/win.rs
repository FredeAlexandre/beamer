// PROTOTYPE: Windows escape hatches (windows crate + raw-window-handle). Everything GPUI does not do itself.
use gpui::{DisplayId, Window};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use windows::Win32::{
    Devices::Display::*,
    Foundation::*,
    Graphics::Gdi::*,
    System::{ProcessStatus::*, Threading::*},
    UI::{HiDpi::*, WindowsAndMessaging::*},
};

use super::{DisplayMeta, OverlayFlags};

fn monitor_device(h: HMONITOR) -> Option<(String, RECT)> {
    let mut info = MONITORINFOEXW::default();
    info.monitorInfo.cbSize = std::mem::size_of::<MONITORINFOEXW>() as u32;
    unsafe { GetMonitorInfoW(h, &mut info as *mut _ as *mut MONITORINFO).ok().ok()? };
    let n = info.szDevice.iter().position(|&c| c == 0).unwrap_or(32);
    Some((String::from_utf16_lossy(&info.szDevice[..n]), info.monitorInfo.rcMonitor))
}

/// The monitor's friendly name (e.g. "GN10") through the DisplayConfig API, matched by GDI device name.
fn friendly_name(gdi_device: &str) -> Option<String> {
    unsafe {
        let (mut np, mut nm) = (0u32, 0u32);
        GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut np, &mut nm).ok().ok()?;
        let mut paths = vec![DISPLAYCONFIG_PATH_INFO::default(); np as usize];
        let mut modes = vec![DISPLAYCONFIG_MODE_INFO::default(); nm as usize];
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &mut np, paths.as_mut_ptr(), &mut nm, modes.as_mut_ptr(), None).ok().ok()?;
        for p in &paths[..np as usize] {
            let mut src = DISPLAYCONFIG_SOURCE_DEVICE_NAME::default();
            src.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = std::mem::size_of::<DISPLAYCONFIG_SOURCE_DEVICE_NAME>() as u32;
            src.header.adapterId = p.sourceInfo.adapterId;
            src.header.id = p.sourceInfo.id;
            if DisplayConfigGetDeviceInfo(&mut src.header) != 0 {
                continue;
            }
            let n = src.viewGdiDeviceName.iter().position(|&c| c == 0).unwrap_or(32);
            if String::from_utf16_lossy(&src.viewGdiDeviceName[..n]) != gdi_device {
                continue;
            }
            let mut tgt = DISPLAYCONFIG_TARGET_DEVICE_NAME::default();
            tgt.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = std::mem::size_of::<DISPLAYCONFIG_TARGET_DEVICE_NAME>() as u32;
            tgt.header.adapterId = p.targetInfo.adapterId;
            tgt.header.id = p.targetInfo.id;
            if DisplayConfigGetDeviceInfo(&mut tgt.header) != 0 {
                continue;
            }
            let n = tgt.monitorFriendlyDeviceName.iter().position(|&c| c == 0).unwrap_or(64);
            let name = String::from_utf16_lossy(&tgt.monitorFriendlyDeviceName[..n]);
            if !name.is_empty() {
                return Some(name);
            }
        }
        None
    }
}

pub fn display_meta(id: DisplayId) -> DisplayMeta {
    // GPUI's DisplayId on Windows is the HMONITOR.
    let h = HMONITOR(u64::from(id) as _);
    let Some((dev, r)) = monitor_device(h) else {
        return DisplayMeta { name: format!("Display {}", u64::from(id)), frame: "?".into(), scale: 1.0 };
    };
    let (mut dx, mut dy) = (96u32, 96u32);
    unsafe { GetDpiForMonitor(h, MDT_EFFECTIVE_DPI, &mut dx, &mut dy).ok() };
    DisplayMeta {
        name: friendly_name(&dev).unwrap_or(dev.clone()),
        frame: format!("{}x{} at ({},{}) physical px, {dev}", r.right - r.left, r.bottom - r.top, r.left, r.top),
        scale: dx as f64 / 96.0,
    }
}

fn hwnd(window: &Window) -> HWND {
    let RawWindowHandle::Win32(h) = HasWindowHandle::window_handle(window).unwrap().as_raw() else { unreachable!() };
    HWND(h.hwnd.get() as _)
}

fn ex_names(ex: u32) -> String {
    let names = [
        (WS_EX_LAYERED.0, "LAYERED"),
        (WS_EX_TRANSPARENT.0, "TRANSPARENT"),
        (WS_EX_TOOLWINDOW.0, "TOOLWINDOW"),
        (WS_EX_NOACTIVATE.0, "NOACTIVATE"),
        (WS_EX_TOPMOST.0, "TOPMOST"),
        (WS_EX_NOREDIRECTIONBITMAP.0, "NOREDIRECTIONBITMAP"),
        (WS_EX_APPWINDOW.0, "APPWINDOW"),
    ];
    names.iter().filter(|(b, _)| ex & b != 0).map(|(_, n)| *n).collect::<Vec<_>>().join("|")
}

/// GPUI's `WindowKind::PopUp` gives WS_EX_TOOLWINDOW | WS_EX_TOPMOST (+ WS_EX_NOREDIRECTIONBITMAP for its
/// DirectComposition swapchain). Added here: LAYERED | TRANSPARENT (click-through) and NOACTIVATE.
pub fn configure_overlay(window: &Window, _id: DisplayId) -> OverlayFlags {
    let h = hwnd(window);
    unsafe {
        let before = GetWindowLongPtrW(h, GWL_EXSTYLE) as u32;
        let mut want = before | WS_EX_NOACTIVATE.0 | WS_EX_TOOLWINDOW.0 | WS_EX_TOPMOST.0;
        // BAKEOFF_NO_CLICK_THROUGH=1: control run, to show the probes do see the overlay.
        if std::env::var_os("BAKEOFF_NO_CLICK_THROUGH").is_none() {
            want |= WS_EX_LAYERED.0 | WS_EX_TRANSPARENT.0;
        }
        SetWindowLongPtrW(h, GWL_EXSTYLE, want as isize);
        // A layered window draws nothing until it has layered attributes; alpha 255 keeps
        // per-pixel alpha from the DirectComposition visual.
        let lwa = if want & WS_EX_LAYERED.0 != 0 { SetLayeredWindowAttributes(h, COLORREF(0), 255, LWA_ALPHA).is_ok() } else { false };
        SetWindowPos(h, Some(HWND_TOPMOST), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED).ok();
        let after = GetWindowLongPtrW(h, GWL_EXSTYLE) as u32;
        let mut r = RECT::default();
        GetWindowRect(h, &mut r).ok();
        let dpi = GetDpiForWindow(h);
        let ctx = GetWindowDpiAwarenessContext(h);
        let pmv2 = AreDpiAwarenessContextsEqual(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2).as_bool();
        let frame = format!("{}x{} at ({},{}) physical px", r.right - r.left, r.bottom - r.top, r.left, r.top);
        OverlayFlags {
            click_through: after & WS_EX_LAYERED.0 != 0 && after & WS_EX_TRANSPARENT.0 != 0,
            frame: frame.clone(),
            describe: format!(
                "exstyle before {} -> after {} (SetLayeredWindowAttributes ok={}), frame {frame}, dpi {dpi}, per-monitor-v2 {pmv2}",
                ex_names(before),
                ex_names(after),
                lwa
            ),
        }
    }
}

fn client_on_screen(h: HWND) -> RECT {
    let mut r = RECT::default();
    unsafe {
        GetClientRect(h, &mut r).ok();
        let mut pts = [POINT { x: r.left, y: r.top }, POINT { x: r.right, y: r.bottom }];
        MapWindowPoints(Some(h), None, &mut pts);
        RECT { left: pts[0].x, top: pts[0].y, right: pts[1].x, bottom: pts[1].y }
    }
}

fn fmt_rect(r: RECT) -> String {
    format!("{}x{} at ({},{})", r.right - r.left, r.bottom - r.top, r.left, r.top)
}

/// GPUI's PopUp is an overlapped window (style 0) whose frame it hides in WM_NCCALCSIZE, placed with
/// border offsets: on the 1920x1080 monitor at (2560,357) the window came out 1936x1088 at
/// (2552,353), its client 4 px too high. Make it WS_POPUP and put it exactly on the monitor.
/// Runs outside GPUI's App borrow (SetWindowPos sends WM_SIZE / WM_MOVE synchronously).
pub fn overlay_fixup(window: &Window, id: DisplayId) -> impl FnOnce() -> String + 'static {
    let h = hwnd(window).0 as isize;
    move || unsafe {
        let h = HWND(h as _);
        let before = client_on_screen(h);
        let Some((_, m)) = monitor_device(HMONITOR(u64::from(id) as _)) else { return "monitor not found".into() };
        let old = GetWindowLongPtrW(h, GWL_STYLE) as u32;
        let frame_bits = WS_CAPTION.0 | WS_THICKFRAME.0 | WS_SYSMENU.0 | WS_MINIMIZEBOX.0 | WS_MAXIMIZEBOX.0;
        SetWindowLongPtrW(h, GWL_STYLE, ((old & !frame_bits) | WS_POPUP.0) as isize); // keeps WS_VISIBLE
        SetWindowPos(h, Some(HWND_TOPMOST), m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_NOACTIVATE | SWP_FRAMECHANGED).ok();
        let mut w = RECT::default();
        GetWindowRect(h, &mut w).ok();
        format!(
            "style -> WS_POPUP (visible {}), client {} -> window {}, client -> {}",
            IsWindowVisible(h).as_bool(),
            fmt_rect(before),
            fmt_rect(w),
            fmt_rect(client_on_screen(h))
        )
    }
}

/// Programmatic checks, run in the desktop session: WindowFromPoint skips windows that are
/// click-through (layered + transparent), and the screen pixel on the edge shows whether the
/// glow actually reaches the screen.
pub fn probe_overlay(window: &Window) -> String {
    let h = hwnd(window);
    unsafe {
        let r = client_on_screen(h);
        let p = POINT { x: r.right - 3, y: (r.top + r.bottom) / 2 };
        let under = WindowFromPoint(p);
        let root = GetAncestor(under, GA_ROOT);
        let mut cls = [0u16; 128];
        let n = GetClassNameW(root, &mut cls) as usize;
        let cls = String::from_utf16_lossy(&cls[..n]);
        // GetPixel on the screen DC misses layered windows; BitBlt with CAPTUREBLT includes them.
        let screen = GetDC(None);
        let mem = CreateCompatibleDC(Some(screen));
        let bmp = CreateCompatibleBitmap(screen, 1, 1);
        let old = SelectObject(mem, bmp.into());
        BitBlt(mem, 0, 0, 1, 1, Some(screen), p.x, p.y, SRCCOPY | CAPTUREBLT).ok();
        let px = GetPixel(mem, 0, 0);
        SelectObject(mem, old);
        let _ = DeleteObject(bmp.into());
        let _ = DeleteDC(mem);
        ReleaseDC(None, screen);
        let (rr, gg, bb) = (px.0 & 0xff, (px.0 >> 8) & 0xff, (px.0 >> 16) & 0xff);
        format!(
            "WindowFromPoint({},{}) -> {} (class '{cls}'), screen pixel #{rr:02x}{gg:02x}{bb:02x}",
            p.x,
            p.y,
            if root == h { "THE OVERLAY: not click-through".to_string() } else { format!("{:?}: passes through", root.0) }
        )
    }
}

pub fn process_stats() -> String {
    unsafe {
        let p = GetCurrentProcess();
        let mut mc = PROCESS_MEMORY_COUNTERS::default();
        GetProcessMemoryInfo(p, &mut mc, std::mem::size_of::<PROCESS_MEMORY_COUNTERS>() as u32).ok();
        let (mut c, mut e, mut k, mut u) = (FILETIME::default(), FILETIME::default(), FILETIME::default(), FILETIME::default());
        GetProcessTimes(p, &mut c, &mut e, &mut k, &mut u).ok();
        let t = |f: FILETIME| ((f.dwHighDateTime as u64) << 32 | f.dwLowDateTime as u64) as f64 / 1e7;
        format!("rss {:.1} MB (working set), cpu {:.3} s", mc.WorkingSetSize as f64 / 1048576.0, t(k) + t(u))
    }
}
