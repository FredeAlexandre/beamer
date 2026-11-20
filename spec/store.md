# Store

The store: the replicated state of which every machine in the cluster has a copy. It is one
mechanism for everything machines share, in two kinds of entry: each machine's own machine record,
which only that machine writes and which a number that only goes up versions, and shared entries,
which any machine may write and where the newest stamp wins, entry by entry. Stamps come from a
hybrid logical clock, never from raw wall time. When a link comes up, its two machines exchange what
the other lacks; after that, each change goes to every other peer with a link up. A machine that no
machine in the cluster lists as a peer is forgotten: its machine record is dropped, and the shared
entries naming it read as unset while it stays forgotten, never written as removed. The store is the
store of F19, which ships without the 1.4.x migration (#11).

The rules come from the store decided in #14 and the words of #20, read as the domains that rely on
it read it. Python had no store: the crossing map, the profiles, displays, audio and camera each had
messages and stamps of their own, so no Python test is assigned to this domain, and a rule here that
replaces what Python did carries the `parity:` marker of the behaviour it replaces.

What stays elsewhere: which keys exist, what their values and record parts are and how a domain
reads them are the domains' own: the crossing map, its footprints, the profiles, every machine's
zone choices in them and own zones on every machine are `spec/crossing.md`'s; a display owner and a
machine's list of displays `spec/display.md`'s; the paired list `spec/pairing.md`'s; hardware
addresses `spec/discovery.md`'s; the values kept the same on all machines `spec/settings.md`'s;
audio routes and camera routes `spec/audio.md`'s and `spec/camera.md`'s. The link that carries the
store's messages, their framing, their traffic class and their decoding are `spec/link.md`'s
(`link.frame.round-trip`, `link.frame.undecodable`, `link.frame.control-limit`). The data file the
store is kept in, when it is written and what happens when it is refused, are `spec/runtime.md`'s
(`runtime.files.folder`, `runtime.files.debounced`, `runtime.files.refused-defaults`,
`runtime.files.refused-kept`).

Dropped, so no rule here (#11): the 1.4.x migration of the settings file (F19), and any
compatibility with the Python apps' messages for each kind of state (`audio_*`, `camera_*`,
`display_*`, `layout`, `arrangement`, `paired`) and their stamps (#14). Nothing in this domain comes
later.

Every rule is `ci`. Behaviour across three or more machines is tested through `beamer-sim`.

## Shared entries

A shared entry is one value under one key, declared with its schema by the domain that owns it. Any
machine may write it.

> r[store.entry.default]
> A shared entry no machine has written reads as unset, or as its key's default when the key
> declares one. Any write of the entry wins over its default, whatever its stamp.
>
> - model: store

> r[store.entry.newest]
> Of the writes of one shared entry a machine has made or taken since a domain last discarded the
> entry there (`store.entry.discard`), it keeps the one with the newest stamp
> (`store.stamp.parts`), whatever order they arrived in. A write that arrives no newer than the
> write this machine has for that entry changes nothing here and is not sent on.
>
> - model: store

> r[store.entry.apart]
> Writes of different shared entries are all kept, even when two machines make them at once before
> either reaches the other: a write replaces only the entry it writes.
>
> - model: store

> r[store.entry.unchanged]
> Writing a shared entry with the value it already has from an earlier write writes nothing: no new
> stamp, and nothing goes to any peer. A domain may instead write an entry again with the value it
> has, as after refusing a write of it (`store.entry.refused`): that is a write like any other,
> stamped as `store.stamp.after-seen` says, and it goes to every peer. An entry never written has
> no value yet, so writing its default to it is a write like any other. An entry whose value is a
> fallback's (`store.stamp.fallback`) is written by a write of the same value that is not a
> fallback.
>
> - model: store

> r[store.entry.never-removed]
> No write removes a shared entry: a write replaces its value with another. Where a domain needs
> "none" or "removed", its schema declares that as a value, which wins or loses by its stamp like
> any other.
>
> - model: store

> r[store.entry.schema]
> A write arriving from a peer whose value breaks its key's schema, or whose key no domain declares,
> is ignored: it is not taken, read or sent on, and the entry stays as it was. The other writes that
> arrive with it are taken.
>
> - model: store

> r[store.entry.refused]
> A domain may refuse writes of its keys as they arrive from a peer, one by one, or all those of one
> change (`store.sync.together`) or of one exchange at link up (`store.sync.exchange`) together. A
> refused write is not taken, read or sent on, and the entry stays as it was.
>
> - model: store

> r[store.entry.discard]
> A domain may discard one of its shared entries from this machine's copy of the store. The entry
> then reads here as one never written, is no longer kept in the store's file, and goes to no peer.
> A write of it arriving later is taken as any other unless the domain refuses it
> (`store.entry.refused`).
>
> - model: store

## Stamps

> r[store.stamp.parts]
> A stamp is a wall time in milliseconds, a counter and a machine id, marked as a fallback's or not
> (`store.stamp.fallback`), and followed, on an answer, by the ids of the machines that answered
> (`store.stamp.answer`). The order of stamps is the order of the writes of one shared entry, and
> every rule that calls a write of an entry newer or older than another means this order. Of two
> stamps, the newer is:
>
> 1. the one not marked as a fallback's, when only one of them is;
> 2. otherwise, the one with the later time; with the same time, the one with the greater counter;
>    with the same counter too, the one with the greater machine id, compared as bytes;
> 3. with all of these the same, the one whose answering machine ids, compared in order, first have
>    the greater id, compared as bytes, or, when one of them runs out first, the longer one.
>
> No two writes of one shared entry have the same stamp.
>
> - model: store
> - parity: Python stamped each crossing map and profile register with whole seconds of wall time, raised to one past the stamp it had when the clock was not ahead of it, and broke a tie by the writer's machine id (#14: stamps are a hybrid logical clock, wall time with a counter and the machine id)

> r[store.stamp.after-seen]
> Every write a machine makes, apart from an answer (`store.stamp.answer`), is stamped with its own
> machine id and a time and counter after those of every stamp it has seen: a later time, or the
> same time and a greater counter. A machine has seen every stamp it made, and every stamp that
> arrived from a peer in a write, whether that write was taken, refused or ignored
> (`store.entry.refused`, `store.entry.schema`). So a write that is not a fallback, made on a
> machine whose clock is behind the stamp of the entry it replaces, still replaces it on every
> machine that takes it, and one made after a refused write arrived is newer than that write.
>
> - model: store

> r[store.stamp.clock]
> A write other than an answer, made on a machine whose clock is ahead of every stamp it has seen
> (`store.stamp.after-seen`), is stamped with its clock's time and a counter of 0. So of two such
> writes of one entry, both fallbacks or neither, made on two machines whose clocks agree, the later
> one wins, even when neither machine had taken the other's write.
>
> - model: store

> r[store.stamp.ahead]
> A write arriving with a stamp ahead of this machine's clock is taken as any other, however far
> ahead, and this machine's next writes, apart from answers, are stamped with a time and counter
> after its (`store.stamp.after-seen`).
>
> - model: store
> - parity: Python ignored a crossing map stamped more than a day ahead of its own clock, and a profile register stamped more than 600 s ahead (#14: the newest stamp wins, and the hybrid logical clock carries a machine whose clock is behind past every stamp it takes)

> r[store.stamp.restart]
> After a restart, every write a machine makes, apart from an answer, is stamped with a time and
> counter after those of the newest stamp the store's file keeps (`store.file.kept`), whatever its
> clock says.
>
> - model: store

> r[store.stamp.fallback]
> A domain may write a shared entry as a fallback. Its stamp is marked as a fallback's
> (`store.stamp.parts`), so it loses to every write of that entry that is not a fallback, older or
> newer, in whatever order they arrive, and wins over the entry's default
> (`store.entry.default`). Of two fallbacks of one entry, the one with the newer stamp wins.
>
> - model: store

> r[store.stamp.answer]
> A domain may write a shared entry in answer to a write of any entry it has taken. The answer is
> stamped with the time, counter, machine id and answering machine ids of the write it answers,
> followed by this machine's id, and is not marked as a fallback (`store.stamp.parts`). So an
> answer to a write that is not a fallback is newer than that write and every write it is newer
> than, and older than every write newer than it that is neither a fallback nor an answer: every
> such write made on any machine after that machine took the write answered is newer than the
> answer, whatever the machines' clocks say. Of two answers to the same write, the one made on the
> machine with the greater machine id is newer. A machine writes one entry at most once in answer to
> one write: a later write of that entry it makes is stamped as `store.stamp.after-seen` says.
>
> - model: store

## Machine records

Each machine has one machine record. Its parts, such as its displays, its paired list, its profile
status, its hardware addresses and its capabilities, are declared by the domains that own them.

> r[store.record.own]
> Only a machine writes its own machine record. A record of this machine arriving from a peer is
> never taken in place of the one this machine writes.
>
> - model: store

> r[store.record.version]
> Each write of a machine record raises its version by one. A record arriving with a newer version
> than the one this machine has for that machine replaces it whole, as its machine wrote it, and
> each domain reads its own parts of it as its rules say. A record arriving with the same version or
> an older one changes nothing and is not sent on.
>
> - model: store

> r[store.record.unchanged]
> A machine writing a part of its machine record as it already is writes nothing: the version stays,
> and nothing goes to any peer.
>
> - model: store

> r[store.record.own-newer]
> When a version of this machine's own record arrives that is newer than the one it has, as after
> the store started empty, this machine writes its record again, its parts as they are here now,
> with a version newer than the one that arrived, and that record replaces the arriving one on every
> machine.
>
> - model: store

> r[store.record.away]
> A machine's record stays in every other machine's store while that machine has no link up, and
> across restarts, until that machine is forgotten (`store.forget.record`).
>
> - model: store

> r[store.record.capabilities]
> Each of a machine's capabilities, and whether it is ready, needs permission, is refused or is
> unsupported here, is a part of its machine record. A peer's capabilities are read only from that
> peer's record.
>
> - parity: Python told a peer its capabilities in a list sent in each link's handshake, and its audio ones again in its audio hello (#14: capabilities become facts in each machine's own record)

## Forgotten machines

> r[store.forget.known]
> On each machine, a machine is known while it is this machine, a peer of this machine (a dormant
> one included), or a machine listed in the paired list of a known machine's record. Every other
> machine is forgotten here. A machine stops being known once no known machine lists it, even when
> forgotten machines' records listed it.
>
> - model: store

> r[store.forget.record]
> A forgotten machine's record is dropped from the store, and a record of a forgotten machine
> arriving from a peer is not taken. Whether a machine is forgotten is judged once everything of
> one change (`store.sync.together`), or of one exchange at link up (`store.sync.exchange`), has
> been taken, so a record that arrives in the same change or exchange as the record whose paired
> list lists its machine is taken.
>
> - model: store

> r[store.forget.entry]
> Each key's schema says which machines a shared entry names: the machines in its key, and those its
> value names. A shared entry whose key names a forgotten machine reads as unset. A part of an
> entry's value that names a forgotten machine reads as unset, and the rest of the value reads as it
> was written; a value that is only a forgotten machine's name reads as unset.
>
> - model: store

> r[store.forget.kept]
> A shared entry naming a forgotten machine stays in the store: it is kept in the store's file, goes
> to peers in an exchange, and is ordered by its stamps like any other entry. Only reading it is
> changed.
>
> - model: store

> r[store.forget.write-keeps]
> A write of a shared entry whose value has parts naming a machine forgotten on the writing machine,
> next to other parts, such as zone choices toward each peer, keeps those parts as the store has
> them there and replaces only the others. A write of an entry whose whole value is a machine's
> name, such as a display owner, replaces that value as any write does.
>
> - model: store

> r[store.forget.nothing-written]
> Forgetting a machine writes nothing, and nothing goes to any peer: no machine writes a shared
> entry naming a forgotten machine as removed, and no machine's store keeps a mark that it was
> forgotten.
>
> - model: store

> r[store.forget.known-again]
> When a forgotten machine is known again, as when it is paired again, every shared entry naming it
> reads as set again, as the store has it then: as it was when it was forgotten, with every write of
> it made meanwhile, on any machine.
>
> - model: store

> r[store.forget.record-again]
> When a machine becomes known here while this machine lacks its record, as when a forgotten
> machine is known again, this machine asks every peer it has a link up with for that record at
> once, without waiting for a link to come up, and takes it from those that have it as a record
> arriving from a peer (`store.record.version`).
>
> - model: store

## Exchange and changes

> r[store.sync.exchange]
> When a link comes up, each of its two machines takes every shared entry and machine record the
> other has that it lacks or has older, so both end with the newer of the two for each, apart from
> the writes and records one of them refuses or ignores as they arrive (`store.entry.schema`,
> `store.entry.refused`, `store.forget.record`). An entry a domain discarded on one of them is one
> that machine lacks, so it crosses the link again at each link up with a machine that has it, and
> is refused or taken there again.
>
> - model: store

> r[store.sync.lacks-only]
> In the exchange at link up, each shared entry and machine record goes over the link only when the
> other machine lacks it, a discarded entry included, or has it older, and at most once. When the
> two machines already have the same store, no entry and no record goes over the link.
>
> - model: store

> r[store.sync.done]
> Each machine of a link tells its domains once the exchange at link up is done: it has, for every
> shared entry and machine record either machine had when the link came up, the newer of the two
> versions, and the other machine has them too, apart from the writes and records one of them
> refused or ignored as they arrived (`store.sync.exchange`).
>
> - model: store

> r[store.sync.change]
> After the exchange, a write made on a machine, or taken from a peer and changing something there,
> goes to every other peer that machine has a link up with, apart from the peer it came from.
>
> - model: store

> r[store.sync.reaches]
> A write made on any machine reaches every machine of the cluster, including machines with no link
> to the machine that made it, through the machines between. Only a machine between that refuses or
> ignores it stops it: it goes from that machine to no peer (`store.entry.refused`,
> `store.entry.schema`), so the machines it reaches only through that one don't take it. A machine
> that was away takes every write it missed in the exchange at its next link.
>
> - model: store

> r[store.sync.any-peer]
> Shared entries and machine records are taken from every peer over any link, whichever machine
> wrote them, whatever this machine and that peer may drive of each other.
>
> - model: store
> - parity: Python took profile entries only from a linked peer that may drive this machine and is not dormant, and refused those from any other peer whole (#14: one store replaces the messages each kind of state had, and domains declare only their keys and schemas)

> r[store.sync.together]
> The writes of one change, however many entries they are and however many messages carry them, are
> taken together on every machine: no domain reads some of the writes of one change taken there
> without the others taken there. The writes of it that break their schema, that a domain refuses
> or that are no newer than this machine's (`store.entry.schema`, `store.entry.refused`,
> `store.entry.newest`) are not taken, and the rest are taken together.
>
> - model: store

> r[store.sync.large]
> An exchange or a change larger than the largest control message a link carries goes over the link
> whole, in as many messages as it needs, and the link stays up.

> r[store.sync.settles]
> After any number of writes, made at once on any number of machines of a cluster, once every link
> between them has been up long enough to carry them, no further store message goes over any link,
> and every two machines with a link up between them have the same machine record of each machine
> known to both, and the same write of each shared entry, apart from an entry one of them discarded
> (`store.entry.discard`), or of which one of them refused or ignored the other's newer write
> (`store.entry.refused`, `store.entry.schema`). So when no domain refused, ignored or discarded
> anything, every machine of the cluster has the same shared entries.
>
> - model: store

## The store's file

> r[store.file.kept]
> The store's file keeps every shared entry with its stamp, a fallback's or an answer's included,
> every machine record with its version, and the newest stamp this machine has seen
> (`store.stamp.after-seen`). After a restart, the store reads exactly as it did when its file was
> last written, entries naming a forgotten machine included.
>
> - model: store

> r[store.file.round-trip]
> Every store written to the store's file loads as the same store at the next read.
>
> - fuzz: store-file

> r[store.file.refused]
> A store's file that is not in the store's form, or in which any stamp, version, machine id or key
> is malformed or unknown, or any value breaks its key's schema, is refused whole: nothing in it is
> taken, the store starts empty, and the file stays as it is (`runtime.files.refused-defaults`,
> `runtime.files.refused-kept`).
>
> - fuzz: store-file
> - parity: Python set aside only the profile state it could not read, a register with a malformed stamp included, under a key of its own in the settings file, and went on from a fresh profile state with the rest of that file kept (#16: the store's copy is a data file of its own, refused whole as `runtime.files.refused-defaults` says)

> r[store.file.empty]
> A store that starts empty, on a first run or after its file was refused, has only this machine's
> record, and every shared entry reads as never written. At its first exchange it takes the
> cluster's entries and records as any machine that was away does (`store.sync.reaches`), and its
> own record is written again as `store.record.own-newer` says.
>
> - model: store
