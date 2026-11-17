# Input

What a driver captures from its keyboard and pointer, what each input message means, and what a
driven machine injects for it: keys and their names, modifiers and the modifier styles, layouts and
the US place, typed text and dead keys, auto-repeat and lock keys, the pointer, buttons and scroll,
pointer speed and scroll speed, trackpad gestures, media and brightness keys, stays-here input and
keyboard stays here, when capture counts as stopped, and the capture word. These are F4 and F10 to
F14 of #11's inventory, which all ship. #11 drops the Mac's tap-based gesture path.

The rules come from the Python app at `archive/python` (`mac_app/bridge.py`,
`mac_app/input_injector_mac.py`, `mac_app/keyboard_layout.py`, `mac_app/gestures.py`,
`win_app/capture_win.py`, `win_app/input_injector.py`, `win_app/sender.py`, `core/keytable.py`,
`core/ignored.py`, `core/receiver.py` and their neighbours), mined through the tests in
`spec/ledger/input.txt`, and from ADR 0004 (the capture word). How input messages cross a link
(sequence numbers, acknowledgements, resends, merging pointer moves, splitting long text) is
`link`'s. Where input goes, and the keys a driver releases when its input moves, are `driving`'s
(`driving.keys.*`). Zones, the shortcut and jump keys are `crossing`'s.

