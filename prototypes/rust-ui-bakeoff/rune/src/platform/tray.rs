//! Tray / menu-bar item through tray-icon (+ muda menus). Rune's own `Tray` is Windows/Linux only
//! and its menu is flat (actions and separators), so the slice's submenus and checks need this.
//! Lives on the main thread; the items stay in a thread-local so Rune handlers can sync them.

use std::cell::RefCell;

use tray_icon::menu::{CheckMenuItem, IsMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem, Submenu};
use tray_icon::{Icon, MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent};

pub enum Cmd {
    SendTo(usize),
    Pause(bool),
    GlowOn(usize),
    TestGlow,
    Settings,
    Quit,
}

struct Items {
    _tray: TrayIcon,
    send: Vec<CheckMenuItem>,
    pause: CheckMenuItem,
    glow: Vec<CheckMenuItem>,
}

thread_local! {
    static ITEMS: RefCell<Option<Items>> = const { RefCell::new(None) };
}

/// A filled circle with a vertical bar through it, 32×32, black on clear (a template on the Mac).
fn glyph() -> Icon {
    let n = 32usize;
    let mut px = vec![0u8; n * n * 4];
    for y in 0..n {
        for x in 0..n {
            let (fx, fy) = (x as f32 + 0.5 - 16.0, y as f32 + 0.5 - 16.0);
            let r = (fx * fx + fy * fy).sqrt();
            let disc = (11.5 - r).clamp(0.0, 1.0);
            let bar = if fx.abs() < 2.0 && fy.abs() < 15.0 { 1.0 } else { 0.0 };
            // The bar cuts a clear gap through the disc, then continues past it.
            let gap = if fx.abs() < 4.0 && r < 11.5 { 0.0 } else { 1.0 };
            let a = (disc * gap).max(bar);
            let i = (y * n + x) * 4;
            let ink = if cfg!(windows) { 255 } else { 0 };
            px[i..i + 4].copy_from_slice(&[ink, ink, ink, (a * 255.0) as u8]);
        }
    }
    Icon::from_rgba(px, n as u32, n as u32).unwrap()
}

pub fn install(displays: &[String], glow_on: usize, on_cmd: impl Fn(Cmd) + Send + Sync + 'static) {
    let (here, there) = if cfg!(windows) { ("This PC", "Studio Mac") } else { ("This Mac", "Desk PC") };
    let send: Vec<CheckMenuItem> = [here, there].iter().enumerate().map(|(i, l)| CheckMenuItem::with_id(format!("send-{i}"), *l, true, i == 0, None)).collect();
    let glow: Vec<CheckMenuItem> =
        displays.iter().enumerate().map(|(i, l)| CheckMenuItem::with_id(format!("glow-{i}"), l, true, i == glow_on, None)).collect();
    let pause = CheckMenuItem::with_id("pause", "Pause crossing", true, false, None);
    let send_refs: Vec<&dyn IsMenuItem> = send.iter().map(|i| i as &dyn IsMenuItem).collect();
    let glow_refs: Vec<&dyn IsMenuItem> = glow.iter().map(|i| i as &dyn IsMenuItem).collect();
    let menu = Menu::with_items(&[
        &Submenu::with_items("Send input to", true, &send_refs).unwrap(),
        &pause,
        &PredefinedMenuItem::separator(),
        &Submenu::with_items("Glow on", true, &glow_refs).unwrap(),
        &MenuItem::with_id("test", "Test glow", true, None),
        &PredefinedMenuItem::separator(),
        &MenuItem::with_id("settings", "Settings…", true, None),
        &MenuItem::with_id("quit", "Quit", true, None),
    ])
    .unwrap();
    let builder = TrayIconBuilder::new().with_menu(Box::new(menu));
    #[cfg(target_os = "macos")]
    let builder = builder.with_icon_templated(glyph());
    #[cfg(not(target_os = "macos"))]
    let builder = builder.with_icon(glyph());
    let tray = builder
        .with_tooltip("Beamer bake-off · rune")
        // Windows: left click opens Settings, right click the menu. Mac: a click opens the menu.
        .with_menu_on_left_click(!cfg!(windows))
        .build()
        .unwrap();
    ITEMS.with(|it| *it.borrow_mut() = Some(Items { _tray: tray, send, pause, glow }));

    let on_cmd = std::sync::Arc::new(on_cmd);
    let menu_cmd = on_cmd.clone();
    MenuEvent::set_event_handler(Some(move |e: MenuEvent| {
        let id = e.id.0.as_str();
        let cmd = if let Some(i) = id.strip_prefix("send-").and_then(|v| v.parse().ok()) {
            radio(|it| &it.send, i);
            Cmd::SendTo(i)
        } else if let Some(i) = id.strip_prefix("glow-").and_then(|v| v.parse().ok()) {
            radio(|it| &it.glow, i);
            Cmd::GlowOn(i)
        } else {
            match id {
                "pause" => Cmd::Pause(ITEMS.with(|it| it.borrow().as_ref().map(|it| it.pause.is_checked()).unwrap_or(false))),
                "test" => Cmd::TestGlow,
                "settings" => Cmd::Settings,
                "quit" => Cmd::Quit,
                _ => return,
            }
        };
        menu_cmd(cmd);
    }));
    TrayIconEvent::set_event_handler(Some(move |e: TrayIconEvent| {
        if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = e
            && cfg!(windows)
        {
            on_cmd(Cmd::Settings);
        }
    }));
}

/// muda has no radio items: a pick checks itself and unchecks its siblings.
fn radio(group: fn(&Items) -> &Vec<CheckMenuItem>, pick: usize) {
    ITEMS.with(|it| {
        if let Some(it) = it.borrow().as_ref() {
            for (i, item) in group(it).iter().enumerate() {
                item.set_checked(i == pick);
            }
        }
    });
}

pub fn set_paused(on: bool) {
    ITEMS.with(|it| {
        if let Some(it) = it.borrow().as_ref() {
            it.pause.set_checked(on);
        }
    });
}

pub fn set_glow(i: usize) {
    radio(|it| &it.glow, i);
}
