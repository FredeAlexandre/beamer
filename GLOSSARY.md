# Beamer

Beamer shares one keyboard, pointer, clipboard, sound, camera and set of displays across several
computers. This glossary fixes the words the Rust rewrite's spec, code and agents use for it.

## Machines and links

**Machine**:
One install of Beamer, identified by its own key pair.
_Avoid_: host, computer, node, side

**Machine id**:
A short name for a machine, derived from its public key, so only the key's holder can claim it.
_Avoid_: uuid, device id

**Peer**:
A machine this one has paired with and whose public key it has pinned.
_Avoid_: partner, remote, client, server

**Pairing**:
The one-time exchange, proved by a six-digit code, after which two machines each hold the other's
public key.
_Avoid_: bonding, trusting, linking

**Link**:
The one encrypted connection between two peers. It carries everything between them, in both
directions.
_Avoid_: connection, session, channel, initiator, responder

**Traffic class**:
One of input, control, media and bulk, in that order of priority. A class never waits behind a
class below it.
_Avoid_: channel, lane, priority

**Capability**:
Something a machine can do, which it reports as ready, needs permission, refused or unsupported
here.
_Avoid_: feature, caps

## Shared state

**Cluster**:
Every machine reachable from this one over links, directly or through other machines.
_Avoid_: mesh, network, group

**Store**:
The replicated state of which every machine in the cluster holds a copy.
_Avoid_: document, flood list, shared settings

**Machine record**:
A store entry that only its own machine writes, such as its devices, displays, capabilities or
paired list.
_Avoid_: device list, announcement

**Shared entry**:
A store entry that any machine may write, where the newest stamp wins, such as an audio route, a
display's owner or a shared setting.
_Avoid_: route document, setting

## Driving

**Driver**:
The machine whose keyboard and pointer someone is using. Never a device driver.
_Avoid_: owner, initiator, controller, source, server, focus

**Driven machine**:
A machine the driver's input is on.
_Avoid_: owned, target, responder, focused machine

**Home**:
The driver itself, as the place its input is when it is on no other machine. Input "comes home".
_Avoid_: local, back

**Move number**:
The number a driver raises each time its input moves, so that a request made before the move cannot
move it again.
_Avoid_: route, epoch, generation

**Reach**:
The peers a driver has a link to and may drive right now. A driven machine's zones lead only to
peers in its driver's reach.
_Avoid_: left out

**May drive / may be driven by**:
A machine's two drive permissions toward a peer: whether it may drive that peer, and whether that
peer may drive it.
_Avoid_: send, allow drive, accepts, direction switch

**Take**:
A driver asking a machine to accept its input. The machine accepts or refuses.
_Avoid_: focus, grab, claim

**Let go**:
A driver releasing a driven machine.
_Avoid_: focus, release, drop

**Pass on**:
A driven machine asking its driver to cross on to a further machine, because its own zone fired.
Input is never relayed: the driver takes the further machine itself.
_Avoid_: switch, forward, relay

**Send home**:
A driven machine's own user sending the driver's input home.
_Avoid_: kick, sent home

**Take over**:
A second driver displacing one that has been idle.
_Avoid_: steal, preempt

**Chain**:
The machines a driver's input passed through to reach the driven machine, in order.
_Avoid_: route, path, hops

## Crossing

**Crossing**:
Input moving from one machine to another, by any crossing method.
_Avoid_: switch, transfer, hop, hand-over

**Crossing method**:
One way of causing a crossing: a zone push, the shortcut, a jump key, a shake or the menu.
_Avoid_: way, trigger

**Zone**:
A stretch of a display's edge (a whole edge, a third, a corner or the notch) that leads to one peer.
The Linux portals' "barriers" are how a zone is made there, and their "zones" are displays.
_Avoid_: way, barrier, region, hot edge

**Edge**:
One of a display's four boundaries: top, bottom, left or right.
_Avoid_: side

**Landing point**:
Where the pointer appears on the driven machine after a crossing.
_Avoid_: arrival position, entry point, side

