# Discovery

How machines find each other on the network: the beacon each machine broadcasts and the machines
heard from it, the address this machine offers for pairing by address, each peer's chosen and
learnt addresses and how they are dialled, the probes that find a peer whose link has been down,
and Wake-on-LAN (F20). LAN discovery and the probes (what #14 calls dormant repair) are kept with F2
by #11, and F20 ships; the beacon, the address lists and the probes are #14's.

The rules come from #14 and ADR 0003, and from the Python app at `archive/python` (`core/pairing.py`'s
beacons and discovery, `core/link.py`, `core/receiver.py`, `core/wol.py`,
`mac_app/wake.py`, `mac_app/hardware_mac.py`, `win_app/hardware_win.py`, `win_app/sender.py`), mined
through its tests in `spec/ledger/discovery.txt`.

What stays elsewhere: the link, its port, its handshake and what it reports are `spec/link.md`'s;
pairing, from a beacon or by address, and the address a pairing saves for a new peer are
`spec/pairing.md`'s; the store that carries each machine record is `spec/store.md`'s; what a page
shows is `spec/viewmodel.md`'s.

Dropped, so no rule here (#11): pair nearby (F3) and its fields in the beacon, the portable build's
discovery (F23), the macOS 27 SSH tunnel (F42), and compatibility with the Python apps: the `find`
datagram that asked a machine for its beacon, and beacons of older pairing versions.

Every rule is `ci` unless it says otherwise.

## Beacons

> r[discovery.beacon.every-two-seconds]
> While Beamer runs, a machine sends a beacon every 2 s, from UDP port 24820 (`link.transport.port`)
> to UDP port 24820 of the limited broadcast address 255.255.255.255 and of the directed broadcast
> address of each of its networks.
>
> - parity: Python sent its beacons to UDP port 24821 (#14: one UDP port, 24820, for links, pairing and beacons)

> r[discovery.beacon.networks]
> The networks a machine sends beacons to are those of its IPv4 interfaces that are up and can
> broadcast, leaving out loopback and point-to-point interfaces (a VPN's) and any network of prefix
> /31 or /32. Each network's directed broadcast address follows its own netmask, and a network that
> two of the machine's addresses share is sent one beacon.

r[discovery.beacon.no-interfaces]
When a machine cannot list its interfaces, it sends each beacon to 255.255.255.255 and to the /24
broadcast address of each of its IPv4 addresses.

> r[discovery.beacon.bytes]
> A beacon is one UDP datagram of exactly 9 bytes: the byte 0x00, so QUIC's fixed bit is clear and
> no beacon is taken for a QUIC packet; the six ASCII bytes `beamer`; the beacon version, 1; and a
> flags byte whose lowest bit is set while this machine shows a code (`pairing.code.beacon`) and
> whose other bits are zero. A beacon carries nothing else: no port, no name, no machine id, no
> platform.
>
> - parity: Python's beacon was JSON carrying the machine's name, platform, link port, pairing port and pairing version, and, while a code was shown, its pairing id and machine id (#14: one UDP port, 24820, for links, pairing and beacons, so a beacon names no port; it says only that a Beamer is here, plus a flag while a code is shown, and never carries a stable id)
> - fuzz: discovery-packet
> - model: pairing

> r[discovery.beacon.ignored]
> A datagram with QUIC's fixed bit clear that is not exactly the 9 bytes `discovery.beacon.bytes`
> sets out (another length, another first byte, other bytes than `beamer`, another version, or a
> flag bit other than the lowest), whatever its size up to 65,535 bytes, is ignored: it is not
> listed, nothing is sent in answer, nothing is logged for it, and the machine goes on sending
> beacons and hearing others.
>
> - fuzz: discovery-packet

r[discovery.beacon.reset]
A report that a datagram this machine sent found no port open (an ICMP port unreachable, which
Windows delivers as an error on the next receive) never stops the machine sending beacons or
hearing them.

> r[discovery.beacon.port-failure]
> Any other failure to receive on UDP port 24820 is a failure of the port the machine takes links
> on, and is dealt with as `link.transport.port-taken` deals with a taken port: the machine lets the
> port go, reports that it cannot take links, naming the port and the error, sends no beacons and
> hears none while it lacks the port, and tries to take the port again every 5 s until it has it.
>
> - parity: Python's beacon service stopped for good on such a failure, closed its socket and showed that Beamer could not look for machines, naming the error, until the app restarted (#14: beacons share the link's one UDP port, so losing it is the link's failure, which `link.transport.port-taken` retries every 5 s)

r[discovery.beacon.not-delayed]
Resolving a DNS name, one typed to pair by address or one of a peer's chosen addresses, never
delays a beacon going out or one being heard.

> r[discovery.beacon.lan]
> With the Mac and `lebig` on one network, each lists the other among the machines heard within 4 s
> of both running Beamer, whatever other adapters (a VPN's, a Hyper-V virtual switch) either has.
>
> - tier: machine (two real machines on one network; hosted runners have no peer on theirs)

## Machines heard

> r[discovery.heard.list]
> Core reports the machines heard: for each address a beacon came from, whether its latest beacon
> says a code is shown. A machine heard is listed until 7 s pass with no beacon from that address.
>
> - model: pairing

> r[discovery.heard.own]
> A machine never lists itself: a beacon from one of its own addresses is not listed, though it
> hears its own broadcasts.
>
> - model: pairing

> r[discovery.heard.limit]
> At most 64 machines heard are listed. A beacon from a machine not listed while 64 are first
> removes those not heard for 7 s, then, if 64 are still listed, the one heard from longest ago.
>
> - parity: Python never removed the address the user had asked for its beacon by address (#14: pairing by address needs no beacon, so no address is asked for one)
> - model: pairing

## The address to pair by

> r[discovery.own-address.offered]
> While a code is shown, Core gives the address to type on another machine to pair with this one by
> address: this machine's address on its default route when that address is in a private range
> (10.0.0.0/8, 172.16.0.0/12 or 192.168.0.0/16), else its first address in a private range, else its
> default route's address. With no IPv4 address it gives none.
>
> - parity: Python's Windows app first gave its address on the network of a peer it had known, and both apps showed the pairing port beside the address (pairing by address dials port 24820, `spec/pairing.md`)

## A peer's addresses

> r[discovery.address.list]
> Each peer has a list of addresses, each an IPv4 address or a DNS name. Each address is a chosen
> address or a learnt address.
>
> - parity: Python kept one address and port per peer, marked as chosen or not (#14: an address list per peer, of addresses learnt on the LAN and ones given by hand)
> - model: link

> r[discovery.address.choose]
> The user can give a peer chosen addresses, each an IPv4 address or a DNS name, and can remove
> them. A chosen address is dialled on the one link port (`link.transport.port`).
>
> - parity: Python took a port with the address the user chose (`link.transport.port`: no machine has a port of its own)

r[discovery.address.choose-learnt]
Choosing an address the peer already has as a learnt address makes it a chosen address, and it is
no longer one of the peer's learnt addresses.

> r[discovery.address.chosen-only]
> From the moment a peer has a chosen address, it is dialled only at its chosen addresses: its learnt
> addresses are not dialled, it is never probed, and no link teaches it an address, neither one this
> machine dials nor one that comes in from the peer, even one already authenticating when the
> address was chosen.
>
> - model: link

> r[discovery.address.unchosen]
> When the user removes a peer's last chosen address, the address stays as a learnt address, the
> peer is dialled at its learnt addresses, and probes and links teach it addresses again.
>
> - model: link

r[discovery.address.one-peer]
Choosing or removing an address for one peer changes no other peer's addresses.

> r[discovery.address.learnt-on-auth]
> An address becomes one of a peer's learnt addresses only once a link at it authenticates as that
> peer. A dial at an address that fails, or at which another machine answers, saves nothing.
>
> - model: link

> r[discovery.address.learnt-incoming]
> A link that a peer dials to this machine teaches the address it came from, unless that address is
> a loopback or link-local one.
>
> - parity: Python learnt it only from a peer that said it took links, by naming a port other than 0 (#14: every machine takes links on its one UDP port)
> - model: link

> r[discovery.address.learnt-limit]
> A peer keeps at most four learnt addresses. When a fifth is learnt, the one at which a link last
> authenticated longest ago is forgotten.
>
> - parity: Python kept one learnt address, which the newest replaced (#14 keeps an address list per peer; four keep a cable, Wi-Fi and two older addresses)
> - model: link

r[discovery.address.saved]
A peer's chosen and learnt addresses are saved, and kept after a restart.

r[discovery.address.concurrent-save]
Saving a learnt address or a hardware address never undoes a change to this machine's settings
saved meanwhile.

## Dialling a peer's addresses

> r[discovery.dial.parallel]
> A peer's addresses are dialled together. Dials at its LAN addresses start at once, and dials at
> its other addresses 250 ms later (RFC 8305's attempt delay), or at once when it has no LAN address
> or every LAN dial has failed. The first dial to authenticate as the peer gives the link, and the
> peer's other dials are given up.
>
> - parity: Python dialled one address per peer (#14: the addresses are dialled in parallel, the first to authenticate wins, and LAN addresses are preferred)
> - model: link

r[discovery.dial.name]
A chosen address that is a DNS name is resolved at each dial, and each IPv4 address it resolves to
is dialled as one of the peer's addresses.

> r[discovery.dial.one-report]
> A peer's dials at all its addresses together make one dial of the peer, in the sense of
> `link.dial.deadline`, `link.dial.backoff` and `link.report.down`. A dial at one of its addresses
> that fails reports nothing, and starts no new dial, while a dial at another of its addresses is in
> progress or still to start. The dial of the peer fails once every address dial has failed, or 5 s
> after its first address dial began, which gives up those still in progress. Only then does the link
> report the peer down: with the reason of the first address dial that ended for a reason other
> than that the peer is unreachable, or as unreachable when every one ended so. Then
> `link.dial.backoff`'s wait begins, and the next dial of the peer dials all its addresses again.
>
> - model: link

## Probes

> r[discovery.probe.when]
> A peer in use with no chosen address, whose link has been down for 10 s, or which has had no link
> in the 10 s since Beamer started, is probed: this machine dials each machine heard, at the address
> its beacon came from (`link.transport.port`), that is neither one of the peer's addresses nor an
> address of another peer that has a link up. It dials each such address at most once every 30 s.
>
> - parity: Python tried only beacons carrying the peer's name, passed over a beacon whose machine id was another peer's, tried another peer's saved address last rather than never, and tried one address every 30 s, the next in turn (#14: probing tries each beacon address it hears that isn't a known address of another linked peer, at most once per address every 30 s, and the beacon carries no name)
> - model: link

> r[discovery.probe.found]
> A probe that authenticates as the peer gives the peer its link, and its address becomes one of the
> peer's learnt addresses. A probe that fails reports as `link.report.probe-silent` says.
>
> - model: link

## Hardware addresses

r[discovery.hardware.form]
A hardware address is six bytes, not all zero. Beamer shows one as six two-digit lower-case
hexadecimal numbers joined by colons, such as `02:1a:2b:3c:0d:4e`.

> r[discovery.hardware.record]
> A machine's machine record lists, for each of its network adapters that has an IPv4 address and a
> hardware address, that IPv4 address and the adapter's hardware address. The machine reads them when
> Beamer starts and again each time one of its links comes up.
>
> - parity: Python sent, in each link's handshake, the hardware address of the adapter that carried that link (#14: each machine's facts go in its own machine record)

r[discovery.hardware.unknown]
An adapter whose hardware address is not six bytes, is all zeros or cannot be read is not listed in
the machine record. When the adapters cannot be read at all, the record lists none, and links come
up as they would otherwise.

> r[discovery.hardware.of-peer]
> A peer's hardware address is the one its machine record gives for the address at which this
> machine last had a link with it. When the record gives none, it is the one this machine read from
> its own neighbour table (ARP) for exactly that address, the last time a link with the peer came
> up.
>
> - parity: Python's Mac read its neighbour table at every link and kept that address over the one the peer had sent when they differed (the peer knows its own adapter; the table is the fallback the Windows app already treated it as)

r[discovery.hardware.not-delayed]
Reading the neighbour table never delays a link reporting up, or anything sent on it.

## Wake-on-LAN

r[discovery.wake.crossing]
A crossing, by any crossing method, from home or from a driven machine, to a peer that has no link
and has a hardware address wakes it instead, unless the peer refused (`discovery.wake.refused`): the
crossing does not happen, input stays where it was, nothing is queued, and the app reports that it
is waking the peer, naming it.

r[discovery.wake.refused]
A peer whose link last reported down because another machine answered, the peer no longer has this
pairing, the peer does not use this machine now, the two run different versions, or a message did
not decode is never woken: a crossing to it reports that it cannot cross, with that reason.

r[discovery.wake.no-address]
A crossing to a peer with no link and no hardware address sends no wake packet, and reports that it
cannot cross, with the reason its link last reported.

r[discovery.wake.packet]
A wake sends the Wake-on-LAN magic packet, 102 bytes: six bytes 0xFF, then the peer's hardware
address sixteen times. It goes to UDP port 9 of 255.255.255.255, of the /24 broadcast address of the
address at which this machine last had a link with the peer, and of that address itself.

r[discovery.wake.send-failed]
When the wake packet cannot be sent to 255.255.255.255, the app reports that the wake packet could
not be sent, and the wake ends.

r[discovery.wake.waiting]
A wake waits up to 60 s for a link with the peer. When one comes up, the app reports the peer awake,
and that crossing again moves input there; input does not move by itself. With no link 60 s after
the packet, the app reports that the peer did not wake.

> r[discovery.wake.once]
> While a wake of a peer is in progress, another crossing to that peer sends no second packet.
>
> - parity: Python's Mac app ran one wake at a time across all its peers (N machines: each peer has its own wake, as the Windows app had)

r[discovery.wake.status]
While a wake of a peer is in progress, the peer's status is waking, whatever its link reports
meanwhile. Once the wake ends, it is the wake's outcome.

r[discovery.wake.lost-link]
A link lost while input is on its peer sends no wake packet.

r[discovery.wake.can-wake]
Core reports whether the peer the shortcut would cross to can be woken: it has no link, has a
hardware address and did not refuse.

> r[discovery.wake.wakes]
> A sleeping peer that is set to wake from Wake-on-LAN wakes when a crossing to it sends the packet.
>
> - tier: human (whether a real machine wakes from sleep is seen at the machine)
> - do: Put `lebig` to sleep, wait until its display is dark, then push the pointer through the Mac's zone that leads to it
> - see: The Mac says it is waking `lebig`, `lebig` wakes, and within a minute the Mac says it is awake; pushing through the zone again moves the pointer onto it
