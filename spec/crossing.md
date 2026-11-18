# Crossing

When and where input crosses from one machine to another: zones on the edges of a machine's
displays and the crossing map they are derived from, push pressure and its guards against
accidental crossings, arrival grace and the landing point, every crossing method (a zone push,
the shortcut, a jump key, a shake, the menu), the crossing pause and the full-screen pause,
pointer hiding, profiles and own zones. These are F5–F9, F16 and F17 of #11's inventory, which
all ship, for any number of machines.

The rules come from #11's outcomes, the store decided in #14, the sans-I/O crossing engine of #16
and ADR 0004, the words of #20, and the Python app at `archive/python` (`core/return_edge.py`,
`core/ways.py`, `core/crossing_state.py`, `core/profiles.py`, `core/shake.py`,
`core/idle_pointer.py`, `mac_app/crossing.py`, and the Mac and Windows apps that carry them out),
mined through its tests in `spec/ledger/crossing.txt`. Crossing decides that input crosses, to
which peer and where it lands; the platform backend declares the displays, their edges and the
pointer's position, and crossing computes nothing from the OS itself.

What stays elsewhere: the take, its answer, refusals, let go, pass on, send home, the reach and
out of reach are `spec/driving.md`'s; waking a peer with no link is `spec/discovery.md`'s; what a
page, notice or alert says is `spec/viewmodel.md`'s; how the crossing map, the edge glow and the
pointer overlay are drawn is `spec/ui.md`'s; capturing and injecting keys and pointer moves is
`spec/input.md`'s; the store that carries the crossing map and the profiles is `spec/store.md`'s;
the settings file is `spec/settings.md`'s.