**Held at edge**:
The pointer pinned at a zone while push pressure builds, or just after it arrives.
_Avoid_: hold, stuck, pinned

**Shortcut**:
The key the user double-taps or long-presses to cross.
_Avoid_: trigger key, switch shortcut, hotkey, hold

**Jump key**:
A key that crosses straight to one particular machine.
_Avoid_: hotkey, direct key

**Shake**:
Shaking the pointer to bring input home.
_Avoid_: wiggle

**Full-screen pause**:
A machine's zones turned off while an app on it is full screen.
_Avoid_: full-screen hold, held

**Queued input**:
Input a driver keeps back while a crossing is under way, then delivers.
_Avoid_: held input

**Zone choices**:
What a machine has chosen for its zones toward each peer: whether its edge crosses and by which
thirds, its wrap-around edges, its corners and, on a Mac, the notch and return through the notch.
Zones are derived from the zone choices and the crossing map, and deriving never changes the
choices.
_Avoid_: ways, intent, row

**Armed**:
Said of a zone that can build push pressure and cross right now. A machine arms its own zones
while its input is home, and the zones it arms for its driver while it is driven.
_Avoid_: live zone, hot zone

**Push pressure**:
How far the pointer has pushed out through a zone's edge, less inward travel and a decay of 300 px
per second. A zone push crosses when push pressure reaches the resistance.
_Avoid_: push force

**Resistance**:
The push pressure, from 0 to 500 px, at which a zone push crosses.
_Avoid_: stiffness

**Arrival grace**:
The 0.3 s after input lands on a machine through an edge, in which its zones keep the pointer held
at edge instead of building push pressure.
_Avoid_: grace period, landing hold, cooldown

**Third**:
One of the three equal stretches of a display's edge: the start (top or left), the middle and the
end (bottom or right).
_Avoid_: part of the edge, segment

**Contact**:
Where a display of this machine touches a display of a peer on the crossing map, along an edge or
at a corner.
_Avoid_: touch line

**Wrap-around zone**:
A zone on an edge of this machine that the peer's displays do not touch on the crossing map, which
leads to the peer all the same, on an edge chosen for it.
_Avoid_: wrap way, wrap edge

**Notch**:
The stretch at the top of a Mac's built-in display taken by its camera housing, usable as a zone.
_Avoid_: cutout

**Return through the notch**:
A Mac's choice, for the peer its notch leads to, to land the pointer just below the notch when
input comes home from that peer.
_Avoid_: notch home

**Return zone**:
The zone a driven machine arms, while its driver drives it, on the edge where the driver's pointer
landed, leading to the driver.
_Avoid_: way back, way home

**Crossing pause**:
A machine's zones turned off from its menu, until the user resumes or Beamer restarts. The
shortcut, jump keys, the shake and the menu still cross.
_Avoid_: paused edges, held edges

**Shortcut key**:
The one key the shortcut is made with, chosen by the user.
_Avoid_: trigger

**Double-tap window**:
The longest time from one press of the shortcut key to the next for the two to fire the shortcut.
_Avoid_: tap threshold

**Shortcut destination**:
Where the shortcut sends input: where input last went, next in order, or next among ticked.
_Avoid_: shortcut mode

**Shortcut's cycle**:
The peers the shortcut steps through in order, followed by home.
_Avoid_: rotation

**Stroke**:
One leg of a shake along its axis, from one turn to the next.
_Avoid_: swing

**Turn**:
The point where the pointer, during a shake, has reversed along the shake's axis by a quarter of a
stroke length.
_Avoid_: flip

**Shake sensitivity**:
How small a shake fires, from 50 % to 200 %: at a higher sensitivity a shorter stroke is enough.
_Avoid_: shake strength

## Crossing map

**Crossing map**:
Every machine's displays placed relative to each other, from which the zones are derived.
_Avoid_: arrangement, layout, picture, desk, tile

**Profile**:
A named, shared crossing map with every machine's zones and the shake settings. One profile is
active across the cluster.
_Avoid_: desk, row, preset

**Own zones**:
Zones a machine keeps for itself instead of taking the active profile's.
_Avoid_: use own, earlier ways

