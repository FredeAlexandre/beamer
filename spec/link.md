# Link

The link between two peers: how it comes up and is authenticated, who dials, how it stays up and
how its loss is seen, its traffic classes, how its messages are framed, and how input crosses it
with acknowledgements of Beamer's own. It is F1 of #11's inventory, which ships.

The rules come from the link protocol decided in #14 and ADR 0003, and from the Python app's link
at `archive/python` (`core/link.py`, `core/receiver.py`, `core/link_timing.py`,
`win_app/sender.py`), mined through its tests in `spec/ledger/link.txt`.
What a message means belongs to the domain that sends it: a take to `driving`, a clipboard to
`clipboard`, a store entry to `store`. Pairing, the key pair and the pinned keys are `pairing`'s,
and a peer's addresses, the beacon and dormant repair's probing are `discovery`'s. This file
replaces `WIRE.md`, whose bytes the Rust app does not keep (#14).

Every rule is `ci` unless it says otherwise.

## Transport

> r[link.transport.quic]
> A link is one QUIC connection between two peers, over UDP. It carries everything the two
> exchange, in both directions.
>
> - parity: Python kept two TCP links per pair, one in each direction, plus a UDP flow of its own for media (#14, ADR 0003: one QUIC link per pair)

> r[link.transport.port]
> A machine takes links on UDP port 24820, and dials every peer on UDP port 24820, at whichever of
> its addresses. No machine has a port of its own: a link port is never chosen, learnt or stored.
>
> - parity: Python took links on TCP port 24820 unless the user chose another port, and carried each machine's port in its beacon, its pairing messages and its peer list (#14: one UDP port, 24820, for links, pairing and beacons, so every machine is on the same port)

r[link.transport.no-tcp]
A machine neither takes nor dials a link over TCP: two peers between which UDP is blocked have no
link.

r[link.transport.port-taken]
When another program has UDP port 24820, the machine reports that it cannot take links, naming the
port, and tries to take the port again every 5 s until it has it.

r[link.transport.port-exclusive]
A machine never shares UDP port 24820 with another program, even one that bound the port allowing
the address to be reused.

## Version

r[link.version.alpn]
A link offers exactly one application protocol, `beamer/1`, and comes up only with a peer that
offers the same one.

> r[link.version.mismatch]
> When a peer answers with another protocol version, no link comes up, and the link reports down
> with the reason that the two machines run different versions of Beamer.
>
> - parity: Python said which of the two was older ("Update Beamer on Far: it is older than this one", or that this machine was the older) (#14: exact version match in the protocol name, which tells no order)

## Authentication

> r[link.auth.pinned]
> A link comes up only once each of the two machines has proved that it has the private key of the
> public key the other pinned for it at pairing.
>
> - model: link

> r[link.auth.stranger]
> A machine that dials this one with a key that no peer of this machine has is refused in
> the handshake, and nothing it sends is dealt with.
>
> - model: link

> r[link.auth.not-the-peer]
> When the machine answering at a peer's address does not prove the peer's pinned key, the dialler
> closes before it sends anything on the link, and the link reports down with the reason that
> another machine answered at that address.
>
> - model: link

> r[link.auth.forgotten]
> When the peer proves its pinned key but refuses this machine's, the link reports down with the
> reason that the peer no longer has this pairing.
>
> - parity: Python concluded it only after 60 s in which every handshake with a peer that had linked before closed unanswered, since its peer closed silently on a pairing it no longer had, and never said it of a peer that had not linked (#14: links authenticate by pinned keys, so the refusal is explicit)
> - model: link

> r[link.auth.forgotten-redial]
> After the peer has refused this machine's key, this machine dials that peer every 30 s, until a
> link with it comes up or this machine's pairing with it changes.
>
> - model: link

> r[link.auth.not-in-use]
> A link dialled by a peer that is not in use here closes as soon as it has authenticated, and the
> dialler reports down with the reason that the peer does not use this machine now.
>
> - parity: Python answered such a peer as if its first message were unreadable, so the dialler said it had sent something the peer could not read (a refusal that names its cause; Python's misled the user)
> - model: link

> r[link.auth.replay]
> Packets recorded from a link and sent again, on a new link, on the same link or to the machine
> that sent them, are refused, and nothing in them is dealt with.
>
> - parity: Python ended the link when a frame was replayed within it (ADR 0003: QUIC discards a replayed packet and the link stays up)

r[link.auth.crowd]
A peer's link comes up within 5 s of its first dial while up to 32 machines whose keys this
machine has not pinned keep handshakes with it open.

## Dialling

> r[link.dial.every-peer]
> A machine dials each peer in use that it has no link with, whatever either may drive of the
> other.
>
> - parity: Python dialled only the peers this machine may drive, so two machines neither of which may drive the other had no link (#14: one link per pair carries everything in both directions, so a link no longer depends on where input may go)
> - model: link

> r[link.dial.not-in-use]
> A peer that is not in use is never dialled.
>
> - model: link

> r[link.dial.in-use-again]
> A peer put in use again is dialled at once.
>
> - model: link

> r[link.dial.backoff]
> After a dial fails or a link closes, the next dial to that peer comes 0.25 s later, and each
> further wait doubles, up to 2 s. A link that comes up starts the waits from 0.25 s again. A dial
> the peer ended by refusing this machine's key is the exception: `link.auth.forgotten-redial`
> replaces these waits until a link with that peer comes up.
>
> - parity: Python dialled again every 2 s (#14: re-dialling waits from 0.25 s, doubling up to 2 s)
> - model: link

> r[link.dial.deadline]
> A dial that has not authenticated 5 s after it began is given up, and the link reports down with the
> reason that the peer is unreachable.
>
> - parity: Python told a refused TCP dial ("not listening on this port"), an unreachable address ("unreachable from this machine"), 5 s of silence ("a Beamer from before version 4") and an answer that was not a Beamer's ("did not answer as a Beamer") apart (#14: one QUIC handshake over UDP, in which these look alike)
> - model: link

r[link.dial.not-self]
A machine never dials an address of its own: a loopback address, the unspecified address, an
address one of its interfaces has, or the address it would send from to reach that address. It
still finds its own address by the last of these when it cannot list its interfaces.

r[link.dial.unknown-own]
An address that a machine can tell neither from its interfaces nor from the address it would send
from is dialled.

## One link per pair

> r[link.pair.one]
> Two peers have at most one link between them at a time.
>
> - model: link

> r[link.pair.redial-replaces]
> When a link with a peer authenticates while an older one with that peer is up, and the same
> machine dialled both, the older one closes, reporting down with the reason that it was replaced.
>
> - model: link

> r[link.pair.smaller-id-dials]
> When a link with a peer authenticates while another with that peer is up, and each machine
> dialled one of them, the link dialled by the machine with the smaller machine id stays up and
> the other closes. Machine ids compare byte by byte. The one that closes reports down with the
> reason that it was replaced.
>
> - parity: Python kept both links, one in each direction (#14: when both links come up at once, the one dialled by the smaller machine id survives)
> - model: link

> r[link.pair.closed-is-silent]
> Nothing that arrives on a link after it closed or was replaced is dealt with.
>
> - model: link

## Staying up

> r[link.alive.keep-alive]
> While a link is up, each end sends something on it at least every 500 ms, so a link with
> nothing to carry is never taken for lost.
>
> - parity: Python's dialler sent a keep-alive message after 1 s without sending, and its other end an acknowledgement every 0.4 s without input (#14: QUIC keep-alive every 500 ms)
> - model: link

> r[link.alive.idle]
> A link on which a machine has received nothing from its peer for 2 s closes, and the link
> reports down with the reason that the peer stopped answering.
>
> - parity: Python's dialler waited 2 s and the machine it dialled 2.5 s (#14: an idle timeout of 2 s)
> - model: link

r[link.alive.anything-received]
Anything a machine receives on a link counts against the link's idle timeout, so a bulk message
arriving slowly keeps the link up as long as its pieces keep coming.

> r[link.alive.not-reading]
> A link whose control messages have waited 2 s to be sent because the peer reads none of them
> closes, and the link reports down with the reason that the peer stopped taking what this machine
> sends.
>
> - model: link

## What a link reports

> r[link.report.up]
> When a link comes up, the link reports it up, naming the peer by machine id and giving the
> address it reaches the peer at.
>
> - model: link

> r[link.report.down]
> When a link closes, or a dial is given up, the link reports it down, naming the peer by machine
> id, with one reason: unreachable, another machine answered, the peer no longer has this pairing,
> the peer does not use this machine now, another version, stopped answering, stopped taking what
> this machine sends, stopped acknowledging input, a message that does not decode, the peer broke
> the protocol, closed by the peer, replaced, this machine forgot the peer or put it out of use,
> the peer's pinned key changed, or a reason the app gave. Each rule that closes a link names its
> reason from this list.
>
> - model: link

> r[link.report.probe-silent]
> A dial at an address being tried for a peer that is not one of the peer's known addresses reports
> nothing when it fails, and leaves the peer's other dials and what the link last reported as they
> were.
>
> - model: link

## When a peer changes

> r[link.peer.gone]
> When this machine forgets a peer or puts it out of use, the peer's link closes at once, without
> waiting for a dial or handshake in progress. The last thing it reports for that peer is down, with
> the reason that this machine forgot the peer or put it out of use, and it reports nothing after,
> even when a handshake in progress then completes.
>
> - model: link

> r[link.peer.rekeyed]
> When this machine pins a new key for a peer, by pairing with it again, the link authenticated
> with the old key closes, reporting down with the reason that the peer's pinned key changed, and
> the peer is dialled with the new one.
>
> - model: link

## What the app gives a link

r[link.send.down]
A message given to a link that is down is refused, and the app is told so at once. Nothing is kept
to send once the link is up.

r[link.send.full]
A message given to a link that has 2048 messages or 32 MiB waiting to be sent is refused, and the
app is told so. A pointer move that merges into one already waiting counts as no new message.

> r[link.send.app-close]
> The app can close a link with a reason of its own. The link then reports down with that reason,
> and is dialled again as after any loss. The reason is never given for a later close.
>
> - model: link

r[link.send.quit]
When the app quits, it waits up to 0.5 s for what it already gave each link to be sent, then
closes the links.

## Traffic classes

r[link.class.never-behind]
A message never waits to be sent behind a message of a lower traffic class: input goes before
control, control before media, and media before bulk.

r[link.class.control]
Control messages travel on one stream per link, and arrive whole, each once, in the order they
were sent.

> r[link.class.input-datagrams]
> Input messages and acknowledgements travel as QUIC datagrams, never on a stream.
>
> - parity: Python sent input in line with everything else on its TCP link (ADR 0003: input rides datagrams with its own resends)

> r[link.class.media-datagrams]
> Media travels as QUIC datagrams on the link, and the link never sends a lost media datagram
> again.
>
> - parity: Python sent media on a UDP flow of its own, with its own keys, turning to the link when UDP failed (#14: media goes as datagrams on the link)

r[link.class.media-and-input]
Media never delays input, and input delays media by at most one datagram.

> r[link.class.bulk-stream]
> Each bulk message travels on a unidirectional stream opened for it alone, which ends when the
> message is whole.
>
> - parity: Python sent clipboard images and large text inline on its one TCP link (#14: each clipboard image or large text gets a new unidirectional stream, below every other class)

r[link.class.bulk-in-flight]
At most 256 KiB of one bulk message is in flight and unacknowledged by QUIC at a time.

> r[link.class.bulk-cancel]
> The sender of a bulk message can cancel it. The receiver then discards what it received of it and
> hands none of it on, and the link stays up.
>
> - parity: Python could not cancel a clipboard once its frame began, and a newer one waited behind it (#14: a bulk stream is reset when a newer clipboard replaces it)

r[link.class.order]
A machine deals with the input and control messages a peer sent it in the order the peer sent
them, across the two classes: an input message sent after a control message is dealt with after
it, and a control message sent after an input message is dealt with after it.

r[link.class.bulk-after-control]
A bulk message is handed on only once every control message its sender sent before it began has
been dealt with.

## Messages on the wire

> r[link.frame.control]
> On the control stream, each message is a varint of its length followed by that many bytes of its
> postcard encoding.
>
> - fuzz: link-frame

> r[link.frame.datagram]
> A datagram on a link carries one message, its postcard encoding with no length before it: a
> batch of input messages, an acknowledgement, or a media packet.
>
> - fuzz: link-frame

> r[link.frame.round-trip]
> Every message a link carries decodes to the message that was encoded.
>
> - fuzz: link-frame

> r[link.frame.undecodable]
> A message that does not decode, on a stream or in a datagram, closes the link, and the link
> reports down with the reason that a message did not decode.
>
> - parity: Python ignored a message that was JSON but malformed in its fields, unless it was the first message, an acknowledgement or a take (#14: exact version match, with no unknown-field rules, so a message that does not decode is an error)
> - fuzz: link-frame

> r[link.frame.control-limit]
> A control message whose length is over 64 KiB closes the link before any of it is read, and the
> link reports down with the reason that the peer broke the protocol.
>
> - parity: Python read a frame of 64 KiB to 16 MiB that was not a clipboard it expected, discarded it unparsed and kept the link, and ended the link only past 16 MiB (#14: clipboard images and large text go as bulk, so no control message needs more)
> - fuzz: link-frame

r[link.frame.control-deadline]
A control message that is not whole 30 s after its first byte arrived closes the link, however
steadily its bytes keep coming, and the link reports down with the reason that the peer broke the
protocol.

> r[link.frame.bulk-deadline]
> A bulk message that is not whole 30 s after its first byte arrived is cancelled by its receiver,
> and the link stays up.
>
> - parity: Python ended the link (#14: each bulk message has a stream of its own, so it can be cancelled alone)

> r[link.frame.bulk-limit]
> A bulk message longer than 16 MiB is cancelled by its receiver once its bytes pass 16 MiB, none
> of it is handed on, and the link stays up.
>
> - parity: Python ended the link on a frame claiming more than 16 MiB (#14: each bulk message has a stream of its own, so it can be cancelled alone)
> - fuzz: link-frame

r[link.frame.bulk-unexpected]
A bulk message that the app does not expect from that peer when its first byte arrives is
cancelled unread, and the link stays up.

## Input

r[link.input.sequence]
A driver gives each input message it sends on a link the next sequence number when it first sends
it, from 1 on each new link.

> r[link.input.redundant]
> Each input datagram carries every input message sent on the link and not yet acknowledged,
> oldest first, as many as fit in one datagram.
>
> - parity: Python sent each input message once and left its resending to TCP (ADR 0003: each datagram carries every input not yet acknowledged)

> r[link.input.resend]
> When no acknowledgement covering the newest input message sent on a link has arrived
> max(2 × QUIC's RTT for the link, 5 ms) after it was sent, the driver sends an input datagram
> again, and keeps doing so at that interval until one arrives.
>
> QUIC's RTT is the one `link.rtt.reported` reports, which the link has from its handshake on, so
> a fresh link with no round trip yet resends on it too.
>
> - parity: Python left resending to TCP, whose minimum retransmission timeout is 200 ms or more (ADR 0003: a resend after max(2 × RTT, 5 ms))

r[link.input.once-in-order]
A driven machine deals with input messages in sequence-number order, each once: it discards one
whose sequence number it has dealt with, and deals with none after a missing one until the missing
one arrives.

r[link.input.window]
At most 32 input messages are sent and unacknowledged on a link at a time. Further input waits to
be sent until an acknowledgement makes room.

> r[link.input.text-split]
> A text input message too long for one datagram is sent as several text input messages, in order,
> each holding whole characters.
>
> - parity: Python sent each run of typed text as one message whatever its length, and a driven machine dropped one over 4096 bytes whole and still acknowledged it (#14: datagram payloads ≤ 1200 bytes, and input rides datagrams)

r[link.input.ack]
An acknowledgement names the highest sequence number the driven machine has dealt with on the
link, and its delay: the microseconds from dealing with that input message to sending the
acknowledgement, from 0 to 2^31 − 1, larger times sent as 2^31 − 1.

> r[link.input.ack-timing]
> When a driven machine deals with input 15 ms or more after its last acknowledgement on that link,
> it acknowledges it at once. When it deals with input sooner, it acknowledges it 15 ms after that
> last acknowledgement, in one acknowledgement for everything dealt with by then. It sends no
> acknowledgement while it has nothing new to acknowledge.
>
> - parity: Python also sent an acknowledgement 0.4 s after the last while no input came, as a heartbeat, the first of them as the link came up (#14: QUIC keep-alive keeps the link up)

r[link.input.ack-repeat]
An input datagram whose input messages the driven machine has all dealt with already is
acknowledged again, on the same timing, naming the same sequence number with a delay of 0.

r[link.input.ack-discarded]
Input the driven machine discards rather than injects, because its sender is not its driver or
because it cannot act on it, still counts as dealt with, and is acknowledged.

r[link.input.ack-ahead]
An acknowledgement naming a sequence number higher than any sent on the link closes the link, and
the link reports down with the reason that the peer broke the protocol.

> r[link.input.ack-older]
> An acknowledgement naming a lower sequence number than one already received on the link is
> ignored.
>
> - parity: Python ended the link on an acknowledgement whose sequence number went down (ADR 0003: acknowledgements are datagrams, which can arrive out of order)

r[link.input.round-trip]
Each acknowledgement naming a higher sequence number than any before it gives one round-trip sample:
the time since the input message it names was first sent, less the delay, and 0 when that is
below 0. An acknowledgement that names no higher sequence number gives none.

> r[link.input.round-trip-recent]
> Whenever the app asks, the link gives it the link's round trip: the upper median of its
> round-trip samples from the last 5 s (of an even number, the higher middle one), or none when
> there are no such samples or the link is down.
>
> - parity: Python's Mac app took only the last 8 of those samples, where its Windows app took all of them (one reading for both platforms: the Windows app's)

> r[link.input.stall]
> A link closes, and reports down with the reason that the peer stopped acknowledging input, when its oldest
> unacknowledged input message was sent more than 1 s ago and no acknowledgement has named a higher
> sequence number for 1 s. Nothing else from the peer keeps it open: keep-alives, control
> messages, bulk messages and repeated acknowledgements do not.
>
> - parity: Python waited 1.5 s (#14: if the oldest unacknowledged input is older than 1 s, the link closes and input comes home)
> - model: link

r[link.input.stall-arrival]
An acknowledgement counts from the moment it reached the machine. Time the machine spends busy
before it gets to an acknowledgement that has arrived never counts toward a stall.

r[link.input.merge]
Pointer moves waiting on the driver to be sent to one peer merge into one move, whose travel is the
sum of theirs.

r[link.input.merge-order]
A pointer move never merges across another input message: keys, buttons, scrolls, gestures and
text keep their places between the moves before and after them.

r[link.input.merge-steps]
A merged move carries the moves it was made of as its steps, at most 64. A merge that would pass
64 steps leaves the merged move one step, its whole travel.

r[link.input.pace]
While 4 or more input messages to a peer are waiting to be sent or unacknowledged, the driver sends
pointer moves to that peer at most once every 2 ms, merging the moves made in between. With fewer,
it sends each move as soon as it is made.

r[link.input.receive-merge]
Pointer moves a driven machine has received but not yet injected merge as they do on the driver:
travel summed, steps kept up to 64, and never across another input message.

r[link.input.receive-current]
When pointer moves arrive at 1000 a second for 400 ms and each injection of a move takes 4 ms, the
last move is injected within 200 ms of being sent, 95 % of them within 100 ms, and the pointer
travels the sum of the moves sent.

r[link.input.drain]
Input that reached the driven machine before its link closed is injected, in order, before the
link reports down.

## QUIC's RTT

> r[link.rtt.reported]
> While a link is up, each end reports QUIC's RTT for the link, QUIC's own measure of the time its
> packets take to the peer and back, using no clock of the peer's, at least once a second. Once the link has closed, it reports none for it.
>
> - parity: Python measured it with probe and echo messages about once a second, only between machines that both announced them, and kept each reading 3 s (#14: QUIC's RTT replaces the probes)

## Logs

r[link.log.up]
When a link comes up, each end logs one line naming the peer, and the addresses and ports at both
ends that the link uses, as the OS gives them.

r[link.log.round-trip]
Every 60 s while input was sent on a link, the driver logs one line for the link: the number of
round-trip samples since its last such line, their median, 95th percentile and largest, and the
mean delay. A minute in which no input was sent on the link logs nothing, and no sample is
counted in two lines.

r[link.log.unacknowledged]
A minute in which input was sent on a link and none was acknowledged logs, in place of the
round-trip line, how many input messages were sent, the oldest unacknowledged sequence number, and
how long it has waited.

r[link.log.stall]
When a link closes for an input stall, the driver logs a warning naming the peer, the oldest
unacknowledged sequence number, the highest acknowledged one, and how many input messages are
unacknowledged.

r[link.log.stages]
Every 30 s while input arrived on a link, the driven machine logs one line for the link with the
largest time an input message spent there waiting to be dealt with, being dealt with, and being
injected, each apart.

r[link.log.slow-injection]
An injection that takes 250 ms or more logs a warning naming the kind of input message and the
injection that was slow, at most once a second.