Dropped, so no rule here (#11): phone peers, Linux-only code (portal and X11 zones), the Ink and
Warp effects, the portable build (F23), any compatibility with the Python apps and their settings
files, and uncalled code. The crossing effects (F30) and the notch island and beam (F31) come
later.

Every rule is `ci` unless it says otherwise. Behaviour across three or more machines is tested
through `beamer-sim`.

## Zones

A zone is a stretch of one of this machine's displays' edges that leads to one peer. Positions are
in this machine's display coordinates, as its platform backend declares them: points on a Mac,
pixels on Windows. Below, "px" means one unit of those coordinates. Positions may be negative. A
display's edge runs from its first pixel to its last, both included; a fraction along an edge is the
pixel's distance from the edge's first pixel divided by the edge's length in pixels, top to bottom
or left to right, kept within 0 and 1. "Pushable" means that a push there builds push pressure
toward the zone's peer; elsewhere the edge is a wall, where the pointer stops and nothing crosses.
Which zones a machine has comes from the crossing map (Zones from the crossing map, below); which
of them are armed at a given moment is Armed zones'.

> r[crossing.zone.edge-wall]
> A zone that names none of this machine's displays is pushable wherever the pointer is on one of
> this machine's displays, within 1.5 px of that display's edge on the zone's edge, and no display
> of this machine lies just beyond that display's edge at the pointer's position. So a display
> whose edge stops short of the others' is pushable along its own edge where nothing lies beyond
> it, and an edge between two of this machine's displays never is: there the pointer passes onto
> the other display and builds nothing.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones counted only the edge's outermost pixel row or column; only the Mac's home zones counted 1.5 px, because a Mac reports fractional positions (#16: one crossing engine for every platform; the Mac's at home)

> r[crossing.zone.display]
> A zone that names one of this machine's displays is that display's edge only. It is pushable
> while the pointer is on that display within 1.5 px of that edge, whether or not another display
> of this machine lies beyond it, so a zone can sit on the edge between two of this machine's
> displays.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones took the pointer as at a zone naming a display wherever it was within that display's stretch along the edge and no more than 1.5 px inside its edge, on that display or on another beyond it; only the Mac's home zones needed it on that display (#16: one crossing engine for every platform; the Mac's at home)

> r[crossing.zone.unknown-display]
> A zone that names a display this machine does not have is never pushable: neither its edge nor
> its corner is replaced by the edge or corner of all this machine's displays.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones took an edge or thirds zone naming a display the machine did not have as a zone on the whole edge of all its displays, while their corner zones, and every home zone on the Mac, were never pushable (#16: one crossing engine for every platform; never crossing where the crossing map put no zone)

r[crossing.zone.overshoot]
When one pointer move takes the pointer from inside the display a zone names across that display's
edge on the zone's edge, the pointer has reached the zone at the point where the move's path meets
the edge: it is held at edge there, and that point decides the zone's thirds and contact. The
point the move started from is the pointer's last known position, or, when there is none, its
position less the move.

r[crossing.zone.overshoot-miss]
A pointer move whose path meets the line of a zone's edge beyond either end of the display the zone
names, or that starts outside that display, does not reach the zone: the pointer is not held at edge
and no push pressure builds.

> r[crossing.zone.thirds]
> A thirds zone is pushable only within its thirds of the edge: the start (the top third of a left
> or right edge, the left third of a top or bottom edge), the middle and the end. The thirds are
> measured along the edge of the zone's display, or, for a zone that names no display, of the
> display the pointer is on, never along the edge of all this machine's displays, and split at the
> fractions 1/3 and 2/3: a point at exactly 1/3 is in the middle third, one at 2/3 in the end third,
> and the edge's last pixel in the end third. The rest of that edge is a wall.
>
> - parity: Python's home zones on the Mac measured these fractions over the edge's length less one pixel, while its zones on every driven machine, the Mac included, and the PC's home zones measured them over the whole length (#16: one crossing engine for every platform; the whole length)

r[crossing.zone.display-contact]
On the crossing map, a display of this machine and a display of a peer touch along an edge when an
edge of one lies on an edge of the other for a length above 0, and touch at a corner when they
share only a corner. Each pair of touching displays is a contact of its own, and a contact along an
edge can give a zone on this display's edge that names the peer's display it touches and lands on
that display's facing edge. A peer whose displays touch two of this machine's displays, as a peer
above two displays next to each other does, has a contact with each and so a zone on each; an edge
touching two of the peer's displays has a zone to each.

r[crossing.zone.contact-span]
A zone from a contact covers the stretch of this machine's display edge that the peer's display
touches, and leads to the stretch of the peer's display edge, on the edge facing this display, that
this display touches, each stretch taken as fractions of its own display's edge. A peer display
spanning 25 to 125 against this machine's display spanning 0 to 100 gives a zone over 0.25 to 1.0
of this display's edge, leading to 0.0 to 0.75 of the peer display's edge.

r[crossing.zone.contact]
A zone with a contact is pushable only within the contact's stretch of its edge; elsewhere on the
edge the pointer passes or meets a wall. A contact that reaches past an end of the edge by no more
than 10⁻⁹ of the edge's length is taken as ending at that end.

> r[crossing.zone.corner]
> A corner zone is pushable only in its corner's box: within 8 px of both of the corner's edges, on
> the zone's display, or, for a zone that names no display, on the display the pointer is on, while
> the pointer is against at least one of those two edges. For a zone that names a display, against
> means within 1.5 px of the edge, even where other displays of this machine lie beyond both; for
> one that names none, an edge with no display of this machine just beyond it.
>
> - parity: Python's home zones on the Mac measured the box of a corner zone naming no display from the corner of the rectangle around all its displays, which no display reaches when they are staggered (#16: one crossing engine for every platform; that of its zones on every driven machine and the PC's home zones)

r[crossing.zone.corner-diagonal]
Only a diagonal push builds pressure against a corner zone: a move outward across both of the
corner's edges at once counts its whole length, and a move outward across only one of them, along
an edge, or diagonally away from the corner counts nothing toward the corner, though it may push an
edge or thirds zone there.

r[crossing.zone.corner-overshoot]
When one pointer move starts in a corner zone's box on the zone's display and leaves that display
across one or both of the corner's edges, the pointer is held at edge where the move's path first
meets one of them, kept on the display, and the move counts toward the corner as a diagonal push.

> r[crossing.zone.corner-edge]
> A corner zone crosses through one of its corner's two edges, the one it names. A corner zone from
> a contact at a corner names the corner's left or right edge. A chosen corner names the edge the
> peer's display touches when that edge is one of the corner's two, and otherwise the corner's left
> or right edge; when the peer's display moves to another edge on the crossing map, the edge it
> names follows.
>
> - parity: Python's Mac, at home and driven, crossed every chosen corner through its left or right edge, whatever edge the peer was on (#16: one crossing engine for every platform; the PC's)

r[crossing.zone.notch]
On a Mac, the notch zone is pushable only within 1.5 px of the top of this machine's displays and
between the notch's two ends, both included, as the platform backend declares them at that push,
whatever edge its peer is at on the crossing map: on a 1512 px wide display with the notch from 656
to 856 px, a push up at 756 px is in the notch zone and pushes at 200 px or 1300 px are not. A notch
declared late, or moved by a change of displays, applies from the next push. While the backend
declares no notch, or the display with the notch is not at the top of this machine's displays,
there is no notch zone.

> r[crossing.zone.order]
> Where zones overlap at the pointer, a push goes to the first zone that keeps the pointer held at
> edge or crosses, in this order: corner zones, whole-edge zones, thirds zones, then the notch. A
> diagonal push in a corner's box goes to the corner zone, even over another peer's edge zone
> there, and a straight push in that box goes to the edge or thirds zone under it.
>
> - model: driving

> r[crossing.zone.leads-to]
> A push through a zone crosses to that zone's peer, and asks nothing of any other peer. Peers whose
> zones share one edge, by thirds or by contact, each receive only the pushes through their own
> zones. However many zones lead to one peer, on several displays or several edges of one display,
> each of them crosses.
>
> - model: driving

### Merged moves at a zone

A driven machine receives pointer moves merged with their steps, as `link.input.receive-merge`
says.

r[crossing.zone.merged-steps]
A merged pointer move of at most 4 steps whose travel could bring the pointer to a display's edge
or an armed zone, or that bends or reverses while a button is down, reaches the zones step by step,
each with the pointer where the steps before it left it. The pointer is held at edge, crosses and
ends exactly as with the same moves delivered one by one.

r[crossing.zone.merged-long]
A merged pointer move of more than 4 steps whose travel could bring the pointer to a display's edge
or an armed zone reaches the zones as at most two moves: its steps up to and including the first
that would take the pointer off every display, then the rest. A push through an armed zone within
it still crosses.

r[crossing.zone.merged-cross-ends]
Once a step of a merged pointer move crosses, the rest of that move's steps are neither fed to the
zones nor injected.

r[crossing.zone.clip]
On a driven machine, a pointer move whose path meets an armed zone is injected only up to the point
where it meets it, so the pointer never passes through a zone onto a display beyond it.

## The crossing map

Each machine has a footprint on the crossing map, and its displays fill it. Footprints are placed
and sized in map units. Each footprint, with its displays' offsets and its name, is one shared entry
in the store (#14), so the crossing map, made of them, is the same on every machine; this machine's
crossing map is the part of it this machine shows and derives its zones from
(Which machines are on the crossing map, below). An inactive peer is a peer that is not in use,
kept off the crossing map, or dormant (`pairing.dormant.kept`); every other machine on the crossing
map is present.

> r[crossing.map.footprint]
> Each machine on the crossing map has a footprint: a rectangle placed in whole map units, whose
> width and height are each at least 1. This machine's footprint is 160 by 100 map units until it
> is resized. A peer placed by default takes the size of this machine's footprint. A change that
> would give a footprint a width or height below 1 is refused and changes nothing.

r[crossing.map.displays.fill]
A machine's displays fill its footprint as they are placed on that machine: the rectangle bounding
all its displays is stretched, across and down separately, to the footprint's width and height, and
each display keeps its share of it. A machine whose displays are not known fills its footprint as
one display.

r[crossing.map.displays.apart]
A display moved apart from its machine sits where `crossing.map.displays.fill` puts it, shifted by
its offset, a whole number of map units across and down. Moving it changes neither its machine's
footprint nor the display's reported position and size, and every machine places that display, and
derives its zones, from the same offset. A display reported at (100, 0), 200 by 100, in a footprint
at (10, 20), 300 by 100, whose displays span 300 by 100, sits at (135, 10) with an offset of
(25, -10).

r[crossing.map.displays.together]
Rejoining a display to its machine removes its offset, so it sits again where
`crossing.map.displays.fill` puts it.

> r[crossing.map.displays.offset-valid]
> An offset counts only when it names a display its machine has and both its numbers are whole, and
> a machine has at most 16 of them. An offset that does not count is ignored, and the rest of the
> crossing map is taken as it is.
>
> - parity: Python refused the whole crossing map a peer sent when one offset named a display its machine did not report or was not whole, and removed the offsets of displays a machine no longer reported when its displays changed (#14: each footprint is a shared entry, so one bad offset does not keep out the rest)

> r[crossing.map.displays.record]
> A machine's displays on the crossing map are those in its machine record, as its platform
> backend declares them: for each, an id and a rectangle in that machine's own coordinates. A
> machine writes its displays to its machine record when they change; reporting the same displays
> again writes nothing. A change to the crossing map arriving from another machine never changes
> this machine's displays, and a newer record of a third machine's displays replaces the older one.
>
> - model: store
> - parity: Python kept every machine's display rectangles inside the shared crossing map, so a change of displays restamped the whole map and sent it, and a machine that took a map with other rectangles for its own displays corrected them and sent the map again (#14: a machine's displays are its machine record)

> r[crossing.map.displays.valid]
> A machine's displays are placed on the crossing map only when it declares 1 to 16 of them, each
> id is 1 to 64 ASCII characters, and each width and height is a whole number of at least 1.
> Otherwise its footprint is filled as one display, as for a machine whose displays are not known.
>
> - parity: Python refused the whole crossing map a peer sent when any machine's display rectangles broke these limits (#14: displays arrive in each machine's own record, so one machine's bad record does not keep out the others' places)

### Which machines are on the crossing map

r[crossing.map.members.this]
This machine is always on its crossing map. While the crossing map has no footprint for it, it is
placed at (0, 0), 160 by 100 map units, before any contact is found or zone derived. That footprint
is written only as `crossing.map.place.default-written` says.

r[crossing.map.members.peers]
Every peer of this machine is on its crossing map, an inactive peer included. A peer whose pairing
is removed stays on this machine's crossing map only as a third party
(`crossing.map.members.third-party`).

> r[crossing.map.members.third-party]
> A machine that is neither this machine nor one of its peers is on this machine's crossing map only
> while a peer of this machine that is in use, not kept off the crossing map and not dormant has it
> in its paired list, and only while the crossing map carries a name for it. Every other footprint
> in the crossing map is kept off this machine's crossing map: it is not shown, and counts here for
> no overlap, contact or snap point. Keeping a footprint off writes nothing: it stays in the
> crossing map as it was, for the machines that show it, and a change this machine makes to the
> crossing map keeps it.
>
> - model: store
> - parity: Python removed from its crossing map every footprint it kept off, an unpaired peer's included, and sent the map, so its peers removed it too, even those that still showed it (#14: each footprint is a shared entry, and a machine leaves the crossing map only when the cluster forgets that machine, so keeping a footprint off is a filter that writes nothing)

r[crossing.map.members.name]
The crossing map carries a name with a machine's footprint, without leading or trailing blanks. A
name that is blank, or longer than 64 characters, is dropped, and the footprint is kept without it.

> r[crossing.map.members.forgotten]
> The footprint of a machine that no machine in the cluster lists as a peer reads as unset, as the
> store reads every shared entry naming a forgotten machine (#14): it is on no machine's crossing
> map and counts for no overlap, contact or snap point anywhere. No machine writes its removal, so
> when the machine is paired again its footprint reads as set again, where it was, unless another
> machine was placed over it meanwhile: at the first link after that pairing, a footprint of the
> machine paired again that overlaps another present machine's counts as none, and the machine is
> placed by default as one with no footprint (`crossing.map.place.default-written`), so the machine
> placed meanwhile keeps its place and its zones.
>
> - model: store
> - parity: Python removed an unpaired peer's footprint from its crossing map at once (`core/peerlist.py:209-210` calling `ways.prune_layout`, `core/ways.py:169-185`), so a peer paired again was always placed by default (#14: a footprint is a shared entry that reads as unset while its machine is forgotten and is never written as removed, so a machine paired again keeps its place unless another machine was placed over it meanwhile)

> r[crossing.map.members.retain]
> A change to the crossing map arriving from another machine replaces only the footprints it
> changed, as `crossing.map.shared.newest` orders them. Every other footprint on this machine's
> crossing map, an inactive peer's included, stays as it was. No change removes a footprint: a
> machine's footprint leaves the crossing map only when the cluster forgets that machine
> (`crossing.map.members.forgotten`).
>
> - model: store
> - parity: Python stamped the whole crossing map, took every footprint a newer arriving map carried, even one it had not changed, and kept the footprint it had for a machine the arriving map lacked (`ways.retain_held_peer_tiles`, `core/ways.py:145-164`) (#14: each footprint is its own shared entry, the newest stamp winning entry by entry, so a change carries only the footprints it changed and no footprint goes missing)

### Where a machine sits

r[crossing.map.place.kept]
A footprint on the crossing map stays where it was put, with its size, its displays' offsets and
its name, until it is dropped, resized or recentred, on this machine or another, or its machine
leaves the map, even when it touches no display of this machine. Placing another machine never moves
it, since a machine is placed by default only while it has no footprint, or one that counts as none
(`crossing.map.place.default-written`), except that a restored peer may re-centre the row it joins
(`crossing.map.place.restored`), and changes the zones that lead to it only as
`crossing.derive.first-zone` says.

r[crossing.map.facing]
A peer faces the edge of this machine on which its footprint first touches one of this machine's
displays, taking this machine's displays and then the peer's in order of their reported positions,
top to bottom, then left to right, whatever edge it faced before. A peer whose footprint touches
this machine's displays only at a corner, or not at all, faces no edge. A peer that loses its
footprint keeps the edge it last faced, as a peer paired again does (`pairing.peers.replace-keeps`).

r[crossing.map.place.facing]
A peer with no footprint that faces an edge of this machine gets a footprint the size of this
machine's, flush against that edge. The peers facing one edge with no footprint sit in a row centred
on the edge, touching each other, in the peer list's order. A footprint so placed that overlaps a
present machine's footprint moves to the nearest free snap point (`crossing.map.snap.choice`, with
no radius), so it shares that edge or touches a corner.

r[crossing.map.place.unplaced]
A peer with no footprint that faces no edge, such as one just paired, gets a footprint the size of
this machine's, centred across this machine's footprint and flush under the lowest bottom edge of
the other present machines' footprints, or under this machine's footprint when no other is on the
map. Several such peers sit in a row centred there, touching each other, in the peer list's order.
Each then moves to the nearest free snap point (`crossing.map.snap.choice`, with no radius), so that
it touches this machine's footprint.

> r[crossing.map.place.first-peer]
> While the crossing map has a footprint neither for a Mac nor for a peer it has just paired, the
> Mac places that peer flush against the right edge of its own footprint, centred on it, in place of
> the place `crossing.map.place.facing` or `crossing.map.place.unplaced` would give it.
>
> - parity: Python placed it so whenever the Mac's saved crossing map had no footprint at all and the peer was its only peer (`core/crossing_state.py:328`), whatever the peer's own crossing map had (#14: each footprint is a shared entry, written by default only for a machine with no footprint, so a Mac places its first peer so only while that peer has no footprint either)

> r[crossing.map.place.default-written]
> A machine the crossing map has no footprint for is placed by default: this machine by
> `crossing.map.members.this`, a peer by `crossing.map.place.first-peer`,
> `crossing.map.place.facing` or `crossing.map.place.unplaced`. Until a footprint for it is
> written, it is only shown there, and showing it writes nothing. A default placement is written
> when a link comes up, once its two machines have exchanged their stores (#14), the first link
> after a pairing included (`pairing.peers.at-once`), and only for a machine of those two that has
> no footprint. At the first link after a pairing, a machine that the other had forgotten before
> that pairing (`crossing.map.members.forgotten`) counts as having no footprint when its footprint
> overlaps another present machine's on the other's crossing map, and is placed by default as if it
> had none; when each of the two counts so for the other, the one the second case below names keeps
> its footprint. So a default placement never moves, replaces or removes a footprint, but one that
> counts as none. Of the two machines:
>
> - when one has a footprint and the other has none, the one with a footprint writes the other's
>   where its own crossing map places it by default;
> - when neither has one, the Mac writes both, or, when both or neither are Macs, the one whose
>   machine id is greater, compared as bytes: the other's footprint where its crossing map places it
>   by default while neither footprint is written, so that `crossing.map.place.first-peer` applies
>   on a Mac, and its own where `crossing.map.members.this` places it;
> - when both have one, nothing is written: each footprint stays where it is
>   (`crossing.map.place.kept`), and each machine derives its zones to the other from there.
>
> The other machine writes nothing for that link. A written default placement reaches every machine
> (`crossing.map.shared.one`), so two machines that pair end with one map, each facing the other's
> facing edge (`crossing.map.shared.facing`). Two writes of one machine's footprint made before
> either reaches the other, as when a machine with no footprint links to two machines at once, or a
> default placement and a drop of the footprint it placed, are ordered as any two changes to that
> footprint are (`crossing.map.shared.newest`). For example:
>
> - A Mac and a machine that is not a Mac pair, neither with a footprint: on both machines' maps the
>   other machine ends flush right of the Mac, centred on it, with its zone to the Mac on its left
>   edge.
> - Two Macs A and B pair, neither with a footprint, A's machine id the greater: A writes its own
>   footprint at (0, 0) and B's flush right of it at (160, 0), centred on it, so A's zone to B is on
>   A's right edge and B's zone to A on B's left edge.
> - A Mac B and a PC pair first, then a Mac A with no footprint pairs the PC: the PC writes A's
>   footprint centred under the PC's (`crossing.map.place.unplaced`), A's zone to the PC is on A's
>   top edge, and B's and the PC's footprints stay where they were.
> - A PC P1 at (0, 0) and a PC P2 flush under it, 160 by 100 each, are paired and never changed,
>   then a PC X with no footprint pairs P1: P1 writes X's footprint at (-160, 100), touching P1's
>   bottom-left corner and P2's left edge, and P1's and P2's footprints, and their zones to each
>   other, stay as they were.
> - P1 then removes X, which no other machine lists, and pairs a PC Y with no footprint, which P1
>   writes at (-160, 100), where X was. When P1 pairs X again, X's footprint overlaps Y's, so it
>   counts as none: P1 writes X's footprint at (160, 100), touching P1's bottom-right corner and
>   P2's right edge, and Y keeps its place and its zones.
>
> - model: store
> - parity: Python wrote no default placement but a Mac's first peer on an empty crossing map, written with the Mac's own displays: each machine kept its own default until a footprint for that machine came from elsewhere, so two paired machines that are not Macs each placed the other below itself, and each one's zone to the other was on its own bottom edge (#14: each footprint is a shared entry and converges, so one machine of the two a link joins writes a default placement, and only for a machine with no footprint)

r[crossing.map.place.inactive]
An inactive peer's footprint stays on the crossing map where it was, but never blocks another
machine: machines are placed, dropped and snapped as if it were not there, they may overlap it, and
it is in no overlap.

> r[crossing.map.place.restored]
> When a peer is restored, or in use again, and its footprint overlaps a present machine's, it
> moves. When the peer faces an edge, it joins the row of present footprints touching that edge: the
> row, in its order along the edge with the peer last, touching end to end, is centred on the edge,
> provided every footprint in it still touches that edge, none overlaps another present footprint,
> and each keeps the contacts it had with the footprints of machines beyond the row. Otherwise no
> other footprint moves, and the peer's footprint moves to the nearest free snap point
> (`crossing.map.snap.choice`, with no radius). The move is a change to the crossing map that
> reaches every machine. A peer restored while it is not in use moves nothing.
>
> - model: store

r[crossing.map.recentre]
Recentring a peer puts its footprint flush against this machine's footprint, centred on the edge it
touches, or, when it touches no edge, on the edge whose centred place is nearest its footprint, in
the order left, right, top, bottom on a tie. When the centred place overlaps another present
machine's footprint, the footprint goes instead to the nearest free snap point from there
(`crossing.map.snap.choice`, with no radius). It keeps its size, name and displays' offsets, and
the move is taken as a drop without `crossing.map.snap.drop-radius`.

> r[crossing.map.place.chosen-edge]
> Choosing an edge for a peer whose footprint is not against it places the footprint centred
> against that edge of this machine's footprint, or at the nearest free snap point
> (`crossing.map.snap.choice`, with no radius) when another present machine's footprint is there,
> and its zones are derived there. Choosing the edge the peer's footprint is already against
> changes nothing on the crossing map.
>
> - parity: Python also kept, per peer, the edge it faced with a stamp and the id of the machine that set it, written whenever that edge changed (#14: a footprint is a shared entry, and the edge a peer faces is read off the crossing map)

### Snapping

r[crossing.map.snap.points]
The snap points for a footprint placed against this machine's footprint all lie flush outside it.
Along each of its four edges there are seven: centred on the edge (the centred point), flush with
either end of the edge, and centred on each of the marks at 0, 1/3, 2/3 and the whole of the edge's
length. Four more touch it only at one of its corners, diagonally. Each point is rounded to the
nearest whole map unit, a half to the even one.

r[crossing.map.snap.free]
A snap point is free when the footprint there would overlap no footprint on the crossing map other
than its own and this machine's. Inactive peers' footprints, and footprints kept off the map, do
not count.

r[crossing.map.snap.choice]
Snapping a footprint with a radius r moves it to a free snap point: the nearest of those within r of
where it is, or within 1.5 r for a centred point, distance taken in a straight line between top-left
corners; or, when none is that near, the nearest free snap point of all. Nearest means the least
horizontal plus vertical distance, then a centred point before another, then the least horizontal
distance, then the least vertical one, and then the first in this order: the edges left, right, top
and bottom in turn, along each the points centred on the marks at 0, 1/3, 2/3 and the whole, then
the centred point, then flush with the edge's start and with its end; then the corners top left, top
right, bottom left and bottom right. With no free snap point the footprint stays where it is.

r[crossing.map.snap.drop-radius]
A peer's footprint dropped within the snap radius of a free snap point moves to the one
`crossing.map.snap.choice` picks with that radius, unless the crossing map with it there would be
refused (`crossing.map.drop.overlap`, `crossing.map.drop.crowded`), in which case it stays where it
was dropped. The snap radius is 12 points of the crossing map as it is drawn, so 18 points for a
centred point. This machine's own footprint never snaps.

### Dropping a footprint

r[crossing.map.drop.inactive]
Dropping, moving or resizing an inactive peer's footprint is refused, and the crossing map stays as
it was.

r[crossing.map.drop.overlap]
A peer's footprint dropped where one of its displays overlaps a display of another present machine
moves to the snap point `crossing.map.snap.choice` picks with a radius of 12 map units, and the
drop is taken there. The drop is refused, leaving the crossing map as it was, when that point is
where it was dropped or the crossing map with it there would still be refused. This machine's own
footprint dropped where one of its displays overlaps a present machine's is refused. An overlap
between two other machines refuses no drop.

r[crossing.map.drop.crowded]
A drop that would leave four or more peers touching one edge of one of this machine's displays
moves the dropped peer's footprint to the snap point `crossing.map.snap.choice` picks with a radius
of 12 map units, and is taken only if that leaves at most three peers on every such edge.
Otherwise, and always for this machine's own footprint, it is refused, leaving the crossing map and
every zone as they were.

> r[crossing.map.drop.change]
> A drop that is taken is a change to the crossing map that reaches every machine, and every machine
> derives its zones from the changed map.
>
> - model: store

### Resizing a footprint

r[crossing.map.resize.proportional]
Resizing a footprint scales its width and height by one factor, so it keeps its shape, and keeps
its top-left corner where it was; its displays' offsets scale by the same factor. Each new width,
height and offset is rounded to a whole map unit, a half to the even one, and a width or height is
never below 1. Its displays' reported positions and sizes do not change.

r[crossing.map.resize.clamp]
A resize sets the longer of the footprint's width and height, rounded to a whole map unit, to at
least 40 and at most 480 map units: asked for less it gives 40, asked for more it gives 480.

r[crossing.map.resize.drag]
Dragging a footprint's resize handle by dx across and dy down, in map units, sets the longer of its
width and height to that length times (1 + c), where c is whichever of dx divided by its width and
dy divided by its height is larger in size, the first on a tie.

r[crossing.map.resize.step]
One resize step from the keyboard sets the longer of a footprint's width and height to that length
plus an eighth of it, or minus an eighth of it.

> r[crossing.map.resize.taken]
> A resize is taken as a drop of the resized footprint where it is, without
> `crossing.map.snap.drop-radius`: refused or moved as a drop is (`crossing.map.drop.overlap`,
> `crossing.map.drop.crowded`), and otherwise a change to the crossing map that reaches every
> machine, so the footprint has the same size on every machine's map.
>
> - model: store

### Overlapping footprints

r[crossing.map.overlap.reported]
Each peer in an overlap is reported as overlapping, naming one machine it overlaps: of those, the
one whose machine id comes first in text order. That report replaces any other state the peer has
on the crossing map.

### Sharing the crossing map

> r[crossing.map.shared.one]
> The cluster has one crossing map: a change to it made on any machine reaches every machine,
> including machines it has no link with, and all of them end with the same map, the footprints each
> keeps off included. Crossings behave the same whichever machine the change was made on.
>
> - model: store
> - parity: Python sent its whole crossing map in `settings` messages to each peer that advertised the `layout` capability, which relayed a map it took on to its other peers, and set the edge a peer without that capability faced by `arrangement` messages (#14: the store replaces per-feature messages)

> r[crossing.map.shared.newest]
> Each footprint is one entry, and a change to the crossing map writes only the footprints it
> places, moves or resizes, or whose displays' offsets or name it changes. When one footprint is
> changed on two machines before either change reaches the other, every machine ends with the newer
> change to it, in the order the store's stamps give them (#14), and a change to a footprint older
> than the one a machine has for it is ignored. Changes to different footprints are all kept, even
> when they are made at once on two machines, so a change that moves several footprints, such as
> `crossing.map.place.restored`'s row, loses to a newer change only for the footprints that
> newer change wrote.
>
> - model: store
> - parity: Python stamped the whole crossing map with the second it changed, or one past the stamp it had when that was not older, and its author's machine id; a map with a newer stamp replaced the one it had, the larger author id winning a tie, and a stamp more than a day ahead of this machine's clock was refused (#14: the store's hybrid logical clock stamps every entry, and the newest stamp wins entry by entry, each footprint being an entry)

> r[crossing.map.shared.facing]
> Placing a peer against an edge of this machine on the crossing map, on either machine, places
> this machine against the facing edge of that peer on both machines' maps: when the map puts a
> peer's display against this machine's right edge, this machine's zone to the peer is on its right
> edge and the peer's zone to this machine on the peer's left edge, and the same for every edge.
> Each zone leads to the other machine at once on the machine where the change was made, and on the
> other once the store has carried the change.
>
> - model: store
> - parity: Python told each peer the edge it faced in its own `arrangement` message, stamped per peer to the second, the larger machine id winning a tie, and answered by the peer's own (#14: the store replaces per-feature messages)

r[crossing.map.shared.own-zones]
Each machine derives its own zones from the crossing map it has and its own zone choices; no
machine sends its zones to another.

> r[crossing.map.shared.crowded-arrival]
> A crossing map that leaves four or more peers touching one edge of one of this machine's
> displays, as a change arriving from another machine, a restored peer or a default placement can,
> is taken like any other: that edge's thirds go to peers by `crossing.derive.share.one-peer`, and a
> peer left with no third there has no zone on that edge.
>
> - parity: Python ignored a crossing map arriving with such an edge and kept its own, and failed with an error when a restored peer or a default placement crowded an edge (#14: the crossing map's footprints are shared entries and converge, so every machine derives its zones from the same map)

r[crossing.map.saved.derived]
Zones are never read from what is saved. A machine derives them from the crossing map and its zone
choices when it starts and whenever either changes, and zone choices that would give two zones over
one stretch never make what is saved unreadable: Zones from the crossing map settles them as zones
are derived.

## Zones from the crossing map

A machine's zones are derived from the crossing map and its zone choices: for each peer, whether
its edge crosses and by which thirds, its wrap-around edges, its corners and, on a Mac, the notch
and return through the notch. Deriving never changes the zone choices.

### Zone choices

r[crossing.derive.choices.default]
A peer whose zone choices were never set, such as one just paired, has its edge on with all three
thirds picked, no wrap-around edge, no corner and no notch. Turning a corner on for it without
naming one turns on the top-left corner.

r[crossing.derive.choices.kept]
Deriving zones never changes a peer's zone choices. A change to the crossing map on any machine, to
this machine's or a peer's displays, or to which peers are inactive leaves every peer's zone choices
as they were: an edge turned off stays off, the thirds picked stay picked even while its contact
covers none of them, and an edge, third, wrap-around edge, corner or notch chosen for which no zone
can be derived now stays chosen.

r[crossing.derive.choices.unknown-peer]
Reading the zone choices of a machine that is not a peer gives no choices and no zones. A change to
the zone choices of a machine that is not a peer is refused, and nothing is written or sent.

r[crossing.derive.choices.one-peer]
A change to one peer's zone choices leaves every other peer's zone choices as they were, apart from
the notch (`crossing.derive.notch.one`), and never changes the crossing map: no footprint moves,
and nothing about the crossing map is written or sent.

r[crossing.derive.choices.partial]
A change to a peer's zone choices sets only the choices it names, and every other choice keeps its
value. A change that picks no third keeps the thirds picked before.

r[crossing.derive.choices.inactive]
A change to the zone choices of a peer that is kept off the crossing map or not in use is refused:
nothing is written, saved or sent, and its zone choices stay as they were for when it is restored.

### Edge and thirds

r[crossing.derive.edge.form]
A peer's edge is one choice with seven values: off, the whole edge, or any one or two of its
thirds. On with all three thirds picked, it crosses by the whole edge; on with one or two thirds
picked, by those thirds; off, by no third. Choosing the whole edge picks all three thirds, whatever
thirds were picked before, and choosing the whole edge and some thirds in one change gives the
whole edge.

r[crossing.derive.edge.off]
A peer's edge turned off leads nowhere: its stretch of the edge is a wall, and the peer's other
zones, such as a corner, still cross. With every zone choice off, the peer has no zone.

r[crossing.derive.edge.off-keeps-thirds]
Turning a peer's edge off keeps its picked thirds and its wrap-around edges. Turning it on again in
a change that picks no third crosses by the thirds picked before it was turned off, and its
wrap-around edges lead to it again.

r[crossing.derive.edge.toggle-third]
Toggling one third of a peer's edge while the edge is on picks that third if it was not picked and
unpicks it if it was. Unpicking the last picked third turns the edge off and keeps that third as
the one picked, so turning the edge on again without naming thirds gives that third alone. Toggling
a third while the edge is off turns the edge on with that third alone.

r[crossing.derive.edge.toggle-edge]
Toggling a peer's edge while it is on, in either form, turns it off and keeps the picked thirds.
Toggling it while it is off turns it on as the whole edge, all three thirds picked, whatever thirds
were picked before.

### Contacts

r[crossing.derive.from-map]
A peer's edge zones are derived from where the crossing map places its displays: each contact
between an edge of one of this machine's displays and one of the peer's displays can give a zone
on that edge of that display, and a display of this machine that no peer display touches has no
edge zone. A contact the peer's display has left gives no zone, whatever was picked while it
touched.

r[crossing.derive.no-displays]
A machine on the crossing map whose displays are not known yet counts as one display filling its
footprint. The zones derived while its displays are unknown, and again once they are known, both
follow the same zone choices.

r[crossing.derive.idempotent]
Deriving zones again from the same crossing map, displays and zone choices gives the same zones,
and reports nothing changed.

r[crossing.derive.overlap]
A peer whose display overlaps another present machine's display on the crossing map, this machine's
included, has no zone derived from the crossing map: no edge, third, corner or wrap-around zone.
Only a notch zone can lead to it (`crossing.derive.notch.independent`). It keeps its zone choices,
and peers in no overlap keep their zones.

r[crossing.derive.no-contact]
A peer whose footprint touches none of this machine's displays has no zone from a contact and no
chosen corner zone, and faces no edge. It keeps its zone choices, and its wrap-around zones and
the notch still lead to it.

r[crossing.derive.between]
A peer whose footprint touches no display of this machine, but touches the footprint of another
peer that touches none either, is reported as crossing between the two.

### Sharing an edge

r[crossing.derive.share.claim]
A peer's contact claims a third of an edge of one of this machine's displays when it covers at
least half of that third, a sixth of the edge's length. A contact over the lower half of a left or
right edge claims only its middle and end thirds. A third one peer claims belongs to each of that
peer's contacts that covers a sixth of the edge within it, so two contacts of one peer may share a
third.

r[crossing.derive.share.one-peer]
Each third of an edge of one of this machine's displays goes to at most one peer. Of the peers with
their edge on whose contacts claim it, it goes to the one whose contact covers more of it, then to
the one whose machine id is greater, compared as bytes, then to the earlier contact. A peer with its
edge off claims no third, so another peer whose contact claims it can have it. Every peer keeps its
zone choices whichever thirds it gets, and two peers sharing an edge keep the same split after
either is moved away and placed there again.

r[crossing.derive.share.zone]
Each contact of a peer whose edge is on gives one zone over the thirds that went to it there and
that it picked: all three make a whole-edge zone, one or two make a thirds zone by those thirds, and
none makes no zone. A third that went to the peer but that it did not pick crosses to no peer: no
other peer's contact or wrap-around zone takes it.

r[crossing.derive.share.unpicked]
When a peer's display touches one of this machine's displays and its edge is on, but none of the
thirds that went to it is picked and it has no other zone from the crossing map, it has no zone,
and it is reported that none of its picked thirds touches this machine's display.

r[crossing.derive.share.no-overlap]
No two zones cover the same stretch: no third of a display edge is in two zones, of two peers or of
one peer, no corner leads to two peers, and the notch leads to one peer. A corner zone may lie
within another peer's edge zone.

### Corners

r[crossing.derive.corner.contact]
A peer's display that touches one of this machine's displays only at a corner has a corner contact
there, and gives a corner zone at that corner of that display, naming the peer's display it
touches, while the peer has its edge or a corner on, unless that corner leads to another peer
(`crossing.derive.corner.one`). A display that shares a stretch of an edge with one of this
machine's displays touches that display along the edge and has no corner contact with it, even
where the stretch ends at a corner: a peer flush under this machine's only display, as wide as it,
has no corner contact at its bottom corners. A peer that touches this machine only at corners has
no edge or thirds zone, and is still reached through such corner zones and the notch.

r[crossing.derive.corner.chosen]
A corner chosen for a peer gives a corner zone only while one of the peer's displays touches an
edge of one of this machine's displays. The zone is on the display of this machine whose corner is
nearest that corner of this machine's footprint, and between two equally near, on the first by
display name. Removing a corner from the peer's corners leaves its other corners as they were.

r[crossing.derive.corner.independent]
A peer's corners and its edge are chosen apart: turning the edge off, or changing its thirds,
leaves its corners, and turning a corner off leaves its edge and thirds. Choosing a corner the peer
already has turns it off.

r[crossing.derive.corner.one]
Each corner of this machine's displays leads to at most one peer: the first in the peer list's
order, among present peers, that has it by a corner contact there (`crossing.derive.corner.contact`)
while its edge or a corner is on, or by choice. A peer whose zone choices list one corner twice has one zone there.

r[crossing.derive.corner.taken]
A corner is taken by a present peer that has chosen it, or that has a corner contact there
(`crossing.derive.corner.contact`) while its edge or a corner is on. Choosing for a peer a corner another peer has taken is refused: turning
on a single corner changes nothing and writes and sends nothing, and the other peer keeps it, for
this machine's own pointer and for a driver's. A change naming several corners drops those taken
and keeps the others, in the order top left, top right, bottom left, bottom right.

### The notch

r[crossing.derive.notch.mac]
Only a Mac derives a notch zone, and only on a Mac can the notch and return through the notch be
chosen: a change of zone choices made on another machine leaves the notch choices as they were.

r[crossing.derive.notch.one]
The notch leads to one peer: the first in the peer list's order that is present and has the notch
chosen. Choosing the notch for a peer clears it for every other peer. While a present peer has the
notch, no other peer has it chosen, an inactive peer included; while none does, an inactive peer
keeps its notch choice for when it is restored.

r[crossing.derive.notch.independent]
The notch zone does not depend on the crossing map: it leads to its peer whatever that peer's
displays touch, with its edge off, and while its display overlaps another. A peer may have its edge
zones and the notch at once. A notch zone alone does not count as the peer crossing on the
crossing map.

### Wrap-around zones

r[crossing.derive.wrap.zone]
A wrap-around edge chosen for a peer whose edge is on gives a zone to that peer on that edge of
each of this machine's displays where a third is free: all three free make a whole-edge zone, one
or two a thirds zone by those thirds. A wrap-around zone takes every free third, whatever thirds the
peer picked. Removing a wrap-around edge leaves the peer's other zones as they were.

r[crossing.derive.wrap.contact-first]
A wrap-around edge chosen for a peer gives no zone on an edge that one of the peer's displays
touches: that edge crosses by the contact.

r[crossing.derive.wrap.free]
A third is not free for a wrap-around zone when a peer's contact claims it, picked or not; when the
display of a peer with its edge off covers at least half of it; when a peer earlier in the peer
list has a wrap-around zone over it; or when another of this machine's displays touches that edge
along at least a sixth of the third. With one peer touching the top third of the left edge, a
wrap-around zone for another peer on that edge takes the other two thirds.

r[crossing.derive.wrap.display]
A wrap-around zone leads to the peer's display furthest out on the edge facing the one it leaves
by. Of several equally far out, it leads to the one whose stretch along that edge, as a fraction of
the peer's footprint, overlaps most the leaving display's stretch as a fraction of this machine's
footprint, and of those, the first by display name.

### Inactive peers

r[crossing.derive.inactive]
An inactive peer has no zone, at home or while this machine is driven. Its zone choices and its
footprint are kept, and it claims no third, corner or notch, overlaps no display and leaves every
other peer's zones as if it were not there. A peer in use again, or restored, has its zones again.

r[crossing.derive.no-link]
A peer with no link, asleep or unreachable, keeps its zones: they are derived as for a peer with a
link, and no other peer takes its thirds, corners or notch for want of a link.

r[crossing.derive.kept-off]
Keeping a peer off the crossing map removes every zone that leads to it and keeps its zone choices.
Restoring it derives its zones again from those choices and the crossing map, its thirds and its
corners as they were, except a stretch another peer took meanwhile, which stays that peer's.

### A peer's first zone

> r[crossing.derive.first-zone]
> A peer just paired has the default zone choices (`crossing.derive.choices.default`), so its zones
> come from where the pairing leaves its footprint and this machine's
> (`crossing.map.place.default-written`): every third its contact claims, its whole edge when no
> other peer's display shares that edge, and otherwise the thirds that go to it
> (`crossing.derive.share.one-peer`). A footprint that touches this machine's displays only at a
> corner, as `crossing.map.place.unplaced` gives a peer paired while another already fills the edge
> below this machine, gives it a corner zone there (`crossing.derive.corner.contact`) and no edge
> zone; the peer below has no corner contact there, so the corner is free for it. A footprint that
> touches none of this machine's displays gives the peer no zone (`crossing.derive.no-contact`), as
> `crossing.map.place.unplaced` leaves a ninth peer placed by default around a machine whose eight
> places touching it, centred on its four edges and at its four corners, are taken by footprints of
> its size. Every other peer keeps its zone choices, its footprint and its zones, apart from sharing
> that edge. This is how `pairing.peers.first-zone` and `pairing.peers.other-zones` are met.
>
> When both machines already had footprints, as when a peer is paired again
> (`pairing.peers.replace-keeps`) or a pairing joins two clusters, neither footprint moves, and the
> peer's zones come from where its footprint is, unless one is a forgotten machine's that now
> overlaps another present machine's (`crossing.map.members.forgotten`). That peer is placed by
> default as a peer just paired is. A machine whose zones come from the active profile has the
> default zone choices toward it, written there with that placement
> (`crossing.profile.choices.unpaired`), so its zones toward it are those of the first paragraph. A
> machine with own zones keeps its own zone choices toward it as they were before it was forgotten,
> and its zones toward it are derived from them at its new place. In the last example of
> `crossing.map.place.default-written`, P1 turns X's edge off before removing it, and pairs X again
> at (160, 100): with its zones from the active profile, P1 has a corner zone to X at its
> bottom-right corner (`crossing.derive.corner.contact`); with own zones, X's edge stays off with
> no corner chosen, so P1 has no zone to X. Where two clusters'
> footprints then overlap, each peer whose display overlaps another present machine's display has no
> zone from the crossing map, a peer paired before included, until a footprint is dropped elsewhere
> (`crossing.derive.overlap`, `crossing.map.overlap.reported`).
>
> - parity: Python's Mac gave a peer paired once its crossing map placed others, and facing no edge, no footprint and so no zone, while the PC placed it and derived its zones as this rule does (#16: one crossing engine for every platform; the PC's)

### What one peer can be given

r[crossing.derive.offer.edge]
For one peer, each edge of this machine is in one of six states: `contact` when one of the peer's
displays touches it; `on` when a wrap-around zone to the peer is on it; `kept` when it is chosen as
a wrap-around edge but no zone is derived there now; `free` when it is none of these and choosing
it would derive a wrap-around zone now; `taken` by another peer; or `unavailable`.

r[crossing.derive.offer.free]
An edge is `free` for a peer exactly when choosing it as a wrap-around edge, with the peer's edge
on, would derive a wrap-around zone to that peer on it.

r[crossing.derive.offer.taken]
An edge that is not `free` is `taken` by the peer that has an edge or thirds zone on it, or, when
none has, by a present peer whose display touches it. An edge taken by no peer is `unavailable`.

r[crossing.derive.offer.corner]
For one peer, each corner of this machine is `on` when a corner zone to the peer is derived there;
`unavailable` when none of the peer's displays touches an edge of this machine's displays; `taken`
by another peer that has a corner zone there or that takes it (`crossing.derive.corner.taken`);
and `free` otherwise.

r[crossing.derive.offer.kept-off]
A peer kept off the crossing map, or not in use, is answered as if it were restored and in use, so
its kept zone choices can still be read.

r[crossing.derive.offer.unavailable]
For a machine that is not a peer, a dormant peer, a peer whose display overlaps another machine's,
and every peer while the crossing map is refused, every edge and every corner is `unavailable`.

r[crossing.derive.offer.unplaced]
A peer with no footprint on the crossing map is answered at the place it is given by default
(`crossing.map.place.default-written`: `crossing.map.place.first-peer`,
`crossing.map.place.facing`, `crossing.map.place.unplaced`).

## Armed zones

A zone is armed when a push of the pointer against it can build push pressure and cross. Which
zones a machine has armed depends on where input is: while its input is home, its own pointer meets
its own zones; while it is driven, its driver's pointer meets the zones it arms for that driver.

> r[crossing.armed.home]
> While a machine's input is home, it arms each of its zones that leads to a peer it may take now
> (`driving.permission.reach-only`): a peer it has a link with, that lets it drive it, that it may
> drive, and that is not out of reach (`driving.take.out-of-reach`). A zone toward any other peer is
> not armed: the pointer moves there as at an edge with no zone, nothing is held at edge or
> reported, and the machine's zones toward other peers still cross. Once the peer can be taken
> again, its zones are armed from the next push.
>
> - parity: Python's Mac kept a zone toward a peer that did not let it drive it armed, so a push there kept the pointer at the edge and said that the peer does not accept input from this Mac (#16: one crossing engine for every platform; zones follow reach, `driving.reach.zones`, as the PC's did)
> - model: driving

r[crossing.armed.zones-only]
Only zones are armed. A peer to which no zone leads, or whose zone choices are all off, has nothing
armed for it, and the pointer stays on the machine at that edge, though the shortcut, a jump key and
the menu still reach the peer. A machine whose only crossing methods are the shortcut and jump keys
arms no zone; a notch zone is a zone.

> r[crossing.armed.follows-changes]
> A change to a machine's zones applies from the next push of the pointer on that machine, at home
> and while driven, with no crossing out and in again: the old zones stop crossing and the new ones
> cross. The changes include a choice made for a peer's zones, a footprint moved on the crossing map
> on any machine, a change of displays, a peer kept off the crossing map or restored, and another
> profile becoming active. The zones a machine arms follow a change before the change is sent to
> any peer, and a change made while a take of this machine is being accepted applies to the zones
> armed for that take.
>
> - model: driving

r[crossing.armed.unchanged-keeps-pressure]
Deriving a machine's zones again when nothing in them changed leaves the armed zones as they were: a
push in progress keeps the push pressure it has built. Two pushes of 80 px against a zone with a
120 px resistance cross, even when the zones are derived again between them.

> r[crossing.armed.driven-only]
> A machine arms zones for another machine's pointer only while that machine drives it. With no
> driver, none is armed, and a change to its zones arms none. When its driving ends, every zone
> armed for that driver is disarmed (`driving.end.keys-up`).
>
> - model: driving

> r[crossing.armed.driven-own-zones]
> The zones a driven machine arms for its driver are its own zones, those its own pointer crosses by
> at home, derived from its crossing map and its zone choices, never the driver's. Each crosses as
> its kind does: a whole edge anywhere along it, a thirds zone only from its thirds, a corner only
> by a diagonal push, whatever crossing method brought input to the machine.
>
> - model: driving

> r[crossing.armed.reach]
> A driven machine arms only its zones that lead to its driver or to a peer in the reach of its
> driver's latest take or reach update (`driving.reach.zones`). With an empty reach, only its zones
> leading to the driver are armed. A push against a zone leading to a peer outside the reach moves
> the driver's pointer as at a point that is no zone. When the reach changes, the zones armed follow
> it from the next push.
>
> - model: driving

> r[crossing.armed.spent]
> While a driven machine's zones lead nowhere, after one of them fired (`driving.pass-on.message`)
> or after it sent its driver home (`driving.send-home.message`), nothing is armed there: no zone
> builds push pressure, keeps the pointer held at edge or crosses, the driver's pointer motions are
> delivered as at any other point, motions still arriving from the push that fired cross nothing,
> and a change to the zones meanwhile arms nothing. The zones are armed again by the driver's next
> take or reach update.
>
> - model: driving

> r[crossing.armed.own-pointer-while-driven]
> On a driven machine, its own pointer is measured against its home zones and the driver's pointer
> only against the zones armed for the driver. A push of its own pointer through a home zone crosses
> to that zone's peer, with the landing point the zone gives, as `driving.role.drive-while-driven`
> carries out; pointer motion received from the driver never counts as its own user's push. Until
> that crossing is made, the pointer motion that fired the zone is not held at edge.
>
> - parity: Python's Mac needed a link of its own to its driver for this: over only the link the driver had opened, a push of its own pointer and its shortcut crossed nothing (#14: one link per pair carries both directions)
> - model: driving

## Return zone

The return zone is the zone a driven machine arms on the edge where its driver's pointer landed,
leading to the driver, so that a push out through the edge the pointer came in by brings input home.

> r[crossing.return.arrival-edge]
> When a take names a display of the driven machine and an edge of it to land on, the driven machine
> also arms a return zone on that edge of that display, leading to the driver, unless a zone of its
> own leading to the driver already covers that edge. A push through the return zone brings the
> driver's input home. A take with no landing point, as by the shortcut, a jump key or the menu, or
> with a landing point that names no display, as through the notch, arms no return zone: then only
> the driven machine's own zones toward the driver lead home.
>
> - model: driving

> r[crossing.return.needs-a-zone]
> The return zone is armed only when the driven machine has a whole-edge or thirds zone that is not
> off leading to the driver, on any display and edge, or has kept the driver off its crossing map,
> so that a driver kept off can still come home though no other zone leads to it. When the driven
> machine's edge for the driver is off, or no zone of its leads to the driver, it arms no return
> zone, even if a corner or the notch leads to the driver, and assumes no edge.
>
> - model: driving

> r[crossing.return.while-driven]
> The return zone stays armed while the driver drives the machine, through every change to the
> machine's zones, and is disarmed when its driving ends.
>
> - model: driving

> r[crossing.return.status]
> Core reports, for every peer, whether that peer has a zone leading to this machine: a whole edge,
> thirds, a corner or the notch that is not off. When it has none, the report names the machine
> whose whole-edge or thirds zone on the peer covers the edge where this machine sits on the peer's
> crossing map (`crossing.return.names-holder`); `viewmodel` makes the peer's notice from it. A
> change on the peer that opens or closes its zone to this machine, or changes which machine covers
> that edge, changes the report once the store has carried it here, including a change made by
> moving a different machine on the peer's map.
>
> - parity: Python told each peer this status in an `arrangement` message, sent only to the peers whose status the change touched; an `arrangement` without the status kept the last one heard (#14: the store replaces per-feature messages)
> - model: store

> r[crossing.return.names-holder]
> When a peer has no zone leading to this machine, the machine `crossing.return.status` names as
> covering its edge is one whose whole-edge or thirds zone is not off on that edge of the peer,
> never this machine itself. When the peer's zone for this machine is off and no other machine's
> zone covers that edge, the report names no machine.
>
> - model: store

## Push

A zone push crosses once the pointer has pushed far enough through the zone's edge, quickly
enough. The platform backend gives Core each pointer motion (its travel across and down) with the
pointer's position after it and the state of the mouse buttons; Core answers with the pointer held
at edge, the motion delivered as usual, or a crossing. The same rules apply on every machine: to a
machine's own pointer at its home zones, and to the driver's pointer at the zones armed on a driven
machine, apart from the guard against drags, which only a machine's own pointer has. Push pressure
is counted in px of pointer travel.

r[crossing.push.outward]
While the pointer is at an armed zone, push pressure grows by the pointer's outward travel: the part
of each motion that leads through the zone's edge, off the display. Travel along the edge adds
nothing. Push pressure adds up over successive motions: two motions of 80 px outward against a zone
with a 120 px resistance cross.

r[crossing.push.not-at-zone]
A motion while the pointer is at no armed zone, or that moves away from the edge or along it,
builds no push pressure, is delivered as usual and crosses nothing, whichever edge it pushes
against.

> r[crossing.push.arrival-overshoot]
> On the motion that brings the pointer to a zone, only its travel beyond the edge counts as outward
> travel: a pointer that moves 40 px toward a zone from 27 px away starts its push at 13 px.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones counted nothing of the motion that reached the edge and all of the next one; only the Mac's home zones counted the travel beyond the edge (#16: one crossing engine for every platform; the Mac's at home)

r[crossing.push.inward]
Inward travel while a push is in progress subtracts from the push pressure, which never falls below
zero.

r[crossing.push.decay]
Push pressure decays at 300 px per second, between motions and while pushing, whatever the
resistance: 100 px of push pressure is 40 px 0.2 s later, at a resistance of 120 px and of 500 px
alike, and 60 px is gone 0.2 s later.

r[crossing.push.slow-lean]
Outward travel at a steady 300 px per second or slower, such as a slow lean against an edge to reach
a menu, never leaves the pointer held at edge and never crosses.

r[crossing.push.sustained]
A steady push faster than 300 px per second crosses once its push pressure reaches the resistance:
at a resistance of 120 px and 500 px per second, after 0.6 s and 300 px of travel; at 500 px and
500 px per second, after 2.5 s and 1250 px; at 500 px and 1000 px per second, after 0.71 s and
714 px, each within 25 ms and 15 px with a motion every 10 ms.

r[crossing.push.resistance]
The resistance, the push pressure at which a zone push crosses, is a whole number of px from 0 to
500, 120 px unless the user sets another, and a change to it applies from the next pointer motion.

r[crossing.push.resistance-home]
Every zone a machine arms as home uses that machine's own resistance, also while the machine is
driven (`crossing.armed.own-pointer-while-driven`).

> r[crossing.push.resistance-driven]
> Every zone armed on a driven machine for its driver, the return zone included, uses the push
> distance the driver's latest take or reach update carries (`driving.take.contents`), whatever the
> driven machine's own resistance: with 500 px carried, a push of 200 px there keeps the pointer
> held at edge and does not cross. A push distance from 501 to 1000 px, which a take may carry, is
> used as it is.
>
> - model: driving

> r[crossing.push.resistance-take]
> The push distance a take or reach update carries (`driving.take.contents`) is the resistance of
> the machine that makes it, never one a peer gave it.
>
> - model: driving

r[crossing.push.breakthrough]
A motion with outward travel that brings push pressure to the resistance crosses to the zone's peer,
with the landing point the zone gives (Landing point, below). Push pressure is zero afterwards.

r[crossing.push.outward-only]
Only a motion with outward travel crosses. Push pressure left from earlier motions, however close
to the resistance, never crosses on a motion without outward travel.

r[crossing.push.zero]
At a resistance of 0, the first motion with outward travel at a zone crosses.

> r[crossing.push.held-at-edge]
> A motion at a zone whose push pressure before it, after decay, is at least 3 px is held at edge:
> it is not delivered, and the pointer stays at the point where the push began, whatever travel
> along the edge the motion has. The first motion of a push, which starts it, is delivered, so the
> pointer is seen to reach the edge.
>
> - parity: Python's zones on every driven machine, the Mac included, kept the pointer at the edge from the first outward motion there and slid it along the edge with the motion's travel along it, as Windows did at the PC's home zones; only the Mac's home zones held it where the push began, once push pressure was at least 3 px (#16: one crossing engine for every platform; the Mac's at home)

> r[crossing.push.held-at-edge-inward]
> While a push is in progress, an inward motion is held at edge as `crossing.push.held-at-edge`
> says, while push pressure before it, after decay, is at least 3 px. An inward motion with less
> push pressure before it is delivered, and the pointer is free again: at 2 px of push pressure, a
> 1 px inward motion is delivered.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones delivered every inward motion, so the pointer left the edge at once with push pressure still counted; only the Mac's home zones held it (#16: one crossing engine for every platform; the Mac's at home)

> r[crossing.push.leave]
> A motion that takes the pointer off the zone it was pushing ends the push: push pressure is zero,
> and the pointer is free.
>
> - parity: Python's zones on every driven machine, the Mac included, and the PC's home zones kept the push pressure after the pointer left them, decaying at 300 px per second, so a return to the edge within the resistance divided by 300 px per second (0.4 s at 120 px, 1.67 s at 500 px) could go on from it; only the Mac's home zones ended the push (#16: one crossing engine for every platform; the Mac's at home)

r[crossing.push.per-peer]
Push pressure built at one peer's zone is not carried to another peer's zone: when the pointer
reaches a zone of another peer, that push starts from zero.

r[crossing.push.input-moves]
When input moves to or from a machine, by any crossing method, any push in progress on it ends: its
push pressure is zero and the pointer is free.

r[crossing.push.pressure-reading]
Core gives a machine's push pressure, as a fraction of the resistance, at any moment, decayed to
that moment, without changing the push: a reading 0.2 s after a push of 100 px gives 40 px as a
fraction of the resistance, and the next motion still decays from the last motion.

r[crossing.push.unreadable-pointer]
A pointer motion whose position the platform backend cannot give builds no push pressure and is
delivered as usual. Zone pushes stay on, and the next motion with a position carries on the push.

> r[crossing.push.failure-off]
> A failure working out a zone push delivers that pointer motion as usual and turns zone pushes off
> for 30 s, or until the next change of settings if that comes sooner. The shortcut, jump keys, the
> shake and the menu keep crossing meanwhile.
>
> - parity: the PC turned its zone pushes off until the next change of settings, with no 30 s limit (#16: one crossing engine for every platform; the Mac's, which heals by itself)

### Guards against accidental crossings

r[crossing.push.drag]
While a mouse button of a machine's own is down, a push of its own pointer at its home zones builds
no push pressure, leaves nothing held at edge and never crosses, and pressing such a button ends
any push in progress, its push pressure dropped to zero, so letting go of the button at the edge
does not finish a crossing. This holds at home and while the machine is driven alike, and is on
unless the user turns off blocking crossings while dragging on that machine.

r[crossing.push.drag-off]
With blocking crossings while dragging turned off on a machine, a push of its own pointer with a
mouse button down crosses as any other push.

r[crossing.push.drag-driver]
A driver's pointer at the zones armed on a driven machine is not guarded against drags: a push
with the driver's mouse button down builds push pressure and crosses as any other push, whatever
either machine's setting for blocking crossings while dragging.

r[crossing.push.drag-unseen-release]
A mouse button counts as down only while the platform backend still reports it down: a button
whose release the capture never saw, released over the secure desktop or an elevated window, stops
blocking crossings as soon as the backend reports it up.

### Edges between a machine's displays

These are zones at an edge of one display that another display of the same machine lies beyond.
Without a zone there, the operating system moves the pointer straight onto the other display.

r[crossing.push.seam-stop]
A pointer motion that would carry the pointer across an armed zone at an edge where another display
of the same machine lies beyond stops where the motion meets that edge, on the zone's display, and
the pointer is held at edge there from that first motion, unlike at an edge with nothing beyond it.
Its travel beyond the edge counts as outward travel. Zones on both displays of that edge each stop
the pointer in their own direction, for a machine's own pointer and for a driver's.

r[crossing.push.seam-stretch]
A motion across such an edge stops only within the zone's stretch of it: its thirds for a thirds
zone, its corner box for a corner zone, its contact when it has one. Across the rest of the edge the
pointer passes onto the other display.

r[crossing.push.seam-free]
An edge between two displays of the same machine with no armed zone on it, or whose zones are paused
or lead nowhere after a crossing, lets the pointer pass onto the other display untouched.

> r[crossing.push.seam-seen]
> On the Mac and on Windows, a pointer stopped at a zone between two of the machine's displays is
> seen on the zone's display, and the motion that would have crossed that edge reaches no app on the
> other display.
>
> - tier: machine (the stop is made by the real capture path and pointer placement)

## Arrival grace

Just after input lands on a machine through an edge, the hand is often still moving from the push
that brought it there. For a moment, every zone on that machine keeps the pointer held at edge
instead of building push pressure, so that push never carries on to another machine.

> r[crossing.arrival.landing]
> When input arrives with a landing point on an edge, every zone armed on the machine it arrives
> at starts arrival grace, not only the return zone: its home zones when input comes home through a
> zone of the driven machine, and every zone armed for the driver, the return zone included, when a
> driven machine accepts a take with a landing point.
>
> - model: driving

> r[crossing.arrival.no-landing]
> A take with no landing point, by the shortcut, a jump key or the menu, starts arrival grace only
> on the zones that lead to the driver. Input that comes home with no landing point starts no
> arrival grace.
>
> - model: driving

r[crossing.arrival.placed]
Arrival grace for a landing starts from the take, and starts again when the platform backend
reports the pointer placed at the landing point, which it may do later.

> r[crossing.arrival.held-at-edge]
> During arrival grace, outward travel at a zone builds no push pressure and never crosses, and the
> pointer is held at edge, at every zone whatever its edge or kind: whole edge, thirds, corner or
> notch.
>
> - parity: the Mac's own zones left the pointer free during arrival grace, so at a zone between two of its displays a pushing hand moved the pointer onto the other display (#16: one crossing engine for every platform; the PC's)

r[crossing.arrival.time]
Arrival grace ends 0.3 s after it starts. A push at a zone from 0.3 s on builds push pressure and
crosses as usual, to the display and edge the zone names.

r[crossing.arrival.inward]
Arrival grace ends early for every zone on a left edge once the pointer has travelled 4 px to the
right in total since the grace started, and likewise for zones on right, top and bottom edges with
travel left, down and up. A push out through such a zone then builds push pressure and crosses as
usual, while the zones on the other edges stay in grace.

> r[crossing.arrival.rearm]
> Zones armed again during arrival grace, after a change to the zones or a peer coming into reach,
> keep the grace of the zone each replaces: same peer, same kind, edge, corner and display. A zone
> that replaces none starts with no grace, as does every zone armed with no landing.
>
> - parity: on the Mac one arrival grace covered all its zones, so a zone added during it started in grace (#16: one crossing engine for every platform; the PC's grace per zone)

> r[crossing.arrival.button]
> A mouse button pressed, or a drag reaching a zone, during arrival grace does not end it.
>
> - parity: the PC ended the arrival grace of its home zones when a drag reached one of them (#16: one crossing engine for every platform; the Mac's)

## Landing point

These rules place the pointer wherever input arrives with an edge: on a driven machine after a take
by a zone push (`driving.accept.lands`), and at home when input comes home through a zone of the
driven machine (`driving.home.send-home`).

> r[crossing.landing.facing-edge]
> A crossing through a zone lands on the edge facing the zone's edge: a left edge lands on a right
> edge, a top edge on a bottom edge, and the reverse. This is so for a wrap-around zone; for a
> corner zone, whose edge is the one it names (`crossing.zone.corner-edge`); for the notch, which
> lands on the bottom edge whatever edge its peer is at on the crossing map; for input coming home
> through a driven machine's zone, which lands on the driver's edge facing that zone; and whatever
> zones the machine it lands on has toward the machine it came from.
>
> - model: driving

> r[crossing.landing.offset]
> A crossing carries the fraction along the zone's edge of the point where the push began, where the
> pointer is held at edge, not of where it was when the push crossed: along the zone's display's
> edge, or, for a zone that names no display, along the edge of the rectangle around all this
> machine's displays. Without a contact or a named display, it lands at that same fraction of the
> landing edge: a push at 17 % of this machine's right edge lands at 17 % of the peer's left edge,
> and 500 px down a 1080 px edge is 0.46. A pixel that leaves at a fraction lands on that fraction
> of the landing edge: the last pixel of an 8 px edge leaves at 0.875 and lands on the last pixel of
> an 8 px edge.
>
> - parity: Python measured these fractions over the edge's length less one pixel when input left the Mac at home or came home to it, and over the whole length on every driven machine, the Mac included, and on the PC at home; and its zones on every driven machine and the PC's home zones took the fraction where the pointer was when it crossed, only the Mac's home zones where the push began (#16: one crossing engine for every platform; the whole length, under which a pixel lands on the pixel it left, and the Mac's point where the push began)

r[crossing.landing.contact]
A crossing through a zone with a contact maps its fraction in proportion: the start of the contact
lands at the start of the stretch of the peer's display that the contact touches, its end at that
stretch's end. A push at 0.5 of the edge, through a contact from 0.25 to 1 into a stretch from 0 to
0.75 of the peer display's edge, lands at 0.25 of that edge, so a crossing through a contact lands
along the contact on the other machine, never along its whole edge.

r[crossing.landing.named-display]
A crossing that names a display of the machine it arrives on, and an edge of that display, lands on
that edge of that display, at the crossing's fraction of that display's edge, wherever that display
sits among the machine's displays, at negative positions included, and even where another of that
machine's displays lies beyond the edge: the middle of the top edge of a display from x 100 to 300
is x 200.

r[crossing.landing.display-and-edge]
A crossing names a display to land on and its edge together, or neither. A zone that names the
peer's display but no edge names the edge facing its own. A zone that names an edge to land on but
no display names neither, and still crosses, landing by `crossing.landing.whole-edge`.

r[crossing.landing.unknown-display]
A crossing that names a display the machine it arrives on does not have lands as one that names no
display.

r[crossing.landing.whole-edge]
A crossing that names no display lands at its fraction of the rectangle around all the displays of
the machine it arrives on, along the landing edge, on the display that is outermost on that edge of
those whose edge spans that point, first and last pixels included. Where no display spans it, as in
the gap beside a shorter display, it lands on the nearest display; between equals, on the primary
display. It lands on that display's own edge, so never in a gap and never on an edge between two
displays.

r[crossing.landing.clear-of-ends]
A landing point is kept at least 10 px from both ends of the landing display's edge, when that edge
is longer than 21 px, so it never lands in a corner zone's box or on the edge beside it. It is not
moved where that would put it beside another display of the same machine that lies beyond the edge,
as beside a neighbour staggered by less than 10 px; there it lands at its unmoved place, within the
stretch with nothing beyond it.

r[crossing.landing.corner]
A crossing through a corner zone lands at the end of the landing edge nearest that corner: for a top
right corner crossing by its right edge, the top end of the peer's left edge, and by its top edge,
the right end of the peer's bottom edge; likewise for every corner. It is kept clear of the end by
`crossing.landing.clear-of-ends`.

> r[crossing.landing.inset]
> The landing point is 2 px inside the landing edge, so the landing itself is not already a push.
>
> - parity: Python landed the pointer on the edge's outermost pixel row or column on every driven machine, the Mac included, and on the PC at home; only input coming home to the Mac landed 2 points inside (#16: one crossing engine for every platform; the Mac's at home)

r[crossing.landing.notch-return]
On a Mac whose notch zone leads to a peer with return through the notch on, input coming home from
that peer through any zone of the peer lands at the middle of the notch's stretch of the top edge,
2 px below the notch's depth. Input coming home from any other peer, or while the backend declares
no notch, lands at the edge the crossing names.

r[crossing.landing.no-edge]
Only a zone push gives a landing point. A crossing by the shortcut, a jump key, a shake or the menu,
and a send home, gives none, so the pointer stays where it is on the machine input reaches: on a
driver, where it stayed while the driver drove (`driving.accept.lands`, `driving.home.how`).

r[crossing.landing.take-ended]
A landing point is never applied once the take it belongs to has ended, by a let go or a later take,
before the platform backend places the pointer.

## Crossing methods

Every crossing method picks a peer, or home, and `driving` makes the move: from home or from a
driven machine (`driving.take.from-driven`), from a driven machine's own user
(`driving.role.drive-while-driven`), or to a peer with no link (`driving.take.no-link`,
`discovery.wake.crossing`). A zone push is Push's; the shortcut, jump keys and the shake have
topics of their own.

> r[crossing.method.menu]
> Choosing a peer in the menu crosses to it with no landing point, also while input is on another
> peer, straight there without coming home between. Choosing this machine brings input home, and
> choosing the machine input is already on moves nothing (`driving.take.same-place`).
>
> - model: driving

### What a crossing reports

Core reports what a crossing does, for the edge glow, the trackpad's haptics and, later, the
crossing effects, which `spec/ui.md` draws. These reports are crossing's own. They are apart from
driving's report that input is home (`driving.home.how`), which every way home makes with the
landing point or the pointer's position, whatever these reports say.

r[crossing.method.report-pressure]
Every pointer motion that builds push pressure at a zone, or is held at edge there, reports the edge
of this machine's display being pushed, the push pressure as a fraction of the resistance from 0 to
1, the part pushed (the third for a thirds zone, the corner for a corner zone, the notch for the
notch, none for a whole-edge zone), and the stretch pushed: the whole edge, 1 px deep; the third
being pushed; the 8 by 8 px box in the corner; or the notch's span along the top edge.

r[crossing.method.report-touching]
While the pointer is at a pushable zone, Core reports that it touches that zone, also once push
pressure has drained to nothing; the report ends when the pointer leaves the zone.

> r[crossing.method.report-tick]
> A zone push reports a tick the first time its push pressure reaches each of a quarter, a half and
> three quarters of the resistance: at most three ticks in one push, and none again when decay takes
> push pressure under a quarter it already reached and a later motion takes it over again.
>
> - parity: only the Mac reported ticks, for its trackpad's haptics (#16: one crossing engine for every platform)

r[crossing.method.report-cross]
A zone push that crosses reports one crossing, with push pressure 1, naming this machine's edge
pushed and the part, after its last pressure report. A push by a driven machine's own pointer
(`crossing.armed.own-pointer-while-driven`) reports it once its driving has ended and the peer is
taken.

r[crossing.method.report-none-out]
A crossing by the shortcut, a jump key or the menu reports no pressure, no crossing and no arrival.

r[crossing.method.report-arrival]
When input comes home through the driven machine's zone, with a landing point, Core reports one
arrival naming this machine's edge and the point the pointer landed at, and no other report of
this section.

> r[crossing.method.report-arrival-no-point]
> When the driven machine sends input home, which names no landing point, Core reports one arrival
> naming no edge and no point, for the edge glow and the trackpad's haptics; driving's report that
> input is home still gives where the pointer is (`driving.home.how`).
>
> - parity: Python's PC filled in the pointer's position for such an arrival, its Mac reported none (#16: one crossing engine for every platform; the Mac's)

r[crossing.method.report-home]
When this machine's own user brings input home by the shortcut, a jump key or the menu, Core
reports, for the edge glow and the trackpad's haptics, that input came home by this machine's own
crossing method, with the pointer's position.

r[crossing.method.report-quiet]
Input that comes home because another machine took the driven machine over, or because the link to
it was lost, makes none of the reports of this section: no crossing, no arrival and no report that
it came home by a crossing method. Nor does bringing input home while it is already home. Driving's
report that input is home is made all the same (`driving.home.how`).

## The shortcut

The shortcut is one key, the shortcut key, used in one of two styles: a double-tap or a long press.
Where it sends input is its destination, one of three: where input last went, next in order, or
next among ticked. How the shortcut key's events are captured and kept from an app is `input`'s;
these rules say when the shortcut fires, where input goes, and which of the key's events reach an
app or a driven machine. Below, a peer is ready to take while it has a link with this machine and
lets this machine drive it.

r[crossing.shortcut.defaults]
Until the user changes them, the shortcut is on, its style is double-tap, its double-tap window is
300 ms, its destination is where input last went, and its key is Right Option on a Mac and Right
Ctrl on Windows.

r[crossing.shortcut.window-range]
The double-tap window is a whole number of milliseconds from 50 to 2000. A window outside that
range is refused, and the window in use stays as it was.

### Double-tap and long press

r[crossing.shortcut.double-tap]
In double-tap style, a press of the shortcut key fires the shortcut when it comes no more than the
double-tap window after the previous press of that key. The window runs from press to press: when
the key was released does not count. A press later than the window after the previous one fires
nothing and starts a new pair.

r[crossing.shortcut.double-tap-spent]
The press that fires the shortcut ends its pair: the next press starts a new one, so three presses
inside the window fire the shortcut once.

r[crossing.shortcut.other-keys]
Only the shortcut key's own presses make a double-tap or a long press. A press of any other key
fires nothing, and does not end a pair half made.

r[crossing.shortcut.repeat-not-a-tap]
A repeat of the shortcut key, as the OS sends while the key is kept down, is never a tap and never
starts a long press. Keeping the key down in double-tap style fires nothing however long it stays
down, also after the press that fired a double-tap, and a double-tap after its release fires as
usual.

> r[crossing.shortcut.missed-release]
> A press of the shortcut key that is not a repeat is a tap even when the key's release before it
> was never seen, so a double-tap fires after a release the capture missed.
>
> - parity: Python's Windows app took a press of the shortcut key while it believed the key still down for a repeat, so a missed release cost one tap (#16: one crossing engine for every platform; the Mac's)

> r[crossing.shortcut.down-before-on]
> A press of the shortcut key that began while the shortcut was off is neither a tap nor a long
> press: neither the repeats that follow once the shortcut is on nor its release count, and the
> next press after that release starts a new pair.
>
> - parity: Python's Windows app started a long press on the first repeat of such a press (#16: one crossing engine for every platform; the Mac's)

> r[crossing.shortcut.reset]
> Changing the shortcut key, its style or its double-tap window, or turning the shortcut off,
> discards a pair half made: the next press of the shortcut key starts a new one. After a change of
> key, only the new key fires the shortcut.
>
> - parity: Python's Mac kept a pair half made across a change its settings window applied without reconnecting, unless that change turned the shortcut off (#16: one crossing engine for every platform; the Windows app's)

> r[crossing.shortcut.long-press]
> In long-press style, a press of the shortcut key crosses to the long-press machine
> (`crossing.shortcut.long-press-machine`), and its release brings input home: input is on that
> machine for as long as the key is down.
>
> - model: driving

> r[crossing.shortcut.long-press-home]
> The release of a long press brings input home wherever input is then, including when it was
> already on that machine before the press, or reached another machine by a pass on during it.
>
> - model: driving

> r[crossing.shortcut.method-off]
> The shortcut can be turned off on its own. While it is off, its key is an ordinary key in every
> state, home, driving or driven: a double-tap or a long press of it moves no input, and each of its
> presses and releases goes where input is, like any other key, to this machine's apps at home and
> to the driven machine while input is away. Turning the shortcut off or on changes no zone, and
> leaves zones and jump keys crossing.
>
> - model: driving

### Which of the key's events go on

r[crossing.shortcut.key-at-home]
In double-tap style with input home, a press of the shortcut key that does not fire the shortcut,
its repeats and its release reach this machine's apps as an ordinary key. So does a lone tap.

r[crossing.shortcut.key-away]
In double-tap style with input away, every press, repeat and release of the shortcut key is kept
from both this machine's apps and the driven machine, and keeping the key down keeps input where
it is.

> r[crossing.shortcut.key-fired]
> The press that fires a double-tap, and the release of that press, are kept from this machine's
> apps and from every driven machine, wherever input went and whether or not it moved. A press
> after that release is again an ordinary key at home.
>
> - parity: Python's Mac let the firing press and its release reach its apps when input was home and the double-tap moved nothing (#16: one crossing engine for every platform; the Windows app's)

> r[crossing.shortcut.key-long-press]
> In long-press style, every press, repeat and release of the shortcut key is kept from this
> machine's apps and from the driven machine, whether or not input moved. The one exception is the
> release of a press that began before the key became the shortcut key, which reaches this
> machine's apps, since they saw its press; from its next press, every press and release of it is
> the shortcut's.
>
> - parity: Python's Mac kept that release too (#16: one crossing engine for every platform; the Windows app's)

### Where the shortcut goes

> r[crossing.shortcut.last-toggle]
> With the destination where input last went, a double-tap with input home crosses to the
> shortcut's machine (`crossing.shortcut.machine`), and a double-tap with input away brings input
> home, wherever input is, however it got there (by a zone, the shortcut, a jump key, the menu or
> a pass on) and however long ago it last moved. With two machines, the shortcut on either crosses
> to the other and the shortcut again brings input home, whatever zones each has.
>
> - model: driving

r[crossing.shortcut.machine]
The shortcut's machine is the peer input was last on (the last peer this machine's input crossed to,
by any crossing method or by a pass on) while that peer is ready to take; else the first peer ready
to take in the peer list's order.

