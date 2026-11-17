# Driving

Who drives whom: the driver, the driven machine and home; the take and its answer, let go, pass
on, send home and take over; the move number, the reach and the chain; and N machines, input
passed on from one to the next. Driving has no row of its own in #11's inventory: it is the
routing under every crossing (F5), which #11's standing decision keeps for three or more
machines, pass on across several machines included.

The rules come from the Python app at `archive/python` (`core/owner.py`, the driven machine's half
in `core/receiver.py`, and the apps that carry them out, `mac_app/bridge.py` and
`win_app/sender.py`), mined through its tests in `spec/ledger/driving.txt`, and from the meaning
#14 keeps from `WIRE.md` sections 4 and 5: ownership and routing, route numbers, `focus`, `accept`,
`refuse` and `switch`, and chains needing a direct link from the driver to every machine. Their
bytes are not kept: a take, its answer, a let go, a pass on and a reach update are control
messages on the link (`spec/link.md`).

What stays elsewhere: the link, its loss and its reasons are `spec/link.md`'s; the peers, and
whether a peer may drive this machine or be driven by it, are `spec/pairing.md`'s; waking a peer
with no link is `spec/discovery.md`'s; which zone fires, where it lands the pointer and the
crossing methods are `spec/crossing.md`'s; what keys and pointer moves mean, and how they are
injected, are `spec/input.md`'s; the clipboard that follows input is `spec/clipboard.md`'s; what a
page, notice or alert says is `spec/viewmodel.md`'s.

Dropped, so no rule here (#11): phone peers, which drove without a hand at an edge and stopped on
a send home, and Windows' lock screen, whose unlock (F21) was what sent a driver home from a locked
PC.

Every rule is `ci` unless it says otherwise. Behaviour across three or more machines is tested
through `beamer-sim`.

## Who drives

> r[driving.role.one-driver]
> A machine has at most one driver at a time, and injects input from no other machine.
>
> - model: driving

> r[driving.role.not-both]
> A machine never drives and is driven at once. From the moment it sends a take until that take
> is refused, goes unanswered or is let go, and for as long as its input is on another machine, it
> refuses every take as driving another machine.
>
> - model: driving

> r[driving.role.take-each-other]
> When two machines each take the other before either take arrives, both refuse as driving
> another machine, both inputs come home, and neither machine is driven. Once the 3 s out of reach
> have passed (`driving.take.out-of-reach`), a take by either is accepted.
>
> - model: driving

> r[driving.role.drive-while-driven]
> When a driven machine's own user crosses to another machine, by any crossing method, the driven
> machine first sends its driver home (`driving.send-home.message`), and makes the crossing once
> its driving has ended, by the driver's let go or by force after 1 s
> (`driving.send-home.forced`). When it is still driven 1.5 s after the send home, the crossing
> is not made.
>
> - parity: Python's Mac app gave up after 1.3 s, and its Windows app after 1.5 s (one value for both platforms: the Windows app's, past the 1 s forced end)
> - model: driving

> r[driving.role.drive-while-driven-once]
> While a driven machine waits for its driving to end before crossing, another crossing its user
> asks for is ignored, and its user picking home cancels the crossing.
>
> - model: driving

> r[driving.role.drive-while-driven-unreachable]
> When a driven machine's own user crosses to another machine and the send home cannot be given to
> the driver's link (`link.send.full`), the crossing is not made, the machine stays driven, and
> Core reports that it cannot cross because its driver could not be reached.
>
> - model: driving

## Permission

> r[driving.permission.told]
> A machine tells each peer it has a link with whether that peer may drive it: once as the link
> comes up, and again each time the user changes it.
>
> - parity: Python told it in the link's first answer and in an `accepts` message, only on the links a peer had dialled (#14: one link per pair carries it both ways)
> - model: driving

> r[driving.permission.reach-only]
> A driver takes only a peer it has a link with that has told it this machine may drive it, and
> that this machine may drive (`pairing.peers.permissions`).
>
> - model: driving

> r[driving.permission.off]
> When the user turns off may be driven by for the driver of this machine, its driving ends
> (`driving.end.keys-up`), this machine refuses it with the move number of the last take or reach
> update it accepted from it, as not allowed, and tells it that it may not drive this machine.
>
> - model: driving

