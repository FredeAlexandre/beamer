//! Windows escape hatch (windows-sys): friendly monitor names (winit gives `\\.\DISPLAY1`),
//! refresh rates, and the overlay window's real extended styles, read back from Win32.

use rune::NativeDisplayId;
use windows_sys::Win32::Devices::Display::*;
use windows_sys::Win32::Foundation::*;
use windows_sys::Win32::Graphics::Gdi::*;
use windows_sys::Win32::UI::WindowsAndMessaging::*;

use crate::DisplayInfo;

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(Some(0)).collect()
}

fn from_wide(w: &[u16]) -> String {
    String::from_utf16_lossy(&w[..w.iter().position(|&c| c == 0).unwrap_or(w.len())])
}

/// The monitor's friendly name for a GDI source name such as `\\.\DISPLAY1`.
fn friendly_name(gdi: &str) -> Option<String> {
    unsafe {
        let (mut np, mut nm) = (0u32, 0u32);
        if GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &mut np, &mut nm) != 0 {
            return None;
        }
        let mut paths = vec![std::mem::zeroed::<DISPLAYCONFIG_PATH_INFO>(); np as usize];
        let mut modes = vec![std::mem::zeroed::<DISPLAYCONFIG_MODE_INFO>(); nm as usize];
        if QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &mut np, paths.as_mut_ptr(), &mut nm, modes.as_mut_ptr(), std::ptr::null_mut()) != 0 {
            return None;
        }
        for path in &paths[..np as usize] {
            let mut src: DISPLAYCONFIG_SOURCE_DEVICE_NAME = std::mem::zeroed();
            src.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = std::mem::size_of_val(&src) as u32;
            src.header.adapterId = path.sourceInfo.adapterId;
            src.header.id = path.sourceInfo.id;
            if DisplayConfigGetDeviceInfo(&mut src.header) != 0 || from_wide(&src.viewGdiDeviceName) != gdi {
                continue;
            }
            let mut tgt: DISPLAYCONFIG_TARGET_DEVICE_NAME = std::mem::zeroed();
            tgt.header.r#type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size = std::mem::size_of_val(&tgt) as u32;
            tgt.header.adapterId = path.targetInfo.adapterId;
            tgt.header.id = path.targetInfo.id;
            if DisplayConfigGetDeviceInfo(&mut tgt.header) == 0 {
                let name = from_wide(&tgt.monitorFriendlyDeviceName);
                if !name.is_empty() {
                    return Some(name);
                }
            }
        }
        None
    }
}

pub fn refine(info: &mut DisplayInfo) {
    let gdi = info.name.clone();
    unsafe {
        let mut dm: DEVMODEW = std::mem::zeroed();
        dm.dmSize = std::mem::size_of::<DEVMODEW>() as u16;
        if EnumDisplaySettingsW(wide(&gdi).as_ptr(), ENUM_CURRENT_SETTINGS, &mut dm) != 0 {
            info.refresh_hz = dm.dmDisplayFrequency as f64;
        }
    }
    if let Some(name) = friendly_name(&gdi) {
        info.name = format!("{name} ({gdi})");
    }
    let _ = matches!(info.native, Some(NativeDisplayId::Windows(_)));
}

pub fn overlay_flags(_window_number: Option<u32>, title: &str) -> (String, String) {
    unsafe {
        let hwnd = FindWindowW(std::ptr::null(), wide(title).as_ptr());
        if hwnd.is_null() {
            return ("unknown (window not found)".into(), format!("no window titled {title:?}"));
        }
        let ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE) as u32;
        let st = GetWindowLongPtrW(hwnd, GWL_STYLE) as u32;
        let mut r: RECT = std::mem::zeroed();
        GetWindowRect(hwnd, &mut r);
        let has = |f: u32| ex & f == f;
        let flags = format!(
            "exstyle {ex:#x}: LAYERED {}, TRANSPARENT {}, TOOLWINDOW {}, NOACTIVATE {}, TOPMOST {}, APPWINDOW {}; style {st:#x} POPUP {}; rect ({}, {})-({}, {}) px",
            has(WS_EX_LAYERED), has(WS_EX_TRANSPARENT), has(WS_EX_TOOLWINDOW), has(WS_EX_NOACTIVATE), has(WS_EX_TOPMOST), has(WS_EX_APPWINDOW),
            st & WS_POPUP == WS_POPUP, r.left, r.top, r.right, r.bottom,
        );
        let ct = if has(WS_EX_TRANSPARENT | WS_EX_LAYERED) { "on (WS_EX_TRANSPARENT|WS_EX_LAYERED, read back)" } else { "OFF (no WS_EX_TRANSPARENT)" };
        (ct.into(), flags)
    }
}
