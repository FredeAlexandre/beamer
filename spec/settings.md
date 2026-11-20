# Settings

Which of a machine's settings are kept the same on all machines, and how (F18: Same on all machines
and design follow), and which stay this machine's own, kept in `local.json` (F19, which ships
without the 1.4.x migration, #11). Same on all machines keeps four groups of shared settings in
step across the cluster: Crossing, Design, Shortcut and Names. Each shared setting is a shared entry
in the store, so a change made on any machine reaches every other, the newest wins, and a machine
that was away catches up at its next link. Design follow lets one machine take a peer's design as
its own.

The rules come from #11's outcomes, the store decided in #14, the two data files of #16, the words
of #20, and the Python app at `archive/python` (`core/settings_sync.py`, the Same on all machines
and design follow code of `mac_app/kvm_bridge_app.py` and `win_app/kvm_bridge_win.py`, and the
settings files of `mac_app/settings_store.py`, `mac_app/config.py` and `win_app/app_config.py`),
mined through its tests in `spec/ledger/settings.txt`.

What stays elsewhere, cited here and never restated:

- **The store** that carries every shared setting: stamps and which write is newest
  (`store.stamp.parts`), a write that breaks its key's schema or that a domain refuses
  (`store.entry.schema`, `store.entry.refused`), the exchange at link up and the changes after it
  (`store.sync.exchange`, `store.sync.change`, `store.sync.reaches`), a machine record and its parts
  (`store.record.version`, `store.record.away`), and forgotten machines (`store.forget.entry`), are
  `spec/store.md`'s.
- **The data files**: their folder, format, writes, and what happens when one is refused or cannot be
  written (`runtime.files.folder`, `runtime.files.debounced`, `runtime.files.refused-defaults`,
  `runtime.files.refused-kept`, `runtime.files.write-failed`) are `spec/runtime.md`'s.
- **What each setting does**: the resistance and blocking crossings while dragging
  (`crossing.push.resistance`, `crossing.push.drag`), the shortcut, its key, style, double-tap window
  and destination (`crossing.shortcut.defaults`, `crossing.shortcut.window-range`,
  `crossing.shortcut.key-allowed`, `crossing.shortcut.key-typing`), and a shortcut key that clashes
  with a jump key (`crossing.shortcut.key-jump-clash`, `crossing.shortcut.shared-key-clash`) are
  `spec/crossing.md`'s; how the edge glow, its styles, colours, length and size and the landing
  animation are drawn is `spec/ui.md`'s; zones, profiles, own zones and jump keys are
  `spec/crossing.md`'s; a peer's name, platform and permissions are `spec/pairing.md`'s.
- **What the window shows**: the Same on all machines module, its groups, its notes and Revert
  (`viewmodel.settings.groups`, `viewmodel.settings.tick`, `viewmodel.settings.revert`), "Match
  design with" and its lock (`viewmodel.design.follow-choices`, `viewmodel.design.follow-lock`), and
  how a machine is named (`viewmodel.machines.label.name`) are `spec/viewmodel.md`'s.