r[crossing.shortcut.machine-none-ready]
When no peer the shortcut may go to is ready to take, the shortcut's machine is the peer input was
last on, when the shortcut may go to it, else the first peer in the peer list that the shortcut may
go to, so the crossing's refusal, or its wake (`discovery.wake.crossing`), names it. A peer listed
above the one input was last on never takes its place.

r[crossing.shortcut.machine-eligible]
The shortcut's machine is only ever a peer that is in use and that this machine may drive.

r[crossing.shortcut.machine-nothing]
With no peer in use that this machine may drive, the shortcut has no machine: with the destination
where input last went, a double-tap from home moves no input, and Core reports that it cannot
cross, with this machine's link status.

r[crossing.shortcut.cycle-members]
The shortcut's cycle is every peer that is in use and that this machine may drive, then home.
With the destination next among ticked, it drops each peer the user unticked.

r[crossing.shortcut.cycle-ticks]
Each peer is ticked for the shortcut's cycle until the user unticks it. A machine keeps its own
ticks: unticking a peer on one machine changes no other machine's cycle.

r[crossing.shortcut.cycle-rows]
The peers placed on the crossing map follow each other in the cycle in reading order of the
centres of their footprints: in rows from top to bottom, each row from left to right, and two peers
of a row whose centres are at the same left-right position in the peer list's order. A peer
belongs to the row above it when its centre is no more than half the smaller of its height and
that row's first peer's height below that first peer's centre; otherwise it starts a new row.

