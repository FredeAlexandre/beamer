// PROTOTYPE: tray / menu-bar item through `tray-icon` (GPUI has none). Its events arrive on the main
// thread from the platform's own loop (AppKit / Win32, which GPUI is already running) and are
// forwarded into GPUI's executor through a channel.
use futures::channel::mpsc::{UnboundedReceiver, unbounded};
use gpui::{App, DisplayId, Global};
use tray_icon::{
    Icon, MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent,
    menu::{CheckMenuItem, Menu, MenuEvent, MenuId, MenuItem, PredefinedMenuItem, Submenu},
};

use crate::{logln, state::GlobalState};

pub enum TrayMsg {
    Menu(MenuId),
    LeftClick,
}

struct Tray {
    _icon: TrayIcon,
    send: Vec<CheckMenuItem>,
    pause: CheckMenuItem,
    glow: Vec<(DisplayId, CheckMenuItem)>,
    glow_menu: Submenu,
    test: MenuItem,
    settings: MenuItem,
    quit: MenuItem,
}
impl Global for Tray {}

/// A filled circle with a vertical bar, drawn at runtime.
fn glyph() -> Icon {
    let n = 32u32;
    let mut px = vec![0u8; (n * n * 4) as usize];
    #[cfg(target_os = "macos")]
    let rgb = [0u8, 0, 0]; // template image: only alpha matters
    #[cfg(not(target_os = "macos"))]
    let rgb = [0x5fu8, 0xd4, 0xf4];
    for y in 0..n {
        for x in 0..n {
            let (fx, fy) = (x as f32 + 0.5, y as f32 + 0.5);
            let d = ((fx - 12.0).powi(2) + (fy - 16.0).powi(2)).sqrt();
            let circle = (7.5 - d).clamp(0.0, 1.0);
            let bar = if (22.0..26.0).contains(&fx) && (5.0..27.0).contains(&fy) { 1.0 } else { 0.0 };
            let a = circle.max(bar);
            let i = ((y * n + x) * 4) as usize;
            px[i..i + 3].copy_from_slice(&rgb);
            px[i + 3] = (a * 255.0) as u8;
        }
    }
    Icon::from_rgba(px, n, n).unwrap()
}

pub fn create(cx: &mut App) -> UnboundedReceiver<TrayMsg> {
    let state = cx.global::<GlobalState>().0.read(cx);
    #[cfg(target_os = "macos")]
    let (me, other) = ("This Mac", "Desk PC");
    #[cfg(not(target_os = "macos"))]
    let (me, other) = ("This PC", "Studio Mac");
    let send = vec![CheckMenuItem::new(me, true, true, None), CheckMenuItem::new(other, true, false, None)];
    let send_menu = Submenu::with_items("Send input to", true, &[&send[0], &send[1]]).unwrap();
    let pause = CheckMenuItem::new("Pause crossing", true, state.paused, None);
    let glow: Vec<(DisplayId, CheckMenuItem)> = state
        .displays
        .iter()
        .map(|d| (d.id, CheckMenuItem::new(&d.name, true, Some(d.id) == state.glow_on, None)))
        .collect();
    let glow_menu = Submenu::new("Glow on", true);
    for (_, item) in &glow {
        glow_menu.append(item).unwrap();
    }
    let test = MenuItem::new("Test glow", true, None);
    let settings = MenuItem::new("Settings…", true, None);
    let quit = MenuItem::new("Quit", true, None);
    let menu = Menu::with_items(&[
        &send_menu,
        &pause,
        &PredefinedMenuItem::separator(),
        &glow_menu,
        &test,
        &PredefinedMenuItem::separator(),
        &settings,
        &quit,
    ])
    .unwrap();

    let builder = TrayIconBuilder::new();
    #[cfg(target_os = "macos")]
    let builder = builder.with_icon_templated(glyph());
    #[cfg(not(target_os = "macos"))]
    let builder = builder.with_icon(glyph());
    let icon = builder
        .with_tooltip("Beamer bake-off · gpui")
        .with_menu(Box::new(menu))
        // Windows: left-click opens Settings, right-click the menu. macOS: click opens the menu.
        .with_menu_on_left_click(cfg!(target_os = "macos"))
        .build();
    let icon = match icon {
        Ok(i) => i,
        Err(e) => {
            logln!("ERROR tray: {e}");
            panic!("tray: {e}");
        }
    };

    let (tx, rx) = unbounded();
    let tx2 = tx.clone();
    MenuEvent::set_event_handler(Some(move |e: MenuEvent| {
        tx.unbounded_send(TrayMsg::Menu(e.id)).ok();
    }));
    TrayIconEvent::set_event_handler(Some(move |e: TrayIconEvent| {
        if let TrayIconEvent::Click { button: MouseButton::Left, button_state: MouseButtonState::Up, .. } = e {
            tx2.unbounded_send(TrayMsg::LeftClick).ok();
        }
    }));

    cx.set_global(Tray { _icon: icon, send, pause, glow, glow_menu, test, settings, quit });
    // Keep the menu's ticks in step with the state, whoever changed it.
    let entity = cx.global::<GlobalState>().0.clone();
    cx.observe(&entity, |_, cx| sync(cx)).detach();
    rx
}

pub fn rebuild_glow_menu(cx: &mut App) {
    if !cx.has_global::<Tray>() {
        return;
    }
    let s = cx.global::<GlobalState>().0.read(cx);
    let items: Vec<(DisplayId, CheckMenuItem)> = s
        .displays
        .iter()
        .map(|d| (d.id, CheckMenuItem::new(&d.name, true, Some(d.id) == s.glow_on, None)))
        .collect();
    let t = cx.global_mut::<Tray>();
    for (_, old) in t.glow.drain(..) {
        t.glow_menu.remove(&old).ok();
    }
    for (_, item) in &items {
        t.glow_menu.append(item).ok();
    }
    t.glow = items;
    logln!("tray: Glow on menu rebuilt with {} displays", t.glow.len());
}

fn sync(cx: &mut App) {
    let s = cx.global::<GlobalState>().0.read(cx);
    let t = cx.global::<Tray>();
    for (i, item) in t.send.iter().enumerate() {
        item.set_checked(i == s.send_to);
    }
    t.pause.set_checked(s.paused);
    for (id, item) in &t.glow {
        item.set_checked(Some(*id) == s.glow_on);
    }
}

pub fn handle(msg: TrayMsg, cx: &mut App) {
    let entity = cx.global::<GlobalState>().0.clone();
    let id = match msg {
        TrayMsg::LeftClick => {
            crate::open_settings(cx);
            return;
        }
        TrayMsg::Menu(id) => id,
    };
    let t = cx.global::<Tray>();
    if id == *t.quit.id() {
        logln!("tray: quit");
        cx.quit();
    } else if id == *t.settings.id() {
        crate::open_settings(cx);
    } else if id == *t.test.id() {
        crate::test_glow(cx);
    } else if id == *t.pause.id() {
        entity.update(cx, |s, cx| {
            s.paused = !s.paused;
            cx.notify();
        });
    } else if let Some(i) = t.send.iter().position(|m| *m.id() == id) {
        entity.update(cx, |s, cx| {
            s.send_to = i;
            cx.notify();
        });
    } else if let Some((d, _)) = t.glow.iter().find(|(_, m)| *m.id() == id) {
        let d = *d;
        logln!("tray: glow on {d:?}");
        entity.update(cx, |s, cx| {
            s.glow_on = Some(d);
            cx.notify();
        });
    }
    sync(cx); // muda flips a check item itself on click; put the radio state back
}