A Mac is a machine running macOS, and a PC one running Windows. Linux and phone peers are out of
scope (#1). Key names are written in backticks, as the Python wire spelled them.

Every rule is `ci` unless it says otherwise. Translation code on a platform (key codes, layouts,
OS events to input messages and back) is `ci` on a hosted runner of its own OS (#15).

## Capture

> r[input.capture.word]
> A machine's capture decides whether each key, button, pointer move and scroll acts here or is
> swallowed as the OS reports it, from where the input is as of the latest move, this machine's
> stays-here input, keyboard stays here and crossing methods, and nothing else: never from the state
> of a link, and never waiting on the rest of Beamer. While the rest of Beamer is busy for 1 s, input
> at home goes on acting here at once, and input while away goes on being swallowed.
>
> - parity: Python's PC let a key or button act here when it saw, as it captured it, that the link was down, and the Mac waited on its own queue toward the link (ADR 0004: capture passes or swallows from the capture word alone, which the engine republishes at each move)

r[input.capture.home]
While a machine's own input is home, its capture swallows none of its own keys, buttons, pointer
moves or scrolls, and sends none of them to a peer. Only the crossing methods take anything then, and the physical release of a key or button the
driver released on a machine it left (`driving.keys.swallowed-after`).

r[input.capture.away]
While a driver's input is on a driven machine, the driver's capture swallows every key, button,
pointer move and scroll from its own keyboard and pointer, so none of them acts on the driver, and
sends each to the driven machine as an input message, in the order the OS reported them. Stays-here
input, the crossing methods, `input.capture.unnamed-button`, and a key or button pressed while the
input was home (`driving.keys.pressed-at-home`), are the exceptions.

r[input.capture.pointer-stays]
While a driver's input is on a driven machine, the driver's own pointer stays at the point where it
was when the input left, however the hand moves the mouse or trackpad.

r[input.capture.mark]
Every key, button, pointer move and scroll a machine injects carries Beamer's mark, 0xBEA3: a Mac
puts it in the posted input's event source user data (CGEvent field 42), and a PC in the extra
information of each input it injects.

> r[input.capture.mark-round-trip]
> Input a machine injects reaches that machine's own capture still carrying the mark, and so do the
> relative mouse counts a PC reports for a pointer move it injected.
>
> - tier: machine (only the real OS carries the mark from an injection back to the capture)

r[input.capture.marked-passes]
A machine's capture lets every input carrying the mark act on the machine untouched: it is never
swallowed, never sent to a peer, and never counted as the machine's own hand, so it neither pushes
at a zone nor shakes input home.

r[input.capture.foreign-injected]
On a PC, a key, button or wheel turn another program injected without Beamer's mark (a key
remapper, a mouse vendor's tool) is captured as the hand's: while input is on another machine it is
swallowed and sent.

r[input.capture.injected-moves-pass]
On a PC, a pointer move another program injected is never swallowed, with or without the mark, even
while input is on another machine. What moves the driven machine's pointer is the mouse's own counts
(`input.pointer.pc-travel`).

r[input.capture.unnamed-button]
A button with no button name, any but left, right, middle, back and forward, is never swallowed and
never sent: it acts on the machine it was pressed on, even while input is on another machine.

r[input.capture.error-passes]
When a machine's capture fails to read one key, button, pointer move or scroll, that input acts on
the machine and nothing is sent for it, and the capture goes on with the next.

r[input.capture.error-home]
When a Mac's capture fails to read one input, the Mac's input also comes home.

r[input.capture.pc-may-drive]
A PC runs its capture only while it may drive at least one peer: a PC that may drive none never
captures its keyboard and pointer.

## Capture stopped

> r[input.capture.stopped]
> A machine's capture counts as stopped, so that its input comes home by `driving.home.capture-lost`,
> in exactly the cases `input.capture.stopped-disabled`, `input.capture.stopped-unseen`,
> `input.capture.stopped-pc-unseen`, `input.capture.stopped-start`, `input.capture.stopped-ended`
> and `input.capture.stopped-pc-ended` name. A pause of any length with no input from the keyboard
> or pointer never counts as a stopped capture.
>
> - model: driving

r[input.capture.stopped-disabled]
A Mac's capture counts as stopped the moment macOS disables it, whether for a timeout or for user
input.

r[input.capture.stopped-unseen]
While a Mac's input is on another machine, its capture counts as stopped when input from the Mac's
own keyboard, trackpad or mouse that macOS registered more than 0.25 s after the last input the
capture saw goes on unseen by the capture: no sooner than 2 s and no later than 2.1 s after the
first such input, as long as the capture sees no input in between. Input macOS registered before
the Mac's input left never counts.

> r[input.capture.stopped-pc-unseen]
> While a PC's input is on another machine, its capture counts as stopped when input from the PC's
> own keyboard or mouse that Windows registered more than 0.25 s after the last input the capture
> saw goes on unseen by the capture: no sooner than 2 s and no later than 2.1 s after the first such
> input, as long as the capture sees no input in between. Input Windows registered before the PC's
> input left never counts.
>
> - parity: Python's PC never noticed that Windows had taken its capture out for answering too slowly, so input stayed on the peer with no key able to reach it (`driving.home.capture-lost`, #56, brings input home when capture stops, on the Mac and the PC alike, so a capture Windows took out no longer strands input on the peer)

r[input.capture.stopped-start]
A capture that has not started 2 s after a Mac asks for it, or 5 s after a PC asks for it, counts as
stopped, and so does one the OS refuses to start, as macOS does without the Accessibility and Input
Monitoring permissions.

r[input.capture.stopped-ended]
A Mac's capture that stops running without Beamer having stopped it counts as stopped.

> r[input.capture.stopped-pc-ended]
> A PC's capture that ends, or fails, without Beamer having stopped it counts as stopped.
>
> - parity: Python's PC took its capture out when it failed, and left input on the peer with no key able to reach it (`driving.home.capture-lost`, #56, brings input home when capture stops, on the Mac and the PC alike, so a capture Windows took out no longer strands input on the peer)

r[input.capture.restart]
When a Mac's capture counts as stopped under `input.capture.stopped-disabled` or
`input.capture.stopped-unseen`, the Mac turns the same capture back on at once, and captures again
from then on.

r[input.capture.retry]
While a Mac's capture is not running and both macOS permissions it needs, Accessibility and Input
Monitoring, are granted, the Mac tries to start it again, at most once every 5 s.

## Key names

r[input.key-name.message]
A key down or a key up input message names one key by one key name. Beside a character, it also
carries that key's US place, or none when the key has no US place.

r[input.key-name.names]
A key that is not a character is named by one of these key names: the modifiers `shift`,
`shift_r`, `ctrl`, `ctrl_r`, `alt`, `alt_r`, `cmd`, `cmd_r` and `caps_lock`; the input-method keys
`hangul` and `hanja`; `backspace`, `tab`, `enter`, `esc`, `space`, `delete`, `insert`, `home`,
`end`, `page_up`, `page_down`, `left`, `right`, `up` and `down`; `f1` to `f24`; `menu`,
`print_screen`, `pause`, `num_lock`, `scroll_lock`, `browser_back`, `browser_forward`,
`browser_refresh`, `browser_stop`, `browser_search`, `browser_favourites`, `browser_home`,
`mail`, `media_select`, `launch_app1` and `launch_app2`; the media keys `volume_mute`,
`volume_down`, `volume_up`, `media_next`, `media_prev`, `media_stop`, `media_play_pause`,
`brightness_up`, `brightness_down` and `mic_mute`; the system actions `search`, `spotlight` and
`dictation`; and the keypad names of `input.keypad.names`.

r[input.key-name.character]
A character key the driver captures sends, as its key name, the character the driver's keyboard
layout types for that key with Shift and Caps Lock applied: `A` with Shift down and `a` without,
and on a Russian layout the key in C's place sends `с`.

r[input.key-name.mac-chord-unshifted]
On a Mac, while Command, Control or Option (left or right) is held, a character key sends the
character its key types unshifted on the Mac's layout, not the character the chord composes:
Option-V sends `v`, not `√`, and a German layout's Command-Z sends `z`. Shift alone does not count
as such a modifier: Shift-V sends `V`.

r[input.key-name.mac-unprintable]
On a Mac, when the text a character key types is not exactly one printable character (Control-A
types U+0001), the key sends the character its key types unshifted on the Mac's layout instead.
A key for which the layout has no such character either sends nothing.

r[input.key-name.pc-chord-character]
On a PC, a character key sends the character the layout types for it with Shift and Caps Lock
applied, as they stand, and with Control, Alt and the Windows key not applied: Control-Shift-A
sends `A`, and Control-A sends `a`.

r[input.key-name.pc-no-character]
On a PC, a key that is not a named key and types no printable character is not sent to the peer,
and acts on the PC itself, even while input is on a peer.

r[input.key-name.release-same-name]
A key up carries the same key name and US place as the key down it releases, even when Shift, Caps
Lock or the keyboard layout changed while the key was held: Shift-A pressed, Shift let go, then A
let go sends a key down `A` and a key up `A`.

r[input.key-name.repeat-same-name]
While a key is held, its auto-repeat is further key downs with no key up between them, each
carrying the key name and US place of its first key down, even when the layout or the modifier
style changed since that key down.

> r[input.key-name.mac-named]
> The Mac's capture sends these Mac key codes as these key names:
>
> | Key code | Name | Key code | Name | Key code | Name |
> |---|---|---|---|---|---|
> | 0x24 | `enter` | 0x30 | `tab` | 0x31 | `space` |
> | 0x33 | `backspace` | 0x35 | `esc` | 0x72 | `insert` |
> | 0x73 | `home` | 0x74 | `page_up` | 0x75 | `delete` |
> | 0x77 | `end` | 0x79 | `page_down` | 0x7B | `left` |
> | 0x7C | `right` | 0x7D | `down` | 0x7E | `up` |
> | 0x7A | `f1` | 0x78 | `f2` | 0x63 | `f3` |
> | 0x76 | `f4` | 0x60 | `f5` | 0x61 | `f6` |
> | 0x62 | `f7` | 0x64 | `f8` | 0x65 | `f9` |
> | 0x6D | `f10` | 0x67 | `f11` | 0x6F | `f12` |
> | 0x69 | `f13` | 0x6B | `f14` | 0x71 | `f15` |
> | 0x6A | `f16` | 0x40 | `f17` | 0x4F | `f18` |
> | 0x50 | `f19` | 0x5A | `f20` | | |
>
> Its modifiers, keypad and input-method keys are named by `input.modifier.mac-names`,
> `input.keypad.mac-codes` and `input.key-name.input-method-names`.

> r[input.key-name.pc-named]
> The PC's capture sends these Windows virtual-key codes as these key names:
>
> | Code | Name | Code | Name | Code | Name |
> |---|---|---|---|---|---|
> | 0x08 | `backspace` | 0x09 | `tab` | 0x0D | `enter` |
> | 0x13 | `pause` | 0x14 | `caps_lock` | 0x1B | `esc` |
> | 0x20 | `space` | 0x21 | `page_up` | 0x22 | `page_down` |
> | 0x23 | `end` | 0x24 | `home` | 0x25 | `left` |
> | 0x26 | `up` | 0x27 | `right` | 0x28 | `down` |
> | 0x2C | `print_screen` | 0x2D | `insert` | 0x2E | `delete` |
> | 0x5D | `menu` | 0x90 | `num_lock` | 0x91 | `scroll_lock` |
> | 0xA6 | `browser_back` | 0xA7 | `browser_forward` | 0xA8 | `browser_refresh` |
> | 0xA9 | `browser_stop` | 0xAA | `browser_search` | 0xAB | `browser_favourites` |
> | 0xAC | `browser_home` | 0xAD | `volume_mute` | 0xAE | `volume_down` |
> | 0xAF | `volume_up` | 0xB0 | `media_next` | 0xB1 | `media_prev` |
> | 0xB2 | `media_stop` | 0xB3 | `media_play_pause` | 0xB4 | `mail` |
> | 0xB5 | `media_select` | 0xB6 | `launch_app1` | 0xB7 | `launch_app2` |
>
> and 0x70 to 0x83 as `f1` to `f20`. F21 to F24 (0x84 to 0x87) have no name on a PC. A keypad
> key's scan code wins over these (`input.keypad.pc-num-lock-off`), and Enter with the extended
> flag is `numpad_enter`. Its modifiers and input-method keys are named by
> `input.modifier.pc-names` and `input.key-name.input-method-names`.

r[input.key-name.input-method-names]
The Mac's key code 0x68 (Han/Yeong, and Kana on a JIS keyboard) is `hangul` and 0x66 (Hanja, and
Eisu on a JIS keyboard) is `hanja`. The PC's virtual-key codes 0x15 (VK_HANGUL, VK_KANA) and 0x19
(VK_HANJA, VK_KANJI) are `hangul` and `hanja`.

r[input.key-name.input-method-as-itself]
`hangul` and `hanja` go to every peer as themselves, from a Mac or a PC, to a Mac or a PC, in
every modifier style.

r[input.key-name.input-method-tap]
Each physical press of `hangul` or `hanja` sends a key down and then a key up of it, together, at
the press. Its physical release sends nothing.

r[input.key-name.mac-input-method-source]
On a Mac, only a Hangul or Hanja key event from the keyboard itself sends a tap: one an input
source posts with the same key code, and an auto-repeat of the key, send nothing.

r[input.key-name.mac-input-method-release]
On a Mac, the physical release of a Hangul or Hanja key acts on the Mac, whether or not its press
was sent.

r[input.key-name.pc-input-method-release]
On a PC, the physical release of a Hangul or Hanja key whose tap was sent to a peer does not act
on the PC. The release of a press that acted on the PC acts on the PC.

r[input.key-name.mac-fn]
The Mac's Fn key (key code 0x3F) is not sent to a peer.

r[input.key-name.link-forgets]
When a link to a peer closes, the driver forgets the names that keys held toward that peer were
sent under. A key down or key up toward that peer afterwards is named afresh by the driver's
saved style.

## Modifiers

r[input.modifier.mac-names]
The Mac's capture names its modifiers by key code: 0x38 `shift`, 0x3C `shift_r`, 0x3B `ctrl`,
0x3E `ctrl_r`, 0x3A `alt`, 0x3D `alt_r`, 0x37 `cmd`, 0x36 `cmd_r` and 0x39 `caps_lock`.

r[input.modifier.pc-names]
The PC's capture names its modifiers by virtual-key code: 0xA0 `shift`, 0xA1 `shift_r`, Control
0xA2 `ctrl`, right Control 0xA3 `ctrl_r`, the Windows key 0x5B `cmd`, the right Windows key 0x5C
`cmd_r`, Alt 0xA4 `alt` and right Alt or AltGr 0xA5 `alt_r`. The undifferentiated 0x10, 0x11 and
0x12 are `shift`, `ctrl` and `alt`.

r[input.modifier.mac-state-from-flags]
The Mac's capture reads whether a modifier went down or up from that event's own modifier flags,
each side from its own device flag. A modifier event whose flags show the key down sends a key
down even when the last event of that key was also a down, so a missed release never inverts the
key's later presses and releases.

r[input.modifier.family]
Every machine is in one of two families: Macs, and PCs. A peer whose platform the driver does not
know is in the PC family.

r[input.modifier.same-family]
Between two machines of the same family every key goes as itself, with its side kept, in every
modifier style and whatever a hand-written key map says: a Mac's Command reaches another Mac as
`cmd` and its right Control as `ctrl_r`, and a PC's Windows key reaches another PC as `cmd`.

r[input.modifier.only-six]
A modifier style renames only Control, Command or the Windows key, and Option or Alt, each side.
Every other key, Shift and Caps Lock included, goes to a peer of the other family as itself in
every style.

r[input.modifier.default-semantic]
A driver with no saved modifier style sends in the Semantic style.

r[input.modifier.style-own]
The names a driver sends follow its own saved modifier style. Its style is not sent to its peers,
and a peer's style plays no part in what the driver sends.

r[input.modifier.mac-semantic]
A Mac driving a PC in the Semantic style sends Control and right Control as `cmd`, Command and
right Command as `ctrl`, and Option and right Option as `alt`.

r[input.modifier.mac-positional]
A Mac driving a PC in the Positional style sends Control and right Control as `ctrl`, Command and
right Command as `cmd`, and Option and right Option as `alt`.

r[input.modifier.mac-mac-layout]
A Mac whose saved style is Mac keyboard layout drives a PC as in the Semantic style.

r[input.modifier.pc-semantic]
A PC driving a Mac in the Semantic style sends Control as `cmd`, right Control as `cmd_r`, the
Windows key as `ctrl`, the right Windows key as `ctrl_r`, Alt as `alt` and right Alt or AltGr as
`alt_r`.

r[input.modifier.pc-positional]
A PC driving a Mac in the Positional style sends Control as `ctrl`, right Control as `ctrl_r`, the
Windows key as `cmd`, the right Windows key as `cmd_r`, Alt as `alt` and right Alt or AltGr as
`alt_r`.

r[input.modifier.pc-mac-layout]
A PC driving a Mac in the Mac keyboard layout style sends Control as `ctrl`, right Control as
`ctrl_r`, the Windows key as `alt`, the right Windows key as `alt_r`, Alt as `cmd` and right Alt or
AltGr as `cmd_r`.

r[input.modifier.hand-map]
A Mac's hand-written key map names, for any keys it lists, the key name each goes to a PC under.
A key it lists goes to a PC under that name; a modifier it does not list goes as the Semantic
style sends it: with a map that lists only Command as `cmd`, Command goes to a PC as `cmd` and Control as `cmd`.

r[input.modifier.unknown-style]
A saved modifier style that is none of Semantic, Positional and Mac keyboard layout renames no
key: every key goes to a peer of the other family as itself.

r[input.modifier.release-press-style]
A key up toward a peer carries the name its key down was sent to that peer under, even when the
saved modifier style changed while the key was held: a PC's Control pressed in Positional
(`ctrl`) and released after a change to Semantic sends a key up `ctrl`.

r[input.modifier.shared-name-release]
When two physical keys held toward one peer were sent under the same key name (a Mac's left and
right Control both as `cmd`), releasing the first sends nothing, and releasing the last sends the
key up for that name.

r[input.modifier.pc-altgr-control]
The left Ctrl that Windows adds to right Alt on an AltGr layout (virtual-key 0xA2 with scan code
0x21D) is never sent to a peer, does not count as a held Ctrl, and its key up does not release a
left Ctrl the user is holding: that Ctrl's one key up goes when the user lets it up.

## Special keys

r[input.special-key.mac-f13-print-screen]
A Mac's F13 goes to a PC as `print_screen`, and to a Mac as `f13`.

r[input.special-key.function-keys-as-themselves]
Every function key other than a Mac's F13 goes to every peer under its own name, `f1` to `f24`.

r[input.special-key.search-to-spotlight]
A PC's browser search key goes to a Mac as `spotlight`, and to a PC as `browser_search`.

r[input.special-key.media-capability]
A key down or key up of `volume_mute`, `volume_down`, `volume_up`, `media_next`, `media_prev`,
`media_stop`, `media_play_pause`, `brightness_up` or `brightness_down` goes only to a peer that
reports it acts on media keys. Toward a peer that does not, it is not sent, and it does not act
on the driver either.

r[input.special-key.windows-key-held-back]
A PC driving a Mac does not send a key down for either Windows key when it is pressed. It sends
it, named by the modifier style, just before the next key down, button down or scroll it sends to
the Mac, unless that key down makes one of the shortcuts below.

r[input.special-key.windows-key-alone]
A Windows key pressed and released toward a Mac with no key down, button down or scroll in between
goes as a key down and a key up together, at its release.

r[input.special-key.windows-s-spotlight]
On a PC driving a Mac, S pressed while either Windows key is held, and no other modifier is held
toward the Mac, sends a key down and a key up of `spotlight` and nothing else: neither the Windows
key nor S is sent, and their auto-repeats and releases send nothing and do not act on the PC.

r[input.special-key.windows-h-dictation]
On a PC driving a Mac, H pressed while either Windows key is held, and no other modifier is held
toward the Mac, sends a key down and a key up of `dictation` and nothing else, as S does for
`spotlight`.

r[input.special-key.windows-n-dropped]
On a PC driving a Mac, N pressed while either Windows key is held, and no other modifier is held
toward the Mac, sends nothing at all, and neither key acts on the PC: the Mac never sees
Control-N.

r[input.special-key.windows-shortcut-other-modifier]
When Shift, Control, Alt or the other Windows key is held toward the Mac, the Windows key with S,
H or N goes as the keys it is: Windows key, Shift and S send the Windows key's name, `shift` and
`S`, and no `spotlight`.

r[input.special-key.windows-shortcut-after-click]
When the Windows key was already sent to the Mac (a click or scroll came first), the Windows key
with S, H or N first sends a key up for the Windows key's name, then the shortcut as above. The
Windows key's later release sends nothing.

r[input.special-key.switcher-mac-to-pc]
On a Mac driving a PC, Tab pressed while Command or right Command is held first moves the held
Command onto `alt`: a key up for the name it was sent under, unless another held key went under
that name too, then a key down of `alt`, unless another held key holds `alt` already. Tab follows,
and from then on that Command key is held toward the PC as `alt`, so its release sends a key up of
`alt`.

r[input.special-key.switcher-pc-to-mac]
On a PC driving a Mac, Tab pressed while Alt or right Alt is held first moves the held Alt onto
`cmd` the same way: a key up of its sent name, then a key down of `cmd`, then Tab, and its release
sends a key up of `cmd`.

r[input.special-key.switcher-held]
Once a Command or Alt key has moved onto the window switcher's modifier, further Tabs and
Shift-Tabs while it is held send only Tab and Shift: the modifier stays down on the peer under its
new name until the physical key is released.

r[input.special-key.switcher-only-across]
Tab moves no modifier between two machines of the same family, nor without the source modifier
held (Command from a Mac, Alt from a PC). A key other than Tab never moves one: Command-C goes
from a Mac to a PC as the style names Command, then `c`.

r[input.special-key.switcher-two-alts]
With both Alt keys held when Tab goes down toward a Mac, Command stays down until the second Alt
comes up, and the PC sends Command up once, after everything else.

## Keypad

r[input.keypad.names]
Keypad keys send keypad names, never characters and never main keyboard names: `numpad_0` to
`numpad_9`, `numpad_decimal`, `numpad_add`, `numpad_subtract`, `numpad_multiply`,
`numpad_divide`, `numpad_enter`, `numpad_equal` and `numpad_separator`.

r[input.keypad.mac-codes]
The Mac's capture sends key codes 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B and 0x5C
as `numpad_0` to `numpad_9`, and 0x41 `numpad_decimal`, 0x43 `numpad_multiply`, 0x45 `numpad_add`,
0x4B `numpad_divide`, 0x4C `numpad_enter`, 0x4E `numpad_subtract`, 0x51 `numpad_equal` and 0x5F
`numpad_separator`.

r[input.keypad.pc-codes]
The PC's capture sends virtual-key codes 0x60 to 0x69 as `numpad_0` to `numpad_9`, and 0x6A
`numpad_multiply`, 0x6B `numpad_add`, 0x6C `numpad_separator`, 0x6D `numpad_subtract`, 0x6E
`numpad_decimal`, 0x6F `numpad_divide` and 0x92 `numpad_equal`.

r[input.keypad.pc-num-lock-off]
On a PC, a keypad key keeps its keypad name with Num Lock off, by its scan code without the
extended flag: 71 `numpad_7`, 72 `numpad_8`, 73 `numpad_9`, 74 `numpad_subtract`, 75 `numpad_4`,
76 `numpad_5`, 77 `numpad_6`, 78 `numpad_add`, 79 `numpad_1`, 80 `numpad_2`, 81 `numpad_3`, 82
`numpad_0`, 83 `numpad_decimal` and 55 `numpad_multiply`. Keypad 7 with Num Lock off is
`numpad_7`, not `home`.

r[input.keypad.enter-distinct]
The main Return key is `enter` and the keypad's Enter is `numpad_enter`, on a Mac by key code and
on a PC by the extended flag on virtual-key code 0x0D.

r[input.keypad.pc-num-lock-while-held]
On a PC, a keypad key held while Num Lock changes is released under the keypad name its press
went under.

r[input.keypad.as-themselves]
Keypad names go to every peer as themselves, from a Mac or a PC, to a Mac or a PC, in every
modifier style.

## Layouts

r[input.layout.us-place]
A key down or key up that names a character carries the key's US place: the unshifted character
the same physical key types on a US layout, whatever layout the driver is on. A Mac or PC on
Russian sends its C key as `с` with the US place `c`. A named key, and a key with no US place,
carry none.

r[input.layout.mac-us-place]
The Mac takes a key's US place from its key code. The 47 keys at Mac key codes 0x00–0x09,
0x0B–0x23, 0x25–0x2F and 0x32 have one, as a US ANSI keyboard types them unshifted (0x00 `a`,
0x06 `z`, 0x08 `c`, 0x10 `y`, 0x12 `1`, 0x18 `=`, 0x21 `[`, 0x1E `]`, 0x32 `` ` ``). Every other
key, the ISO key at 0x0A among them, has none.

r[input.layout.pc-us-place]
The PC takes a key's US place from its set-1 scan code: 0x02–0x0D are `1234567890-=`, 0x10–0x1B
`qwertyuiop[]`, 0x1E–0x29 `` asdfghjkl;'` ``, and 0x2B–0x35 `\zxcvbnm,./`, 47 places in all. A key
the PC reports with the extended flag, or with any other scan code, has none.

r[input.layout.pc-capture-foreground]
The PC's capture reads a key's character from the keyboard layout of the app in front, so that a
switch of input language in that app changes the names its keys are sent with.

r[input.layout.mac-unshifted]
The Mac takes each character key to type what its current layout types for that key with no
modifier held, read from the layout itself: on German, Mac key codes 0x06, 0x10 and 0x21 type
`y`, `z` and `ü`; on French, 0x00, 0x0C and 0x0D type `q`, `a` and `z`; on Russian, 0x00, 0x08 and
0x2F type `ф`, `с` and `ю`. The Mac's capture and its injection use the same reading.

r[input.layout.mac-us-until-read]
Until the Mac has read its current layout, it takes every character key to type the character a
US layout types there: a received `z` is posted on Mac key code 0x06.

r[input.layout.mac-read]
The Mac reads its current layout when Beamer starts and again each time the selected input source
changes.

r[input.layout.mac-read-fails]
When the Mac cannot read its layout, or the selected input source has no key layout of its own,
the Mac keeps the reading it had and logs that keys follow US positions.

r[input.layout.mac-dead-key]
A key that is a dead key on the Mac's layout types no character there. A received character that
a US layout types on that key is typed as text, never posted on it: on German, whose Mac key code
0x18 is the acute accent, a received `=` is typed as text.

r[input.layout.mac-left-out]
A key the Mac's layout gives no single printable, non-space character keeps the character a US
layout types there, both for naming the key and for placing that character, unless the layout
types that character on another key.

r[input.layout.mac-retyped]
A character a US layout types on a key that the Mac's layout types something else on, and that
the Mac's layout types on no key, has no key on the Mac: on German, a received `[` is typed as
text, not posted on Mac key code 0x21, which types `ü`.

r[input.layout.mac-first-key]
When the Mac's layout types one character unshifted on two keys, a received character is posted
on the one with the lower Mac key code.

r[input.layout.typed-first]
A driven machine presses a received character on the key its own layout types it with, and the
US place plays no part, even when that key is elsewhere: a `z` on a German Mac goes on Mac key
code 0x10 whatever its US place, and a `с` on a Mac on Russian on Mac key code 0x08.

r[input.layout.can-type-mac]
A Mac's layout can type a received character when one of its keys types the character's lower
case with no modifier held. A character typed only with Shift or Option, such as `!` on US, is
one the Mac's layout cannot type.

r[input.layout.can-type-pc]
A PC's layout can type a received character when one of its keys types it, with or without Shift,
Control and Alt.

r[input.layout.place-chord]
When the driven machine's layout cannot type a received character and `cmd`, `ctrl` or `alt`
(left or right) is held there, it presses the key at the character's US place on its own layout.
`cmd` with `с` and US place `c`, as a PC on Russian sends Control-C, presses the C key (Mac key
code 0x08) of a Mac on English under Command; `cmd` with `é` and US place `2` presses the 2 key.

r[input.layout.place-other-script]
With no `cmd`, `ctrl` or `alt` held, a letter the driven machine's layout cannot type is pressed
on the key at its US place when that key types a letter of another script on the driven
machine's layout: a Cyrillic `с` with US place `c` types the C key on an English layout, and a
Latin `c` with US place `c` types `с` on a Russian one, as that machine's own keyboard would.

r[input.layout.place-same-script]
A character the driven machine's layout cannot type, received with no `cmd`, `ctrl` or `alt` held,
is typed as text when it is not a letter, or when the key at its US place types a letter of the
same script, or types no letter, on the driven machine's layout: a French `é` with US place `2`
types `é` as text on an English Mac or PC, not `2`.

r[input.layout.place-caps-capital]
An upper-case letter that `input.layout.place-other-script` would press, received with no
modifier held at all (Caps Lock on the driver), is typed as text instead: the upper case of the
letter the key at its US place types on the driven machine's layout. `С` with US place `c` types
`C` on an English Mac or PC, and its key up releases that `C`.

r[input.layout.place-none]
A character with no US place that the driven machine's layout cannot type is typed as text, also
while `cmd`, `ctrl` or `alt` is held.

r[input.layout.mac-korean]
The Mac reads the 2-Set Hangul keyboard layout as a key table of 48 keys in which Mac key code
0x00 types `ㅁ`, and the 2-Set Korean input method as having no key table.

## Key injection

r[input.key-injection.held-modifiers]
A driven machine takes a modifier name (`shift`, `shift_r`, `ctrl`, `ctrl_r`, `alt`, `alt_r`,
`cmd`, `cmd_r`) as held from the key down naming it to the key up naming it, and only the
modifiers so held make a character a chord.

r[input.key-injection.no-own-modifier]
A driven machine presses no Shift, Control, Alt, Option or Command of its own around a received
character: the only modifiers held while it posts the character are those the driver sent.

r[input.key-injection.release-same-key]
A driven machine posts a character's key up on the key its key down was posted on, even when the
layout changed in between, a modifier was released first, or the key up carries a different US
place or none.

r[input.key-injection.repeat-same-key]
A repeated key down for a character that is down on a driven machine is posted on the key its
first key down was, even when the layout changed in between.

r[input.key-injection.release-text]
The key up for a character whose key down a driven machine typed as text is typed as text too: the
release of the same character.

r[input.key-injection.mac-text-form]
A Mac types a character as text by posting a key event at Mac key code 0 that carries the
character as its string, down and then up.

r[input.key-injection.mac-character]
A Mac posts a received character on the key its current layout types the character's lower case
on, unshifted, when the character is lower case or not a letter, or when any modifier is held, with
the flags of the modifiers held: `c` on Mac key code 0x08 on US, `z` on 0x10 and `ü` on 0x21 on
German, and `a` under `cmd` on 0x0C on French.

r[input.key-injection.mac-capital-plain]
A Mac types an upper-case letter received with no modifier held as text, even when its layout has
a key for the lower case: `C`, `Ф` and `Ω` are typed as text there.

r[input.key-injection.mac-capital-chord]
A Mac posts an upper-case letter received while `cmd`, `ctrl` or `alt` is held, with or without
`shift`, on the key of its lower case, and its key up on the same key after those modifiers are
released.

r[input.key-injection.mac-text]
A Mac types as text a received character its layout cannot type and no rule of its US place
presses: `é` on US, a shifted symbol such as `!`, and `[` on German.

r[input.key-injection.mac-modifiers]
A Mac presses `cmd` on Mac key code 0x37 (Command), `cmd_r` on 0x36, `ctrl` on 0x3B (Control),
`ctrl_r` on 0x3E, `alt` on 0x3A (Option), `alt_r` on 0x3D, `shift` on 0x38, `shift_r` on 0x3C and
`caps_lock` on 0x39.

r[input.key-injection.mac-modifier-flags]
Every key event a driven Mac posts, other than one typed as text, and every pointer move, button
and scroll event it posts, carries the flag of each modifier held there:
Command for `cmd` and `cmd_r`, Control for `ctrl` and `ctrl_r`, Option for `alt` and `alt_r`,
Shift for `shift` and `shift_r`, and Caps Lock for `caps_lock`. A modifier's own key down carries
its flag, and its key up does not.

r[input.key-injection.mac-named]
A Mac presses each named key on its Mac key code: `enter` 0x24, `tab` 0x30, `space` 0x31,
`backspace` 0x33, `esc` 0x35, `delete` 0x75 (forward delete), `insert` 0x72, `home` 0x73, `end`
0x77, `page_up` 0x74, `page_down` 0x79, `left` 0x7B, `right` 0x7C, `down` 0x7D, `up` 0x7E; `f1`
0x7A, `f2` 0x78, `f3` 0x63, `f4` 0x76, `f5` 0x60, `f6` 0x61, `f7` 0x62, `f8` 0x64, `f9` 0x65, `f10`
0x6D, `f11` 0x67, `f12` 0x6F, `f13` 0x69, `f14` 0x6B, `f15` 0x71, `f16` 0x6A, `f17` 0x40, `f18`
0x4F, `f19` 0x50, `f20` 0x5A; `numpad_0` to `numpad_7` 0x52 to 0x59, `numpad_8` 0x5B, `numpad_9`
0x5C, `numpad_decimal` 0x41, `numpad_multiply` 0x43, `numpad_add` 0x45, `numpad_divide` 0x4B,
`numpad_enter` 0x4C, `numpad_subtract` 0x4E, `numpad_equal` 0x51 and `numpad_separator` 0x5F.

r[input.key-injection.mac-input-method]
A Mac presses `hangul` on Mac key code 0x68 (Kana) and `hanja` on 0x66 (Eisu), with no modifier
flag of their own; neither counts as a held modifier.

r[input.key-injection.mac-fn-flag]
A Mac's key down and key up for `left`, `right`, `up`, `down`, `f1` to `f20`, `home`, `end`,
`page_up`, `page_down` and `delete`, and for the PC keys that land on `f13` to `f15`, carry the
Fn flag, as the Mac's own keyboard sends them.

r[input.key-injection.mac-numpad-flag]
A Mac's key down and key up for `left`, `right`, `up`, `down` and every keypad name carry the
numeric-pad flag.

r[input.key-injection.mac-flags-join]
The Fn and numeric-pad flags join the flags of the modifiers held: `left` with `shift` held
carries Shift, Fn and numeric-pad.

r[input.key-injection.mac-plain-flags]
A Mac's key events for every other key, such as `a`, `enter`, `space`, `backspace`, `tab`, `esc`,
`hangul` and `num_lock`, carry neither the Fn nor the numeric-pad flag.

r[input.key-injection.mac-pc-keys]
A Mac presses `print_screen`, `scroll_lock` and `pause` on Mac key codes 0x69, 0x6B and 0x71, the
F13, F14 and F15 an Apple extended keyboard puts in their places.

r[input.key-injection.mac-num-lock]
A Mac presses `num_lock` on the keypad's Clear key, Mac key code 0x47.

r[input.key-injection.mac-browser-keys]
On a key down of `browser_back` a Mac posts a key down and a key up of Mac key code 0x21, and on
one of `browser_forward` of 0x1E: Command-[ and Command-], each event carrying Command and the
flags of the modifiers held. A key up of either posts nothing.

r[input.key-injection.mac-browser-us-key]
A Mac posts `browser_back` and `browser_forward` on Mac key codes 0x21 and 0x1E whatever its
layout, also on a layout with no unshifted bracket.

r[input.key-injection.mac-search]
On a key down of `search` or `spotlight` a Mac posts Command down (Mac key code 0x37), Space down
(0x31), Space up and Command up, the first three carrying Command and the flags of the modifiers
held, the last only the flags of the modifiers held. A key up of either posts nothing.

r[input.key-injection.mac-dictation]
A Mac presses `dictation` on `f5`, Mac key code 0x60: its key down and key up are F5's.

r[input.key-injection.mac-dropped]
Of the names a PC sends, a Mac presses nothing for `menu`, `media_stop`, `media_select`,
`launch_app1`, `launch_app2`, `mail`, `browser_stop`, `browser_home`, `browser_refresh`,
`browser_search` and `browser_favourites`, and places every other one.

r[input.key-injection.mac-unknown]
A Mac drops a key name it cannot press and posts nothing for it, logging one warning naming it the
first time it arrives and none after.

r[input.key-injection.pc-modifiers]
A PC presses `shift` on virtual key 0xA0, `shift_r` 0xA1, `ctrl` 0xA2, `ctrl_r` 0xA3, `alt`
0xA4, `alt_r` 0xA5, `cmd` 0x5B (the Windows key), `cmd_r` 0x5C and `caps_lock` 0x14.

r[input.key-injection.pc-named]
A PC presses each named key on its virtual key: `enter` 0x0D, `esc` 0x1B, `tab` 0x09, `space`
0x20, `backspace` 0x08, `delete` 0x2E, `insert` 0x2D, `home` 0x24, `end` 0x23, `page_up` 0x21,
`page_down` 0x22, `left` 0x25, `up` 0x26, `right` 0x27, `down` 0x28, `menu` 0x5D, `num_lock` 0x90,
`scroll_lock` 0x91, `print_screen` 0x2C, `pause` 0x13; `f1` to `f12` 0x70 to 0x7B and `f13` to
`f20` 0x7C to 0x83; `numpad_0` to `numpad_9` 0x60 to 0x69, `numpad_multiply` 0x6A, `numpad_add`
0x6B, `numpad_separator` 0x6C, `numpad_subtract` 0x6D, `numpad_decimal` 0x6E, `numpad_divide`
0x6F, `numpad_enter` 0x0D and `numpad_equal` 0x92.

r[input.key-injection.pc-media-keys]
A PC presses `browser_back` on virtual key 0xA6, `browser_forward` 0xA7, `browser_refresh` 0xA8,
`browser_stop` 0xA9, `browser_search` 0xAA, `browser_favourites` 0xAB, `browser_home` 0xAC,
`volume_mute` 0xAD, `volume_down` 0xAE, `volume_up` 0xAF, `media_next` 0xB0, `media_prev` 0xB1,
`media_stop` 0xB2, `media_play_pause` 0xB3, `mail` 0xB4, `media_select` 0xB5, `launch_app1` 0xB6
and `launch_app2` 0xB7.

r[input.key-injection.pc-input-method]
A PC presses `hangul` on virtual key 0x15 and `hanja` on 0x19, without the extended flag; neither
counts as a held modifier.

r[input.key-injection.pc-scan]
A PC posts each named key with the scan code its keyboard layout gives that key's virtual key.

r[input.key-injection.pc-extended]
A PC posts with the extended-key flag, down and up, exactly `numpad_divide`, `numpad_enter`,
`up`, `down`, `left`, `right`, `insert`, `delete`, `home`, `end`, `page_up`, `page_down`,
`ctrl_r`, `alt_r`, `cmd`, `cmd_r`, `num_lock`, `print_screen`, `media_play_pause`, `media_next`,
`media_prev`, `media_stop`, `volume_mute`, `volume_down`, `volume_up`, every `browser_` name,
`mail`, `media_select`, `launch_app1` and `launch_app2`. Every other name, `ctrl` among them, goes
without it.

r[input.key-injection.pc-named-held]
A PC repeats and releases a held named key other than a modifier with the virtual key, scan code
and extended flag of its first key down.

r[input.key-injection.pc-system-actions]
On a key down of `search` a PC presses and releases Windows-S, on one of `dictation` Windows-H,
and on one of `mic_mute` Windows-Alt-K, all in one batch; a key up of any of them posts nothing.

r[input.key-injection.pc-unknown]
A PC drops a key name it cannot press, posts nothing for it, and logs a warning naming it each time
it arrives.

r[input.key-injection.pc-character]
A PC presses a received character on the virtual key its layout types it with, and the scan code
the layout gives that key, when the layout types it with no modifier, or with Shift alone while
`shift` or `shift_r` is held: `k` on its K key, and `K` with `shift_r` held on the same K key.

r[input.key-injection.pc-shift-unheld]
A PC types as text, and logs nothing, a character its layout types only with Shift, received with
no `shift`, `shift_r`, `ctrl`, `alt` or `cmd` held (Caps Lock on the driver): `K`, and `Ф` on a
Russian layout.

r[input.key-injection.pc-altgr]
A PC types as text a character its layout types only with Control and Alt (AltGr), received with
no `ctrl`, `alt` or `cmd` held, and logs a warning naming the character the first time it arrives
and none after.

r[input.key-injection.pc-chord-key]
While `ctrl`, `alt` or `cmd` (left or right) is held, a PC presses a character its layout can type
on the virtual key that types it, whatever Shift, Control or Alt state the layout types it with:
`c` under `ctrl` on the C key, `e` under `cmd` on the E key, `C` under `ctrl` with or without
`shift` on the C key, and `@`, which a German layout types with AltGr, under `ctrl` on the key
that types it.

r[input.key-injection.pc-unresolvable]
A PC types as text a received character its layout cannot type and no rule of its US place
presses, and logs a warning naming the character the first time it arrives and none after.

r[input.key-injection.pc-backspace]
A PC driven machine injects `backspace` as the Backspace key (Windows virtual-key 0x08) under any
modifiers held, Ctrl and Alt together included.

## Typed text

r[input.text.characters]
A driven machine types a run of typed text as characters, not as keys: what appears is the text as
it stands, whatever the driven machine's keyboard layout and whatever modifiers the driver holds
on it.

r[input.text.return-tab]
In a run of typed text, each `\n` is typed as a key down and key up of `enter`, and each `\t` as a
key down and key up of `tab`, in their places between the characters around them.

r[input.text.control-characters]
In a run of typed text, every character from U+0000 to U+001F other than `\n` and `\t`, and every
character from U+007F to U+009F, is not typed: `a\r\nb` types `a`, then Return, then `b`.

r[input.text.empty]
A run of typed text with nothing left to type injects nothing.

r[input.text.pc-units]
A PC driven machine types each UTF-16 code unit of the text as a Unicode key down and key up, in
order, so a character outside the Basic Multilingual Plane goes as its surrogate pair: 😀 is
U+D83D down and up, then U+DE00 down and up.

r[input.text.pc-one-injection]
A PC driven machine injects all the characters of one run of text between a Return or Tab and the
next in one injection, and every one of them carries Beamer's mark, 0xBEA3.

r[input.text.mac-chunks]
A Mac driven machine posts text as key downs and key ups that each carry at most 16 UTF-16 code
units, one key down and one key up per piece, in order. A character outside the Basic Multilingual
Plane is never cut between two pieces.

r[input.text.mac-no-flags]
Every key down and key up a Mac driven machine posts for text carries no modifier flags, so a
modifier held for the driver never turns the text into a shortcut.

r[input.text.pc-brackets]
When a driving PC sends a run of typed text to a peer on which it holds modifiers, it sends a key
up for each of those modifiers, in the order they were pressed, then the text, then a key down for
each of them again in the same order. The key ups and downs name the modifiers as they were sent.

r[input.text.altgr]
On a driving PC, while right Alt is held and no physical Ctrl is, a key whose character right Alt
changes goes to the peer, on its press, as typed text of the character it types with right Alt,
and its key up sends nothing.

r[input.text.altgr-repeat]
While a key whose press went as typed text with right Alt is held on a driving PC, each auto-repeat
sends that character again as typed text.

r[input.text.ctrl-altgr]
On a driving PC, a key pressed while a physical Ctrl and right Alt are both held goes as a key down
and key up of the key's own character, never as typed text.

r[input.text.shortcut-while-held]
While a key whose press went as typed text is held (a composed character on a driving Mac, a right
Alt character on a driving PC), a press of Command or Control on the Mac, or of Ctrl on the PC,
turns the key's next auto-repeat into a key down of the key's own character with its US place,
and its key up then goes as the matching key up.

r[input.text.altgr-home]
On a driving PC, a key that typed a right Alt character at home keeps acting on the PC until its
key up: its auto-repeats and its key up after input has gone to a peer act on the PC, and nothing
of it is sent.

r[input.text.altgr-fresh-press]
Once a key that went as typed text with right Alt is released on a driving PC, its next press is a
new press: the stays-here list as it stands then decides whether it acts on the PC or goes to the
peer.

## Dead keys

r[input.dead-key.pc-none-waiting]
On a PC, a dead key captured for a peer leaves no dead key waiting on the PC itself: once input is
home, the next character typed on the PC is not composed with it.

r[input.dead-key.swallowed]
While a driver's input is on a peer, a dead key's press, its auto-repeats and its key up act on
neither machine and send nothing; its accent waits on the driver for the next key.

r[input.dead-key.pc-link-not-live]
A dead key pressed on a driving PC whose link to the driven machine is not live acts on the PC,
and input comes home.

r[input.dead-key.mac-composed-text]
On a driving Mac, the character key pressed after a waiting accent goes to the peer, on its press,
as typed text of the composition, and no key down or key up of it is sent.

r[input.dead-key.mac-composes-from-layout]
A driving Mac composes the waiting accent with the character the key types on the Mac's keyboard
layout under the modifiers held, whatever character macOS reports for that key press (the bare
letter, or one it composed already).

r[input.dead-key.mac-layout-table]
On a driving Mac, the composition of an accent and a character is the one the Mac's keyboard
layout defines for that dead key and that character. When the layout defines none for that
character, the accent followed by the character is sent instead: `'` then `s` is `'s`.

r[input.dead-key.mac-chained]
A key that, pressed after a dead key, starts a further dead state in the Mac's keyboard layout
has no composition with it: that pair goes as the accent followed by the key's character.

r[input.dead-key.unicode-compose]
Where a driver has no layout table of compositions for a dead key (always on a driving PC), the
composition is the character followed by the combining form of the accent, normalised to NFC,
when that is one character: the combining form is the character Unicode names `COMBINING` and
then the accent's own name, less any `MODIFIER LETTER` prefix (`´` then `e` is `é`, `¨` then `u`
is `ü`).

r[input.dead-key.cannot-compose]
When an accent and the character after it make no single character, the driver sends the accent
followed by the character as one run of typed text: `´` then `x` is `´x`.

r[input.dead-key.text-held]
While the key whose press went as typed text after an accent is held (on a driving Mac, any such
key; on a driving PC, one whose accent and character made no single character), each auto-repeat
sends the key's own character as typed text, and its key up sends nothing.

r[input.dead-key.pc-composed-key]
On a driving PC, a character key whose waiting accent composes with it into one character goes as
a key down and key up of the composed character, carrying the key's US place: `^` then `e` is a
key down and key up of `ê` with US place `e`.

r[input.dead-key.pc-altgr-text]
On a driving PC, a composition made while right Alt is held and no physical Ctrl is goes as typed
text, and its key up sends nothing.

r[input.dead-key.space]
Space pressed after a dead key sends the accent alone. A driving Mac sends it as typed text; a PC
driver as a key down and key up of the accent character, or as typed text while right Alt is held
and no physical Ctrl is.

r[input.dead-key.mac-same-twice]
On a driving Mac, the same dead key pressed twice sends its accent once, as typed text, and leaves
no accent waiting.

r[input.dead-key.mac-other-dead]
On a driving Mac, a dead key pressed while another dead key's accent waits sends the waiting accent
as typed text, and the new one's accent waits in its place.

r[input.dead-key.cancel-shortcut]
A key pressed while Command or Control is held on a driving Mac, or while Ctrl is held on a PC
driver, discards the waiting accent and goes as the ordinary key down and key up it would be
without one.

r[input.dead-key.pc-ctrl-dead-key]
On a driving PC, a dead key pressed while Ctrl is held goes as a key down and key up of its accent
character, and no accent waits.

r[input.dead-key.cancel-non-character]
A key that types no printable character, other than a modifier and Space, discards the waiting
accent and goes as it would without one. A modifier pressed or released leaves the accent
waiting, except Ctrl on a driving PC, whose key down discards it.

r[input.dead-key.mac-home-only]
A driving Mac composes only while its input is on a peer, the keyboard does not stay here and the
key is not stays-here input. A key pressed otherwise discards the accent waiting for the peer, and
a dead key pressed otherwise is macOS's own dead key, acting on the Mac.

r[input.dead-key.mac-layout-change]
A change of the Mac's keyboard layout discards the waiting accent: the next character key goes as
an ordinary key down.

r[input.dead-key.mac-shortcut-key]
Pressing the shortcut on a driving Mac discards the waiting accent.

> r[input.dead-key.mac-home-drops]
> When a driving Mac's input comes home, the accent waiting for the peer is discarded.
>
> - model: driving

r[input.dead-key.release-swallowed]
The key up of a dead key whose press sent nothing also sends nothing and acts on neither machine,
even when the keyboard layout changed in between so that the key is no longer dead or types
nothing.

r[input.dead-key.pc-home-press]
On a driving PC, a dead key pressed at home keeps acting on the PC until its key up: its
auto-repeats and key up after input has gone to a peer act on the PC, and nothing of it is sent,
even when the keyboard layout changed in between.

r[input.dead-key.fresh-press]
Once a dead key whose press sent nothing is released on a driving PC, its next press is a new
press: the stays-here list as it stands then decides whether it acts on the PC or goes to the
peer.

r[input.dead-key.mac-installed-layouts]
With the Mac's Spanish ISO, Spanish or German keyboard layout, the acute dead key followed by `e`
sends `é` as typed text.

r[input.dead-key.mac-us-international]
With the Mac's U.S. International – PC keyboard layout, the `'` dead key followed by `a` sends `á`,
by `c` sends `ç` and by `s` sends `'s`, and the shifted `"` dead key followed by `t` sends `"t`,
each as typed text.

## Auto-repeat

r[input.repeat.capture]
While a key whose press went to a peer as a key down is held, each auto-repeat the driver's OS
reports goes to the peer as another key down of the same key name, with no key up between them;
the key's one key up follows when it is released.

r[input.repeat.mac-flag]
A Mac driven machine injects a key down of a key it already holds down as an auto-repeat (CGEvent
field 8, keyboard autorepeat, set to 1), and every first key down and every key up as not one.
This holds for characters, characters typed as text, and named keys alike.

r[input.repeat.mac-per-key]
On a Mac driven machine, each key is held on its own: a key down of another key between two key
downs of the first does not make the first's next key down a first press.

r[input.repeat.mac-modifiers]
A Mac driven machine never injects a modifier as an auto-repeat, and the auto-repeats of a key
carry the flags of the modifiers held, as its first key down did.

r[input.repeat.fresh-after-release]
After a driven machine injects a key's key up, by the driver's key up or by releasing the driver's
held keys, the next key down of that key is injected as a first press.

## Caps Lock

r[input.caps-lock.mac-tap]
While a driving Mac's input is on a peer, each Caps Lock change macOS reports (Mac key code 0x39)
sends the peer a key down and a key up of `caps_lock` together, and acts on neither machine. This
holds whatever modifier flags the change carries, with or without the Caps Lock flag, as when
Caps Lock changes the keyboard layout.

r[input.caps-lock.mac-home]
While a driving Mac's input is at home, a Caps Lock change acts on the Mac and sends nothing,
whatever modifier flags it carries and after any change of keyboard layout.

r[input.caps-lock.mac-key-down-up]
A Caps Lock that macOS reports as a key down and a key up instead of a modifier change goes to the
peer as a key down and a key up of `caps_lock`, each when it comes, while a driving Mac's input is on
a peer, and acts on the Mac while input is at home.

## Lock keys

r[input.lock-key.pc-sent]
A Caps Lock, Num Lock or Scroll Lock press on a driving PC whose input is on a peer goes to the
peer, and the PC's own lock state does not change.

r[input.lock-key.pc-peer-once]
A PC driven machine's Caps Lock or Num Lock changes once per press it injects, however many key
downs the driver's auto-repeat sends before the key up.

r[input.lock-key.pc-caps-own]
A driving PC gives letters the case of its own Caps Lock. A Caps Lock press that acts on the PC
changes that case once, however many auto-repeats follow before its key up.

r[input.lock-key.pc-caps-sent]
A Caps Lock press that a driving PC sends to a peer does not change the case of the letters the PC
sends after it.

r[input.lock-key.pc-caps-injected]
A Caps Lock press that Beamer injects on a PC changes the case that PC gives letters when it
drives, once per press however many key downs repeat it.

r[input.lock-key.pc-caps-start]
When a PC's capture starts, it reads Caps Lock from Windows, so its letters have the case Caps
Lock gives them from the first key.

## Held keys

> r[input.held-key.driven-release]
> When a driver stops driving a machine (it lets go, its link closes, its user sends the input home, or it is taken
> over), the driven machine injects a key up for every key and a button up for every button it
> injected for that driver and has not released, the last pressed first, before it injects
> anything else.
>
> - model: driving

> r[input.held-key.driven-release-all]
> After releasing a driver's held keys and buttons, the driven machine also releases every key,
> modifier and button its injection still holds down, so none stays down: a Ctrl, a Shift, an `a`
> and the left button held for the driver are three key ups and one button up.
>
> - model: driving

r[input.held-key.ordinary-stays]
A key whose press went to the peer as an ordinary key down stays one until its key up: a modifier
pressed while it is held (Option on a Mac, right Alt on a PC) that would make it a dead key or a
typed-text key does not change its auto-repeats, which go as key downs of the same name, or its
key up.

r[input.held-key.pc-physical-key]
A driving PC matches a key's key up to its key down by the physical key: its scan code and
extended flag, and its virtual key except for a keypad key. So a keypad key whose virtual key Num
Lock changed while it was held sends the key up of the name it went down with (`numpad_1`); main
Return and keypad Enter held together are two keys (`enter`, `numpad_enter`); left and right Ctrl
stay two keys whether or not Windows marks the right one extended; and right Alt without the
extended flag is not the held left Alt.

r[input.held-key.pc-windows-key-cancelled]
A Windows key whose key down a driving PC delays, waiting for the next key to tell whether it begins a
Windows-key shortcut, reaches neither machine if input moves first, and its key up acts on neither
machine. The next press of the Windows key is a new one: tapped on its own, it sends a key down and
a key up.

r[input.held-key.windows-key-here]
A Windows key held on a driving PC that the peer never got, because it was pressed at home or kept
on the PC as stays-here input or because the keyboard stays here, makes no Windows-key shortcut of
a later S, H or N on a Mac peer: that key goes as itself, and the Windows key's key up acts on the
PC.

r[input.held-key.modifier-here]
A modifier held on a driving PC that the peer never got, because it was pressed at home or kept on
the PC as stays-here input or because the keyboard stays here, does not stop a Windows-key
shortcut: the Windows key and S still go to a Mac peer as `spotlight`, and the modifier's key up
acts on the PC.

r[input.held-key.pc-secure-desktop-repair]
When Windows returns to the default desktop from a secure desktop (Ctrl-Alt-Del, a UAC prompt),
a PC's capture forgets each Ctrl it saw pressed, and right Alt, that Windows now reports up, and
adds none that Windows reports down. It sends no key message for them, so the next keys translate
without them: `^` then `e` composes `ê`, and the `q` key is `q`, not right Alt's `@`.

r[input.held-key.pc-secure-desktop-entered]
When Windows changes to a secure desktop, a PC's capture keeps every Ctrl and right Alt it holds
as held and sends nothing, although Windows there reports every key up.

r[input.held-key.pc-secure-desktop-held]
A Ctrl or right Alt that Windows still reports down when the default desktop returns stays held on
a PC's capture, and its real key up later goes to the peer as usual. The left Ctrl that Windows
adds to a held right Alt does not become a held Ctrl.

r[input.held-key.pc-secure-desktop-only]
A PC's capture changes what it holds as Ctrl and right Alt only on their key downs and key ups and
on a return to the default desktop, never on anything else Windows tells it.

r[input.held-key.pc-desktop-listener]
When a PC's capture cannot be told that Windows changed desktop, it does not start: it captures
no keys or pointer, its start reports the failure, and the failure is logged at error level.

## Injection

r[input.injection.each-kind]
A driven machine injects each input message from its driver as the same kind of input on its OS: a
key down or up of the key its key name names, a pointer move, a button down or up, a scroll on both
axes in its mode, and a gesture, in the order `link.input.once-in-order` deals with them.

r[input.injection.error-status]
When the driven machine's OS refuses an injection, the machine's status says "Input error from
<label>; see log", where the label is the name this machine gives the driver. The link stays up,
and the input counts as dealt with and is acknowledged, as `link.input.ack-discarded` says of input
the driven machine discards.

r[input.injection.error-interval]
The status of `input.injection.error-status` is set at most once every 5 s per link, however many
injections fail.

r[input.injection.error-continues]
After a refused injection the driven machine goes on injecting its driver's next input messages.

r[input.injection.slow-log]
When one injection takes 250 ms or more, the driven machine logs a warning naming the driver, the
kind of input and the milliseconds it took, at most once a second per link.

r[input.injection.activity]
A driven Mac declares user activity to macOS as it posts injected input, paced by
`input.injection.activity-throttle`, as the Mac's own keyboard and pointer would, which wakes its
display and holds off sleep.

r[input.injection.activity-throttle]
A driven Mac declares user activity with the first input it posts, then again with the first input
it posts 1 s or more after its last declaration. An input message that posts nothing declares
nothing.

r[input.injection.activity-failure]
When macOS refuses a declaration of user activity, the Mac still posts the input, and logs one
warning in the run however often declarations fail. Once the declaration cannot be made at all, the
Mac stops declaring for the run.

## Pointer

r[input.pointer.mac-capture-fractions]
A driving Mac sends pointer moves in whole points: it adds each move macOS reports to the fraction
left from the moves before, sends the whole part, truncated toward zero, and keeps the fraction.
A move that makes no whole point sends nothing. The fraction is dropped when the Mac's input comes
home.

r[input.pointer.pc-travel]
While a PC's input is on another machine, the pointer moves it sends carry the mouse's own relative
counts as Windows reports them, not the pointer's position, so a move against a display's edge still
has travel.

r[input.pointer.pc-marked]
Mouse counts carrying Beamer's mark are never the hand's: a PC sends nothing for them, and they
never push at a zone. Counts carrying any other value there, such as a pen's own signature, are the
hand's.

r[input.pointer.pc-absolute]
A pointer reading that Windows gives as an absolute position, as a pen tablet or Windows Remote Desktop
does, has no travel: a driving PC sends no move for it.

r[input.pointer.pc-still]
A mouse reading with no movement on either axis sends nothing.

r[input.pointer.clamp]
A driven Mac moves the pointer to its position plus the move's travel when that point is on a
display, and otherwise to the nearest point of the display nearest to it, so the pointer never stops
off every display, such as in the space beside a shorter display.

r[input.pointer.mac-travel]
The pointer move a driven Mac posts carries the move's own travel as its deltas, even when its
position was clamped, so an application that reads raw movement, such as a game, sees the move.

r[input.pointer.mac-drag]
While a button injected on a driven Mac is down, the Mac posts a pointer move as a drag of that
button. With several down, the drag is of the first of left, right, middle, back and forward that is
down.

## Buttons

r[input.button.names]
A button input message names one of five buttons: left, right, middle, back and forward.

r[input.button.mac-capture]
A driving Mac sends its other buttons 2, 3 and 4, by macOS's numbering from 0, as middle, back and
forward.

r[input.button.pc-capture]
A driving PC sends its left, right and middle buttons by those names, X button 1 as back and X
button 2 as forward.

r[input.button.mac-inject]
A driven Mac posts middle as other button 2, back as other button 3 and forward as other button 4,
by macOS's numbering from 0.

r[input.button.pc-inject]
A driven PC posts back as X button 1 and forward as X button 2, both down and up.

r[input.button.click-count]
A driven Mac gives each button down a click count: one more than that button's last count, up to
3, when its last down came 0.5 s or less before and no more than 5 points away on either axis, and
1 otherwise. Two quick downs in one place are a double click.

r[input.button.click-count-up]
A button up on a driven Mac carries the click count of that button's last down, or 1 when it has
none.

## Scroll

r[input.scroll.message]
A scroll input message carries two axes, vertical and horizontal, and a mode: pixel, for a
trackpad's continuous movement in points, or line, for wheel notches, whole or in part.

r[input.scroll.convention]
Both axes of a scroll are in the Mac's convention, with the driver's own scrolling direction setting
already applied: a positive vertical moves the view up, and a positive horizontal moves the view
left.

r[input.scroll.mac-capture]
A driving Mac sends a scroll as macOS reports it, axis 1 as the vertical and axis 2 as the
horizontal: a continuous scroll, from a trackpad or its momentum, in pixel mode with its point
deltas, fractions kept, and a wheel in line mode with its whole line deltas. A scroll of zero on
both axes sends nothing.

r[input.scroll.pc-capture]
A driving PC sends each wheel turn as a scroll in line mode of the turn in notches, 120 wheel units
to a notch with fractions kept, so a high-resolution wheel sends parts of a notch: the vertical
wheel on the vertical axis as it stands, and the horizontal wheel turned round on the horizontal
axis.

r[input.scroll.mac-inject]
A driven Mac posts a scroll's axes as they stand, the vertical as axis 1 and the horizontal as axis
2: in pixels in pixel mode, and in lines in line mode.

r[input.scroll.mac-lines-per-notch]
In line mode a driven Mac posts 3 lines a notch when its driver is a PC, and 1 line a notch when
its driver is a Mac.

r[input.scroll.pc-inject]
A driven PC posts the vertical axis on its wheel as it stands and the horizontal axis turned round on
its horizontal wheel: 120 wheel units a notch in line mode, and 3 a point in pixel mode. When both
axes move it posts the wheel first, and it posts nothing on an axis with no whole wheel unit.

r[input.scroll.direction]
A scroll moves the driven machine's view the way the same movement would have moved the driver's
own view, on both axes and in both modes, whether the driver and the driven machine are Macs or PCs
and whichever scrolling direction the driver is set to.

> r[input.scroll.direction-felt]
> Scrolling on the driver scrolls the driven machine's view the way it would scroll the driver's
> own, on both axes.
>
> - tier: human (which way a view moves is judged by eye, against the real OS's scroll handling)
> - do: With input on the PC, scroll up, down, left and right on the Mac's trackpad; then with input on the Mac, turn the PC's mouse wheel both ways and tilt it left and right
> - see: Each time the page on the driven machine moves the way the same movement moves a page on the driver

r[input.scroll.remainder]
A driven machine never loses part of a scroll. On each axis it adds the scroll, in the units it
posts, to what is left from the scrolls before, posts the whole units, truncated toward zero, and
keeps the fraction for the next. Ten quarter-notch scrolls from a Mac post two lines on a driven
Mac, and four half-notch scrolls post 240 wheel units on a driven PC.

r[input.scroll.remainder-sign]
A scroll remainder keeps its sign, so a scroll back the other way cancels it rather than adding to
it: 0.6 of a line and then −0.6 of a line post nothing.

r[input.scroll.remainder-axes]
Each axis keeps its own scroll remainder: three half-notch horizontal scrolls on a driven Mac post
one line on the horizontal axis and nothing on the vertical.

r[input.scroll.mac-remainder-modes]
A driven Mac keeps its line-mode and pixel-mode remainders apart: a pixel-mode scroll never
completes a line, nor a line-mode scroll a pixel.

r[input.scroll.pc-remainder-units]
A driven PC keeps its scroll remainders in wheel units, one per axis, whichever mode each scroll
came in.

> r[input.scroll.mac-remainder-reset]
> When a driven Mac stops being driven by its driver, by a let go, a closed link or any other end,
> it drops its scroll remainders, so a fraction from before never posts later.
>
> - model: driving

## Pointer speed and scroll speed

r[input.speed.pointer]
A driven machine multiplies the travel of each pointer move from its driver by its own pointer
speed: at 200 %, a move of 3 right and 4 up travels 6 right and 8 up.

r[input.speed.pointer-fractions]
At a pointer speed other than 100 %, a driven machine carries the fractions of scaled moves to the
next move, truncated toward zero, so a slow hand still moves the pointer: at 50 %, four moves of 1
point travel 0, 1, 0 and 1. A move that scales to no whole point on either axis injects nothing.

r[input.speed.pointer-steps]
A driven machine scales the steps of a merged move (`link.input.merge-steps`) one by one, so the
pointer travels what the steps would have travelled had they arrived one by one.

r[input.speed.scroll]
A driven machine multiplies both axes of each scroll from its driver by its own scroll speed, in
either mode, before `input.scroll.remainder`: at 200 % a notch from a PC posts 6 lines on a driven
Mac, and at 25 % four notches from a PC post 3 lines.

r[input.speed.reverse]
With reverse scrolling on, a driven machine turns both axes of each scroll from its driver round.

r[input.speed.default]
Pointer speed and scroll speed are 100 % and reverse scrolling is off until the user changes them;
then a driven machine injects pointer moves and scrolls as they arrived.

r[input.speed.range]
Pointer speed and scroll speed run from 25 % to 400 %; a value outside that range counts as the
nearer end.

## Gestures

r[input.gesture.names]
A gesture input message names one system gesture: `swipe_up`, `swipe_down`, `swipe_left` or
`swipe_right`, a three- or four-finger swipe named for the way the fingers move; `spread`, the
thumb and three fingers spreading apart; or `pinch`, the thumb and three fingers pinching together.

> r[input.gesture.panel-shown]
> Within 0.4 s of the Mac's input leaving for another machine, the gesture capture panel is up: a
> panel 400 × 400 points, centred on the point where the Mac holds its pointer while input is away,
> drawn at an opacity of 1/255, above ordinary windows, and present on every Space. It stays up,
> and moves with that point, for as long as input is away.
>
> - tier: machine (the panel and the gestures it receives need the real window server)

> r[input.gesture.panel-gone]
> Within 0.4 s of the Mac's input coming home, the gesture capture panel is gone, and a pinch or
> page swipe acts on the Mac's own apps again.
>
> - tier: machine (the panel needs the real window server)

> r[input.gesture.panel-no-focus]
> The gesture capture panel never becomes the key or main window and never activates Beamer: the
> app in front keeps keyboard focus while the panel is up.
>
> - tier: machine (key and main windows need the real window server)

r[input.gesture.panel-failure]
When the Mac cannot put the gesture capture panel up, it logs the failure, makes no further attempt
until Beamer restarts, and still sends keys, buttons, pointer moves and scrolls while input is away.

r[input.gesture.away-only]
While the Mac's input is home, a pinch or page swipe sends nothing to any machine.

> r[input.gesture.swallowed]
> While the Mac's input is on another machine, a two-finger pinch or page swipe on the Mac's
> trackpad acts on that machine and not on the Mac.
>
> - tier: human (whether each machine's app zoomed or paged is judged by eye)
> - do: With input on the PC and a browser in front on both machines, pinch to zoom in on the Mac's trackpad, then swipe two fingers to the right
> - see: The PC's browser zooms in and then goes back a page; the Mac's browser does neither

> r[input.gesture.system-stays]
> Three- and four-finger system gestures on the Mac's trackpad (Mission Control, switching Spaces,
> application windows, Show Desktop) act on the Mac even while its input is on another machine,
> and the Mac sends no gesture input message for them.
>
> - tier: human (whether macOS switched Space is seen on the Mac)
> - do: With input on the PC, swipe four fingers sideways on the Mac's trackpad
> - see: The Mac switches Space; nothing changes on the PC
> - parity: Python's event-tap path sent these as gesture input messages on macOS 26 (#11 drops the Mac tap-based gesture path, which never fires on macOS 27)

r[input.gesture.zoom-step]
While the Mac's input is away, its capture adds up the magnification of a pinch's events and sends
one zoom step for each 0.05 the total reaches: a zoom in while the total is positive (fingers
spreading) and a zoom out while it is negative. Each step takes 0.05 off the total's size, and what
is left carries into the pinch's next event: an event of 0.05 sends one zoom in, one of −0.05 one
zoom out, one of 0.12 two zoom ins leaving 0.02, and two of 0.03 send one zoom in, with the second.

r[input.gesture.zoom-end]
A pinch event in the ended or cancelled phase sends the zoom steps its own magnification completes,
then sets the total back to 0: what was left below 0.05 never adds to the next pinch, so 0.02 that
ends a pinch followed by 0.04 sends nothing.

r[input.gesture.zoom-no-phase]
A pinch event whose phase the Mac's capture cannot read counts as one in the middle of a pinch: its
magnification adds to the total, which carries on to the next event.

r[input.gesture.unreadable]
A gesture event the Mac's capture cannot read sends nothing and leaves the zoom total as it was,
and the capture carries on: the next event sends what it would have sent had the unreadable one
never come.

r[input.gesture.page-swipe]
While the Mac's input is away, each two-finger page swipe event sends one page swipe: back when its
horizontal delta is positive, forward when it is negative, and nothing when it is 0, as for a swipe
that is only vertical.

r[input.gesture.smart-magnify]
While the Mac's input is away, a smart magnify (a two-finger double tap) on the gesture capture
panel sends nothing.

r[input.gesture.capture-log]
The first gesture the gesture capture panel receives in a run while input is away logs one line
saying gesture capture is active, whether or not it makes a zoom step or page swipe. Later gestures
in the same run log no such line.

> r[input.gesture.chord-when-sent]
> A zoom step or page swipe becomes the chord of the machine it is sent to, chosen when it is sent.
> One made while a crossing is under way waits as queued input, and reaches the machine that then
> takes the input as that machine's chord, and nothing of it reaches the machine the crossing
> left.
>
> - model: driving

r[input.gesture.chord-pc-zoom]
To a PC, a zoom step's chord is a key down of `ctrl`, a scroll of one line notch (vertical +1 for a
zoom in or −1 for a zoom out, horizontal 0), then a key up of `ctrl`.

r[input.gesture.chord-mac-zoom]
To a Mac, a zoom step's chord is a key down of `cmd`, a key down and a key up of `=` for a zoom in
or `-` for a zoom out, then a key up of `cmd`. It holds no scroll.

r[input.gesture.chord-pc-page]
To a PC, a page swipe's chord is a key down of `alt`, a key down and a key up of `left` for back or
`right` for forward, then a key up of `alt`.

r[input.gesture.chord-mac-page]
To a Mac, a page swipe's chord is a key down of `cmd`, a key down and a key up of `[` for back or
`]` for forward, then a key up of `cmd`.

r[input.gesture.chord-us-place]
Each character key in a chord carries itself as its US place (`=`, `-`, `[` or `]`), so a Mac whose
layout has no key for the character still presses the key at that place under Command. A chord's
modifier carries no US place.

r[input.gesture.chord-own-words]
A chord's key names are the receiving machine's own words, and the driver's modifier style never
changes them: under every modifier style a zoom step reaches a PC as Control and a scroll, never
as the Windows key, and reaches a Mac as Command.

r[input.gesture.mac-swipes]
A Mac injects a received system swipe as the shortcut macOS ships for it: `swipe_up` as
Control-Up (Mission Control), `swipe_down` as Control-Down (application windows), `swipe_left` as
Control-Right (the Space to the right) and `swipe_right` as Control-Left. Each is Control down, the
arrow down, the arrow up, then Control up.

r[input.gesture.mac-arrow-flags]
The arrow of a system swipe a Mac injects goes down with Control in its event flags, besides the
function and numeric-pad flags a real arrow key's events carry.

r[input.gesture.mac-spread]
A Mac injects a received `spread` as F11 (Show Desktop): F11 down, then F11 up, with no modifier
pressed.

r[input.gesture.mac-pinch]
A Mac injects nothing for a received `pinch`, because macOS 26 removed Launchpad and its shortcut.
It logs a warning the first time in a run, and nothing for later ones.

r[input.gesture.mac-held-modifier]
When the driver already holds Control on a Mac, a system swipe the Mac injects presses and releases
only its arrow, and Control is still held afterwards.

r[input.gesture.mac-no-modifier-left]
After a Mac injects a system gesture, it holds no modifier that the gesture pressed.

r[input.gesture.pc-touchpad]
A PC injects a received system gesture as finger contacts played through its synthetic touchpad,
so that Windows' own touchpad gestures, and the user's touchpad settings, decide what it does:
`swipe_up` as three fingers up, `swipe_down` and `spread` as three fingers down, `swipe_left` as
four fingers left and `swipe_right` as four fingers right. When the contacts are played, the PC
presses no key for the gesture.

r[input.gesture.pc-fallback]
When the PC's synthetic touchpad is refused, it presses the shortcut Windows ships for the same
gesture instead: Windows-Tab for `swipe_up`, Windows-D for `swipe_down` and `spread`,
Windows-Control-Right for `swipe_left` and Windows-Control-Left for `swipe_right`. The keys go down
in that order and up in the reverse order.

r[input.gesture.pc-pinch]
A PC injects a received `pinch` as a press and release of the Windows key, which opens Start, and
plays no touchpad contacts for it.

> r[input.gesture.pc-device]
> The first system gesture a PC injects in a run makes its synthetic touchpad, which later gestures
> reuse: a gesture-only Precision Touchpad 100 mm wide and 60 mm high taking up to 5 contacts, whose
> contacts Windows reads only as touchpad gestures, never as a click or a pointer move.
>
> - tier: machine (the synthetic touchpad needs Windows 11's real pointer injection)

r[input.gesture.pc-device-refused]
When Windows refuses to make the synthetic touchpad, the PC logs one warning, makes no further
attempt until Beamer restarts, and injects every system gesture by its shortcut
(`input.gesture.pc-fallback`).

r[input.gesture.pc-contacts]
The contacts of a played swipe start in a row across the middle of the touchpad, 15 mm apart, and
move together by 30 mm for up or down and 40 mm for left or right, in 12 steps: 13 frames in
contact, frame k moved k/12 of the distance (rounded down to 0.01 mm), so the row's centre goes from
half the distance before the touchpad's centre to half the distance past it. Every contact is on
the touchpad in every frame.

r[input.gesture.pc-contact-timing]
The frames of a played swipe are 10 ms apart, and a further frame 10 ms after the 13th lifts every
contact at its last position. Each frame carries a later time than the one before it.

r[input.gesture.pc-contacts-refused]
When Windows refuses a frame of a played swipe, the PC logs a warning, plays no further frame of
it, and presses the gesture's shortcut (`input.gesture.pc-fallback`).

## Media keys

r[input.media-key.message]
A media key travels as a key down and a key up whose key name is the media key's: `volume_up`,
`volume_down`, `volume_mute`, `media_play_pause`, `media_next`, `media_prev`, `media_stop` or
`mic_mute`. It carries no US place, and the driver's modifier style never changes it.

r[input.media-key.mac-codes]
The Mac's capture reads a media key from an auxiliary control button event (a system-defined event
of subtype 8) by the NX key code in the top 16 bits of its data: 0 is `volume_up`, 1 `volume_down`,
7 `volume_mute`, 16 `media_play_pause`, 17 and 19 `media_next`, and 18 and 20 `media_prev`.

r[input.media-key.mac-state]
A media key event whose key state, bits 8 to 15 of its data, is 0x0A is a key down, and any other
state is a key up. An auto-repeat (bit 0 set) of a key down is another key down.

r[input.media-key.mac-sent]
While the Mac's input is away, a media key its capture reads is sent to the machine the input is on
and does not act on the Mac.

r[input.media-key.mac-other-events]
A system-defined event that is not a media or brightness key the Mac's capture reads (another
subtype, or another NX key code such as the keyboard backlight's) acts on the Mac and sends
nothing, while input is away as at home.

r[input.media-key.mac-home]
While the Mac's input is home, its media keys act on the Mac, and its capture does not read them.

r[input.media-key.mac-unreadable]
A system-defined event the Mac's capture cannot read while input is away acts on the Mac and sends
nothing, and input stays on the machine it is on.

r[input.media-key.pc-codes]
The PC's capture names the media keys by virtual-key code: 0xAD `volume_mute`, 0xAE `volume_down`,
0xAF `volume_up`, 0xB0 `media_next`, 0xB1 `media_prev`, 0xB2 `media_stop` and 0xB3
`media_play_pause`. While its input is away, each is sent and does not act on the PC, as any key.

r[input.media-key.mac-inject]
A Mac injects a received media key as an auxiliary control button event (a system-defined event of
subtype 8) whose data holds the NX key code in its top 16 bits and the key state in bits 8 to 15,
0x0A for a key down and 0x0B for a key up, with no repeat bit, and whose modifier flags are 0xA00
for a key down and 0xB00 for a key up. The NX key codes are 0 for `volume_up`, 1 `volume_down`, 7
`volume_mute`, 16 `media_play_pause`, 17 `media_next` and 18 `media_prev`.

r[input.media-key.mac-inject-not-key]
A Mac never injects a media key as a keyboard key event: no character is typed and no ordinary key
is pressed for it.

r[input.media-key.mac-stop]
A Mac injects nothing for `media_stop`, since a Mac has no stop key and play/pause would start
playback that was paused. It logs a warning the first time in a run, and nothing for later ones.

r[input.media-key.mac-mic-mute]
A Mac injects nothing for `mic_mute`, and logs a warning the first time in a run.

> r[input.media-key.felt]
> While the Mac's input is on the PC, the Mac keyboard's volume and play/pause keys act on the PC
> and not on the Mac.
>
> - tier: human (sound and volume are heard and seen on each machine)
> - do: With input on the PC and music playing on both machines, press the Mac keyboard's volume up key, then its play/pause key
> - see: The PC's volume rises and its music pauses; the Mac's volume and music do not change

## Brightness keys

r[input.brightness.message]
A brightness key travels as a key down and a key up of `brightness_up` or `brightness_down`, with
no US place.

r[input.brightness.mac-capture]
The Mac's capture reads NX key code 2 as `brightness_up` and 3 as `brightness_down` from auxiliary
control button events, with key states as for media keys (`input.media-key.mac-state`), and while
its input is away sends them and they do not act on the Mac.

r[input.brightness.mac-inject]
A Mac injects a received `brightness_up` or `brightness_down` as an auxiliary control button event
with NX key code 2 or 3, shaped as a media key's (`input.media-key.mac-inject`).

r[input.brightness.pc-step]
A PC acts on each key down of `brightness_up` or `brightness_down` by moving every built-in display
whose brightness Windows exposes one supported level up or down from its current level. A key up
does nothing, and external displays are never changed.

r[input.brightness.pc-levels]
Several brightness steps applied at once move a display one supported level per step from its
current level, and stop at its highest or lowest supported level: with levels 0, 20, 40 and 80 at
20, two steps up set 80, and so would three.

r[input.brightness.pc-coalesce]
Brightness key downs that arrive while the PC is changing brightness add up, and once that change
ends, one change for their net count follows: after a step up that is in progress, four more key
downs, up, up, down, up, make one further change of two steps up. A net count of 0 makes no change.

r[input.brightness.pc-never-blocks]
Injecting a brightness key never waits for the display's change: the injection returns, and the
input messages after it are injected, while the change is still in progress.

r[input.brightness.pc-no-panel]
When no display on the PC exposes its brightness to Windows, a brightness key changes nothing, and
the PC logs once per run that no built-in display exposes brightness control and external displays
are unchanged.

r[input.brightness.pc-failure]
When Windows fails a brightness change, the PC logs the failure, and later input and later
brightness keys are still injected.

## Stays-here input

> r[input.stays-here.list]
> Each machine keeps its own stays-here list of at most 64 entries. An entry is a key, by its code
> on that machine (a Mac key code or a PC virtual-key code, 0 to 99999); a mouse button, by its
> name on the link (`left`, `right`, `middle`, `back`, `forward`) or, for a button the link has no
> name for, by its number counting the left button as 1 (`6`), as 1 to 24 lower-case letters,
> digits and underscores; or, on a Mac, a media or brightness key, by its key name, as 1 to 24
> lower-case letters and underscores.

r[input.stays-here.pc-media-as-key]
On the PC, a media key is a key on the stays-here list, by its virtual-key code (0xAF for
`volume_up`), like any other key.

r[input.stays-here.pc-sided-modifier]
On the PC, a modifier enters the stays-here list as the sided virtual-key code the PC's capture
reports for it: Shift as 0xA0, or 0xA1 for scan code 0x36; Control as 0xA2, or 0xA3 when the key is
extended; Alt as 0xA4, or 0xA5 when extended. A key is extended when its scan code has bit 0x100
set or carries the 0xE0 prefix (0xE01D, 0xE038). Every other key enters as its own code.

r[input.stays-here.own-machine]
A machine's stays-here list and its keyboard stays here setting are its own: neither is sent to a
peer, and neither decides anything about input another machine captures.

r[input.stays-here.saved]
A saved stays-here list reads back with the same entries in the same order.

r[input.stays-here.saved-default]
A machine whose settings hold no stays-here list has an empty one, and nothing stays here.

r[input.stays-here.refused]
Saving a stays-here list that is not a list, that holds more than 64 entries, or that holds an entry
`input.stays-here.list` does not allow (a key with no code or a code that is not a number, an entry
of a kind other than key, button or media key, an entry that is not text) is refused with an error,
and nothing is saved.

r[input.stays-here.press]
While a machine's input is on another machine, a press of a key, button or media key on its
stays-here list acts on this machine and is not sent, and so does that press's release.

r[input.stays-here.others-cross]
While a machine's input is on another machine, every press and release the stays-here list and
keyboard stays here do not keep is sent to the machine the input is on, as with an empty list.

r[input.stays-here.change-applies]
A change to the stays-here list or to keyboard stays here applies from the next press, also while
input is away.

r[input.stays-here.release-of-sent]
A key or button whose press was sent to another machine goes on being sent until its release: its
auto-repeats and its release are sent there, even when it was put on the stays-here list, or
keyboard stays here was turned on, after the press.

r[input.stays-here.release-of-kept]
A key or button whose press stayed here stays here until its release: its auto-repeats and its
release act on this machine and are not sent, even when it was taken off the stays-here list, or
keyboard stays here was turned off, after the press.

r[input.stays-here.release-unseen]
While input is away, a release whose press the capture did not see goes to the machine the input
is on, unless its key or button is on the stays-here list, when it acts on this machine.

> r[input.stays-here.home-forgets-sent]
> When input comes home, the capture forgets which presses it sent. Once input is away again, the
> release of a key it sent before is decided as a release whose press it did not see
> (`input.stays-here.release-unseen`).
>
> - model: driving

> r[input.stays-here.kept-through-home]
> A press that stayed here and is still held when input comes home stays here: when input is away
> again before the key is released, its release acts on this machine and is not sent, whatever the
> stays-here list says by then.
>
> - model: driving

> r[input.stays-here.kept-release-at-home]
> The release, while input is home, of a press that stayed here acts on this machine, is not sent
> to the machine the input was on, and ends that press: the key's next press, once input is away
> again, follows the stays-here list as it then stands.
>
> - model: driving

r[input.stays-here.taps]
A key the capture reads as a whole press, its press and release together, follows the stays-here
list as it stands at each press: Caps Lock, each toggle of which is one press, and the input method
keys `hangul` and `hanja`, on the Mac and the PC; and on the PC a dead key, and composed text that
no key press produced. Put on the list between two of its presses, such a key stays here from its
next press.

r[input.stays-here.composed-text]
On the PC, composed text that a key press produced follows that key's press: it stays here when
the key is on the stays-here list or keyboard stays here is on, and while the key is held, the
text its repeats produce goes where its first text went, until the key's release.

## Keyboard stays here

r[input.keyboard-stays-here.keys]
While keyboard stays here is on and the machine's input is on another machine, every key press and
release the crossing methods do not use, every dead key, every composed text and every media key
acts on this machine and is not sent.

r[input.keyboard-stays-here.pointer]
While keyboard stays here is on, mouse buttons, pointer moves, scrolls and gestures are still sent
to the machine the input is on, apart from a button on the stays-here list, which still stays here.

r[input.keyboard-stays-here.default]
Keyboard stays here is off until the user turns it on: settings that do not hold it read as off.

r[input.keyboard-stays-here.saved]
A saved keyboard stays here setting reads back as saved, and saving a value that is neither on nor
off (the string `"true"`) is refused with an error, and nothing is saved.