> r[driving.permission.on]
> When the user turns may be driven by on again for a peer, the peer is told, and its next take is
> answered as any other.
>
> - model: driving

## The move number

> r[driving.move.one-number]
> A driver keeps one move number across all its links, 0 when Core starts, and never lowers it. It
> raises it by one for each move of its input, each take it makes for a pass on, and each reach
> update it sends, and every message that one of these sends carries the number it raised to.
>
> - model: driving

> r[driving.move.shared]
> When the driver's input moves from one machine to another, the let go of the machine it leaves
> and the take of the machine it goes to carry the same move number.
>
> - model: driving

> r[driving.move.not-higher]
> A machine ignores, and does not answer, a take, reach update or let go whose move number is not
> higher than that of the last take or reach update it accepted, or let go it acted on, on that
> link. Move numbers on different links are never compared.
>
> - model: driving

## The take

> r[driving.take.contents]
> A take carries the move number, the reach, and how far, from 0 to 1000 pixels, a push must go
> through one of the driven machine's zones to cross. A take made by a zone also carries the
> landing point the zone gives: an edge of the driven machine and a fraction along it, and the
> display and its edge when the zone names them. A take made by any other crossing method carries
> no landing point.
>
> - model: driving

> r[driving.take.offset-clamped]
> A landing fraction below 0 is taken as 0, and one above 1 as 1.

