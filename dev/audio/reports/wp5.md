# WP5 report: link integration on both apps (`audio/wp5`)

**Done.** §2.2 hunks: `core/link.py` (`_CAP_FOR` maps all 11 `audio_*` types to `audio`), `core/receiver.py` (`_Link.post` drops `audio_*` without the cap; `_dispatch` hands `audio_*` to the `audio` callback only when both ends list `audio`), `core/feature_flags.py` (`AUDIO = True`), `mac_app/bridge.py`, `win_app/sender.py`, `win_app/kvm_bridge_win.py` (caps + wiring), `mac_app/setup.py` (`audio_mac` first, `core.audio.*` includes), `WIRE.md` §11 "Audio" + cap row. New: `mac_app/audio_mac.py`, `win_app/audio_win.py` (AudioHost, `LinkPeers`, service construction), `core/tests/test_audio_link_gating.py` (23 tests).
- Caps: Mac adds `audio` (14) when `audio_mac.available(<settings dir>)`; Windows when `audio_win.available`; Linux never. No helper = no cap, no service, old behaviour.
- Service: made before any link can come up (so no first `audio_hello` is lost), started with the app, stopped first on quit. Mac: `controller.audio`; PC: `window.audio` (the AudioService for WP6/WP7; None without audio).
- `link_up`/`link_down` per peer from per-link events of both directions (`LinkPeers`); the caps given are those of the link used to post. A responder link replaced without an end report (step 10) is seen as down+up when it was the peer's only link, so both sides agree.
- AudioHost: "which link" rule (own link if up; a full own queue returns False, never reorders onto the other link); `outbox_depth` = own link `pending()` else responder outbox; `peer_host` = dialled IPv4 (not via tunnel) else the inbound link's source; loopback/names/IPv6 -> None.
- `audio_hello`/`audio_devices` (both ways) and per-link caps are logged at INFO under `beamer.audio`.

**Acceptance (measured).**
- `test_audio_link_gating`: 23 OK on Mac (5 repeated runs) and PC. Covers gating both directions, both-ends dispatch, LinkPeers, both hosts, and a real OutboundLink<->LinkResponder pair on loopback with two AudioServices (fake helper): direct peers, device lists both ways, offer/answer `flow_open`, and a 1.5.x-style peer without `audio` that receives no `audio_*`. PASS.
- Full `core/tests` (Mac): 1961 tests, only the 4 known `git archive` errors. PASS.
- `mac_app` suite (Mac, `discover -t . -s mac_app/tests`): 3749 tests, 1 failure + 8 errors: the 4 known + `test_app_nap`, `test_appearance`, `test_appearance_contrast` (import `kvm_bridge_app`), `test_glow_falloff` (no pytest), `test_click_focus` ring test; the same 5 fail identically on a `feat/audio` export. PASS (no regression).
- `win_app` suite (PC, win_app venv, from `win_app`): 4011 tests, 6 failures + 6 errors: the 7 known (dev/README) + 5 `core.tests.test_audio_helper` failures, which only fail with cwd `win_app` (`python -m core.audio.fake_helper` not importable there); from the repo root the same file passes 9/9. Audio core tests on the PC: 54 OK. PASS (no regression from WP5).
- Live (`dev/deploy.sh` both, under the lock, final 3545ae2): helper = WP1's stdio fake helper (`audio-dev.enabled`; WP2's `beamer-audio-null` exists on the Mac only, not in `C:\Users\Alex\audio-wp2`, and would bind UDP on a LAN address on the Mac). Both logs show `audio_hello` and `audio_devices` sent and received, helper `ok` both sides; the PC logs the Mac's link "14 capabilities including audio"; Mac `direct` = lebig. asusrog: "out link with asusrog up, 12 capabilities, no audio", nothing `audio_*` sent, no warning for it after the deploy. PASS. Both apps left running with links up and the fake helpers on (files below).

**Deviations.**
1. `receiver.py` beyond the named hunk: `audio_link_callback(peer, up, caps)`, `set_audio()` (the Mac builds its responder in WindowsInput, not mine), `host_of()`, `outbox_depth()`. `sender.py`: `audio_link_callback` in `on_link`, `LinkSet.host()`. Mac bridge: `responder` became a property whose setter wires the audio callbacks.
2. The Mac decides the cap in `links_from_store` from the settings folder (`identity.audio_state_dir`), not a module-level `CAPABILITIES` change.
3. `mac_app/setup.py` `excludes` `core.tests`, `mac_app`, `win_app`, `tools`: `core/audio/helper.py` reads `__file__`, so py2app's `detect_dunder_file` recipe copies all of `core` (tests included, +7.7 MB), and the tests pulled the repo's `mac_app` folder in recursively (build failed "File name too long").

**Needs from other WPs.**
- WP1: drop `__file__` from `core/audio/helper.py` (then remove deviation 3's excludes and `core` stops shipping whole); `test_audio_helper` should not depend on cwd. A Python helper argv needs `-I` on the Mac (py2app's PYTHONHOME is inherited); HelperProcess could clear PYTHONHOME/PYTHONPATH.
- WP6/WP7: the service is `controller.audio` (Mac) / `window.audio` (PC). WP6: add `core.audio.view` to setup.py `includes` when it lands.

**Live leftovers (dev only):** `~/Library/Application Support/Beamer/audio-dev.enabled` -> `/usr/bin/python3 -I ~/Developer/audio-wp5/fake_helper.py`; `%LOCALAPPDATA%\Beamer\audio-dev.enabled` -> Python312 + `C:\Users\Alex\audio-wp5\fake_helper.py`. Delete both (and restart via deploy) to return to "no audio".

**Commits:** dbc3173 integration + tests + WIRE.md · 3545ae2 py2app excludes · (this report).