r[crossing.shortcut.cycle-rotate]
The cycle starts with the first placed peer after this machine in that reading order and goes
round: a peer comes after this machine when its centre is lower by more than half the smaller of
the two heights, or level with it within that half and to its right. The placed peers before this
machine follow those after it. A machine not placed on the crossing map starts its cycle at the
first placed peer.

> r[crossing.shortcut.cycle-unplaced]
> Peers in the cycle with no footprint on the crossing map follow the placed ones, in the peer
> list's order. A peer kept off the crossing map stays in the cycle so.
>
> - parity: Python's Mac counted as placed only a peer with a zone to it, or one between or set from elsewhere, and put any other placed peer with the unplaced ones (#16: one crossing engine for every platform; the Windows app's)

> r[crossing.shortcut.refused]
> With the destination next in order or next among ticked, and for a long press with any
> destination, the shortcut passes over a peer that is out of reach (`driving.take.out-of-reach`) as
> if it were not in the cycle, and does not report it as skipped. When every peer in the cycle is
> out of reach, a double-tap with input home in those destinations crosses to no machine and reports
> none. With the destination where input last went, a double-tap passes over no peer out of reach:
> when the shortcut's machine (`crossing.shortcut.machine`) is out of reach, the double-tap picks it
> all the same, so it sends nothing, reports nothing and moves no input.
>
> - model: driving