**Footprint**:
The rectangle a machine's displays fill on the crossing map, placed and sized in map units.
_Avoid_: tile rectangle

**Map unit**:
The crossing map's own unit of length, in which footprints, offsets and snap points are whole
numbers. It is neither a pixel nor a point of any display.
_Avoid_: map pixel

**Snap point**:
A place flush outside this machine's footprint where a dropped footprint may settle.
_Avoid_: snap target

**Inactive peer**:
A peer that is not in use, kept off the crossing map, or dormant. Its footprint and zone choices
are kept, but no zone leads to it and it blocks nothing on the crossing map.
_Avoid_: shelved peer

**Kept off the crossing map**:
Said of a peer the user has set aside: its zone choices and footprint are kept, but no zone leads
to it until it is restored. Pairing already writes it (`pairing.peers.replace-keeps`).
_Avoid_: shelved, put back

**Own zones on every machine**:
One setting for the whole cluster: while it is on, every machine's zones follow its own zone
choices, as if each kept its own zones. It is the inverse of the Python app's "Same ways on every
machine".
_Avoid_: same ways, shared zones

**Saved zones**:
Zone choices a machine had in force that something replaced, kept on that machine with when and
why, so the user can restore them.
_Avoid_: earlier ways

**Profile status**:
What a machine publishes in its machine record about its zones: whether they come from its own
zone choices or a profile, which profile, the stamp of the zone choices it applied, whether it is
a Mac, and the peers its zone choices name that it is not paired with.
_Avoid_: status register, desk status

## Input

**Input**:
Keyboard, pointer, scroll and gesture events. Never an audio device.
_Avoid_: events, HID

**Stays-here input**:
A key, button or media key that always acts on the machine it was pressed on, even while that
machine drives another.
_Avoid_: ignored input, blocked input, filtered input

**Keyboard stays here**:
A mode in which only the pointer crosses and the keyboard keeps acting on its own machine.
_Avoid_: mouse-only

**Follows input**:
Moves to whichever machine input is on, as the clipboard does.
_Avoid_: follows focus

## Pointer

**Driving hide**:
A driver's own pointer hidden while its input is on another machine.
_Avoid_: pointer hold

**Idle hide**:
A driven machine's pointer hidden after its driver lets go, until it moves or a driver takes the
machine.
_Avoid_: idle pointer

**Veil**:
A nearly invisible window a driving Windows machine puts under the point where its pointer stays,
so that no app's own cursor shows there.
_Avoid_: cursor blocker

**Pointer overlay**:
The arrow Beamer draws at the pointer of a driven Windows machine that has no mouse, whose own
pointer Windows does not show.
_Avoid_: cursor overlay, fake cursor

## Displays

**Display**:
One screen a machine shows.
_Avoid_: screen, monitor, output, tile

**Shared display**:
A display cabled to two or more machines.
_Avoid_: shared monitor, KVM display

**Display owner**:
The machine allowed to keep a shared display on.
_Avoid_: display route

**Handover**:
A shared display's owner changing, with the display switching to the new owner's cable.
_Avoid_: hand-over (for input), switch

**Identify displays**:
Showing a number on every display for a moment, so the user can tell them apart.
_Avoid_: identify screens

## Audio and camera

**Microphone**:
An audio device that records.
_Avoid_: input, input device, source

**Output**:
An audio device that plays, such as speakers or headphones.
_Avoid_: sink, playback device

**Virtual microphone**:
The microphone Beamer adds to the Mac, where an audio route delivers another machine's microphone.
Its product name is Beamer Microphone.
_Avoid_: driver, HAL driver, mac driver

**Virtual camera**:
The camera Beamer adds to Windows, where a camera route delivers another machine's camera.
_Avoid_: vcam, camera source, driver

**Audio route**:
A shared entry saying which machine's microphone another machine uses, or where a machine's sound
plays.
_Avoid_: audio follows input

**Camera route**:
A shared entry saying which machine's camera another machine sees.
_Avoid_: camera follows input

