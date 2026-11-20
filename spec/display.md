# Display

Shared displays and their handover (F45), and identify displays (F28), both of which ship (#11).
A shared display is cabled to two or more machines; each one has at most one display owner, chosen
by the user on any machine, and every machine that is not its display owner turns the display
off. Identify displays shows a number on every display for 3 s, here and on the peers, so the user
can tell the displays apart and match them to the crossing map.

The rules come from #11's outcomes, the store of #14 (machine records, shared entries, capabilities
as facts in each machine's own record), the platform backend of #16, and the Python app at
`archive/python`: `core/display/`, `mac_app/shared_display_mac.py`, `win_app/shared_display_win.py`
and their design note `dev/display/DESIGN.md`, `core/identify_screens.py`,
`mac_app/identify_overlay.py`, `win_app/identify_overlay.py`, and the code in `mac_app/bridge.py`
and `win_app/kvm_bridge_win.py` that sends and receives the requests to identify displays. They are
mined through the tests in `spec/ledger/display.txt`.

**No DDC/CI.** Beamer never uses DDC/CI, to write or to read. Measured on Alex's machines on
2026-11-12 (`dev/display/DESIGN.md`): DDC/CI from the Mac blocks for good in the kernel through the
shared display's cable, and from the PC the display answers only while it shows the PC, and does
nothing on a command to show another cable. A handover therefore works by every machine that is not
the display owner turning the display off; the display's own automatic input selection then shows
the cable that still carries a signal, the display owner's. The rule behind every handover rule is
that Beamer never leaves on a display that nobody sees.

What stays elsewhere: syncing machine records and shared entries across the cluster, their stamps,
and keeping them across restarts are the store's (#14, `spec/store.md`). What the tray menu shows
for each shared display and what choosing in it asks are `viewmodel.tray.display-menu`,
`viewmodel.tray.display-choices` and `viewmodel.tray.display-choose`; the button that starts
identify displays is `viewmodel.identify.button` and `viewmodel.identify.press`; how a number is
drawn and placed is `ui.identify.card`, `ui.identify.place` and `ui.map.footprint.numbers`. Where
a machine's displays sit on the crossing map is `crossing.map.displays.record`,
`crossing.map.displays.fill` and `crossing.map.displays.apart`; identify displays numbers the
displays that record declares. The link that carries a request to identify displays is
`spec/link.md`'s.

Dropped, so no rule here (#11): phone peers, Linux-only code (the X11 and layer-shell helpers that
showed the numbers on Linux), and any compatibility with the Python apps and older Beamers (#14),
so the `display1` word, the audio capability that `display_*` messages rode, and the
`identify_screens` capability that older peers lacked all go. Nothing in this domain comes later.

Every rule is `ci` unless it says otherwise.

## Display keys

A shared display is recognised across machines by its display key, which comes from the display's
EDID and is the same on every machine it is cabled to.

r[display.key.edid]
A display key is the display's EDID manufacturer id as three capital letters, five bits each with
1 for A, followed by its EDID product code as four upper-case hexadecimal digits: manufacturer id
0x2163 with product code 0x2716 gives `HKC2716`. A display whose manufacturer id does not give
three letters from A to Z, such as 0, has no display key.

r[display.key.pc-name]
On a PC, a display's display key is read from the device name Windows gives it: the part between its
first and second `#`, in capitals, when that part is three letters and four hexadecimal digits, so
`\\?\DISPLAY#HKC2716#7&1bd4b3b4&0&UID260#{e6f0…}` gives `HKC2716`. A device name without such a
part gives the display no display key.

## A machine's displays

What each machine tells the cluster about the displays it could share, in its machine record.

r[display.record.listed]
A machine's machine record lists each of its displays that is not built in and has a display key,
with its display key, its name and whether it is on. On a PC, a display that is still connected
but turned off is listed, as off, whoever turned it off. On a Mac, a display that is turned off is
listed only when Beamer turned it off (`display.record.mac-off-kept`): macOS gives no other display
that is off, so one the user or another program turned off is not listed.

> r[display.record.same-key]
> When two or more of a machine's displays have the same display key, none of them is listed. They
> are never shared displays, and Beamer never turns them off or on.
>
> - parity: Python's Mac listed two displays of one model as one display under their display key, and turning that display off turned off whichever of them macOS gave first (`mac_app/shared_display_mac.py:81-92,117-123`), while Python's PC left them alone, as here (`core/display/part.py:166-167`) (a display key cannot tell the two apart on the other machines, so which of them a display owner names is unknowable, and `dev/display/DESIGN.md`'s known limits leave them unmanaged)

r[display.record.limits]
A peer's list of displays counts only when it has at most 16 displays, each with a display key
of `display.key.edid`'s form and an on or off. A name longer than 128 characters is cut to its first
128, and a name that is missing or not text reads as empty. A list that breaks these is ignored,
and the displays this machine knows that peer has stay as they were.

r[display.record.change]
When this machine's displays change, by being connected, removed, turned off or turned on, by
Beamer or by anything else, its machine record shows the change within 2 s. Finding the same
displays again writes nothing.

r[display.record.capability]
A machine whose OS offers no call to turn a display off and on reports that capability as
unsupported here. It has no shared displays, and its machine record carries no list of displays,
not even an empty one. Its peers have no list of displays for it, whatever its machine record
carries, so a display whose display owner it is stays off on them (`display.handover.owner-away`).

> r[display.record.mac-off-kept]
> On a Mac, a display Beamer turned off stays listed, as off and under the name it had while on,
> though macOS no longer lists it, also after Beamer restarts. It is dropped from the list once it
> is on again, by Beamer or otherwise, or once macOS no longer gives the display it turned off the
> same display key.
>
> - tier: machine (only a real Mac turns a display off)

> r[display.record.mac-coming-on]
> On a Mac, a display Beamer turns back on is listed as on from that moment, for up to 15 s while
> macOS brings it back, and can be turned off again during that time.
>
> - tier: machine (only a real Mac turns a display back on)

## Shared displays

r[display.shared.with]
A display of this machine is a shared display with a peer when that peer has a link up with this
machine, can turn its displays off and on, and lists a display with the same display key in its
machine record. A display no such peer lists is not a shared display.

r[display.shared.from-record]
Which displays a peer has comes only from that peer's own machine record. A list of displays that
says it is another machine's is ignored, and makes no display a shared display.

r[display.shared.chosen]
A display of this machine that has a display owner stays one of its shared displays while no peer
that has it has a link up, so its display owner can still be chosen here while the display owner
is away.

## The display owner

> r[display.owner.entry]
> Each display key has at most one display owner, a shared entry naming a machine or none. A
> display key that was never given one has none. A display owner chosen on any machine becomes
> the display owner on every machine of the cluster, the newest choice winning.
>
> - model: handover

> r[display.owner.later-wins]
> A display owner chosen on a machine whose clock is behind the stamp of the display owner it
> replaces still replaces it, on every machine.
>
> - model: handover

> r[display.owner.same]
> Choosing for a display the machine that already is its display owner, or none when an earlier
> choice on any machine gave it none, changes nothing and writes nothing. Choosing none for a
> display key that was never given a display owner is a new choice: it writes none as the display
> key's display owner, which every machine of the cluster then has, as `display.owner.entry` says.
>
> - model: handover

> r[display.owner.valid]
> A display owner counts only when its display key has `display.key.edid`'s form and the machine it
> names is a machine id. A choice or an entry that breaks these is ignored, and every other display
> owner is taken as it is.
>
> - parity: Python ignored a whole `display_route` message, every display owner in it, when one entry broke these or when it carried more than 32 (#14: each display owner is its own shared entry, so one bad entry does not keep out the rest)

> r[display.owner.kept]
> A display key keeps its display owner, or none, for good: across restarts of every machine, and
> while no machine has the display. No change removes a display owner; a new choice replaces it.
>
> - model: handover

## Handover

Each machine decides for itself, for each display it lists that is a shared display or has a
display owner, whether it is on. Whatever the decision, Beamer never turns a display back on that
it did not turn off (`display.handover.own-only`).

> r[display.handover.not-managed]
> A display with no display owner is on: Beamer turns it back on if it turned it off.
>
> - model: handover

> r[display.handover.owner-here]
> A display whose display owner is this machine is on: Beamer turns it back on if it turned it off.
>
> - model: handover

> r[display.handover.other-owner]
> A display whose display owner is a peer that has a link up with this machine and lists the
> display is off here: Beamer turns it off.
>
> - model: handover

> r[display.handover.owner-away]
> A display whose display owner is another machine that has no link up with this machine, or whose
> list of displays this machine does not have, as for a machine that cannot turn its displays off
> and on, is off here: Beamer turns it off, or keeps it off, however long the display owner is
> away. The display keeps showing the display owner's cable, so turning it back on here would make
> a display nobody sees. It comes back on only when the display owner changes to this machine or
> to none, or when the display owner, with a link up again, does not list it.
>
> - model: handover

> r[display.handover.owner-lacks]
> A display whose display owner is a peer that has a link up with this machine and whose list of
> displays lacks it, an empty list included, is on here.
>
> - model: handover

> r[display.handover.own-only]
> Beamer turns back on only a display it turned off itself, and remembers which those are across
> restarts. A display turned off by the user or by another program stays off whatever its display
> owner, and a display Beamer turned off that is found on while it should be on is no longer
> counted as turned off by Beamer.
>
> - model: handover

> r[display.handover.last-display]
> Beamer never turns off the last display of a machine that is on, a built-in display counted, as
> on a Mac with its lid closed. Turning it off counts as failed.
>
> - model: handover

> r[display.handover.retry]
> When turning a display off or on fails, Beamer tries again 30 s later, and not sooner, for as
> long as the decision stands.
>
> - model: handover

> r[display.handover.again]
> A display that comes back on while it should be off, as when the Mac restarts or its cable is
> plugged in again, is turned off again within 2 s of coming back, and at Beamer's start.
>
> - model: handover

r[display.handover.quit]
Quitting Beamer turns no display back on: each display it turned off stays off.

r[display.handover.log]
Each time Beamer turns a display off or on, it logs one line naming the display key, on or off,
the reason (no display owner, this machine is the display owner, the display owner does not list
it, another machine is the display owner, the display owner is away) and, when it failed, that it
failed.

> r[display.handover.moves]
> With the shared display's automatic input selection on, making another machine its display owner
> moves the display to that machine's cable, and the machine that was its display owner loses it.
>
> - tier: human (which cable a display shows is judged by eye)
> - do: With GN10 showing the Mac, choose lebig in the Mac's menu for GN10; then choose This Mac in the PC's tray menu for GN10
> - see: Within about 10 s GN10 shows the PC and the Mac's windows leave it; after the second choice it shows the Mac again within about 10 s and the PC's windows leave it

## Turning a display off

How each OS turns a display off and back on, as seen on the machine.

> r[display.turn.mac-off]
> On a Mac, a display Beamer turns off leaves the displays macOS shows until Beamer turns it back
> on, the user logs out, the Mac restarts or the display's cable is plugged in again. Nothing is
> saved to the display configuration macOS keeps.
>
> - tier: machine (only a real Mac turns a display off)

> r[display.turn.pc-off]
> On a PC, a display Beamer turns off leaves the displays Windows shows, without the change being
> saved to the display configuration Windows keeps. The displays left keep their places, moved
> together so that one of them sits at the origin and is the main display.
>
> - tier: machine (only a real PC turns a display off)

> r[display.turn.pc-on]
> On a PC, turning a display back on restores the extended display configuration Windows keeps for
> the connected displays, so the display comes back where it was, as the main display when it was
> one. Any other connected display turned off by hand comes back on with it.
>
> - tier: machine (only a real PC turns a display back on)

## Identify displays

Which numbers the displays get, which machines show them and for how long.

r[display.identify.numbering]
A machine's displays are numbered from 1, in the order of where each sits on that machine: by its
top edge from top to bottom, then by its left edge from left to right, then by display id. The
displays are those its machine record declares (`crossing.map.displays.record`). A display moved
apart on the crossing map (`crossing.map.displays.apart`) keeps its number.

r[display.identify.unknown]
A machine whose displays are not known on the crossing map (`crossing.map.displays.valid`) is
numbered as one display, number 1.

r[display.identify.here]
Identifying displays on a machine shows each of its own displays' numbers on that display.

r[display.identify.peers]
Identifying displays on a machine asks each peer it has a link up with, once, to show its
numbers, a peer kept off the crossing map included. It asks no other machine.

r[display.identify.peer-numbers]
The request to a peer names each display that peer's machine record declares
(`crossing.map.displays.record`), by display id, with its number as `display.identify.numbering`
gives it, a peer kept off the crossing map included. The request to a peer whose displays are not
known (`display.identify.unknown`) names one display, display id `0`, with number 1.

> r[display.identify.request-valid]
> A request to identify displays counts only when it names 1 to 16 displays, each display id 1 to
> 64 characters from ASCII letters (`A` to `Z`, `a` to `z`), digits `0` to `9`, `_`, `.`, `:` and
> `-`, each with a whole number from 1 to 16, and no two with the same number. Any other request,
> one with `é` in a display id included, is ignored and shows no number.
>
> - fuzz: link-frame

r[display.identify.from-peer]
A request to identify displays from a peer shows numbers on this machine's displays, and this
machine asks no other machine. A request from a machine that is not a peer of this machine shows
nothing.

r[display.identify.no-driving]
Identifying displays, here or at a peer's request, takes no machine, lets none go and injects no
input.

r[display.identify.renumber]
Whether identify displays was asked here or by a peer, when the displays the numbers name are
exactly this machine's current displays, each shows the number given for it. When they are not, as
when a display was attached or removed since the asking machine numbered them, those numbers are
dropped and every display this machine has now shows a number, from `display.identify.numbering`
over its current displays.

r[display.identify.duration]
The numbers show for 3 s from the request, then all go at once.

r[display.identify.repeat]
A request that arrives less than 3 s after the last request this machine acted on, from its own
identify displays or from a peer, is ignored: the numbers showing stay, with the same numbers, and
still go 3 s after that last request. A request 3 s or more after it is acted on, and shows the
numbers afresh. A request is acted on from its arrival, also when no number could be shown, and
the numbers going away early does not change when the next one is acted on.
