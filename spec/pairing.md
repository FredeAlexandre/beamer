# Pairing

How two machines become peers: each machine's key pair, the six-digit code, the CPace exchange
that proves it inside a pairing connection, pairing from a beacon and by address, the peer list
that pairing writes, and dormant peers paired again. Feature F2, as #11 keeps it, in the protocol
#14 and ADR 0003 decided. It replaces `PAIRING.md` and section 6 of `WIRE.md`.

What stays elsewhere: the link itself is `spec/link.md`; the beacon, the address lists and the
dormant repair's address probing are `spec/discovery.md`; the store that carries each machine's
paired list is `spec/store.md`; what each screen says is `spec/viewmodel.md`.

Dropped, so no rule here (#11): pair nearby (F3), the pairing version 2 exchange and any
compatibility with the Python apps, the 1.4.x migration of the settings file (F19), phone peers,
and the QR code with its second secret (#14 drops it; it was for phones).

The published vectors the CPace rules name are copied as data in
[`pairing-vectors.json`](pairing-vectors.json), each section naming its source.

## Identity

> r[pairing.identity.key-pair]
> On its first start a machine makes one Ed25519 key pair. Every later start uses the same key
> pair, and nothing in Beamer replaces it.

> r[pairing.identity.machine-id]
> A machine's machine id is the first 16 bytes of the SHA-256 of its 32-byte Ed25519 public key.
> A peer's machine id is always computed from the public key pinned for it, never taken from
> anything the peer says.
>
> - parity: Python's machine id was 16 random bytes made once per install, unrelated to any key, and sent in the clear at pairing (#14 and ADR 0003 derive the id from the key pair, so only its holder can claim it)

> r[pairing.identity.keychain]
> On the Mac, the private key is kept in the Keychain, and survives Beamer being quit, updated or
> reinstalled.
>
> - tier: machine (the Keychain is reached only on a real Mac, by the signed app)

> r[pairing.identity.dpapi]
> On Windows, the private key is kept in a file protected by DPAPI for the user, and survives
> Beamer being quit, updated or reinstalled.
>
> - tier: machine (DPAPI is reached only on a real Windows machine, under the user's account)

> r[pairing.identity.no-export]
> Beamer offers no way to export or import a private key. A machine whose private key is lost
> makes a new key pair, so it has a new machine id, and every peer must pair with it again.
>
> - parity: Python kept its machine id and tokens in the settings file, so a copied settings file made two machines with one id (ADR 0003: the private key is the identity, and losing it means pairing again)

## The code

> r[pairing.code.digits]
> A code is six decimal digits, each drawn uniformly from the operating system's secure random
> source.
>
> - model: pairing

> r[pairing.code.lifetime]
> A code lasts 60 seconds from when it is shown, whether or not an exchange has started on it.
> Once it has expired, nothing is accepted under it, an exchange under way on it does not
> complete, and the showing machine reports the code expired.
>
> - model: pairing

> r[pairing.code.seconds-left]
> While a code is shown, the showing machine reports the whole seconds it has left, counting down
> from 60 to 0.
>
> - model: pairing

> r[pairing.code.one-at-a-time]
> A machine shows at most one code. Showing a new code ends the one before it, together with any
> exchange under way on it: nothing sent for the old code is accepted under the new one.
>
> - model: pairing

> r[pairing.code.single-use]
> A code pairs at most once. When its exchange ends, whatever the outcome, the code ends with it,
> and a later attempt under it is told no code is shown.
>
> - model: pairing

> r[pairing.code.cancel]
> Cancelling a code ends it and its exchange, and reports no outcome.
>
> - model: pairing

> r[pairing.code.stop]
> When Beamer stops, the code it shows ends, and every pairing connection it holds is closed.
>
> - model: pairing

> r[pairing.code.beacon]
> While a code is shown, this machine's beacon says a code is shown. Once the code ends, its
> beacons stop saying so.
>
> - model: pairing

> r[pairing.code.either-machine]
> Any machine can show a code, and any machine can enter a code another machine shows. A machine
> may show a code and enter another machine's code at the same time.

## The pairing connection

> r[pairing.connection.alpn]
> The entering machine opens a pairing connection to the showing machine: a QUIC connection on the
> showing machine's link port, with the ALPN `beamer-pair/1`. One exchange runs on one
> bidirectional stream of it, and pairing from a beacon and pairing by address use the same
> connection.
>
> - parity: Python sent the exchange as UDP datagrams to port 24821, or as length-prefixed frames over TCP on port 24821 for pairing by address (#14 runs pairing inside a `beamer-pair/1` QUIC connection on the one UDP port)

> r[pairing.connection.keys-from-tls]
> Each machine presents a self-signed certificate holding its own Ed25519 public key. Each machine
> takes the other's public key from the pairing connection's TLS handshake, never from a pairing
> message, and checks no pinned key there: the exchange is what proves it.

> r[pairing.connection.no-code]
> A pairing connection to a machine that shows no code is answered with a refusal saying no code
> is shown, then closed. The entering machine reports that the other machine is not showing a
> code.
>
> - model: pairing

> r[pairing.connection.version]
> A pairing connection offering an ALPN other than `beamer-pair/1` is refused during its handshake.
> The entering machine reports that the two machines run different versions of Beamer, and the
> showing machine's code is untouched.
>
> - parity: Python answered a start of another pairing version with `version` and dropped its code (#14's version policy is an exact ALPN match, settled before any pairing message, so no guess at the code is spent)

> r[pairing.connection.four]
> A showing machine holds at most four pairing connections at once, and at most one from each
> address: a new connection from an address that already has one replaces it. With four held, a
> new connection from another address replaces the one held longest, except the connection whose
> start the code is bound to, which is never replaced. This holds whatever the connections send,
> a start replayed on each of them included.
>
> - model: pairing

> r[pairing.connection.ten-seconds]
> A showing machine closes a pairing connection 10 seconds after accepting it, with nothing more
> sent, if its exchange has not ended by then.
>
> - model: pairing

> r[pairing.connection.no-answer]
> The entering machine reports that the other machine did not answer when no pairing connection
> to it is made within 3 seconds, or when the connection ends before the exchange does without a
> refusal, or 10 seconds after it was made. A connection that cannot be made is never reported as
> a refusal or as the other machine denying permission. The showing machine's code is untouched
> by an attempt that never reached it.
>
> - parity: Python sent each datagram up to three times, 1.5 seconds apart, before reporting no answer (#14: QUIC's own loss recovery replaces the resends)
> - model: pairing

> r[pairing.connection.order]
> The exchange's messages come in one order: the entering machine's start, the showing machine's
> answer, the entering machine's confirmation, the showing machine's receipt. A message out of that
> order, a second start on the same connection included, closes the pairing connection with
> nothing sent. The code and an exchange under way on another connection are untouched.
>
> - model: pairing
> - fuzz: pairing-message

> r[pairing.connection.malformed]
> A pairing message that does not decode, or that is longer than 1024 bytes, closes its pairing
> connection with nothing sent, before any of it is acted on. The code is untouched.
>
> - fuzz: pairing-message

> r[pairing.connection.message-size]
> Every pairing message a machine sends is at most 1024 bytes, with names of 48 code points that
> each take four bytes of UTF-8 included.
>
> - fuzz: pairing-message

> r[pairing.connection.refusal-first]
> A machine that refuses during an exchange sends its refusal first, then closes the pairing
> connection.

> r[pairing.connection.answer-time]
> The showing machine sends its answer to a start 50 milliseconds after the start arrived,
> however long working it out took. A refusal of a start is sent at once: everything that can
> refuse a start is checked before the code is used, so no refusal carries the time the code's
> generator took.
>
> - model: pairing

## CPace

> r[pairing.cpace.suite]
> The exchange is CPace as draft-irtf-cfrg-cpace-21 defines it: the suite CPACE-X25519-SHA512, the
> draft's initiator-responder setting with the entering machine as A and the showing machine as B,
> and the explicit key confirmation of its section 10.4. The draft's own vector, appendix B.1,
> reproduces step by step: `cpace_draft` in `pairing-vectors.json`.

> r[pairing.cpace.lv-cat]
> Length-value concatenation is the draft's `lv_cat`: each string preceded by its length in
> LEB128. It reproduces `lv_cat` in `pairing-vectors.json`.

> r[pairing.cpace.generator]
> The generator is the draft's `calculate_generator` for X25519 with SHA-512, mapping to the curve
> by Elligator 2 as RFC 9380 section 6.7.1 defines it with Z = 2. The map reproduces `elligator2`
> in `pairing-vectors.json`.

> r[pairing.cpace.inputs]
> The exchange's inputs are:
>
> - PRS: the code's six ASCII digits;
> - CI: the ASCII bytes `beamer-pair/1`;
> - sid: 32 bytes of the pairing connection's TLS exporter, with the label
>   `EXPORTER-beamer-pair` and an empty context;
> - ADa: `lv_cat("requester", the entering machine's public key, its name as UTF-8, its platform
>   as ASCII, its link port as 2 bytes big-endian)`;
> - ADb: the same for the showing machine, starting `"host"`.
>
> Each machine builds its own associated data from what it sent and the other's from what it
> received, with the public key from the TLS handshake.
>
> - parity: Python's CI was `beamer-pair-v3`, its sid was made from the beacon's pairing id and a nonce from the entering machine, and its ADa and ADb held 16-byte machine ids in place of public keys (#14 binds both public keys and the TLS exporter into the exchange)

> r[pairing.cpace.low-order]
> A share that gives an all-zero shared secret is refused, as the draft's `scalar_mult_vfy`
> requires. The shares in `low_order_shares.must_refuse` of `pairing-vectors.json` are refused,
> and those in `must_give` give their products. The showing machine checks the entering machine's
> share before it works out the code's generator.
>
> - fuzz: pairing-message

> r[pairing.cpace.fresh-scalar]
> Each exchange draws a new random scalar on each machine. Two exchanges never send the same
> share.

> r[pairing.cpace.tags]
> The tags are the draft's: with `mac_key = SHA-512("CPaceMac" || sid || ISK)`, the entering
> machine's tag Ta is HMAC-SHA-512 over `lv_cat(Ya, ADa)` and the showing machine's tag Tb over
> `lv_cat(Yb, ADb)`, each the full 64 bytes. The showing machine's receipt tag Td is HMAC-SHA-512
> under the same key over `lv_cat("beamer-pair-done")`.

> r[pairing.cpace.no-code-sent]
> No pairing message carries the code, or anything from which the code could be tested offline.

> r[pairing.cpace.reflection]
> The showing machine's own tag Tb, sent back to it as a confirmation, is refused as a wrong tag.
>
> - model: pairing

## The exchange

> r[pairing.exchange.messages]
> The start carries the entering machine's share, name, platform and link port. The answer
> carries the showing machine's share, tag Tb, name, platform and link port. The confirmation
> carries tag Ta, and the receipt carries tag Td. A refusal names one of: no code shown, refused,
> already paired, full.
>
> - parity: Python's messages also carried the pairing id, a nonce, a version number, the 16-byte machine id and base64 text (#14: the pairing connection and the TLS handshake carry what those did)
> - fuzz: pairing-message

> r[pairing.exchange.fields]
> A name is 1 to 48 Unicode code points of valid Unicode. A platform is 1 to 16 characters from
> `a` to `z`, `0` to `9` and `_`: a platform Beamer does not know, of that shape, pairs like any
> other. A link port is 1 to 65535. The showing machine refuses a start with any of them out of
> shape, which ends the code. The entering machine refuses an answer with any of them out of shape
> as it would a wrong tag Tb.
>
> - parity: Python took link port 0 as a phone, saved without an address and never dialled (phone peers are out of scope, #11)
> - model: pairing
> - fuzz: pairing-message

> r[pairing.exchange.name-cut]
> A machine whose name is longer than 48 code points sends its first 48, cut on a code point
> boundary, and the exchange binds the name as sent.

> r[pairing.exchange.entering-checks-first]
> The entering machine checks tag Tb before it sends anything more. When Tb does not check out,
> or the answer is malformed, it sends an abort, a confirmation with no tag, once, stores nothing,
> and reports that the code was not accepted. The showing machine answers an abort with a refusal,
> ends the code, and reports that a wrong code was entered.
>
> - model: pairing

> r[pairing.exchange.showing-stores-after-ta]
> The showing machine stores nothing before tag Ta checks out. A confirmation whose tag does not
> check out is refused, ends the code, stores nothing, and the showing machine reports that a
> wrong code was entered.
>
> - model: pairing

> r[pairing.exchange.receipt]
> The entering machine stores nothing before the receipt's tag Td checks out. A receipt that is a
> refusal, or whose tag does not check out, ends the attempt with nothing stored.
>
> - model: pairing

> r[pairing.exchange.bound-fields]
> The name, platform and link port each machine sends are proved by the exchange: one changed on
> the way, in either direction, fails the exchange, and neither machine stores anything.
>
> - model: pairing

> r[pairing.exchange.bound-keys]
> The exchange proves both public keys and the pairing connection it ran on: messages relayed
> between two different pairing connections, or from a machine whose certificate holds another
> key, fail the exchange, and neither machine stores anything.

> r[pairing.exchange.proved-name]
> The entering machine records the showing machine's name, platform and link port from the
> answer, once its tag Tb checks out, never from a beacon.

> r[pairing.exchange.one-attempt]
> A machine enters one code at a time. Asked to start a second attempt while one runs, it reports
> that another pairing is still finishing, and sends nothing.
>
> - model: pairing

## One guess a side

The rules that hold whoever takes part without the code to one guess against each machine.

> r[pairing.guess.one-start]
> A code answers one start. The first start that passes the checks binds the code. Any later
> start, on any pairing connection, is refused, and the exchange under way is left alone.
>
> - model: pairing

> r[pairing.guess.one-answer]
> The entering machine judges the first answer of an attempt, a refusal included, and no other.
> A new attempt is a new pairing connection with a new scalar.
>
> - model: pairing

> r[pairing.guess.failed-code]
> Once an answer that claimed success has failed at the entering machine, for whatever reason,
> that machine refuses to send that code again for 120 seconds: it reports the code not accepted
> without opening a pairing connection, whichever machine it is asked to pair with. A refusal
> from the showing machine is not remembered.
>
> - model: pairing

## Already paired

> r[pairing.known.entering]
> Once tag Tb checks out, the entering machine refuses a showing machine whose public key is its
> own or a peer's, unless a rule below has the new pairing replace that peer. It sends an abort
> marked already paired, once, stores nothing, and reports the machine already paired, naming the
> peer and the day it was paired, or that it is this machine itself.
>
> - model: pairing

> r[pairing.known.showing]
> Once tag Ta checks out, the showing machine refuses an entering machine whose public key is its
> own or a peer's, unless a rule below has the new pairing replace that peer. It sends a refusal
> saying already paired, ends the code, stores nothing, and reports the machine already paired,
> naming the peer and the day it was paired, or that the other machine gave this machine's own
> key. The entering machine reports that the other machine already has this one paired.
>
> - model: pairing

> r[pairing.known.there]
> A showing machine that receives an abort marked already paired answers it as any abort, and
> reports that the other machine may already be paired here, or that the code was wrong.
>
> - model: pairing

> r[pairing.known.lost-receipt]
> A peer that has never linked, was paired less than 10 minutes before, and has the same name and
> platform as the new pairing proves is replaced by it, on either machine, so a pairing whose
> receipt was lost can be done again at once. Paired 10 minutes or more before, under another name
> or platform, or linked since, it is already paired.
>
> - model: pairing

> r[pairing.known.linked]
> Pairing never replaces a peer that has linked and is not dormant. To pair two such machines
> afresh, the user removes each from the other's peer list first.
>
> - model: pairing

> r[pairing.known.linked-since]
> When the peer a pairing would replace links before the pairing is stored, the pairing is not
> stored and that peer is kept as it is. A showing machine then reports the pairing not saved.
>
> - model: pairing

> r[pairing.known.last-check]
> A machine checks for an existing peer, and for the limit of 32, again as it stores, in one step
> with the store, shared by the code it shows and the code it enters. Two pairings finishing at
> once never leave two peers with one public key, nor more than 32 peers. A peer with the same key
> stored by another pairing in the meantime makes this one already paired.
>
> - model: pairing

## The limit of 32

> r[pairing.capacity.limit]
> A machine has at most 32 peers.
>
> - model: pairing

> r[pairing.capacity.showing]
> A machine with 32 peers, none of them dormant, shows no code and reports itself full.
>
> - model: pairing

> r[pairing.capacity.entering]
> A machine with 32 peers, none of them dormant, starts no attempt and reports that one of the
> two machines is full.
>
> - model: pairing

> r[pairing.capacity.full-start]
> A showing machine that has reached 32 peers, none of them dormant, since its code was shown
> answers the next start with a refusal saying full, ends the code, and reports itself full.
>
> - model: pairing

> r[pairing.capacity.full-confirm]
> A showing machine that reached 32 peers between a start and its confirmation refuses the
> confirmation, ends the code, stores nothing, and reports itself full, unless the new pairing
> replaces one of its peers.
>
> - model: pairing

> r[pairing.capacity.dormant-room]
> With 32 peers, one or more of them dormant, a machine shows codes and enters them, but completes
> only a pairing that replaces one of its peers. The entering machine finds that out once tag Tb
> checks out: it then aborts, stores nothing, and reports that one of the two machines is full.
>
> - model: pairing

## What a pairing stores

> r[pairing.peers.showing-stores-first]
> The showing machine stores the new peer before it sends the receipt, so the receipt proves the
> peer was stored. When storing fails, it sends a refusal instead, keeps nothing, and reports the
> pairing not saved. The entering machine then stores nothing.
>
> - model: pairing

> r[pairing.peers.entry]
> After a pairing, each machine has the other as a peer, with: its public key pinned and its
> machine id computed from it; its name, platform and link port as the exchange proved them; the
> time of the pairing; never linked yet; this machine may drive it and may be driven by it; and
> no edge chosen to face it.
>
> - parity: Python saved a shared token from the exchange in place of a pinned public key (#14 and ADR 0003: links authenticate by pinned keys)
> - model: pairing

> r[pairing.peers.address]
> The entering machine saves the address it reached the showing machine at. The showing machine
> saves the address the pairing connection came from. Each saves the address as one discovery may
> replace, unless the user typed it (`pairing.by-address.typed`).

> r[pairing.peers.order]
> A new peer goes after the peers already listed. A peer the new pairing replaces gives its place
> in the list to the new one.

> r[pairing.peers.permissions]
> Each peer has its own may drive and may be driven by. Changing them for one peer changes no
> other peer.

> r[pairing.peers.replace-keeps]
> A pairing that replaces a peer with the same public key keeps the old peer's label, the edge
> chosen to face it with when and by which machine that edge was chosen, whether it was kept off
> the crossing map, its zones and its place on the crossing map.
>
> - model: pairing

> r[pairing.peers.replace-address]
> A pairing that replaces a peer with the same public key keeps the old peer's address when the
> user typed it, even when the new pairing reached the peer at another address. An address the
> user typed for the new pairing replaces it.

> r[pairing.peers.at-once]
> A peer is linked with as soon as the pairing stores it: a link from it right after the pairing
> is accepted, with nothing restarted.

> r[pairing.peers.first-zone]
> A peer just paired, toward which this machine has no zone, gets one zone along a whole edge of
> this machine's display, so input can cross to it before its zones are chosen. Which edge it is
> the crossing map's choice (`spec/crossing.md`).

> r[pairing.peers.other-zones]
> A pairing leaves every other peer's zones as they were, apart from sharing an edge with the new
> peer's zone as the crossing map lays them out.

> r[pairing.peers.concurrent-save]
> A change to this machine's settings saved while a pairing or a removal completes never undoes
> it: the new peer stays, and a removed peer does not come back.

> r[pairing.peers.unreadable]
> While the file holding the peer list cannot be read, the links go on using the last peer list
> read. A change that needs the peer list written is refused rather than written from that copy,
> so nothing removed comes back and nothing changed since is lost. With no copy read yet, the
> links admit no one.

> r[pairing.peers.paired-list]
> The paired list in this machine's machine record holds the machine id of each of its peers that
> is not dormant.
>
> - parity: Python sent each peer a `paired` message listing every other live peer, leaving out the peer it was sent to (#14: one machine record in the store replaces the per-peer messages)

## Removing a peer

> r[pairing.remove.peer]
> Removing a peer deletes it, its zones and its place on the crossing map, together with the
> place of any machine on the crossing map that only it had listed in its paired list. Every other
> peer, and its zones, stay as they were.

> r[pairing.remove.link]
> Removing a peer closes its link at once, and a link it opens afterwards is refused.

> r[pairing.remove.unknown]
> Removing a peer that is not in the peer list changes nothing.

> r[pairing.remove.last]
> Removing the only peer leaves this machine with no peers, after a restart too.

## Dormant peers

> r[pairing.dormant.forgotten]
> A peer this machine has linked with becomes dormant when, for 60 seconds, every link attempt to
> it ends with the peer refusing this machine's key (`spec/link.md`). Its reason is that the other
> machine no longer has this pairing.
>
> - parity: Python's evidence was every handshake closed unanswered for 60 seconds, and it also marked a peer dormant when its token's link answered under another machine id (#14: links authenticate by pinned keys, so a key refused is the evidence, and a machine id can no longer differ from its key's)
> - model: link

> r[pairing.dormant.saved]
> A peer's dormant state and its reason are saved, and hold after a restart.

> r[pairing.dormant.cleared]
> A link that authenticates as a dormant peer ends its dormant state.
>
> - model: link

> r[pairing.dormant.kept]
> A dormant peer keeps its zones, the edge chosen to face it and its place on the crossing map, but none of its
> zones leads anywhere while it is dormant, and it blocks no other peer's zone or place on the
> crossing map. Once it is no longer dormant, its zones lead to it again, laid out with the other
> peers' as the crossing map places them.

> r[pairing.dormant.repair]
> Pairing again with a dormant peer, one with the same public key, replaces it rather than being
> refused as already paired, on either machine and whether or not the machine has 32 peers. The
> new peer is not dormant, and keeps what `pairing.peers.replace-keeps` and
> `pairing.peers.replace-address` say.
>
> - model: pairing

## Pairing from a beacon

> r[pairing.by-beacon.dial]
> The entering machine can pair with a machine whose beacon says a code is shown: it opens the
> pairing connection to the address the beacon came from, at the link port the beacon names, and
> runs the exchange with the code the user enters. What the beacon says proves nothing: the name
> recorded is the one the exchange proved.

> r[pairing.by-beacon.stale]
> Entering a code for a machine heard showing a code that has since ended, by cancelling or
> expiry, reports that the machine is not showing a code, and stores nothing on either machine.
>
> - model: pairing

## Pairing by address

> r[pairing.by-address.dial]
> The user can pair by typing another machine's address, an IPv4 address or a host name, with the
> code it shows. The entering machine resolves the address and opens the pairing connection to it
> at port 24820, then runs the same exchange as from a beacon.
>
> - parity: Python paired by address over TCP to port 24821 (#14: one UDP port, 24820, for links, pairing and beacons)

> r[pairing.by-address.typed]
> An address the user typed to pair is saved for the new peer as chosen by the user, which
> discovery never replaces. The showing machine saves the address the connection came from as
> one discovery may replace.

## Outcomes

> r[pairing.outcome.showing]
> When a code ends, the showing machine reports one outcome: paired, naming the new peer; a wrong
> code was entered; the code expired; already paired, naming the peer and the day it was paired,
> or that the other machine gave this machine's own key; the other machine may already be paired
> here, or the code was wrong; full; or not saved. A cancelled code reports none.
>
> - model: pairing

> r[pairing.outcome.entering]
> When an attempt ends, the entering machine reports one outcome: paired, naming the new peer by
> the name the exchange proved; not showing a code; the code was not accepted; the two run
> different versions of Beamer; did not answer; one of the two machines is full; already paired
> here, naming the peer and the day it was paired, or that it is this machine itself; already
> paired there; or another pairing is still finishing.
>
> - model: pairing
