# What Linux forces on the platform seam

Research for [#8](https://github.com/FredeAlexandre/beamer/issues/8), part of the Rust port map
([#1](https://github.com/FredeAlexandre/beamer/issues/1)). Linux is designed for, not built. This note
lists the shape constraints a later Linux backend puts on the Rust platform seam. It is not a Linux
implementation. Researched 2026-11-13 against `main` at `fffbede`.

## Answer

The macOS and Windows backends are both "global hook" shaped. The app installs a tap or hook, sees
every event, decides per event whether to swallow it, reads and warps the pointer whenever it
likes, and draws windows anywhere. Wayland breaks every one of those assumptions:

- Capture is a session that the desktop grants and starts.
- Capture triggers only at barriers on the outer edge of the screens.
- Permissions are asynchronous dialogs that can be revoked at any time.
- No app can read or move the pointer.
- No per-event pass-through exists.
- Overlays are compositor-managed per-output layers, and GNOME has none.
- The clipboard rides on another portal session.

X11 sits close to the Windows shape. Two conditions keep a Wayland backend from forcing a redesign:

1. The seam is asynchronous and session-oriented, with explicit permission states and "this
   capability is unavailable here" answers.
2. The seam treats edges, display geometry, the pointer position and overlays as things the
   backend owns and declares. Core must not compute them from global OS queries.

Both cost the Mac and Windows backends almost nothing to satisfy, so they should be built in from
the start. Section 2 lists the 17 constraints. The six that shape the seam most:
[C2](#c2-capture-is-a-granted-session-with-a-lifecycle-not-a-hook),
[C3](#c3-the-app-cannot-start-a-capture-only-a-barrier-crossing-can),
[C5](#c5-the-edge-hold-runs-on-captured-input-and-letting-go-carries-a-position),
[C7](#c7-no-global-pointer-position-and-no-warp),
[C11](#c11-permissions-are-asynchronous-multiple-revocable-and-persisted-by-rotating-tokens) and
[C14](#c14-overlays-are-per-output-compositor-layers-and-gnome-has-none).

Source keys used below:

- **[IC]** [`org.freedesktop.portal.InputCapture`](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.InputCapture.xml) (v2)
- **[RD]** [`org.freedesktop.portal.RemoteDesktop`](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.RemoteDesktop.xml) (v2)
- **[CB]** [`org.freedesktop.portal.Clipboard`](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.Clipboard.xml) (v1)
- **[GS]** [`org.freedesktop.portal.GlobalShortcuts`](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.GlobalShortcuts.xml) (v2)
- **[SE]** [`org.freedesktop.portal.Session`](https://github.com/flatpak/xdg-desktop-portal/blob/main/data/org.freedesktop.portal.Session.xml)
- **[EI]** the [libei protocol](https://gitlab.freedesktop.org/libinput/libei/-/blob/main/proto/protocol.xml) (`ei_seat`, `ei_device`, `ei_keyboard`, `ei_text`)
- **[LS]** [wlr-layer-shell-unstable-v1](https://gitlab.freedesktop.org/wlroots/wlr-protocols/-/blob/master/unstable/wlr-layer-shell-unstable-v1.xml) and its [compositor support table](https://wayland.app/protocols/wlr-layer-shell-unstable-v1)
- **[PW]** [pointer-warp-v1](https://wayland.app/protocols/pointer-warp-v1)
- **[ashpd]** [ashpd 0.14 docs](https://docs.rs/ashpd/latest/ashpd/)
- **[reis]** [reis 0.7.1 docs](https://docs.rs/reis/latest/reis/)
- Code references are to this repository at `fffbede`, for example `win_app/capture_portal.py:366`.
  The Python Linux backends are upstream's, and their comments record compositor behaviour that
  upstream measured (KWin 6.7.5, GNOME, Fedora 44). This research did not re-measure it.

---

## 1. How each platform does the five things the seam covers

| | macOS (`mac_app/`) | Windows (`win_app/*_win.py`) | X11 (`*_x11.py`) | Wayland (`*_portal.py`) |
|---|---|---|---|---|
| **Capture** | `CGEventTapCreate`. The app may tap at any time and swallows per event by returning NULL (`mac_app/bridge.py:1784`) | `WH_*_LL` hooks plus Raw Input. The app swallows per event by returning 1 (`win_app/capture_win.py:1`) | XInput2 raw events plus an active grab of every master device. All or nothing, no per-event swallow (`win_app/capture_x11.py:1`) | The InputCapture portal arms barriers and the **compositor** starts capture. Events arrive over libei until Release (`win_app/capture_portal.py:1`) |
| **Starting a capture** | Any time (hotkey, tray, edge) | Any time | Any time (grab) | Only when the pointer crosses a barrier. "There is currently no way for an application to activate immediate input capture" [IC] |
| **Edge hold** | Freeze the cursor (`CGAssociateMouseAndMouseCursorPosition`) and read deltas | The OS clamps the cursor at the desktop edge while raw deltas keep flowing | XFixes barriers at internal seams (`win_app/x11_seams.py`) | The barrier triggers capture. The app emulates the hold (`Press`, `capture_portal.py:255`) and then Releases at a point |
| **Injection** | `CGEventPost`, events marked as injected | `SendInput` with `INJECTED_MARK` in `dwExtraInfo` | XTest. Can remap spare keycodes, the server repeats keys | The RemoteDesktop portal plus a libei sender. Devices, regions and keymap all come from the compositor (`inject_portal.py`) |
| **Pointer position** | `CGEventGetLocation` and `CGWarpMouseCursorPosition` | `GetCursorPos` and `SetCursorPos` | Query pointer, `XWarpPointer` | **None.** The backend keeps a believed position (`desktop_portal.py:1`) |
| **Overlays** | `NSPanel` at `NSStatusWindowLevel`, anywhere | Qt top-level window, anywhere | Override-redirect window | Layer-shell, per output and anchored, with no support on GNOME. Fallbacks: XWayland windows or a GTK helper process (`identify_layer_shell.py`, `effect_overlay_x11.py`) |
| **Clipboard** | `NSPasteboard`, `changeCount` polling | `OpenClipboard` (contended) | The owner serves on demand from the GUI event loop (`clipboard_x11.py`) | The Clipboard portal **on the RemoteDesktop session**. Ownership comes by signal, data through file descriptors (`clipboard_portal.py`) |
| **Global hotkey** | Read from the tap (double-tap triggers work) | Read from the hook | Read from raw events | GlobalShortcuts portal: a named action that the user can rebind. No key stream (`global_shortcut_portal.py`) |
| **Permission** | Accessibility and Input Monitoring. Synchronous query, relaunch after a grant | None | None | Asynchronous dialog per portal, refusable, revocable, persisted with a restore token |

How the backend is chosen today: `win_app/platform_parts.py:19-77`. The choice happens **at runtime** from
`XDG_SESSION_TYPE` (or `DISPLAY` without `WAYLAND_DISPLAY`). Under Wayland, `DISPLAY` belongs to XWayland, which
cannot capture or inject for other apps. Firewall and unlock have no Linux backend and get explicit
stand-ins (`win_app/linux_stand_ins.py`).

---

## 2. The constraints

Each entry gives the constraint, the reason it exists, and its source. "Cost on Mac and Windows"
says what the constraint asks of the backends that are being built.

### C1. The backend is chosen at runtime, per session, not per `target_os`

- **Constraint:** within the Linux build, the X11 and Wayland backends are chosen at startup. A build
  must be able to hold both, behind the same trait objects or enum.
- **Why:** one Linux binary runs under either display server. Under Wayland, `DISPLAY` belongs to
  XWayland and must not be used for capture or injection.
- **Source:** `win_app/platform_parts.py:19-30`.
- **Cost on Mac and Windows:** none, if the seam is a set of traits picked at startup rather than
  `#[cfg]` functions.

### C2. Capture is a granted session with a lifecycle, not a hook

- **Constraint:** the capture part exposes a state machine, not just `start()` and `stop()`. The
  states are: `Unavailable(reason)`, `AwaitingPermission` (with no time limit),
  `Refused`, `Armed`, `Active`, and `Closed(by desktop)`. The part also offers an explicit "ask again"
  operation. Every transition is reported as an event.
- **Why:**
  - `CreateSession2` and then `Start` "typically presents a dialog" [IC]. The answer comes through a
    `Request::Response` that can be refused (code 1) or ended another way (code 2).
  - The compositor "may filter events or stop capturing at any time" [IC].
  - The session can be closed by the portal at any time (`Session::Closed`) [IC] [SE].
  - The Python backend waits for the dialog with no timeout and logs once after 60 s
    (`win_app/portal.py:38`, `:149`).
  - The Python backend reports `ready`, `refused` and `unavailable` to a permission UI
    (`capture_portal.py:545`, `kvm_bridge_win.py:567`).
- **Source:** [IC] (CreateSession2, Start, Activated, Deactivated, Disabled), [SE],
  `win_app/capture_portal.py:297-402` and `:574-621`.
- **Cost on Mac and Windows:** the Mac already has a grant (Accessibility and Input Monitoring) and
  maps onto `AwaitingPermission` and `Refused`. Windows always moves straight to `Armed`.

### C3. The app cannot start a capture: only a barrier crossing can

- **Constraint:** "send input to machine X now" (from the hotkey, the tray, the Send button or a
  peer's request) is a request that **completes asynchronously or fails**, for example with
  `on_captured` and `on_failed` callbacks and a deadline. The backend may refuse it outright. Core
  must also handle "send away unsupported, bring home only".
- **Why:**
  - "Only the compositor decides when to enter this state … There is currently no way for an
    application to activate immediate input capture" [IC].
  - Beamer works around this by pushing the pointer through one of its own barriers with
    RemoteDesktop **relative** motion (`capture_portal.py:475`, `inject_portal.py:845`).
  - That push needs a non-zero relative delta: KWin 6.7.5 gives absolute motion a zero delta, and
    its barriers ignore it (`inject_portal.py:850`).
  - Without the push, Send can only bring input home (`kvm_bridge_win.py:1463-1469`, "Send only
    beeped (KDE report, 13-10-2026)").
- **Consequence for the seam:** the capture backend depends on the injection backend (the push).
  The seam must allow that edge between parts, or offer one Linux "input session" object that
  owns both.
- **Cost on Mac and Windows:** they complete the request synchronously.

### C4. Edges are declared up front as coarse geometry and re-declared after every display change

- **Constraint:** core hands the backend the set of armed edges as a declarative value: per
  display, a whole edge, chosen thirds, or a corner's two walls. The backend answers with which
  edges it placed. Core keeps the fine filtering (thirds, corners, per-peer routing) and must
  accept that a trigger can be coarser than the edge it asked for. The set is diffed and pushed
  again only when it changes.
- **Why:**
  - Barriers "must be placed at the outside boundary of zones and must be fully contained within
    one zone". Internal monitor seams are impossible, unlike `win_app/x11_seams.py` [IC].
  - `SetPointerBarriers` "immediately suspends the current session", needs `Enable` again, and
    returns `failed_barriers` [IC].
  - The barriers must carry the current `zone_set`. `ZonesChanged` invalidates them and the zones
    must be fetched again [IC].
  - KDE's portal rejects partial edges, so Beamer places whole edges there and lets go of captures
    outside the chosen part (`capture_portal.py:223`, `:713`).
  - GNOME refuses barriers on an enabled session (`capture_portal.py:366-383`).
  - `barrier_id` may be 0 or missing (KWin omits it), so the backend works out which wall from the
    reported position (`capture_portal.py:754-767`) [IC Activated].
  - Barriers on the Mac's notch stretch (`SpanEdge`) are impossible (`capture_portal.py:216`).
- **Cost on Mac and Windows:** they accept the armed-edge set and keep testing edges on every move,
  as today.

### C5. The edge hold runs on captured input, and letting go carries a position

- **Constraint:** capture has a third state between "local" and "away": **held at the edge**.
  Input is technically captured, but logically still on this machine. The backend feeds the
  resistance and zone models from captured deltas, and the "let go" operation takes the point
  where the pointer should reappear. Anything pressed during the hold may be **lost** rather than
  passed through.
- **Why:**
  - On Windows the OS clamps the cursor, and raw deltas keep flowing without any capture
    (`win_app/sender.py:820-830`). On Wayland, reaching the barrier already captures every
    keyboard and pointer.
  - `Activated.cursor_position` "is usually outside the Zones … It is the application's
    responsibility to calculate and adjust the cursor position" [IC].
  - `Release.cursor_position` is only "a suggestion … The compositor is not required to honor
    this suggestion" [IC].
  - A key or button during the hold is dropped: "nothing can play it back here"
    (`capture_portal.py:12-20`, `:1040-1046`).
- **Cost on Mac and Windows:** the hold state already exists there implicitly. On those platforms,
  let-go is "stop swallowing, optionally warp".

### C6. There is no per-event pass-through: capture is all devices or nothing

- **Constraint:** do not make "the hook returns swallow or pass for each event" the only capture
  contract. Pass-through of single events becomes an optional capability (`can_pass_through`).
  The features built on it are capabilities that the backend may deny, and the UI hides them:
  - the "stays on this PC" keys,
  - keep-keyboard-here ("mouse only"),
  - the key that happens to be down as input comes home.
- **Why:**
  - Once capture is active, "events … are sent directly to the application instead of being used
    to update the pointer position" [IC]. Beamer's code has no way to hand one back.
  - `mouse_only_supported=not platform_parts.WAYLAND` (`kvm_bridge_win.py:674`), and "InputCapture
    owns the keyboard too" (`:3443`).
  - The capability set is fixed when `Start` is called [IC]. Pointer-only capture would therefore
    need a new session, and possibly a new dialog. That last point is inferred and was not tested.
  - On X11, pass-through is a replay through XTest with the grab released around it
    (`capture_x11.py:22-25`). It works, but is not a true pass-through.
- **Cost on Mac and Windows:** the backends report `can_pass_through = true`.

### C7. No global pointer position, and no warp

- **Constraint:** the pointer position is **owned by the backend**. Core asks for it and gets an
  answer that may be stale or `None`. The backend keeps the position current from what it knows:
  the barrier hit, its own injected moves, and its own let-go point. The following become
  capabilities that can be absent, and core must not rely on polling for them:
  - `cursor_position` from the OS,
  - warping the pointer outside capture,
  - `button_down` (the drag guard),
  - `full_screen_app` (the full-screen hold),
  - idle pointer hiding,
  - local-motion notification while not capturing.
- **Why:**
  - Wayland tells no app where the pointer is and lets none move it. The Python backend therefore
    keeps "the last position as the truth" (`win_app/desktop_portal.py:1-23`). `button_down`
    returns `False` and `full_screen_app` returns `None` (`:303-308`).
  - `pointer-warp-v1` warps only inside the client's own surface while it has pointer focus,
    and honouring it is "implementation defined" [PW].
  - Pointer hiding happens only while InputCapture is active, and the compositor does it
    (`desktop_portal.py:20-23`, `kvm_bridge_win.py:1372-1388`).
- **Cost on Mac and Windows:** they back these calls with the real OS queries.

### C8. Capture, injection and toolkit use three coordinate spaces, and none carries a display ID

- **Constraint:** display identity and geometry are mapped inside the backend. The seam's geometry
  types say which space they belong to. Core's arrangement names a display by a key the backend
  resolves, and never by an assumption that one global pixel space is shared by capture,
  injection and overlays. Display-change events must come from the backend (`ZonesChanged`), not
  only from the UI toolkit.
- **Why:**
  - Zones are logical, "may be a logical representation of the physical screens (e.g. a 4k
    screen may be represented as low-resolution screen)", and "are session-specific" [IC].
  - libei regions are "private to the EIS compositor and coordinates may not match the size of
    the actual desktop", each absolute device has its own regions, and events outside them are
    not allowed [EI `ei_device.region`].
  - Beamer matches the toolkit's screens to portal rectangles by aspect ratio and relative layout,
    using a Hungarian assignment, and refuses the match when two assignments tie
    (`desktop_portal.py:142-237`). Positions are kept in zone space and translated before absolute
    libei moves (`:258-278`).
- **Cost on Mac and Windows:** the mapping is the identity function.

### C9. Injection gets compositor-chosen devices that arrive late, pause, and have a fixed keymap

- **Constraint:**
  - The injector accepts **semantic** input: a named key, a character with its US position,
    button, scroll, and relative or absolute motion. The backend plans the actual key codes.
  - The injector queues input until its devices are ready, keeps the queue bounded, and drops
    stale input.
  - It reports characters it cannot type.
  - It makes `release_all` safe whatever state the devices are in.
  - Key-repeat policy belongs to the backend.
- **Why:**
  - The EIS side decides seats and devices. Clients bind capabilities, and the compositor "may
    create multiple devices" [EI `ei_seat.bind` and `request_device`].
  - New devices start paused, and pausing "resets the logical state of the device to neutral"
    [EI `ei_device.paused`].
  - The keymap is sent by the compositor and "constant for the lifetime of the device". A sender
    must send the keycodes that produce the keysym it wants [EI `ei_keyboard.keymap`]. Beamer
    therefore cannot remap spare keys (`inject_portal.py:104`, `can_remap = False`), and a
    character with no plain, Shift, AltGr or dead-key sequence is not typed (`inject_portal.py:11-15`).
  - The protocol now has `ei_text.utf8` and `ei_text.keysym` [EI]. Which compositors support them
    was not verified.
  - GNOME offers absolute pointer devices (`inject_portal.py:20-21`). The device set is awaited for
    up to `DEVICE_SECONDS` (`:61`).
  - On X11 the server repeats a held key, while on Wayland the receiving app repeats it
    (`inject_portal.py:13-15`, `inject_x11.py:13-17`).
  - Lock state arrives asynchronously after the lock key is released (`inject_portal.py:16-19`, `:63`).
- **Wire implication** (for the protocol ticket): Linux speaks evdev codes and XKB keysyms. If the
  wire names keys by Windows VK codes, every Linux backend needs a translation table.
- **Cost on Mac and Windows:** they already plan keys this way (`plan_key_inputs`).

### C10. Injected input is not marked, so loop prevention has to rely on state

- **Constraint:** core prevents capture/injection feedback loops through state: "while a peer
  drives this machine, local motion never starts a crossing". It must not rely on a per-event
  "this was injected" flag, which the backend may not have.
- **Why:**
  - Windows filters `LLMHF_INJECTED` and an `INJECTED_MARK` (`capture_win.py:20-31`). X11 drops
    events from XTEST devices (`capture_x11.py:34-35`).
  - On Wayland, "barriers also capture injected motion", so the sender ignores local pushes while
    driven (`win_app/sender.py:831-833`). Nothing in [EI] or [IC] marks an event as coming from
    another client.
- **Cost on Mac and Windows:** none. They keep the flag and gain the state check as a second
  guard.

### C11. Permissions are asynchronous, multiple, revocable, and persisted by rotating tokens

- **Constraint:**
  - Permission is modelled **per part**: capture, injection, clipboard and global shortcut. Each
    part has the states from C2 and a UI action "ask again".
  - No caller thread ever blocks on a dialog. For example, `set_clipboard` returns `false` while a
    session is still waiting for permission.
  - The backend gets a private, per-user store for its grant tokens.
  - The app can choose **when** to prompt, at startup while the user is at the machine, rather
    than at a peer's first input.
  - Closing a session must free input whatever else has failed. In Rust that means RAII: dropping
    the session closes the D-Bus connection.
- **Why:**
  - Each portal shows its own dialog: InputCapture `Start`, RemoteDesktop `Start`, and
    GlobalShortcuts `BindShortcuts` [IC] [RD] [GS].
  - `restore_token` "is invalidated after using it once. To restore the same session again, use
    the new restore token", with `persist_mode` 2 meaning "until explicitly revoked" [IC] [RD].
    Beamer stores the token mode 0600 under `$XDG_STATE_HOME/beamer` (`portal.py:67-104`).
  - "A client who started a session vanishing from the D-Bus is equivalent to closing all active
    sessions" [SE]. Beamer relies on this as its way home after a crash, and adds a watchdog that
    aborts the connection when the capture thread stalls (`capture_portal.py:651-659`).
  - A session closed by the user is not reopened behind their back (`inject_portal.py:533`).
  - Clipboard writes are never made to wait on the consent dialog (`clipboard_portal.py:148-153`).
- **Cost on Mac and Windows:** the Mac maps its Accessibility and Input Monitoring grant onto the
  same model. Windows reports "granted".

### C12. The clipboard rides on another part's session: ownership is announced, data is pulled lazily

- **Constraint:** the clipboard part:
  - reports change events that carry an "it is ours" flag (polling is allowed but not required),
  - offers data as a MIME list and serves each paste lazily through a callback or stream with a
    timeout,
  - reads through a stream with a bounded wait,
  - may be **unavailable until another part's permission is granted**,
  - never blocks the link thread.
- **Why:**
  - The Clipboard portal "does not create its own sessions". It extends RemoteDesktop or
    InputCapture sessions, and `RequestClipboard` must be called before `Start` [CB].
  - On InputCapture sessions, "clipboard access is only available while the session is active"
    [CB] [IC]. That means only while input is away.
  - Ownership arrives through `SelectionOwnerChanged` with `session_is_owner`. Reads go through
    `SelectionRead`, which returns a file descriptor. Pastes arrive as `SelectionTransfer(mime,
    serial)` and are answered with `SelectionWrite` and `SelectionWriteDone` [CB].
  - GNOME offers the clipboard to a background app only through the RemoteDesktop session, so
    Beamer's clipboard depends on the injection grant even on a machine that only sends input
    (`clipboard_portal.py:1-12`, `inject_portal.py:2-4`).
  - X11 is also an on-demand owner model (`clipboard_x11.py:1-12`). The Mac (`changeCount`) and
    Windows fit the lazy-serve shape by serving eagerly.
- **Cost on Mac and Windows:** small. They serve eagerly behind the same interface.

### C13. A global hotkey is a desktop-bound named action, not a key pattern

- **Constraint:** hotkeys are declared as **actions**, each with a preferred trigger. The backend
  binds them and reports the trigger the user actually has. Trigger detection from the raw key
  stream (modifier double-tap, the trigger key) is an optional capability, available only where a
  key stream exists.
- **Why:**
  - `BindShortcuts` takes an ID, a description and a `preferred_trigger`. The desktop lets the user
    configure the shortcut and returns a `trigger_description`. Activation is a signal per ID, and
    `ShortcutsChanged` reports a rebinding [GS].
  - Outside capture, Beamer sees no keys at all (`capture_portal.py:8-10`), so the double-tap
    "cannot be seen" (`kvm_bridge_win.py:3607-3618`).
- **Cost on Mac and Windows:** they implement actions by matching the trigger in the hook.

### C14. Overlays are per-output compositor layers, and GNOME has none

- **Constraint:**
  - The overlay part is addressed by **display, plus an anchor or edge, plus a size**, never by
    global desktop coordinates.
  - It reports overlay availability per display.
  - It accepts that the renderer may live in another surface type, toolkit or process.
  - The UI toolkit choice needs a layer-shell escape hatch, for example a raw Wayland surface made
    with `smithay-client-toolkit`'s `wlr_layer` module.
  - Edge effects and identify numbers must degrade gracefully where no overlay can be drawn.
- **Why:**
  - Layer-shell surfaces are created per `wl_output`, anchored to output edges, and placed on a
    z-layer (`overlay` is the top). Size and anchor follow the output, not desktop coordinates
    [LS].
  - wlr-layer-shell is supported by KWin, Sway, Hyprland, COSMIC and others, **but not by Mutter
    (GNOME) or Weston** [LS support table].
  - Beamer draws identify numbers through a GTK layer-shell helper process with an empty input
    region and no keyboard interactivity (`win_app/identify_layer_shell.py`). It draws edge effects
    through XWayland windows only when `DISPLAY` is set (`effect_overlay_x11.py:1`,
    `kvm_bridge_win.py:5709-5711`). Otherwise it tells the user that Wayland "does not let Beamer
    draw at the edges" (`kvm_bridge_win.py:4002-4007`).
- **Conflict with the map:** the map makes "transparent click-through overlays" a hard requirement.
  On GNOME Wayland that cannot be met natively. The requirement has to say "where the desktop
  allows it".
- **Cost on Mac and Windows:** they place a window at the display's rectangle.

### C15. Raising your own window needs an activation token

- **Constraint:** "show the main window" takes an optional opaque activation token. The backend
  carries that token from the event that caused the action: a global shortcut, or a second
  instance being launched.
- **Why:** GlobalShortcuts `Activated` carries an `activation_token` for this purpose [GS].
  Beamer sets `XDG_ACTIVATION_TOKEN` before raising its window (`kvm_bridge_win.py:5558-5563`).
- **Cost on Mac and Windows:** they ignore the token.

### C16. Backends run their own async event loop: core must never wait for a synchronous decision

- **Constraint:**
  - Backends talk to core through channels. Events flow from the backend to core; commands flow
    from core to the backend.
  - The state the backend needs on its hot path, such as "input is away" or "the edges are held",
    is shared as cheap lock-free values that the backend reads. This is today's
    `grab_while(predicate)` (`capture_portal.py:467`).
  - The seam must not require a backend to call into core synchronously and wait for a verdict.
- **Why:**
  - The Wayland backend must multiplex the D-Bus connection and the libei fd. Beamer selects on
    both (`capture_portal.py:643-649`, `portal.py:1-14`).
  - In Rust, `ashpd` is async on `zbus`, with `tokio` by default or `async-io` for smol and glib.
    It covers `input_capture`, `remote_desktop`, `clipboard`, `global_shortcuts`, `background`
    and `notification`, plus `PersistMode` [ashpd].
  - `reis` is a pure-Rust libei/libeis implementation with a `tokio` feature and a high-level
    `event` module, and is described as "currently incomplete and subject to change" [reis].
  - Windows pulls the other way: an LL hook callback must answer at once on its own thread
    (`capture_win.py:5-16`). Shared atomics serve both models. A synchronous call into core does
    not.
- **Cost on Mac and Windows:** they are already built this way (hook thread plus predicate).

### C17. Every part may be absent, and sandboxing moves some parts behind portals

- **Constraint:** every part (firewall, unlock, autostart, notifications, overlays, hiding) can
  answer "unsupported here" with a reason sentence, rather than being faked. Autostart and
  notifications are platform parts too.
- **Why:**
  - Linux has no firewall check or unlock backend (`win_app/linux_stand_ins.py`).
  - In a Flatpak, start-at-login needs the Background portal (`docs/flatpak-build.md:39`), and
    tray notifications do not reach the desktop, so the Notification portal is used
    (`portal.py:260-267`).
- **Cost on Mac and Windows:** none.

---

## 3. What X11 adds beyond Windows

X11 needs little that Windows does not already need:

- capture without per-event swallow (C6: the grab and an XTest replay),
- barriers at internal monitor seams, because X has no clamp there (`x11_seams.py`),
- an on-demand clipboard owner tied to the GUI event loop (C12),
- server key repeat (C9).

On X11 the pointer can be queried and warped, capture can start at any time, and override-redirect
overlays can be placed anywhere. An X11 backend fits any seam that already fits Windows plus C6
and C12.

## 4. Suggested seam shape (summary)

This list compresses the constraints above. It does not prescribe the architecture, which belongs
to the architecture ticket.

- Parts are chosen at runtime: `Capture`, `Inject`, `Clipboard`, `Pointer` (believed position and
  capabilities), `Displays` (mapped geometry), `Overlay`, `Hotkeys`, `Permissions`, `Autostart`,
  `Notify`, plus the Windows-only extras. Each part reports `capabilities()` and a permission state.
- Capture states: `Local → HeldAtEdge → Away → Local`. Two operations matter:
  - `request_capture(target) -> async Result`,
  - `let_go(at: Point)`.
  Armed edges are a declarative value, answered with the edges actually placed.
- Every OS interaction can be asynchronous. Events go out on channels, and hot-path state comes in
  through atomics.
- Geometry is tagged with its space, and displays are named by keys the backend resolves.
- Each session is an RAII object whose drop frees input.

## 5. Not verified

- How mutter, KWin and xdg-desktop-portal-wlr support InputCapture and when they added it. GNOME
  added the portal in xdg-desktop-portal-gnome 45
  ([45.beta news](https://download.gnome.org/sources/xdg-desktop-portal-gnome/45/xdg-desktop-portal-gnome-45.beta.news)).
  KDE's first release with it (around Plasma 6.1) is inferred from a
  [planning post](https://mail.kde.org/pipermail/kde-devel/2024-March/002515.html) and was not
  confirmed. The code's claim that v2 (restore tokens) came with "GNOME 51, Plasma 6.7"
  (`capture_portal.py:29`) was not re-checked.
- The compositor quirks recorded in the Python comments: GNOME refusing barriers on an enabled
  session, KDE rejecting partial edges, KWin omitting `barrier_id`, and KWin ignoring absolute
  motion for barriers. They come from upstream's own testing and were not re-measured.
- Whether any compositor implements `ei_text` (utf8 and keysym injection), which would lift the
  "untypable character" limit in C9.
- Whether a pointer-only InputCapture session would make "mouse only" possible on Wayland (C6).
- Whether `reis` 0.7 covers every event Beamer reads, such as `modifiers` and `keymap` on receiver
  contexts, and the `start_emulating` sequence used to match `activation_id`.
- The smithay-client-toolkit `wlr_layer` API was seen only at module level (0.21.1). Its variants
  were not checked.
- Audio and camera on Linux (PipeWire, the Camera portal) are out of scope for this ticket.
