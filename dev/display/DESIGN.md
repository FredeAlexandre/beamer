# Shared displays: design (fork)

Date 2026-11-12. Branch `feat/shared-display`. Wire: WIRE.md section 13. Code: `core/display/`,
`mac_app/shared_display_mac.py`, `win_app/shared_display_win.py`, the menus in `mac_app/kvm_bridge_app.py`
and `win_app/kvm_bridge_win.py`. Tests: `core/tests/test_display.py`.

## The problem

Alex's desk: the Mac (built-in + GN10 over a UGREEN USB-C to HDMI cable, HDMI 2) and the PC `lebig`
(GN10 over DisplayPort + an MSI G24C6). GN10 is shared. Switching its input by hand leaves the other
machine convinced it still drives GN10: windows, the pointer and, on the PC, the primary display stay on
a screen nobody sees.

## What was measured (2026-11-12)

| Question | Answer |
|---|---|
| DDC/CI from the Mac (IOAVServiceWriteI2C, Apple Silicon) | **Blocks forever in the kernel** through the UGREEN cable, whichever input GN10 shows. Unusable. |
| DDC/CI from the PC (dxva2) | GN10 answers **only while it shows the PC**; reads are flaky (retry), the capabilities string never parses. GN10 reports DP as input `0x07` (non-standard); writes of `0x11`, `0x12`, `0x0F` do nothing. HDMI 2's code is unknown. |
| What moves the monitor | GN10's OSD "Automatic Input" (on): when the input it shows loses its signal, it moves to an input that has one. Measured both ways: the Mac turning its output off moves GN10 to the PC in < 8 s; the PC turning its output off moves it to the Mac. |
| Turning the output back on | Does **not** take the monitor back: GN10 keeps the input it moved to. The machine that turned back on then has a hidden screen. |
| Turning off on macOS | Private `CGSConfigureDisplayEnabled` in a display configuration, completed for the session: survives the calling process, ends at logout, restart or replugging. The display leaves `CGGetOnlineDisplayList`; `CGDisplayVendorNumber`/`ModelNumber` still answer for its id; it takes > 2 s to be online again after turning on. |
| Turning off on Windows | CCD API: `SetDisplayConfig` with the active paths minus the monitor (`SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES`, not saved to the database); the MSI becomes primary at (0,0). Back on with `SDC_TOPOLOGY_EXTEND`, which restores the exact previous layout (GN10 primary at (0,0), MSI at (2560,357)). Must run in the signed-in session, as Beamer does. |
| Who has the screen, by software | The PC can read GN10 over DDC exactly when GN10 shows the PC. Not used yet. |

## Decisions

1. **No DDC.** Ownership is done by the non-owners turning their output off; the monitor's automatic
   input selection does the switch. Alex's rule: **never a hidden screen**.
2. **Ownership is a cluster-wide route** (display key -> owner machine or "not managed"), stamped per
   entry, flooded and merged like `audio_route`. Each machine also tells its direct peers which displays
   it has (`display_state`).
3. **A non-owner keeps the display off even while the owner is away** (link down, Beamer quit, PC off),
   because turning it back on would make a hidden screen. It comes back on only on a new choice (this
   machine or "not managed") or when the owner says it does not have that display. Consequence: if the
   owner is switched off, the monitor goes dark until the person picks the other machine in the menu.
4. **A machine turns back on only what it turned off itself** (`turned_off` in `display.json`), never a
   display the person turned off, and never its last display.
5. **Rides cap `audio`** with the feature word `display1` in `audio_hello`, inside the AudioService
   worker as a `DisplayPart`, like the camera: no new capability, the link plumbing and worker reused.
6. **UI: the menus.** Mac menu bar and PC tray: "Screen GN10: <owner>" with a choice per machine that has
   it plus "Not managed". Shown for each local display a linked `display1` peer also has, or that has an
   owner in the route (so it can be taken back while the owner is away).

## Files on disk

- `display.json` (both, Beamer's state folder): `route`, `turned_off`, `rev`.
- `display-mac.json` (Mac): each display this Mac turned off, `{key: {"id", "name"}}`, to list it and turn
  it back on (it is no longer online). A remembered id whose vendor/model no longer match is forgotten.

## Known limits

- Two monitors of the same model on one machine share a key and are not managed.
- Windows "on" restores the extend topology from Windows' database, which also turns on any other
  connected monitor someone had turned off by hand.
- Needs the monitor's automatic input selection. Without it, the person still switches the input by
  hand; Beamer then only keeps the other machine off it.
- The macOS call is private API; a future macOS may remove it (the backend then reports nothing and
  the feature stays out of the menus).

## Possible follow-ups

- Use the PC's DDC read as a "who really has it" signal (status line, detection of a hand switch).
- Find GN10's HDMI 2 code (try values from the PC while it shows the PC) to switch PC -> Mac faster than
  automatic input selection.
- A hotkey or a Settings page entry for the owner.