> r[crossing.shortcut.next]
> With the destination next in order or next among ticked, a double-tap with input on a peer in the
> cycle crosses to the first peer ready to take after it in the cycle, passing over the peers that
> are not ready to take. With none after it, the double-tap brings input home. Input's place in the
> cycle is the machine it is on now, whichever crossing method or pass on brought it there. So each
> double-tap while driving moves input to a machine other than the one it is on, and input reaches
> home within as many double-taps as the cycle has machines.
>
> - model: driving

> r[crossing.shortcut.next-from-home]
> With the destination next in order or next among ticked, a double-tap with input home crosses to
> the first peer ready to take in the cycle. When none is ready to take, it crosses to the first
> peer in the cycle, so the crossing's refusal, or its wake (`discovery.wake.crossing`), names it.
>
> - model: driving

> r[crossing.shortcut.next-not-in-cycle]
> With the destination next in order or next among ticked, a double-tap with input on a peer that is
> not in the cycle crosses to the first peer ready to take in the cycle, and brings input home when
> none is ready to take.
>
> - model: driving

> r[crossing.shortcut.empty-cycle]
> When the cycle has no peer, or only peers that are out of reach, a long press, and a double-tap
> with input home and the destination next in order or next among ticked, move no input.
>
> - parity: Python's Mac sent a long press to the shortcut's machine (`crossing.shortcut.machine`) when the cycle had no peer at all, while its Windows app moved nothing (#16: one crossing engine for every platform; the Windows app's)
> - model: driving

r[crossing.shortcut.skipped]
Each peer the shortcut passes over because it is not ready to take is reported, in the order the
shortcut tried them, as skipped because it is not connected: in cycle order, except that a long
press with the destination where input last went tries the peer input was last on first
(`crossing.shortcut.long-press-machine`), so that peer is reported first wherever it sits in the
cycle. When no peer in the cycle is ready to take, every one is reported so, the one the crossing
then names included (`crossing.shortcut.next-from-home`).

> r[crossing.shortcut.long-press-machine]
> The long-press machine, with the destination where input last went, is the peer input was last on
> when it is in the cycle and ready to take; else, with any destination, the first peer ready to
> take in the cycle; else the first peer in the cycle, so the crossing's refusal or wake names it.
> It is never a peer that is out of reach, and each peer passed over because it is not ready to take
> is reported as `crossing.shortcut.skipped` says.
>
> - model: driving

> r[crossing.shortcut.while-driven]
> On a driven machine with the shortcut on, the shortcut key on its own keyboard fires that
> machine's own shortcut, as it would at home, and the crossing it picks is made as
> `driving.role.drive-while-driven` says. The driver's presses of a shortcut key never fire the
> driven machine's shortcut (`driving.send-home.own-user-only`).
>
> - model: driving

### The shortcut key

> r[crossing.shortcut.key-allowed]
> Any key that types nothing can be the shortcut key: a modifier of either hand, Caps Lock, a
> function key from F1 to F20, an arrow, Insert, Delete, Home, End, Page Up, Page Down, and, where
> the keyboard has them, Num Lock, Scroll Lock, Pause and the Menu key. A key that types a
> character (the numeric keypad's included), a media or volume key, a browser key and an
> input-method key (Han/Yeong, Hangul, Hanja) can never be it.
>
> - parity: Python's Mac took the numeric keypad's keys as the shortcut key (#16: one crossing engine for every platform; the Windows app's)

> r[crossing.shortcut.key-typing]
> Backspace, Tab, Enter, Esc, Space and Print Screen can never be the shortcut key: recording one
> as the shortcut is refused, and a shared shortcut key naming one is not applied.
>
> - parity: the Mac's recorder refused Enter, Tab, Space, Backspace, Esc, Hangul and Hanja and the PC's Backspace, Tab, Enter, Esc, Space and Print Screen, and both apps loaded any of them from a settings file that named them (#16: one crossing engine for every platform; one list for every source of the setting)

r[crossing.shortcut.key-jump-clash]
Changing the shortcut key to a key that is in a jump key's chord, as its key or as one of its
modifiers of either hand, is refused, the refusal naming that jump key's chord, and the shortcut key
stays as it was.

r[crossing.shortcut.shared-key-clash]
A shortcut key that arrives as a shared setting and clashes with one of this machine's jump keys
(as `crossing.shortcut.key-jump-clash` says) is not applied here: the jump key is kept, and Core
reports that the shortcut key clashes with that jump key, naming its chord. The other shared
shortcut settings that arrived with it are applied.

## Jump keys

A jump key is a chord of modifiers and one other key, recorded on one machine for one of its peers.

> r[crossing.jump-key.cross]
> A press of a peer's jump key crosses to that peer, from home, from any machine input is on, and
> on a driven machine for its own user (`driving.role.drive-while-driven`), in either shortcut
> style and whether the shortcut is on or off.
>
> - model: driving

> r[crossing.jump-key.home]
> A press of the jump key of the peer input is on brings input home.
>
> - model: driving

r[crossing.jump-key.in-use]
Only a peer that is in use has a jump key that works: the chord of a peer not in use moves no
input, and reaches apps or the driven machine as ordinary keys.

r[crossing.jump-key.own]
Each machine keeps its own jump keys: a jump key recorded on one machine works only on that
machine's keyboard, and is never shared with its peers.

r[crossing.jump-key.kept]
The press of a jump key's key, its repeats and its release are kept from this machine's apps and
from every driven machine. The release is matched to the press by the key, whatever Shift made of
its name between them. Its modifiers are not kept: they go where input was as ordinary keys.

> r[crossing.jump-key.repeat-no-move]
> Only the first press of a jump key's chord crosses: its repeats move no input, so a jump key kept
> down that brought input home does not cross out again.
>
> - model: driving

r[crossing.jump-key.repeat]
A key that was already down when the chord's modifiers went down is not a press of the chord: its
repeats move no input and go on as ordinary keys.

r[crossing.jump-key.recording]
While the user is recording a jump key, no jump key works: the chord pressed is recorded.

r[crossing.jump-key.chord]
A jump key's chord has at least one modifier other than Shift, may add Shift, and has exactly one
key that is not a modifier. Shift alone with a key is refused, since it would take that key from
typing; so is a modifier alone, two keys that are not modifiers, and one modifier twice.

r[crossing.jump-key.keys]
The key of a chord is a letter, a digit, a function key from F1 to F24, any other single
printable character, or one of Backspace, Caps Lock, Delete, the arrows, End, Enter, Esc, Home,
Insert, Page Up, Page Down, Pause, Print Screen, Space, Tab, Menu, Num Lock, Scroll Lock, Browser
Back, Browser Forward, Volume Mute, Volume Down, Volume Up, Next Track, Previous Track, Stop and
Play/Pause.

r[crossing.jump-key.spelling]
A chord is written in lowercase as its modifiers, by the names `input`'s key table gives them, in
the order ctrl, alt, shift, cmd, then its key, joined by `+`: a right-hand modifier is written as
its left-hand one, Space as `space`, the plus key as `plus`, and a printable key as the character
it types with the chord's Shift, so Ctrl+Shift+2 on a US keyboard is `ctrl+shift+@`. A value not
written so is refused.

r[crossing.jump-key.shortcut-clash]
A chord whose key is the shortcut key, or that has the shortcut key's modifier of either hand, is
refused, the refusal saying that the key belongs to the shortcut.

r[crossing.jump-key.unique]
A chord that is already another peer's jump key is refused, the refusal saying that it is assigned
to another machine.

r[crossing.jump-key.clear]
Recording an empty jump key clears the peer's jump key. A peer without a jump key has no chord that
crosses to it.

## Shake

A shake is measured in strokes along one axis. **S** below is the shake's stroke length at the
current sensitivity (`crossing.shake.stroke`, `crossing.shake.sensitivity`); every distance and
speed threshold is a multiple of S, so motion scaled by S fires at the same moment on every
platform and at every sensitivity.

> r[crossing.shake.home]
> A shake on a driver whose input is on another machine brings the input home, from whichever
> machine it is on, however many machines its chain passed through.
>
> - model: driving

> r[crossing.shake.away-only]
> Only a driver's own pointer motion while its input is on another machine counts toward a shake.
> Motion while input is home never fires one, and neither does a driven machine's own pointer.
>
> - model: driving

r[crossing.shake.defaults]
The shake is on, at a sensitivity of 100 %, until the user changes either. Where these settings
live, in the active profile or a machine's own, is Profiles' (`crossing.profile.shake.in-profile`).

r[crossing.shake.off]
While the shake is off, no motion brings input home. Turning it off while input is on another
machine ends any gesture in progress at once.

r[crossing.shake.on-while-away]
Turning the shake on while input is on another machine lets a shake bring it home from 300 ms after
the moment it was turned on, without input first coming home.

r[crossing.shake.stroke]
At a sensitivity of 100 %, S is 48 units of the relative pointer motion the Mac's capture reports,
and 192 raw mouse counts on Windows.

r[crossing.shake.sensitivity]
At a sensitivity of s %, S is the platform's stroke length at 100 % times 100 / s: 96 on the Mac at
50 %, 24 at 200 %.

r[crossing.shake.sensitivity-range]
A shake sensitivity is a whole number of percent from 50 to 200. Setting any other value is
refused and changes nothing, a value outside that range arriving in a profile is not taken, and a
stored value outside that range, or not a whole number, is read as 100.

r[crossing.shake.relative-motion]
The shake reads the relative motion of each pointer event as capture reports it, fractions
included, before it is rounded for the driven machine. It never reads the pointer's position.

r[crossing.shake.turn]
A stroke ends at a turn: the motion event at which the pointer has reversed along the shake's axis
by at least S / 4 from the farthest point of the stroke. Reversals shorter than S / 4, however many,
make no turn.

r[crossing.shake.stroke-span]
The first stroke of a gesture runs from its first motion event to its farthest point; each later
stroke runs from the farthest point of the stroke before it to its own farthest point. Its length,
duration and number of motion events are measured between those two points.

r[crossing.shake.fires]
A shake fires at the first turn of a gesture, from its fourth turn on, whose four latest strokes,
the four that turn ends, meet every condition below: a gesture whose first stroke fails them and
whose next four meet them fires at its fifth turn. Three turns, a single flick, a sweep with no
turn and a circle never fire.

r[crossing.shake.stroke-length]
Each of the four strokes is at least S long along the axis.

r[crossing.shake.stroke-duration]
Each of the four strokes lasts from 30 ms to 200 ms, both included.

r[crossing.shake.stroke-events]
Each of the four strokes is made of at least 2 motion events.

r[crossing.shake.stroke-speed]
Each of the four strokes averages at least 6 S per second along the axis.

r[crossing.shake.total]
The four strokes together are at least 5 S long along the axis.

r[crossing.shake.endpoint]
The end of the last of the four strokes is within 2 S, along the axis, of the start of the first.

r[crossing.shake.cross-axis]
The four strokes' travel across the axis, summed as distance, is at most half their travel along
it.

r[crossing.shake.window]
The first of the four strokes starts at most 650 ms before the motion event that makes the turn
they end.

r[crossing.shake.axis]
A gesture's axis is whichever of horizontal and vertical has more travel over the motion events of
the last 650 ms, horizontal on a tie. When that changes, the gesture starts over from the motion
event that changed it, on the axis of that event alone.

r[crossing.shake.pause]
Two motion events 120 ms or more apart start the gesture over from the later one. An event that
moves the pointer by nothing adds no travel but counts as motion for this gap.

r[crossing.shake.arm-delay]
Motion in the first 300 ms after input leaves home does not count toward a shake, and a gesture in
progress at that point starts over; motion from 300 ms on counts. Input moving on from one driven
machine to another does not start the 300 ms again.

r[crossing.shake.scroll]
A scroll on the driver starts the gesture over, and motion less than 250 ms after the scroll does
not count toward a shake. Motion 250 ms or more after it counts.

r[crossing.shake.down]
While any key or mouse button is down on the driver, its motion does not count toward a shake, and
every press or release starts the gesture over. A key or button counts as down from its press to
its release, whether it was pressed before or after input left home, so a key pressed at home and
kept down across a crossing still stops a shake, and the shortcut's own key, once released, does
not. Caps Lock never stays down after it is released.

r[crossing.shake.down-reconcile]
When a driver lets go of this machine, a key or mouse button this machine counted as down stays
down only if the OS reports it still down, the middle button and other buttons included: a release
it did not see while it was driven no longer stops a shake.

r[crossing.shake.down-fresh]
A press reported while this machine is reading from the OS which keys and buttons are still down
stays down, whatever that reading says.

> r[crossing.shake.once]
> After a shake fires, no shake fires for 1.5 s, measured from the motion event that fired it, even
> when input leaves home again, or the shake is turned off and on again, within that time.
>
> - parity: Python's Windows app made a new shake detector when the shake was turned on again, which dropped the 1.5 s (#16: one crossing engine for every platform; the Mac's)

r[crossing.shake.sensitivity-change]
A change of sensitivity that changes S starts any gesture in progress over, and leaves the 1.5 s
after the last shake running.

r[crossing.shake.firing-motion]
The motion events of a shake before it fires reach the driven machine as any motion does. The
motion event that fires it reaches neither the driven machine nor the driver's own pointer.

r[crossing.shake.home-still]
After a shake brings input home, the driver's zones take no push from its pointer until the pointer
has been still for 200 ms: each motion event in that time starts the 200 ms again and drops any push
pressure built so far, and the pointer moves on the driver as usual. Input that comes home by any
other crossing method is not kept from its zones like this.

r[crossing.shake.notice]
Each of the first three shakes that bring input home on a machine, counted across restarts, brings
a notice naming the machine input came home from and where the shake is turned off. From the fourth
on, a shake brings no notice.


## Pausing

Two guards turn a machine's zones off for a while without touching its zone choices: the crossing
pause, which the user sets from the menu, and the full-screen pause, which follows the app in front.

> r[crossing.pause.zones]
> While the user has paused crossing on a machine, from its menu, none of its zones is armed, for
> its own pointer or for its driver's: no zone builds push pressure, keeps the pointer held at edge
> or crosses, and no zone stops the pointer at an edge between two of its displays. A crossing pause
> disarms only the zones of the machine it is set on, its zones armed for its driver included, never
> another machine's. Resuming arms the zones again with no other change.
>
> - model: driving

> r[crossing.pause.other-methods]
> During a crossing pause the shortcut, jump keys, a shake and the menu still cross.
>
> - model: driving

r[crossing.pause.restart]
A crossing pause is not kept across a restart: Beamer starts with crossing resumed.

### Full-screen pause

> r[crossing.full-screen.zones]
> While a machine's full-screen pause setting is on and an app other than Beamer is full screen in
> front on it, none of its zones is armed: neither its home zones nor those armed for its driver,
> so pushing against them crosses nowhere, and the driver's pointer neither comes home nor passes on
> through them. Its zones are armed again as soon as no app is full screen. The setting of the
> machine whose app is full screen decides, whichever machine drives the pointer on it: the driver's
> setting and its full-screen apps never disarm a driven machine's zones.
>
> - model: driving

> r[crossing.full-screen.other-methods]
> During a full-screen pause the shortcut, jump keys, a shake and the menu still cross.
>
> - model: driving

r[crossing.full-screen.setting]
The full-screen pause setting is off until the user turns it on, and is each machine's own. While
it is off, a full-screen app leaves every zone armed. Turning it on or off applies at once, before
the settings are saved, to an app already full screen: turned off, the pause ends by the next
pointer motion; turned on, the zones pause within 1 s. A change to the crossing map made meanwhile
does not undo it.

r[crossing.full-screen.check]
A machine with the full-screen pause setting on checks once a second whether the app in front is
full screen: a full-screen pause starts within 1 s of an app going full screen and ends within 1 s
of it leaving full screen or the front.

r[crossing.full-screen.check-fails]
When a machine cannot tell whether an app is full screen, its zones work as if none were, and it
stops checking until Beamer restarts, logging the failure once.

r[crossing.full-screen.not-beamer]
Beamer in front is never a full-screen app, even when the OS reports it full screen.

r[crossing.full-screen.front-app-only]
Only the app in front can cause a full-screen pause: a window of any other app, display-sized or
not, does not.

> r[crossing.full-screen.mac-ax]
> On the Mac, an app in front whose active window is full screen, as the window's green button or
> the View menu's Enter Full Screen makes it, is full screen. An app whose windows Accessibility
> cannot read is not full screen by this test.
>
> - tier: machine (an app's full-screen state comes only from the real Accessibility API)

> r[crossing.full-screen.mac-borderless]
> On the Mac, an app in front with an ordinary window whose position and size each match one of the
> displays to within 1 point is full screen, as a borderless full-screen game is.
>
> - tier: machine (window bounds and layers come only from the real window server)

> r[crossing.full-screen.mac-zoomed]
> On the Mac, a window that fills a display below its menu bar, as a zoomed window does, is not full
> screen, on a display with a notch included.
>
> - tier: machine (window bounds and the menu bar come only from the real window server)

> r[crossing.full-screen.mac-desktop]
> On the Mac, the desktop, the wallpaper and any other window outside the ordinary window layer are
> never full screen, whatever their size, so clicking the desktop does not pause the zones.
>
> - tier: machine (window layers come only from the real window server)

> r[crossing.full-screen.windows-exclusive]
> On Windows, an exclusive full-screen game or presentation mode in front is full screen.
>
> - tier: machine (the notification state comes only from the real Windows shell)

> r[crossing.full-screen.windows-busy]
> On Windows, while the shell reports the user busy, the app in front is full screen only when its
> window covers its whole display exactly and is not the desktop or the taskbar. A maximised window,
> which stops at the taskbar, is not.
>
> - tier: machine (the notification state and window bounds come only from the real Windows shell)

## Pointer hiding

The driver hides its own pointer while its input is on another machine (the driving hide), and a
driven machine hides its pointer after its driver lets go of it (the idle hide), so that only the
machine in use shows a pointer.

r[crossing.pointer.still-while-driving]
A driver's own pointer stays where it was while it drives (`input.capture.pointer-stays`,
`input.capture.away`). Once input is home, whatever brought it home (a crossing method, capture
lost, the link lost, settings applied, a failure), the pointer follows its mouse again, and no
pointer motion is undone.

r[crossing.pointer.hide-driving]
From the moment a driver's input moves to another machine until it comes home, the driver's own
pointer is hidden, where the hide-pointer setting allows it
(`crossing.pointer.setting-off-driving`).

r[crossing.pointer.no-crossing-no-hide]
A crossing that does not happen, because the driver may not drive that peer or the peer refuses,
hides nothing.

r[crossing.pointer.shown-home]
Whenever a driver's input is home, its pointer is shown, whichever path brought the input home: a
crossing method, the link to the driven machine lost, the driven machine silent, capture failing,
another machine taking over the driven machine, or settings being saved.

r[crossing.pointer.in-order]
The driving hide and its show follow the order in which input moved: input that comes home while
the hide for its crossing is still in progress ends with the pointer shown.

r[crossing.pointer.idle-hide]
When the driver lets go of a machine, that machine's pointer is hidden, unless the hide-pointer
setting is off or Beamer is quitting.

r[crossing.pointer.idle-settle]
A move of an idle-hidden pointer within 0.3 s of the hide is taken to be the driver's last and does
not show it.

r[crossing.pointer.idle-show-move]
An idle-hidden pointer is shown within 0.1 s of moving from where it was 0.3 s after the hide,
whatever moves it: this machine's own mouse or trackpad, or a driver.

r[crossing.pointer.idle-show-driver]
A driver taking a machine shows that machine's idle-hidden pointer at once.

r[crossing.pointer.idle-unreadable]
When a machine cannot read its pointer's position while the pointer is idle-hidden, it shows the
pointer.

r[crossing.pointer.idle-afresh]
An idle hide that follows a show is watched from its own start: its own 0.3 s, then the next move
shows it.

r[crossing.pointer.reasons]
A pointer hidden both by the driving hide and by the idle hide is shown only once neither applies.

r[crossing.pointer.hide-once]
Beamer asks the OS to hide the pointer once for each hide, however often the cause of that hide
recurs while it is in effect, and to show it once when the hide ends. It never asks the OS to show a
pointer it has not hidden.

r[crossing.pointer.hide-refused]
A hide the OS refused counts as not made: no show follows it, and an idle hide the OS refused does
not watch the pointer.

r[crossing.pointer.shown-on-quit]
When Beamer quits, every pointer hide it made ends, and the pointer is shown.

r[crossing.pointer.setting-default]
The hide-pointer setting, which hides the pointer on the machine not in use, is on until the user
turns it off, and is saved.

r[crossing.pointer.setting-per-machine]
The hide-pointer setting is each machine's own, never a shared entry: changing it on one machine
changes it on no other, even when that machine shares its other settings with the cluster.

r[crossing.pointer.setting-off]
With the hide-pointer setting off, a driver letting go hides nothing. Turning it off shows an
idle-hidden pointer at once.

> r[crossing.pointer.setting-off-driving]
> The hide-pointer setting also governs the driving hide: off, a driver's pointer is shown while it
> drives, and turning it on while the machine drives hides the pointer at once.
>
> - parity: Python's Mac hid the driver's pointer whatever the setting, and only its Windows app let the setting govern the driving hide (#16: one crossing engine for every platform; the Windows app's)

r[crossing.pointer.veil]
While a Windows machine drives another with its pointer hidden, Beamer puts a veil under the point
where its pointer stays. The veil goes when input comes home. No veil appears while the driving
hide does not apply.

> r[crossing.pointer.veil-app-cursor]
> While the veil is up, no cursor of an app's own (a browser page's custom cursor, a game's) shows
> at the point where the driver's pointer stays.
>
> - tier: machine (an app's own cursor is drawn only by the real Windows desktop)

> r[crossing.pointer.veil-clicks]
> From the moment input comes home, a click at the point where the pointer stayed reaches the window
> under the veil, even before the veil is gone.
>
> - tier: machine (where a click lands is decided by the real Windows desktop)

> r[crossing.pointer.os-hidden]
> A hidden pointer is not visible on any of the machine's displays, over any app, until Beamer shows
> it again; shown again, it has the user's own pointer scheme, size and colour.
>
> - tier: machine (whether the OS pointer is really hidden needs the real window server)

> r[crossing.pointer.mac-background]
> On the Mac, a hidden pointer stays hidden whichever app is in front.
>
> - tier: machine (the window server's handling of a background app's hide needs the real Mac)

> r[crossing.pointer.mac-background-ask]
> On the Mac, Beamer asks the window server once, before its first hide, to let it hide the pointer
> while another app is in front. When that is refused, Beamer still hides the pointer, and crossing
> goes on.
>
> - tier: machine (the window server's answer comes only from the real Mac)

> r[crossing.pointer.windows-shapes]
> On Windows, a hidden pointer is hidden in every shape it can take: the arrow, the text cursor,
> busy, the hand and every resize and move shape.
>
> - tier: machine (the system pointer shapes are the real Windows desktop's)

> r[crossing.pointer.secure-desktop]
> On Windows, while the sign-in desktop, Ctrl+Alt+Del or an administrator prompt has the input, a
> hidden pointer is shown within 0.25 s of that desktop taking the input, and hidden again within
> 0.25 s of the user's desktop getting it again.
>
> - tier: machine (secure desktops exist only on a real Windows session)

> r[crossing.pointer.crash-windows]
> If Beamer ends while it has the pointer hidden on Windows, by a crash included, the pointer is
> shown when Beamer next starts, before its window shows and before its first crossing.
>
> - tier: machine (a hidden Windows pointer outlives the process that hid it)

> r[crossing.pointer.crash-mac]
> If Beamer ends while it has the pointer hidden on the Mac, by a crash included, the pointer is
> shown when its process ends.
>
> - tier: machine (the window server shows the pointer again when the process that hid it ends)

### Pointer overlay

A Windows PC with no mouse attached shows no pointer of its own, so a driver moving its pointer
would move an invisible one. The pointer overlay draws the system arrow there. How it is drawn is
`ui`'s; when it shows is crossing's.

r[crossing.pointer.overlay.shows]
While a driver drives a Windows machine that has no mouse attached, and Windows is not showing its
own pointer, the machine shows the pointer overlay at its pointer's position, once the driver has
moved the pointer there.

r[crossing.pointer.overlay.native-visible]
While Windows shows its own pointer, no pointer overlay is shown.

r[crossing.pointer.overlay.suppressed]
A pointer Windows reports as suppressed, as it does while touch or a pen is in use, counts as not
shown.

r[crossing.pointer.overlay.mouse-present]
While a mouse is attached, no pointer overlay is shown, even when Windows hides its pointer: an app
may hide it on purpose. A mouse plugged in while the overlay shows removes it within 16 ms.

r[crossing.pointer.overlay.first-move]
Until the driver has moved this machine's pointer since it took this machine, no pointer overlay
is shown and no overlay window exists. A move counts only once Windows has accepted it and only if
it moves the pointer by more than nothing.

r[crossing.pointer.overlay.driven-only]
No pointer overlay is shown while no driver drives the machine. When the driver lets go, the overlay
is gone within 16 ms, and the driver's moves are forgotten: a driver that takes the machine later
moves the pointer again before the overlay shows.

r[crossing.pointer.overlay.unknown]
When Windows cannot say whether its pointer is showing, no pointer overlay is shown, and the
failure is logged once until Windows can say again.

r[crossing.pointer.overlay.other-desktop]
While the sign-in desktop, Ctrl+Alt+Del or an administrator prompt has the input, no pointer
overlay is shown.

r[crossing.pointer.overlay.follows]
While the pointer overlay shows, it is placed at the pointer's position at least every 16 ms.

> r[crossing.pointer.overlay.no-input]
> The pointer overlay takes no clicks and never becomes the active window: clicks at the pointer
> reach the window under it.
>
> - tier: machine (where a click lands is decided by the real Windows desktop)


## Profiles

A profile is a named, shared crossing map with every machine's zone choices and the shake settings.
Profiles, their names, every machine's zone choices in them and the active profile are shared
entries in the store. Whether a machine's zones come from the active profile or from its own zone
choices is Own zones'.

r[crossing.profile.first]
Every machine starts with one profile, named "Desk", whose id is the same on every machine, with
the shake on at a sensitivity of 100 %. Its entries are older than any change made to them, so the
first profiles of machines that have never met are one profile, and a change to it made on any
machine wins over the start values.

r[crossing.profile.make]
Making a profile gives it a new id and the name given, and starts it with a copy of every
machine's zone choices and of the shake settings of the active profile. The active profile does not
change.

r[crossing.profile.name]
A profile's name is the text given, or received, with its leading and trailing whitespace removed,
and is 1 to 32 Unicode code points long. Making or renaming a profile with any other name is
refused and changes nothing, and a name arriving that breaks this is not taken.

r[crossing.profile.same-name]
Two profiles may have the same name.

r[crossing.profile.make-limit]
Making a profile is refused, and changes nothing, while 8 profiles that are not removed exist.

r[crossing.profile.made-apart]
Profiles made on machines that were not linked are all kept once the machines link, even when that
makes more than 8 profiles that are not removed.

r[crossing.profile.rename]
Renaming a profile changes its name only. Renaming a profile that is removed, or that the machine
does not have, is refused and changes nothing.

r[crossing.profile.remove-refused]
Removing the active profile, the last profile that is not removed, a profile already removed or a
profile the machine does not have is refused and changes nothing.

> r[crossing.profile.removed]
> A removed profile is removed on every machine, a machine that was away when it was removed
> included. It is never made active or renamed again on this machine; a newer entry making it
> active that arrives from another machine is `crossing.profile.remove-active`'s.
>
> - model: store

> r[crossing.profile.remove-active]
> A removal of the active profile that arrives from another machine is kept, but the profile stays
> as it was while it is active, and is removed once another profile is made active. A removal of a
> profile and another profile being made active leave every machine with the same profiles in
> either order of arrival.
>
> - model: store

> r[crossing.profile.kept-limit]
> A machine keeps at most 64 profiles, removed ones included. Past 64, it drops removed profiles,
> the oldest removal first and, between removals with the same stamp, the lowest profile id first,
> never the active profile, so every machine drops the same ones whatever order they arrived in.
> Making a profile first drops removed profiles in that order until it keeps at most 63.
>
> - model: store

> r[crossing.profile.dropped-stays-gone]
> An entry that arrives for one of the last 256 profiles this machine dropped is ignored, so a late
> name, zone choices or removal does not make the profile exist again.
>
> - model: store

r[crossing.profile.live-limit]
Profile entries arriving from a peer that would leave this machine keeping more than 64 profiles,
after it has dropped removed ones, are refused whole: none of them is taken.

> r[crossing.profile.partial]
> Entries for a profile whose name has not arrived yet, an active-profile entry naming it included,
> are kept, across a restart too, and the profile is complete once its name arrives.
>
> - model: store

r[crossing.profile.listed]
The profiles a machine offers are the ones it has a name for that are not removed.

### The active profile

> r[crossing.profile.active.shared]
> One profile is active across the cluster. Making a profile active on any machine makes it active
> on every machine; of two profiles made active concurrently, the one made active with the newer
> stamp is active everywhere.
>
> - model: store

r[crossing.profile.active.refused]
Making active a profile that is removed or that the machine does not have is refused and changes
nothing. Making the active profile active changes nothing.

> r[crossing.profile.active.zones]
> While own zones on every machine is off, a machine that does not keep its own zones has the zones
> its zone choices in the active profile give. When another profile is made active, anywhere, its
> zones become those its zone choices in that profile give, from its next push
> (`crossing.armed.follows-changes`).
>
> - model: store

> r[crossing.profile.active.no-choices]
> While own zones on every machine is off, when the active profile has no zone choices for a
> machine, that machine writes its current zone choices there, as entries that lose to any zone
> choices for it written by another machine.
>
> - model: store

> r[crossing.profile.active.map]
> Each profile has its own crossing map, footprint sizes included. Making another profile active
> keeps the crossing map in force as the map of the profile left, and puts the new profile's
> crossing map in force on every machine. Making the profile left active again restores the
> crossing map it had when it was left.
>
> - model: store

r[crossing.profile.active.map-new]
A profile that has never been active takes the crossing map in force when it is first made active.

r[crossing.profile.active.map-displays]
When a profile's crossing map is put in force, each machine keeps its displays as they are now, the
map's display offsets for displays a machine no longer has are dropped, and a machine the map does
not place keeps the footprint it had.

r[crossing.profile.active.map-refused]
Making active a profile whose crossing map yields no valid crossing map once put in force is
refused and changes nothing.

> r[crossing.profile.active.dropped]
> An active-profile entry naming a profile this machine has dropped is not taken: the machine keeps
> the profile it has active, writes that profile as active again with a stamp newer than the one
> that arrived, and sends it, so every machine ends with the profile this machine keeps. Nothing
> else changes, the shake settings in force included.
>
> - model: store

### Zone choices in a profile

> r[crossing.profile.choices.entry]
> A machine's zone choices in a profile are one entry: of two concurrent changes to them, the one
> with the newer stamp wins whole on every machine, even when the two changed the choices toward
> different peers. Concurrent changes to different machines' zone choices all land.
>
> - model: store

> r[crossing.profile.choices.edit-here]
> While own zones on every machine is off, an edit made on a machine to its own zones, when it does
> not keep its own zones, changes its zone choices in the active profile, and the change reaches
> every machine.
>
> - model: store

> r[crossing.profile.choices.edit-other]
> While own zones on every machine is off, any machine may change another machine's zone choices in
> the active profile. The change alters that machine's zone choices only, and the editing machine's
> zones stay as they were. Once the change reaches the machine it is for, that machine's zones
> follow it.
>
> - model: store

r[crossing.profile.choices.edit-other-refused]
An edit of another machine's zone choices is refused, and changes nothing, when either the machine
or the peer is not a valid machine id, or when it would add a 33rd peer to those zone choices.

r[crossing.profile.choices.notch-mac]
An edit of another machine's zone choices leaves its notch and return through the notch as they
were unless that machine is a Mac, as its profile status says or, before it has one, as its
pairing recorded.

r[crossing.profile.choices.corner-taken]
In one machine's zone choices, a corner leads to at most one peer. An edit of another machine's
zone choices giving a peer one corner that another peer there already has is refused and changes
nothing; one giving a peer several corners gives it only those no other peer there has.

r[crossing.profile.choices.notch-stable]
Taking zone choices that give the notch to several peers leaves them as they arrived: the machine
writes and sends no change to them, and its notch zone leads to one of those peers
(`crossing.derive.notch.one`).

> r[crossing.profile.choices.unpaired]
> A machine's zone choices in a profile may name peers it is not paired with. It has no zones toward
> them until it pairs with them, and then its zones toward them follow those choices. When a peer is
> unpaired, the machine keeps its zone choices toward it while any machine in the cluster still
> lists that peer, and pairing with that peer again makes its zones toward it follow them. Once no
> machine in the cluster lists the peer, the zone choices toward it in every profile read as unset,
> as the store reads every shared entry naming a forgotten machine (#14). No machine writes their
> removal, and a change to a machine's zone choices meanwhile keeps its choices toward the forgotten
> peer, so when the peer is paired again they read as set again, as they were, as its footprint
> does (`crossing.map.members.forgotten`), and the zones toward it follow them. When, at the first
> link after that pairing, its footprint counts as none and it is placed by default
> (`crossing.map.members.forgotten`), the zone choices toward it in every profile start from the
> default zone choices (`crossing.derive.choices.default`), as a peer just paired does
> (`crossing.derive.first-zone`): the machine that writes that default placement writes, in the
> same change, the default zone choices toward it in every profile, in the zone choices of each
> machine paired with it. This resets only zone choices in profiles. A machine's own zone choices,
> which are never shared, keep their choices toward it as they were, so a machine with own zones
> derives its zones toward it at its new place from them (`crossing.derive.first-zone`).
>
> - model: store
> - parity: Python kept a machine's zone choices toward an unpaired peer in every profile for good: they stayed readable while no machine listed it, the peer still named in the profile status as one waiting for a pairing (`core/profiles.py:358-362`), and pairing with it again followed them even though it always placed the peer by default (#14: shared entries naming a forgotten machine read as unset while it is forgotten and are never written as removed, and a machine paired again over another starts over as one just paired)

r[crossing.profile.choices.may-not-drive]
A machine keeps its zone choices toward a peer it may not drive, and any edit made to them
meanwhile. When it may drive that peer again, its zones toward it follow the kept choices.

r[crossing.profile.choices.no-permissions]
Zone choices never set what a machine may drive of a peer or what a peer may drive of it: those
come from pairing alone (`pairing.peers.permissions`).

r[crossing.profile.choices.new-peer]
A peer newly paired is added to the machine's zone choices with the choices its zones toward it
have now. While the machine has own zones, adding it saves nothing to saved zones and does not
count as an edit. While its zones come from the active profile, adding it is an edit made there: it
changes its zone choices in the active profile, which reaches every machine, and saves the choices
before it as `crossing.profile.saved.edit` says.

r[crossing.profile.choices.only]
Zone choices contain, for each peer, only whether the whole edge leads there, which thirds, which
edges wrap round to it, which corners, whether the notch leads there, and whether return through
the notch is on. They never carry a key, token, address, name or permission.

r[crossing.profile.choices.machine-id]
Zone choices belong to a machine id: a machine with a new machine id never takes the zone choices
written for its old one.

### What a profile entry may contain

r[crossing.profile.schema.choices]
A machine's zone choices name at most 32 peers, each by a valid machine id. For each peer, the
whole edge, the corner, the notch and return through the notch are each true or false; the thirds
are one to three of start, middle and end; the wrap-around edges are any of top, bottom, left and
right; the corners are any of the four corners; none is repeated, and there is nothing else.

r[crossing.profile.schema.map]
A profile's crossing map places at most 33 machines, each with an integer x, y, width and height,
the width and height above 0, and at most 16 display offsets, each named by 1 to 64 ASCII
characters and giving an integer x and y.

r[crossing.profile.schema.alone]
A profile entry that arrives breaking its schema (`crossing.profile.name`,
`crossing.shake.sensitivity-range`, `crossing.profile.schema.choices`,
`crossing.profile.schema.map`) is ignored, and the other entries that arrive with it are taken.

### Shake settings

> r[crossing.profile.shake.in-profile]
> While own zones on every machine is off, the shake on or off and the shake sensitivity in force
> on every machine are the active profile's, whether or not the machine keeps its own zones, and
> changing either on any machine changes them in the active profile for every machine.
>
> - model: store
> - parity: Python carried them as registers of its own in per-feature crossing messages, merged by its own rules (#14: the store replaces per-feature messages)

r[crossing.profile.shake.own]
While own zones on every machine is on, each machine has its own shake on or off and shake
sensitivity: changing them sends nothing, a profile's shake settings arriving or changing never
overwrite them, and turning own zones on every machine on puts them in force again.

r[crossing.profile.shake.applies]
The shake settings in force apply to shake detection from the moment a machine starts capturing
input, and from the moment they change. A change that leaves both shake settings as they were does
not restart shake detection.

### Profile status

> r[crossing.profile.status.record]
> Each machine publishes its profile status in its machine record: whether its zones come from its
> own zone choices or from a profile, the profile it has active, the stamp of its zone choices in
> that profile it applied, whether it is a Mac, and the peers its zone choices name that it is not
> paired with.
>
> - parity: Python sent the status as a register of its own in its profile messages, with the app's version, and any machine could relay it (#14: a machine's own state is its machine record; `link.version.alpn` makes every linked machine the same version)

r[crossing.profile.status.changes]
A machine writes its profile status again only when what it says changes.

r[crossing.profile.status.invalid]
A profile status that lacks any of its fields, or names more than 32 peers, is ignored.

### Other machines' zones

r[crossing.profile.view.other-zones]
A machine derives another machine's zones from that machine's zone choices in the active profile
and the crossing map, by the same rules as its own zones, with every peer it has kept off its own
crossing map kept off for that machine too.

r[crossing.profile.view.machines]
The machines a profile covers are this machine, its peers on its crossing map, every machine with a
profile status, and every machine named in zone choices of a profile that is not removed, less
every inactive peer of this machine.

r[crossing.profile.no-zone-home]
For each peer this machine's zones lead to, the machine warns that input cannot come home from it
when the zones that peer's known zone choices give it include no whole-edge or thirds zone leading
to this machine.

r[crossing.profile.no-zone-home.known]
A peer's zone choices are known when its profile status says it takes them from a profile and
names the stamp of the zone choices this machine has for it in that profile, or when it has no
profile status and has zone choices in the active profile. The machine never warns about a peer
that keeps its own zones, or whose zone choices it does not know.

r[crossing.profile.no-zone-home.actual]
The check reads this machine's zones as they are: no zone leads to a peer it may not drive, a third
that misses where the two machines touch on the crossing map is no zone, and a peer kept off this
machine's crossing map is no zone of the peer's either.

### Saved zones

r[crossing.profile.saved.never-sent]
A machine's saved zones stay on that machine and are never sent.

r[crossing.profile.saved.edit]
The first edit made on a machine to its zone choices, after they were last set by anything other
than an edit made there, saves the choices as they were before it. Later edits save nothing until
the choices are next set by something else.

r[crossing.profile.saved.replaced]
When a change from another machine replaces this machine's zone choices in any profile, the choices
replaced go to its saved zones, naming the machine that made the change.

r[crossing.profile.saved.active-change]
When another profile is made active while own zones on every machine is off and the machine does
not keep its own zones, its zone choices in the profile left go to its saved zones, naming the
machine that made the new profile active.

r[crossing.profile.saved.dedupe]
Saved zones contain each set of zone choices once: saving choices already there keeps one entry,
with the newer time and naming the machine named last.

r[crossing.profile.saved.restore]
Restoring saved zone choices makes them the machine's own zone choices and has it keep its own
zones. The zone choices in force before, and its own zone choices before, go to its saved zones
first.

r[crossing.profile.saved.delete]
Deleting saved zone choices removes them from saved zones only: no zone and no profile changes.

r[crossing.profile.saved.missing]
Restoring or deleting saved zone choices the machine does not have is refused and changes nothing.

r[crossing.profile.saved.no-loss]
No zone choices a machine has had in force are lost: whatever replaces them (an edit, a change
arriving, own zones on every machine turned on or off, keeping its own zones or not, making a
profile or making one active, a peer unpaired or paired again, a restore), they stay its own zone
choices, its zone choices in a profile, or in its saved zones.

### Profiles across the cluster

> r[crossing.profile.sync.reaches]
> A change to a profile, to the active profile or to own zones on every machine, made on any
> machine, reaches every machine in the cluster, through other machines where two machines have no
> link, and a machine that was away takes every such change it missed when it links again.
>
> - model: store
> - parity: Python sent its own profile messages: the whole set to a peer on link up, then what it took on to its other peers and what it had newer to the sender, never to a peer without the profiles capability, and in frames of at most 48 KiB (#14: the store replaces per-feature messages)

> r[crossing.profile.sync.newer]
> Each profile entry ends, on every machine, as the one with the newest stamp. An entry no newer
> than the one the machine has changes nothing and is not sent on.
>
> - model: store
> - parity: Python stamped each register with whole seconds of wall time raised above the stamp it had, broke ties by the writer's machine id, and ignored a register stamped more than 600 s ahead of its own clock (#14: stamps are a hybrid logical clock, never raw wall time)

> r[crossing.profile.sync.any-machine]
> Profile entries are taken from every machine in the cluster, whichever machine wrote them and
> whichever link they came over, whatever this machine and that peer may drive of each other.
>
> - model: store
> - parity: Python took profile entries only from a linked peer that may drive this machine and is not dormant, and refused those from any other peer whole, nothing changing and nothing sent on (#14: the store takes shared entries from every machine, and features declare only their keys and schemas)

> r[crossing.profile.sync.settles]
> After concurrent changes to profiles from up to four machines, once every change sent has been
> taken, every machine has the same profiles, active profile and own zones on every machine, one
> crossing map, and no further change to profiles is sent.
>
> - model: store

## Own zones

A machine has own zones when it keeps its own zones, a choice of its own, or while own zones on
every machine, one setting for the whole cluster, is on. With own zones, its zones follow its own
zone choices; otherwise they follow its zone choices in the active profile.

> r[crossing.own-zones.every-machine]
> Own zones on every machine is one setting for the whole cluster, on until the user turns it off:
> turning it on or off on any machine turns it on or off on every machine, and of two concurrent
> changes the one with the newer stamp wins.
>
> - model: store

r[crossing.own-zones.every-machine-same]
Turning own zones on every machine on while it is on, or off while it is off, changes nothing and
sends nothing.

> r[crossing.own-zones.every-machine-off]
> When own zones on every machine turns off, each machine whose zone choices in the active profile
> are missing or were written before the change writes its current zone choices there. No machine's
> zones change, and no machine's own zone choices change.
>
> - model: store

> r[crossing.own-zones.every-machine-off-loses]
> Zone choices a machine writes because own zones on every machine turned off lose to any zone
> choices for it written by a machine that had already taken that change, whatever the two
> machines' clocks say.
>
> - model: store

> r[crossing.own-zones.every-machine-off-again]
> A change turning own zones on every machine off that arrives while it is already off rewrites no
> machine's zone choices.
>
> - model: store

> r[crossing.own-zones.every-machine-on]
> When own zones on every machine turns on, each machine that does not keep its own zones takes its
> zone choices in the active profile as its own zone choices, and its previous own zone choices go
> to its saved zones. A machine that keeps its own zones keeps its own zone choices unchanged. No
> machine's zones change.
>
> - model: store

r[crossing.own-zones.every-machine-edits]
While own zones on every machine is on, an edit to a machine's zones changes only its own zone
choices, and nothing is sent to its peers. Editing another machine's zone choices is refused and
changes nothing.

r[crossing.own-zones.this-machine]
Keeping its own zones is a choice of each machine alone: it is never sent, and nothing that arrives
turns it on or off.

r[crossing.own-zones.in-force]
While a machine keeps its own zones, its zones follow its own zone choices. Changes to its zone
choices in the active profile, made anywhere, are taken into the profile and change none of its
zones.

r[crossing.own-zones.edits]
While a machine keeps its own zones, an edit made on it to its zones changes its own zone choices,
and nothing is sent.

> r[crossing.own-zones.stop]
> When a machine stops keeping its own zones, while own zones on every machine is off, its zones
> follow its zone choices in the active profile, as they are then, and its own zone choices stay as
> they were.
>
> - model: store

> r[crossing.own-zones.share]
> Sharing its own zones writes a machine's own zone choices as its zone choices in the active
> profile, and the change reaches every machine. While own zones on every machine is on, it is
> refused and changes nothing.
>
> - model: store

## Logs

Each move of a driver's input, and the crossing method that started it, is logged on the driver
(`driving.log.move`, `driving.log.started`). These are the lines crossing adds.

r[crossing.log.arrival]
When a driven machine accepts a take, it logs at info level one line naming the driver and the
edge the pointer landed at: the named display's edge when the take names one. For a take with no
landing point, the line says so and gives the pointer's position, which stays where it was.

r[crossing.log.zone-fired]
When one of a driven machine's zones fires, it logs one line naming the driver and the edge of its
own display the pointer pushed through.

r[crossing.log.shortcut-key]
While the shortcut is on, each press of the shortcut key that is not a repeat logs a line naming
the shortcut's style and where input is: home, or on the named peer. While it is off, its presses
log nothing.

r[crossing.log.pointer-unreadable]
A pointer motion whose position the platform backend cannot give writes one warning; later such
motions write none until a position is given again.

r[crossing.log.zones-failed]
A failure working out a zone push (`crossing.push.failure-off`) writes one error line saying that
zone pushes are off for 30 s.

r[crossing.log.map-unchanged]
A change to the crossing map arriving from a peer that changes nothing here writes no log line.