> r[driving.take.malformed]
> A take or reach update is refused as malformed when its landing fraction is not a finite number,
> its push distance is over 1000 pixels, its reach names more than 32 machines or one machine twice,
> or its display name is empty, longer than 64 bytes or contains a byte other than an ASCII letter,
> digit, `_`, `.`, `:` or `-`.
>
> - parity: Python also refused as malformed a take whose fields had the wrong JSON type, an edge without a fraction or the reverse, or numbers strict JSON does not allow, and ignored one whose move number or machine id would not parse (#14: messages are postcard-encoded enums, and one that does not decode closes the link, `link.frame.undecodable`)

> r[driving.take.input-at-once]
> From the moment a driver sends a take, its input goes to that machine, without waiting for the
> answer, and none of it acts here, until the input moves again.
>
> - model: driving

> r[driving.take.input-here]
> While the input is home, the driver's input acts here and goes to no machine.
>
> - model: driving

> r[driving.take.from-driven]
> When the driver's input is on a machine and its own crossing method picks another peer, it
> first lets go of the machine the input is on (`driving.keys.released-on-leaving`), then takes the
> peer picked, with the same move number. A refusal of that take brings the input home, not to
> the machine it left.
>
> - model: driving

> r[driving.take.same-place]
> Picking the machine the input is already on, or picking home while the input is home, sends
> nothing and leaves the move number as it was.
>
> - model: driving

> r[driving.take.no-link]
> Picking a peer this machine may not take (`driving.permission.reach-only`) sends no take,
> leaves the input and the move number as they were, and Core reports that it cannot cross to
> that peer, with the reason: it has no link, with the reason its link last reported down
> (`discovery.wake.crossing` decides first whether it is woken instead); it does not let this
> machine drive it; or this machine may not drive it.
>
> - model: driving

> r[driving.take.accepted]
> An accept with the move number of the take awaited ends the wait, and the input stays on that
> machine.
>
> - model: driving

> r[driving.take.no-answer]
> A take that has had no accept or refusal 1 s after it was sent brings the input home, with the
> reason that the machine did not answer. The machine's link stays up.
>
> - model: driving

> r[driving.take.refused]
> A refusal from the machine the input is on, with the current move number or, while the take is
> still awaited, that take's move number, brings the input home with the refusal's reason: driven by
> another machine, not allowed, driving another machine, this machine sent the driver home, or
> malformed. When the refusal names the machine in the way, Core reports it too.
>
> - parity: Python acted on a refusal whose reason it did not know as a refusal it could not name (#14: an unknown reason does not decode, and the link closes, `link.frame.undecodable`)
> - model: driving

> r[driving.take.refusal-ignored]
> A refusal from a machine the input is not on, or with any other move number, is ignored.
>
> - model: driving

> r[driving.take.out-of-reach]
> A peer that refuses a take, or does not take a pass on (`driving.pass-on.not-taken`), is out of
> reach for 3 s, and for 5 s when it refused because it sent this driver home. While it is out of
> reach, picking it sends nothing, reports nothing, and leaves the input and the move number as
> they were. Other peers are not affected.
>
> - model: driving

> r[driving.take.late-accept]
> An accept that arrives after its take's wait ended, for a take made from home or by the driver's
> own crossing method, on a link that stayed up, is answered with a let go carrying the current
> move number and naming where the input is now: the driver itself when it is home.
>
> - model: driving

> r[driving.take.unknown-accept]
> An accept whose move number is not that of a take this driver sent to that machine in the last
> 30 s is ignored.
>
> - model: driving

## The reach

> r[driving.reach.contents]
> The reach a driver sends a machine names every peer it has a link with that lets this driver
> drive it, except the driver, that machine and any peer out of reach.
>
> - model: driving

> r[driving.reach.kept-true]
> While the input is on a machine and no pass on is awaited, each time the reach for that machine
> changes, because a link comes up or goes, a peer changes whether it lets this driver drive it, or
> a peer's time out of reach ends, the driver raises the move number and sends that machine a
> reach update with the new reach. A change that leaves the reach as last sent sends nothing.
>
> - model: driving

> r[driving.reach.at-home]
> While the input is home, a change to the reach sends nothing and leaves the move number as it
> was.
>
> - model: driving

> r[driving.reach.unknown-peer]
> A peer that says whether this machine may drive it, with no link up, changes nothing.
>
> - model: driving

> r[driving.reach.zones]
> A driven machine passes on only to its driver and to the peers in the reach of its driver's
> latest take or reach update, leaving out any entry that names the driver or itself.
>
> - model: driving

> r[driving.reach.update-not-take]
> A reach update is answered as `driving.accept.order` says. One it accepts, only ever from its
> driver, changes the driver's reach and push distance, and does not move the pointer, report an
> arrival, or end or begin driving.
>
> - model: driving

> r[driving.reach.update-answer]
> A driver does not wait for the answer to a reach update. Its accept is ignored
> (`driving.take.unknown-accept`), and its refusal, when it carries the current move number,
> brings the input home with the refusal's reason (`driving.take.refused`).
>
> - model: driving

## Accepting a take

> r[driving.accept.order]
> A machine answers a take or reach update that `driving.move.not-higher` does not ignore by the
> first of these that is true:
>
> 1. it is malformed (`driving.take.malformed`): refused as malformed;
> 2. this machine may not be driven by that peer: refused as not allowed;
> 3. this machine drives or has a take of its own unanswered (`driving.role.not-both`): refused as
>    driving another machine;
> 4. this machine sent that peer home, or ended its driving by force, less than 5 s ago: refused
>    as this machine sent the driver home;
> 5. another peer drives this machine, and it is a reach update or that driver may not be taken
>    over (`driving.take-over.idle`): refused as driven by another machine;
> 6. it is a reach update and this machine has no driver: refused as malformed;
> 7. otherwise accepted, and the peer is this machine's driver.
>
> - model: driving

> r[driving.accept.names-other]
> A refusal as driven by another machine names that machine's driver. A refusal as driving another
> machine names the machine this machine's input is on, when it is on one. No other refusal names
> a machine.

> r[driving.accept.answer-first]
> A machine sends its answer to a take before it places the pointer: the accept does not wait for
> the landing.

> r[driving.accept.lands]
> When a machine accepts a take with a landing point, it places the pointer there
> (`spec/crossing.md` says where), and Core reports the driver's arrival with that point. When the
> take has none, the pointer stays where it is, and Core reports the arrival with where it is.

> r[driving.accept.again]
> A take from this machine's driver that `driving.accept.order` accepts moves the pointer to its
> landing point when it has one, and does not end or begin driving.
>
> - model: driving

> r[driving.accept.link-stays]
> A refusal never closes the link it is sent on.

## Who is driven

> r[driving.driven.report]
> Core reports each time driving of this machine begins, naming the driver, and each time it ends,
> once each, in the order they happened.
>
> - model: driving

> r[driving.driven.other-input]
> Input from a peer that is not this machine's driver is not injected. It is still acknowledged
> (`link.input.ack-discarded`).
>
> - model: driving

> r[driving.driven.other-let-go]
> A let go from a peer that is not this machine's driver is ignored.
>
> - model: driving

> r[driving.driven.own-input]
> While a machine is driven, its own user's input acts on it as before, and is sent to no machine
> unless one of its own crossing methods moves it (`driving.role.drive-while-driven`).

## Letting go

> r[driving.let-go.names]
> A let go names where the driver's input went: the machine it took next, or the driver itself
> when the input came home.

> r[driving.end.when]
> Driving of this machine ends when its driver lets go; when the driver's link closes or is
> replaced; when the user removes the driver, puts it out of use or turns off may be driven by for
> it; when a send home is not obeyed within 1 s (`driving.send-home.forced`); when another peer
> takes over (`driving.take-over.idle`); and when Beamer quits.
>
> - model: driving

> r[driving.end.keys-up]
> When driving ends, for any reason, this machine releases every key and button it injected for
> that driver and has not released, before it injects anything else, and its zones lead nowhere
> until a take is accepted again.
>
> - model: driving

> r[driving.end.refuse]
> When driving ends by force after a send home, or by turning off may be driven by, and the driver's
> link is still up, this machine sends the driver a refusal with the move number of the last take or
> reach update it accepted from it: this machine sent the driver home, or not allowed. Until a take
> of the driver's is accepted again, its input on that link is not injected.
>
> - model: driving

> r[driving.end.removed]
> When the user removes the driver or puts it out of use, driving ends before its link closes
> (`pairing.remove.link`, `link.peer.gone`). Every other link stays up.
>
> - model: driving

## Coming home

> r[driving.home.how]
> When input comes home, the driver raises the move number and, when the link to the machine the
> input was on is up and did not bring the input home by refusing a message
> (`driving.home.send-refused`), sends it the releases of what it pressed there
> (`driving.keys.released-on-leaving`) and then a let go naming the driver. The input is home even
> when the link refuses those messages (`driving.home.send-refused`). The pointer stays where it was
> captured, unless the move gives a landing point. Core reports that the input is home, the machine
> it left, why, and the landing point when there is one, or where the pointer is when there is none.
>
> - model: driving

> r[driving.home.reasons]
> Input comes home, saying why, when: the driver's own crossing method picks home; the machine it is
> on sends the driver home (`driving.home.send-home`); that machine refuses the take
> (`driving.take.refused`) or a reach update (`driving.reach.update-answer`), or does not answer the
> take (`driving.take.no-answer`); another machine takes that machine over
> (`driving.home.taken-over`); the link to that machine is lost or refuses a message
> (`driving.home.link-lost`, `driving.home.send-refused`); or this machine can no longer capture
> input (`driving.home.capture-lost`).
>
> - model: driving

> r[driving.home.link-lost]
> When the link to the machine the input is on reports down, for any reason
> (`link.report.down`), input it stopped acknowledging included (`link.input.stall`), the input
> comes home with the reason that the link was lost. Nothing is sent to that machine.
>
> - model: driving

> r[driving.home.send-refused]
> When the link to the machine the input is on refuses a message the driver gives it, the input
> comes home with the reason that the link was lost, and nothing is sent to that machine, whether
> the link refused the message because it is full or because it is down. When the link refused the
> message because it is full (`link.send.full`), the driver closes it (`link.send.app-close`), with
> the app's reason that the link had too much waiting to send, as it does too when the link refuses
> as full the releases or let go that `driving.home.how` sends it. When the link refused the message
> because it is down (`link.send.down`), the driver closes nothing. A refused message is not sent
> again, and refused input does not act here.
>
> - parity: Python's Windows app let a key or button act here when it saw, as it captured it, that the link was down, and the Mac app let one act here when its own queue toward the link was full (ADR 0004: capture passes or swallows input from the capture word alone, so the link refuses input only after it was swallowed)
> - model: driving

> r[driving.home.capture-lost]
> When this machine can no longer capture input while its input is on another machine
> (`spec/input.md` says when), the input comes home as `driving.home.how` says, with the reason
> that capture stopped.
>
> - model: driving

> r[driving.home.quit]
> When Beamer quits while its input is on another machine, the driver sends that machine the
> releases of what it pressed there and a let go before it closes the links (`link.send.quit`).

> r[driving.home.send-home]
> A send home, or a pass on naming the driver, from the machine the input is on, whose move number
> is at least that of the take that began the current visit, brings the input home, to the landing
> point it names when it names one, with the reason that the machine sent the input home. One from
> any other machine, or with an older move number, is ignored.
>
> - model: driving

> r[driving.home.taken-over]
> A send home that names the peer that took the machine over brings the input home with the reason
> that the machine was taken over, and Core reports the new driver's machine id, whether or not it
> is a peer of this machine.
>
> - model: driving

> r[driving.home.second-link]
> When a new link with the peer the input is on replaces the old one (`link.pair.redial-replaces`,
> `link.pair.smaller-id-dials`), the input comes home with the reason that the link was lost, and
> the peer stays in reach.
>
> - model: driving

## Sending home

> r[driving.send-home.message]
> A driven machine's own user can send its driver home: from the menu, or by crossing from it
> (`driving.role.drive-while-driven`). The driven machine's zones then lead nowhere, and it sends
> the driver a send home with the move number of the last take or reach update it accepted, naming
> the driver and no landing point.
>
> - model: driving

> r[driving.send-home.own-user-only]
> Input injected for the driver never acts as a crossing method of the driven machine: the
> driver typing the driven machine's shortcut does not send the driver home.

> r[driving.send-home.not-driven]
> Sending home while this machine has no driver sends nothing.
>
> - model: driving

> r[driving.send-home.refuses]
> For 5 s after it sends its driver home, a machine refuses that peer's takes as this machine sent
> the driver home, whether or not the driver obeyed.
>
> - model: driving

> r[driving.send-home.forced]
> When the driver has not let go 1 s after a send home, its driving of this machine ends by force
> (`driving.end.keys-up`, `driving.end.refuse`), and the 5 s of `driving.send-home.refuses` count
> again from then.
>
> - model: driving

## Take over

> r[driving.take-over.idle]
> A take from a peer that is not this machine's driver takes this machine over when the driver has
> had no input injected here for 3 s or more, counted from when its driving began or from its latest
> input injected. This machine then sends the old driver a send home with the move number of the
> last take or reach update it accepted from it, naming the old driver and the peer that took over;
> ends the old driving (`driving.end.keys-up`); refuses the old driver's takes for 5 s as this
> machine sent the driver home; and accepts the new take. Before 3 s the take is refused as driven
> by another machine.
>
> - model: driving

> r[driving.take-over.what-counts]
> Only the driver's input that this machine injects counts as activity. Input from another peer, a
> reach update, the clipboard, keep-alives, input this machine cannot act on and input it
> discards as malformed do not.
>
> - model: driving

> r[driving.take-over.not-by-update]
> A reach update never takes a machine over: from a peer that is not the driver, it is refused
> (`driving.accept.order`, steps 5 and 6).
>
> - model: driving

## Pass on

> r[driving.pass-on.message]
> When one of a driven machine's zones fires, it sends its driver a pass on naming the zone's peer,
> which may be the driver itself, with the move number of the last take or reach update it
> accepted, and the landing point the zone gives on that peer. Its zones then lead nowhere until
> the driver's next take or reach update; input that arrives meanwhile is injected.
>
> - model: driving

> r[driving.pass-on.direct-link]
> Input is never relayed: the driver takes the peer named in a pass on over its own link, and a
> machine is reached in a chain only when the driver has a link with it.
>
> - model: driving

> r[driving.pass-on.ignored]
> A driver ignores a pass on to another machine that does not come from the machine its input is
> on, that does not carry the current move number, or that arrives while it awaits the answer to
> an earlier pass on.
>
> - model: driving

> r[driving.pass-on.outside-reach]
> A pass on to a peer that was not in the reach last sent to that machine, or that the driver may
> no longer take (`driving.permission.reach-only`), is not taken, without asking the peer
> (`driving.pass-on.not-taken`), and Core reports the peer unreachable.
>
> - model: driving

> r[driving.pass-on.take]
> A pass on that the driver acts on makes it raise the move number and take the peer named, with
> the landing point the pass on gave, or none when it gave none. Until the answer, the input stays
> on the machine that passed on, which is not let go, and the driver's input is queued.
>
> - model: driving

> r[driving.pass-on.accepted]
> When the peer accepts, the driver sends the machine that passed on the releases of what it
> pressed there and a let go naming the peer, then sends the queued input to the peer. Core reports
> the input on the peer, the machine it left, that a pass on moved it, and the landing point.
>
> - model: driving

> r[driving.pass-on.update-after]
> When the reach for the peer changed while its answer was awaited, a reach update with the new
> reach follows the accept.
>
> - model: driving

> r[driving.pass-on.not-taken]
> When the peer refuses, does not answer within 1 s, or its link is lost while the answer is
> awaited, the input stays on the machine that passed on: the driver raises the move number, sends
> that machine a reach update that leaves the peer out, then the queued input. The peer is out of
> reach (`driving.take.out-of-reach`). Core reports the peer unreachable with the reason, and the
> machine in the way when a refusal named one.
>
> - model: driving

> r[driving.pass-on.given-up]
> When a pass on's wait ends without an answer from the peer, by its 1 s or by anything else that
> moves the input, the driver closes the peer's link (`link.send.app-close`), with the reason that
> a pass on to it was given up, before it sends anything else. Until that link is up again the
> peer is out of reach, and nothing that arrived on the closed link is acted on, an accept
> included.
>
> - model: driving

> r[driving.pass-on.ended-by]
> A send home from the machine that passed on, the loss of its link, or the driver's own crossing
> method picking home or a third machine ends the wait for a pass on's answer, and the input moves
> as that event says. No reach update follows later for that wait.
>
> - model: driving

> r[driving.pass-on.picked-current]
> When the driver's own crossing method picks the machine that passed on while the answer is
> awaited, the driver gives the pass on up (`driving.pass-on.given-up`), sends that machine a reach
> update, then the queued input.
>
> - model: driving

> r[driving.pass-on.picked-same]
> When the driver's own crossing method picks the peer a pass on is awaiting, the peer's link is
> not closed: the new take follows on the same link, and its answer is the one awaited.
>
> - model: driving

> r[driving.pass-on.queued-home]
> Input queued for a pass on that ends with the input home goes nowhere: a key or button pressed
> while it was queued is not let through here, and neither is its release.
>
> - model: driving

## The chain

> r[driving.chain.report]
> Core reports the chain: the driver, then each machine the input reached in the current visit,
> in order. A take by the driver's own crossing method starts it again from the driver and that
> machine; each pass on accepted adds the peer; coming home leaves the driver alone.
>
> - model: driving

> r[driving.chain.in-the-way]
> When a take or pass on is refused as driven by another machine, or as driving another machine,
> and the refusal names the machine in the way, the chain Core reports ends with the machine that
> refused, then the machine in the way.
>
> - model: driving

> r[driving.chain.only-one]
> Across a chain of any length, only the machine the input is on injects the driver's input, and
> every machine the input left has been let go and has no driver.
>
> - model: driving

> r[driving.chain.lost-behind]
> Losing the link to a machine the input passed through, but is no longer on, leaves the input
> where it is. That machine leaves the reach (`driving.reach.kept-true`), so a pass on to it
> is not taken.
>
> - model: driving

## Keys pressed across a move

> r[driving.keys.released-on-leaving]
> Before a driver lets go of a machine, it sends it a release of every key and button it sent down
> there and has not released, in the order they were pressed.
>
> - model: driving

> r[driving.keys.swallowed-after]
> After the driver leaves a machine, the physical repeats and release of the keys and buttons it
> released there go to no machine, and a key's repeats and release do not act here either. A
> press after that release goes where the input is.
>
> - model: driving

> r[driving.keys.pressed-at-home]
> A key or button pressed while the input is home belongs here: its release acts here, even after
> the input has left and during a pass on, and its repeats while the input is on another machine
> act nowhere. A press after that release goes where the input is.
>
> - model: driving

> r[driving.keys.link-lost]
> When the link to a machine goes, the driver forgets what it pressed there: it sends no release
> for it, then or on a later link, and the physical releases of those keys and buttons go nowhere.
>
> - model: driving

## Logs

r[driving.log.move]
Each move of the driver's input is logged on the driver in one line naming the machine it went to
or home, the machine it left, the reason it moved, and the edge of the machine reached that the
pointer lands at when the move gives a landing point.

r[driving.log.started]
Each move the driver makes is logged on the driver in one line naming what started it: the
crossing method, or a pass on from the machine the input is on.