Dropped, so no rule here (#11): the 1.4.x migration of the settings file and the profiles
migration that came after it (F19), phone peers, and any compatibility with the Python apps (#14):
their `settings` message and its fields, the `settings`, `design_sync` and `layout` capabilities, a
peer of 1.5.1 that sent no groups or no stamps of its own, the Length and Size presets that 1.5.0 to
1.5.2 sent beside the rulers, and the "Needs beta.6 or later" notes. Later (#11): the crossing
effects (F30), whose room here is `settings.design.style` and `settings.design.colour`, which keep a
crossing effect's style or colour pack.

Every rule is `ci`. Behaviour across three or more machines is tested through `beamer-sim`. Every
rule about shared settings is about the store's state machine and names its `model: store`.

## Groups

> r[settings.groups.four]
> Same on all machines has four groups of shared settings, and every shared setting is in exactly
> one of them:
>
> | Group | Its shared settings |
> |---|---|
> | Crossing | the resistance, blocking crossings while dragging |
> | Design | the crossing style, the colour, the Length, the Size, the frame rate, showing where the pointer lands, the shortcut-and-menu style |
> | Shortcut | the shortcut on or off, the shortcut key, its style (double-tap or long press), the double-tap window, the shortcut's destination |
> | Names | the name of each machine (`settings.names.entry`) |
>
> No other setting is shared (`settings.own.never-shared`).
>
> - model: store

> r[settings.groups.entry]
> Which groups are kept in step is one shared entry, holding a set of the four groups. Until it is
> written it reads as none, so on a first run no group is kept in step and each machine keeps its
> own values.
>
> - model: store

> r[settings.groups.everywhere]
> Choosing which groups are kept in step on any machine writes that entry, and once the write
> reaches a machine (`store.sync.reaches`), the same groups are kept in step there: turning a group
> on at one machine turns it on at every other, and turning it off turns it off at every other. Of
> two choices made at once on two machines, the newer write (`store.stamp.parts`) stands on both.
>
> - model: store

> r[settings.groups.schema]
> A write of the groups entry naming anything but the four groups breaks its schema, and is
> ignored as `store.entry.schema` says.
>
> - model: store

> r[settings.groups.off-keeps]
> When a group stops being kept in step, each machine keeps the values its settings in that group
> have then, and changes none of them back.
>
> - model: store

## Shared settings

> r[settings.shared.entry]
> Each shared setting is one shared entry. Every change of a shared setting made on this machine,
> by the user or by Revert (`settings.revert.values`), writes its entry with the new value, whether
> its group is kept in step or not. A change that leaves the setting's value as it is writes
> nothing (`store.entry.unchanged`).
>
> - model: store
> - parity: Python stamped each shared value it changed, but sent its values only while its group was on, so a value changed while its group was off reached no peer until the group was turned on (#14: one store carries every shared setting, entry by entry)

> r[settings.shared.only-changed]
> A change of one shared setting writes that setting's entry alone: the other shared settings'
> entries keep the writes they had, so a machine that missed another machine's change of one setting
> never puts back the older value of that setting when it changes another.
>
> - model: store

> r[settings.shared.change-wins]
> A change of a shared setting made on this machine is newer than every write of that setting this
> machine has taken, whatever its clock says (`store.stamp.after-seen`), so while its group is kept
> in step it becomes that setting's value on every machine, even when a peer whose clock is ahead
> made the write it replaces.
>
> - model: store

> r[settings.shared.reaches]
> A change of a shared setting made on any machine reaches every machine of the cluster, those with
> no link to the machine that made it included, through the machines between
> (`store.sync.reaches`). Each machine takes it once and sends it on to every other peer it has a
> link up with, never back to the peer it came from, and a copy arriving again changes nothing and
> goes no further.
>
> - model: store

> r[settings.shared.any-peer]
> A change of a shared setting is taken from any peer over its link, whichever machine drives
> which (`store.sync.any-peer`).
>
> - model: store

> r[settings.shared.applied]
> While a group is kept in step, each of its settings on this machine takes the value of its entry
> whenever a newer write of that entry is taken here (`store.entry.newest`), and keeps it after a
> restart.
>
> - model: store

> r[settings.shared.turned-on]
> When a group becomes kept in step on this machine, chosen here or taken from a peer, each of its
> settings whose entry has been written takes the entry's value, and each whose entry was never
> written keeps its own. So of the changes made to one setting while its group was off, on any
> machine, the newest is the value every machine has once the group is on.
>
> - model: store

> r[settings.shared.waits]
> While a group is not kept in step, a write of one of its settings' entries taken from a peer
> changes nothing of this machine's setting.
>
> - model: store

> r[settings.shared.newest]
> Of two changes of one shared setting made on two machines, the newer write
> (`store.stamp.parts`) is the value both machines have while its group is kept in step, whichever
> arrives first: with the same time, the one from the machine with the greater machine id.
>
> - model: store
> - parity: Python stamped a change with whole seconds of wall time and broke a tie by the larger machine id (#14: stamps are a hybrid logical clock)

> r[settings.shared.apart]
> Two machines changing different shared settings at once both keep their changes: once their
> writes cross, every machine has both new values while their groups are kept in step.
>
> - model: store

> r[settings.shared.same-value]
> Two machines changing one shared setting to the same value at once settle on that value, and on
> one write of it (`store.sync.settles`).
>
> - model: store

> r[settings.shared.away]
> A machine that was away while shared settings changed takes, at its next link, every change it
> missed, and the machines it links with take every change it made while away: each setting ends
> with its newest change on every machine.
>
> - model: store

> r[settings.shared.fresh]
> A machine whose store starts empty, on a first run or reinstalled (`store.file.empty`), writes
> none of its shared settings at its first link: while their groups are kept in step, it takes every
> shared setting the cluster has written, and changes no other machine's.
>
> - model: store

> r[settings.shared.paired-again]
> A machine forgotten and then paired again takes, at its next link, every shared setting changed
> while it was forgotten, as any machine that was away does.
>
> - model: store

> r[settings.shared.schema]
> A write of a shared setting's entry whose value is not one that setting can hold is ignored as
> `store.entry.schema` says, and the other writes of that change are taken. The values each shared
> setting can hold are these:
>
> | Shared setting | Its values |
> |---|---|
> | the resistance | a whole number from 0 to 500 (`crossing.push.resistance`) |
> | blocking crossings while dragging, the shortcut on or off, showing where the pointer lands | on or off |
> | the shortcut key | a key that `crossing.shortcut.key-allowed` allows and `crossing.shortcut.key-typing` does not refuse |
> | the shortcut's style | double-tap or long press |
> | the double-tap window | a whole number of milliseconds from 50 to 2000 (`crossing.shortcut.window-range`) |
> | the shortcut's destination | where input last went, the next machine, or next among ticked |
> | the crossing style | `settings.design.style`'s |
> | the colour | `settings.design.colour`'s |
> | the Length | a number from 0.25 to 2.5 |
> | the Size | a number from 0.02 to 0.25, a fraction of a display's shorter edge |
> | the frame rate | Match display, Half or Quarter |
> | the shortcut-and-menu style | Same as crossing, Ring, or a crossing style other than Glow and Beam |
> | a machine's name | `settings.names.entry`'s |
>
> - model: store

> r[settings.shared.key-clash]
> A write of the shortcut key whose key clashes with one of this machine's jump keys
> (`crossing.shortcut.shared-key-clash`) is refused here as `store.entry.refused` says: this
> machine keeps its shortcut key and its jump key, does not send the write on, and takes the other
> writes of that change. Machines that each refuse it for a jump key of their own pass it no
> further, so nothing goes round between them.
>
> - model: store

> r[settings.shared.one-length]
> The Length is one value and the Size is one value. A change of either replaces the whole value, so
> the newest change of each is the one every machine has while Design is kept in step.
>
> - model: store
> - parity: Python kept a preset (Short, Normal, Long; Small, Medium, Large) beside each ruler's value, and sent both, for peers of 1.5.0 to 1.5.2 that knew only presets (#11 and #14: no compatibility with the Python apps)

## Names

> r[settings.names.entry]
> The name of each machine is a shared entry of the Names group, keyed by that machine's id, so it
> reads as unset while that machine is forgotten (`store.forget.entry`). Its value is a name of at
> most 64 characters with no leading or trailing blanks; a name written with them has them taken off
> first, so "  Desk  " is "Desk", and a write of a longer name is ignored (`settings.shared.schema`).
>
> - model: store

> r[settings.names.written]
> Renaming this machine writes this machine's name entry, and giving a peer a name here
> (`viewmodel.machines.rename.save`) writes that peer's, with the name as given, its leading and
> trailing blanks taken off and cut to its first 64 characters, whether Names is kept in step or not.
>
> - model: store

> r[settings.names.applied]
> While Names is kept in step, a newer write of a machine's name entry taken here becomes this
> machine's own name when the entry is this machine's, and that peer's given name here when it is a
> peer's. A name entry of a machine that is neither changes nothing here.
>
> - model: store

> r[settings.names.turned-on]
> When Names becomes kept in step, each machine's name whose entry has been written takes the
> entry's value, as `settings.shared.turned-on` says for every group, and a name whose entry was
> never written stays as it is.
>
> - model: store

> r[settings.names.not-kept]
> While Names is not kept in step, renaming a machine changes its name on this machine only: no
> other machine's name for it changes, even while another group is kept in step.
>
> - model: store

## Revert

> r[settings.revert.kept]
> When taking writes from a peer changes the value of any of this machine's shared settings, or
> changes which groups are kept in step here, this machine keeps what was replaced: the groups kept
> in step just before, and the values just before of every setting in the groups that change
> applied to. Each such change replaces what was kept before. A change that replaces nothing, such
> as a write refused here (`settings.shared.key-clash`), keeps nothing and leaves what was kept as it
> was.
>
> - model: store

> r[settings.revert.thirty-days]
> What was kept stays available for 30 days after it was kept, the 30th day's last second included,
> across restarts, and is then gone: a Revert asked for after that changes nothing.

> r[settings.revert.values]
> Revert puts back the kept values as changes made on this machine (`settings.shared.entry`), so
> while their groups are kept in step every machine takes them, and leaves which groups are kept in
> step as they are now.
>
> - model: store

> r[settings.revert.groups-only]
> When the change that was kept changed only which groups are kept in step, and no value, Revert puts
> those groups back as they were, and changes no value, so a value changed since stays as it is.
>
> - model: store

> r[settings.revert.names]
> Revert puts back the names it kept as renames made on this machine (`settings.names.written`),
> this machine's own name included, so they reach every machine while Names is kept in step. A
> peer that had no given name here before gets none again, and its blank name is written as the
> entry's value.
>
> - model: store

> r[settings.revert.key-clash]
> Revert keeps the current shortcut key when the kept one is now in one of this machine's jump keys'
> chords (`crossing.shortcut.key-jump-clash`), and puts back every other kept value.
>
> - model: store

> r[settings.revert.once]
> Once Revert has put back what was kept, nothing is kept until a peer's change replaces values
> again.

## Design follow

A machine may follow a peer's design: it takes the values of that peer's Design group as its own,
and each change to them after. Which peer this machine follows is its own setting
(`settings.own.never-shared`).

> r[settings.follow.record]
> Each machine's record (`store.record.version`) has a part holding its design: the values of its
> Design group's seven settings as they are on that machine, and the machines its design came
> through, the machine it was first chosen on first and that machine itself last. A machine that
> follows no peer lists itself alone. The part changes whenever this machine's design or the
> machines it came through change, whatever changed them and whether or not Design is kept in
> step, and only then (`store.record.unchanged`).
>
> - model: store

> r[settings.follow.choose]
> Following a peer makes this machine's Design group's settings take the values of the peer's design
> (`settings.follow.record`) at once, all seven, whichever was changed more recently. A peer whose
> design is not in this machine's store yet cannot be followed: asking to follow it changes nothing.
>
> - model: store

> r[settings.follow.changes]
> While this machine follows a peer and Design is not kept in step, each change of that peer's
> design taken here (`store.record.version`) becomes this machine's design, all seven values, and
> this machine's record shows it. A record of the peer that is not newer, or that leaves this
> machine's design as it is, changes nothing of this machine's record, and nothing goes to any peer
> (`store.record.unchanged`).
>
> - model: store

> r[settings.follow.not-written]
> Values taken by following are not changes made on this machine: they write none of the Design
> group's shared entries (`settings.shared.entry`).
>
> - model: store
> - parity: Python stamped each value it took from the followed machine's design with its own design stamp (#14: a machine's design is a part of its own record, apart from the shared entries)

> r[settings.follow.chain]
> A machine that follows a peer which itself follows another machine has the other machine's
> design: in A ← B ← C, each following the machine to its left, C has A's design, and its record
> lists A, B and C. A change of A's design reaches C through B.
>
> - model: store

> r[settings.follow.cycle]
> When the design of the peer this machine follows lists this machine among the machines it came
> through, this machine stops following and keeps its design as it is. So machines that follow each
> other in a ring, two or more, end with at least one of them following no peer, and store messages
> about their designs stop (`store.sync.settles`).
>
> - model: store
> - parity: Python broke a ring only when a design arrived carrying the machines it came through, and two machines that each followed the other before either's design arrived settled on the design with the newer stamp, both still following (#14: a machine's design is a part of its own record, which has a version but no stamp to compare across machines)

> r[settings.follow.ignore-others]
> While this machine follows a peer, a change of any other peer's design changes nothing of this
> machine's design.
>
> - model: store

> r[settings.follow.away]
> While the followed peer has no link, this machine keeps its last design as taken
> (`store.record.away`) and goes on following it: a change the peer made while away becomes this
> machine's design when its record arrives.
>
> - model: store

> r[settings.follow.paused]
> While Design is kept in step, following is paused: the Design group's settings are the shared
> entries' (`settings.shared.applied`), and this machine goes on following the same peer. When
> Design stops being kept in step, chosen here or on another machine, this machine's design becomes
> the followed peer's design as this machine has it then.
>
> - model: store

> r[settings.follow.locked]
> While this machine follows a peer and Design is not kept in step, a change of any of the Design
> group's seven settings asked of Core is refused and changes nothing, and this machine goes on
> following.
>
> - model: store

> r[settings.follow.stop]
> Stopping following keeps this machine's design as it is, and the Design group's settings can be
> changed again.
>
> - model: store

> r[settings.follow.removed]
> Removing the followed peer (`pairing.remove.peer`) makes this machine follow no peer, keeping its
> design as it is.
>
> - model: store

## Design values

> r[settings.design.defaults]
> Until the user changes them, the crossing style is Glow, the colour is Colourful, the Length is 1,
> the Size is 0.045, the frame rate is Match display, showing where the pointer lands is on, and the
> shortcut-and-menu style is Same as crossing.

> r[settings.design.style]
> The crossing style is Glow, Beam, or one of the crossing effects' styles (F30): `aperture`,
> `skin`, `film`, `rupture`, `flint`, `filings`, `discharge`, `rule`, `gauge`, `rangefinder`,
> `crease`, `pleat`, `concertina`, `thread`, `weave` and `jacquard`. A crossing effect's style is
> kept as it is chosen, though this build draws it as Glow (`ui.glow.unknown-style`).

> r[settings.design.colour]
> The colour is one of Signal, Colourful, Ocean, Sunset, Aurora, White, Silver, Graphite, Ink and
> Warm grey, or one of the crossing effects' colour packs (F30): `oil_slick`, `soap_bubble`,
> `pearl`, `opal`, `sea_glass`, `ember`, `forge`, `sparkler_gold`, `arc`, `magnesium`,
> `phosphor`, `sodium`, `contrast`, `ruby`, `cobalt`, `vellum`, `carbon_copy`, `marbled`, `moss`,
> `plum`, `flax`, `madder`, `tide`, `woad`, `saffron`, `citron`, `laser`, `electric`, `fuchsia` and
> `ultraviolet`. A colour pack is kept as it is chosen, though this build draws it as Signal
> (`ui.palette.unknown`).

## This machine's own settings

> r[settings.own.never-shared]
> Every setting that is not a shared setting (`settings.groups.four`) is this machine's own: no
> group carries it, no write from a peer changes it, and Same on all machines sends it to no peer.
> Among them: whether crossings are animated on this machine, the window's appearance, the
> full-screen pause, the Mac's notch and trackpad settings, the modifier style and the rest of the
> Keyboard page, Show in Dock, Show in menu bar and Show in tray, start at login, admin mode, hiding
> addresses, the settings window's last frame, and which peer's design this machine follows.
>
> - model: store

> r[settings.own.not-shared-crossing]
> Zones, the edge a peer faces, own zones, jump keys, a peer kept off the crossing map and the drive
> permissions are never carried by Same on all machines, whichever groups are kept in step: they are
> `spec/crossing.md`'s and `spec/pairing.md`'s.
>
> - model: store

> r[settings.own.file]
> This machine's own settings are kept in `local.json` (`runtime.files.folder`), with this
> machine's own value of each shared setting, what Revert keeps (`settings.revert.kept`) and the
> peer it follows. Each keeps its value across restarts.

> r[settings.own.default]
> A setting that `local.json` does not hold, as on a first run or in a file a hand left it out of,
> reads as its default, and the settings the file holds read as saved.

> r[settings.own.refused]
> A `local.json` that is not one JSON object, or in which any setting's value is not one that
> setting can hold, such as a word where on or off goes or a number out of its range, is refused
> whole as `runtime.files.refused-defaults` and `runtime.files.refused-kept` say: every own setting
> reads as its default, and the file stays as it is.
>
> - fuzz: store-file
> - parity: Python read a window appearance it did not know as System and loaded the rest of the file, and refused the whole file for every other value it could not hold (one rule for every own setting, as `store.file.refused` has for the store)

> r[settings.own.unknown-kept]
> A field of `local.json` that this build does not know, such as one a later Beamer wrote, is
> ignored, and is written back as it was at each later write of the file.

> r[settings.own.appearance]
> The window's appearance is System, Light or Dark, System until the user chooses another.

> r[settings.own.animate]
> Animating crossings on this machine is on until the user turns it off.
