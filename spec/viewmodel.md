# Viewmodel

What every screen, overlay, tray menu and notification of Beamer shows, enables and validates, and
what each of the user's actions on them asks of `Core`. It covers the settings window's pages
(Overview, Machines and the pairing sheet, Crossing with its map and ledger, Profiles, Keyboard,
Audio & Camera, Design, Permissions (F38), Connection (F39)), the tray menu and the Mac's Dock and
menu bar (F27), notifications (F36), and every user-visible string through `tr()` (F34).

`beamer-viewmodel` is pure (#16, ADR 0004). `Core` and the platform crates hand it typed values,
never text. It turns each snapshot into what the screens show, and turns each action into a
command. **Every user-visible string is made here and only here, through `tr()`**, in English with
British spelling (#11). It is tested like a domain crate, so its rules are `ci`.

What stays elsewhere: the state these screens show is the other domains' rules, cited in prose
where a page shows it. Those domains are `spec/link.md`, `spec/pairing.md`, `spec/discovery.md`,
`spec/driving.md`, `spec/crossing.md`, `spec/input.md` and `spec/platform.md`, as well as the audio,
camera, display and settings specs. How a page is drawn is `spec/ui.md`'s: its pixels, geometry,
sizes, colours, fonts, contrast and motion, which Rune draws in `beamer-ui`.

Dropped, so no rule here (#11): phone peers, pair nearby (F3), the update check and its notice
(F22), the portable build (F23), Italian, the pseudo-locale and US spelling at run time (F34), the
arrangement diagram and the other uncalled functions, the 1.4.x migration (F19), and any
compatibility with the Python apps (#14), older Beamers included. Later (#11): other languages, for
which the room is `viewmodel.words.through-tr`; the crossing effects (F30) and the notch island
(F31), whose choices the Design page leaves out (`viewmodel.design.styles`).

Quoted strings are the exact English a page shows. "This Mac" in a rule is "This PC" on a PC, and
"Option" is "Alt", unless the rule says otherwise. `{name}` stands for a peer's label
(`viewmodel.machines.label.name`, `viewmodel.machines.label.shared`). Where the Mac and the PC
showed different words, a rule picks one for both machines and its `parity:` marker says which.

## Words

Every string in Beamer is the view-model's, made through one translation function, `tr()`. English
with British spelling is the only language (#11, F34); the one function is the room left for others.

> r[viewmodel.words.through-tr]
> Every string the view-model gives a page, overlay, tray menu, tooltip, accessibility label or
> notification passes through one translation function, `tr()`, apart from the values put into a
> template (`viewmodel.words.template-then-values`) and the product's name, "Beamer". A test that
> swaps that function's catalogue for one that marks every message finds every other string marked.

> r[viewmodel.words.british]
> Every string is English with British spelling, as the view-model writes it ("colour", "centre",
> "behaviour", "minimise", "cancelled"), whatever the OS's region and preferred languages.
>
> - parity: Python showed Italian when the OS preferred it, the pseudo-locale when asked, and US spelling ("color", "center") in a US region (#11, F34: English only, with British spelling; Italian, the pseudo-locale and US spelling at run time are dropped)

> r[viewmodel.words.template-then-values]
> A string that carries values is translated as one template, with a named field for each value,
> and the values go in after: a machine's name, an address, a number, a key's name or an error's
> text is never itself passed through `tr()`, and `tr()` is given the same template whatever the
> values.

> r[viewmodel.words.translated-once]
> Each piece of text passes through `tr()` once: a value that is itself translated text, such as a
> page's name in "{name}, needs attention", goes into its template as it is.

> r[viewmodel.words.typed-as-typed]
> A name the user typed, such as a peer's or a profile's, shows exactly as typed wherever it
> appears, alone or inside a template: a peer named "Colourful" is "Colourful" everywhere.

> r[viewmodel.words.joined]
> Names listed in a string read "Mac" for one, "Mac and PC" for two, and "Mac, PC and Laptop" for
> three or more: ", " between all but the last two, " and " before the last. No names make the empty
> string.

> r[viewmodel.words.count]
> A string with a count in it takes its singular form for a count of 1 and its plural form for any
> other count, 0 included: "1 machine", "0 machines", "3 machines".

> r[viewmodel.words.peer-named]
> A string about a peer names it by its label, the name the view-model gives it (two peers of one
> name told apart as `viewmodel.machines` says), never by its OS: no string calls a peer "Windows",
> "the PC" or "the Mac" in place of its label.

> r[viewmodel.words.unknown-machine]
> A string about a machine that `Core` gives no machine id for, so that the view-model has no label
> for it (`viewmodel.machines.label.unknown` labels any machine it has an id for), calls it
> "another machine", and "Another machine" at the start of a sentence. No string calls any machine
> "the other machine".
>
> - parity: Python called such a machine "the other machine" in the Mac's link words and "The other machine" in the Windows app's statuses and notifications, mid-sentence too (#11's standing decision, N machines: no string pictures exactly one other machine)

## The settings window's pages

What the settings window's sidebar lists, which page it opens on, which pages carry a dot, the
footer, and how every page follows the snapshot Core gives. A Mac is a machine running macOS and a
PC one running Windows.

> r[viewmodel.pages.order]
> The settings window's sidebar lists its pages in this order. On a Mac: Overview, Permissions,
> Crossing, Audio & Camera, Keyboard, Design, Connection. On a PC: Overview, Crossing, Audio & Camera,
> Keyboard, Design, Connection.

> r[viewmodel.pages.purpose]
> Each page shows its name and, under it, one sentence saying what the page is for:
>
> | Page | On a Mac | On a PC |
> |---|---|---|
> | Overview | "Where input is, whether each link is up, and the controls you reach for every day. Pair your machines here first." | "The machines paired with this PC, where input is right now, and the controls you reach for every day. Pair a machine here first." |
> | Permissions | "macOS must allow Beamer to read this Mac's input and to move its pointer and type when another machine drives it." | no such page |
> | Crossing | "Drag a machine on the map to where it sits. The ledger says how each one is reached; click a row to pick it on the map." | "Choose how the pointer or a key moves input to each machine, and how hard the edge pushes back first." |
> | Audio & Camera | "Every machine's microphones, speakers and camera. Choose the microphone your apps hear, the camera they see, and where each machine's sound plays." | the same |
> | Keyboard | "How this Mac's modifier keys arrive on other machines, which keys and buttons stay on this Mac, and how fast another machine's pointer moves here. The switch key is on the Crossing page." | "How this PC's Ctrl and Windows keys arrive on another machine, the keys and buttons that stay here, and how fast another machine's pointer moves here." |
> | Design | "How crossing looks and feels on this Mac: the edge, the corner, the notch and the trackpad." | "How crossing looks on this PC: the light as you push toward another machine, and where the pointer lands." |
> | Connection | "This Mac's address; hide addresses in this window and its menu." | "This PC's network access and address; hide addresses in this window and tray." |
>
> - parity: Python's Mac Connection page was "Local connection details for the first machine you paired. Pair again to change another machine's entry.", and the PC's named "listening port" (#14: no machine has a port of its own, `link.transport.port`, and a peer's chosen addresses are set for each peer, `discovery.address.choose`)

> r[viewmodel.pages.first-open]
> The first time the settings window opens in a run of Beamer, it shows Overview, except on a Mac
> while Accessibility or Input Monitoring is not granted (`platform.permissions.mac-input`), when it
> shows Permissions.

> r[viewmodel.pages.reopen]
> Each later opening of the settings window in the same run shows the page last shown, whatever the
> permissions are by then.

> r[viewmodel.pages.number-keys]
> On a Mac, Command-1 to Command-7 show the pages in sidebar order; on a PC, Ctrl+1 to Ctrl+6 do.

> r[viewmodel.pages.arrow-keys]
> With the sidebar focused, the up and left arrow keys show the page above the one shown, and the
> down and right arrow keys the page below it. At the first page the up and left arrows stay there,
> and at the last page the down and right arrows stay there.
>
> - parity: Python's PC went round from the last page to the first and back (one behaviour for both machines, the Mac's)

> r[viewmodel.pages.dot-permissions]
> On a Mac, while Accessibility or Input Monitoring is not granted, the sidebar puts an amber dot on
> Permissions.

> r[viewmodel.pages.dot-dormant]
> While any peer is a dormant peer, the sidebar puts an amber dot on Overview and on Crossing.
>
> - parity: Python's Mac also put a fault dot on Overview while the link reported that the peer refused this Mac's token (that refusal is what makes a peer dormant, `link.report.down`, so the dormant peer's amber dot stands for it)

> r[viewmodel.pages.dot-firewall]
> On a PC, the sidebar's dot on Connection follows the firewall check (`platform.firewall.*`): none
> while the firewall is ready or a check is running; an amber dot while a block rule blocks, Beamer's
> rule is missing or the LAN is on the Public profile; a fault dot while the check failed or was
> refused for lack of permission.

> r[viewmodel.pages.dot-settings]
> On a PC, while Core reports that this machine's settings could not be read, the sidebar puts a
> fault dot on Connection, whatever the firewall check says.

> r[viewmodel.pages.no-dots]
> With none of the cases of `viewmodel.pages.dot-permissions`, `viewmodel.pages.dot-dormant`,
> `viewmodel.pages.dot-firewall` and `viewmodel.pages.dot-settings`, no page carries a dot.

> r[viewmodel.pages.footer]
> The settings window's footer says "Changes apply as you make them. Closing this window keeps Beamer
> running in the menu bar." on a Mac and "Changes apply as you make them. Closing this window keeps
> Beamer running in the tray." on a PC, on every page but Connection, where it says only "Closing this
> window keeps Beamer running in the menu bar." on a Mac and "Closing this window keeps Beamer running
> in the tray." on a PC.

> r[viewmodel.pages.follows-snapshot]
> Every page of an open settings window shows the settings and states of the latest snapshot Core
> gave: a setting changed on this machine, by a peer through a shared entry, or by reloading the
> settings shows on the page shown without the window being closed and opened again.

> r[viewmodel.pages.opened-current]
> When the settings window is opened, or restored from the Dock or taskbar, everything it shows comes
> from the latest snapshot Core gave, from the first moment it is seen: no page shows a state from
> before that snapshot, even one that changed while the window was closed or minimised.

## Link status

The window's link status, the sidebar's link block and the round trip shown while input is on a
peer. Each is the view-model's reading of what the link reports (`spec/link.md`).

> r[viewmodel.status.peer]
> The link status speaks of one peer, the status peer: the peer this machine's input is on; else
> the shortcut's machine (`crossing.shortcut.machine`); else the first peer in the peer list that is
> in use and that this machine may drive. A peer that is not in use is never the status peer.
>
> - parity: Python's Mac fell back to the first peer it paired with, in use or not (one view-model for every platform: the Windows app's, which never names a peer not in use)

> r[viewmodel.status.parts]
> The link status shows a word, a caption beside its light, a sentence, the word's tone and the
> light's tone, each tone one of signal, amber, fault and ink. The light blinks in the waking and
> looking states only.

> r[viewmodel.status.order]
> The link status is the first of these that holds: this machine is driven
> (`viewmodel.status.driven`); the status peer is being woken (`viewmodel.status.waking`); the
> status peer's link is up (`viewmodel.status.on-peer`, `viewmodel.status.paused`,
> `viewmodel.status.full-screen`, `viewmodel.status.linked`, in that order); no peer is paired
> (`viewmodel.status.not-paired`); no peer is in use (`viewmodel.status.none-in-use`); the status
> peer's link has reported nothing yet (`viewmodel.status.looking`); and the reason the status
> peer's link last reported down (`link.report.down`). A reason reported down earlier is never
> shown while the link is up.
>
> - parity: Python's Windows app showed its receiver's state instead, "Waiting for a machine" ("Waiting on port {port}"), "Machine connected", "Receiver needs attention" or "Receiver stopped", with the words "Waiting", "Connected", "Error" and "Stopped" (#14: one link per pair, and no receiver apart from it; one view-model for every platform, the Mac app's states)

> r[viewmodel.status.driven]
> While a peer drives this machine, the link status's word is "From {name}" naming the driver, its
> caption "Live", its sentence "{name}'s keyboard and pointer are on this Mac. Push its pointer back
> through the edge it arrived by to send them home." ("this PC" on a PC), and both tones signal.

> r[viewmodel.status.waking]
> While the status peer is being woken (`discovery.wake.status`), the word and the caption are
> "Waking", the sentence "Waking {name}. It connects on its own once it is up.", both tones amber,
> and the light blinks.

> r[viewmodel.status.on-peer]
> While this machine's input is on the status peer, the word is "On {name}", the caption "Live", the
> sentence "Keyboard and pointer are on {name}. Do the same again to bring them home." and both
> tones signal.

> r[viewmodel.status.paused]
> While the status peer's link is up, input is home, a crossing pause is on (`crossing.pause.zones`)
> and this machine has a zone, the word is "Paused", the caption "Held", the sentence "Connected to
> {name}. Crossing is held, so you can lean on an edge.", the word's tone ink and the light's
> signal.

> r[viewmodel.status.full-screen]
> While the status peer's link is up, input is home, no crossing pause is on, a full-screen pause
> is (`crossing.full-screen.zones`) and this machine has a zone, the word and the caption are "Held",
> the sentence "Held: an app is full screen.", the word's tone ink and the light's signal.

> r[viewmodel.status.linked]
> While the status peer's link is up, input is home, and neither `viewmodel.status.paused` nor
> `viewmodel.status.full-screen` holds, the word is "On this Mac" ("On this PC" on a PC), the caption "Linked", the sentence "Connected to {name}.", the word's tone ink
> and the light's signal.
>
> - parity: Python's Mac said "On Mac" (the word names this machine as the Windows app's "On this PC" did)

> r[viewmodel.status.not-paired]
> With no peer paired, the word is "Not paired", the caption "Setup", the sentence "Pair a machine on
> Overview and Beamer connects on its own." and both tones amber.

> r[viewmodel.status.none-in-use]
> With peers paired but none in use that this machine may drive, the word is "Not connected", the
> caption "Setup", the sentence "Put a machine in use on Overview and Beamer connects on its own." and
> both tones amber.
>
> - parity: Python's Mac named its first peer even when it was not in use, and its Windows app said only "Not connected" (#11's standing decision, N machines: a peer not in use is never the status peer)

> r[viewmodel.status.looking]
> While the status peer's link has reported nothing since Beamer started or since the peer was put
> in use, the word is "Waiting", the caption "Looking", the sentence "Looking for {name}. It connects on
> its own once Beamer is open there.", both tones amber, and the light blinks.

> r[viewmodel.status.unreachable]
> When the status peer's link last reported down as unreachable, the word is "Unreachable", the caption
> "No reply", the sentence "{name} is not answering. It may be asleep or off this network." and
> both tones fault.
>
> - parity: Python's sentence began with the address and port it dialled ("192.0.2.20 port 24820 is not answering."), and it told apart a machine that answered with no Beamer listening ("Not listening", "{name} answers, but Beamer is not listening on port {port}. Open Beamer on {name}.") (#14: a QUIC dial tells no such answer apart, and `link.report.down` names no address)

> r[viewmodel.status.not-the-peer]
> When the status peer's link last reported down because another machine answered at the peer's
> address (`link.auth.not-the-peer`), the word is "Wrong machine", the caption "Refused", the sentence
> "Another machine answered at {name}'s address. If {name} was set up again, remove it, then pair
> again." and both tones fault.
>
> - parity: Python called this, and an unreadable first message, a token mismatch: "Token mismatch", "{name} refused this Mac's token. Remove it, then pair again to write a fresh one." (ADR 0003: links authenticate by pinned keys, and there is no token)

> r[viewmodel.status.forgotten]
> When the status peer's link last reported down because the peer no longer has this pairing
> (`link.auth.forgotten`), the word is "Pairing removed", the caption "Refused", the sentence "{name} no
> longer has this pairing: remove {name} here and pair the two again." and both tones fault.

> r[viewmodel.status.not-in-use]
> When the status peer's link last reported down because the peer does not use this machine now
> (`link.auth.not-in-use`), the word is "Not in use", the caption "Refused", the sentence "{name} does
> not use this Mac now. Put this Mac in use again on {name}." ("this PC" on a PC) and both tones
> fault.
>
> - parity: Python's peer answered as if the first message were unreadable, which the Mac showed as a token mismatch (`link.auth.not-in-use`: a refusal that names its cause)

> r[viewmodel.status.version]
> When the status peer's link last reported down because the two machines run different versions
> (`link.version.mismatch`), the word is "Version mismatch", the caption "Refused", the sentence "{name}
> runs a different Beamer from this Mac. Update both to the same version." ("this PC" on a PC) and
> both tones fault.
>
> - parity: Python's Windows app said which machine was older: "Update Beamer on {name}: it is older than this one" or "{name} runs a newer Beamer: update Beamer on this machine" (`link.version.mismatch`: the version tells no order)

> r[viewmodel.status.did-not-wake]
> When a wake of the status peer ended with no link (`discovery.wake.waiting`), the word is "Did not
> wake", the caption "No reply", the sentence "{name} did not answer within a minute of the wake-up
> packet." and both tones fault, until its link next reports.

> r[viewmodel.status.no-reply]
> When the status peer's link last reported down because the peer stopped answering, stopped taking
> what this machine sends, stopped acknowledging input or closed the link, or because the driver
> closed it for having too much waiting to send (`driving.home.send-refused`), the word is "No
> reply", the caption "Dropped", the sentence "{name} stopped responding. Beamer keeps trying to
> reconnect." and both tones fault.

> r[viewmodel.status.disconnected]
> When the status peer's link last reported down because of a message that does not decode or a
> peer that broke the protocol, the word is "Disconnected", the caption "Dropped", the sentence "{name}
> did not answer like Beamer. Beamer keeps trying to reconnect." and both tones fault; because the
> key this machine holds for the peer changed (`link.peer.rekeyed`), the same with the sentence "The
> pairing with {name} changed. Beamer keeps trying to reconnect."

> r[viewmodel.status.replaced]
> A link that reports down because a new link with the same peer replaced it changes nothing the
> link status shows: no down state shows between the two links, and the status follows the new one.

### The sidebar's link block

> r[viewmodel.status.sidebar-peer]
> The sidebar's link block names one peer: the peer this machine's input is on, else this machine's
> driver, when that peer's link is up; else the status peer, when its link is up; else, of the
> peers whose link is up, the one with the smallest machine id. With no peer's link up it names the
> status peer, and with no peer paired it shows "—" in place of a name.

> r[viewmodel.status.sidebar-word]
> The sidebar's link block shows the link status's word in the word's tone, and its light in the
> light's tone, blinking as the link status's light does.

> r[viewmodel.status.response-time]
> While the sidebar's peer has a link up, the sidebar shows "Response time: {n} ms", n being the
> QUIC's RTT that link last reported (`link.rtt.reported`) in whole milliseconds, or
> "Response time: —" before the link has reported one. While no peer's link is up, it shows none.
>
> - parity: Python showed the time its own probe took to the peer and back, each reading kept 3 s, and the Windows app said "Response time to {name}" (#14: QUIC's RTT replaces the probes; one view-model for every platform, the Mac app's words)

> r[viewmodel.status.response-compact]
> While the sidebar is collapsed, its link block shows QUIC's RTT as "{n} ms" alone, or "—" before
> one is reported, and nothing while no peer's link is up.

> r[viewmodel.status.spoken]
> The sidebar's link block's accessibility label and tooltip are its word, its peer's name and its
> response time joined by ", ", leaving out what it does not show: "On this Mac, Studio,
> Response time: 24 ms". A response time not yet reported is spoken "Response time: no fresh
> measurement".

### Round trip

> r[viewmodel.status.round-trip]
> While this machine's input is on a peer and the link to it gives a round trip
> (`link.input.round-trip-recent`), the window's link status shows it as "{n} ms", rounded to the
> nearest whole millisecond. While input is home, or the link gives none, it shows "—".
>
> - parity: Python's Windows app hid the round trip's row instead of showing "—" (one view-model for every platform: the Mac app's)

## Machines

The Machines list is the section of the Overview page that lists this machine's peers, one row
each, with each peer's state, its switches, and the ways to rename, re-pair or remove it. Where a
rule gives a word for a Mac and one for a PC, a Mac shows the first and a PC the second. Showing an
address hidden, while the user hides addresses, is the Connection page's setting and follows its
rules wherever an address appears here.

### What a machine is called

Every page, status, notice and menu names a machine by its label, as these rules give it. A
machine's id tail is the last four characters of its machine id written in unpadded URL-safe
base64 (RFC 4648, section 5), the 16 bytes of the id making 22 characters.

> r[viewmodel.machines.label.name]
> A peer's base name is the label the user gave it, without leading or trailing blanks, or, with no
> label or a blank one, the peer's name, without leading or trailing blanks. A peer's label is its
> base name, unless `viewmodel.machines.label.shared` or `viewmodel.machines.label.no-name` says
> otherwise: a peer named "Desk" with the label "  Work Mac  " is "Work Mac", and with the label
> "  " is "Desk".

> r[viewmodel.machines.label.cut]
> A base name longer than 64 characters, once its leading and trailing blanks are gone, is cut to
> its first 64: a label of 70 "X" between blanks is shown as 64 "X".

> r[viewmodel.machines.label.no-name]
> A peer whose base name is blank is labelled "Machine · " followed by its id tail, such as
> "Machine · BBBw". Its label never shows any of its addresses.
>
> - parity: Python called such a peer by its saved address, and with none "Unnamed machine", while its Audio page and crossing map called a machine they had no name for "Machine · " and the last six characters of its id (one name for a nameless machine on every page, with the id tail `viewmodel.machines.label.shared` uses)

> r[viewmodel.machines.label.shared]
> When two or more peers share a base name, each of their labels is the base name followed by a
> space and its id tail between parentheses: two peers labelled "Desk" are "Desk (AAAA)" and
> "Desk (BBBw)", while a third labelled "Laptop" stays "Laptop". A blank base name is shared with
> no one (`viewmodel.machines.label.no-name`).

> r[viewmodel.machines.label.shared-compare]
> Two base names are shared when they are equal once letter case is ignored: "Desk" and " desk "
> are shared, and "Studio" and "studio" are shown as "Studio (…)" and "studio (…)", each keeping
> its own case. Only this machine's peers are compared, never a machine that is not a peer.

> r[viewmodel.machines.label.everywhere]
> Every page, status, notice and menu that names a peer, such as the peer driving this machine or
> the peer this machine's input is on, names it by its label as the Machines list shows it, the
> id tail of `viewmodel.machines.label.shared` included.

> r[viewmodel.machines.label.unknown]
> A machine that is not one of this machine's peers, such as a driver just removed from the peer
> list or a machine on the crossing map that only a peer has paired with, is labelled by the name
> Core reports for it, without leading or trailing blanks and cut to its first 64 characters, or,
> with no name or a blank one, "Machine · " followed by its id tail. Where a string names no
> machine at all, because Core gives no machine id, `viewmodel.words.unknown-machine` says what it
> says.
>
> - parity: Python's Mac called a machine no longer in its peer list by the name its link last gave, or else "the other machine", and its Audio page and crossing map called one they had no name for "Machine · " and the last six characters of its id (one name for a nameless machine on every page, with the id tail `viewmodel.machines.label.shared` uses)

### The list

> r[viewmodel.machines.list.title]
> The Overview page has a section titled "Machines", which lists this machine's peers.

> r[viewmodel.machines.list.rows]
> The Machines list has one row for each peer, in the peer list's order (`pairing.peers.order`),
> each headed by the peer's label: a dormant peer, a peer not in use and a peer kept off the
> crossing map each keep their row. A peer removed leaves the list, and a peer paired joins it.

> r[viewmodel.machines.list.empty]
> With no peer, the Machines list has no row and says "No machines paired yet." Whether the
> pairing sheet then shows is `viewmodel.pairing-sheet.open-without-peers`.

### What a row says

> r[viewmodel.machines.row.platform]
> A row names the peer's platform as the pairing proved it: `macos` as "Mac", `windows` as
> "Windows", `linux` as "Linux", and any other platform, or none, as "Other".
>
> - parity: Python also named `ios` "iPhone" and `android` "Android" (phone peers are out of scope, #11)

> r[viewmodel.machines.row.address]
> A peer's address, as the Machines list shows it, is its first chosen address, or with none, the
> learnt address at which a link with it last authenticated (`discovery.address.list`), written
> without a port. A peer with neither has none.
>
> - parity: Python showed the one address it kept for the peer, followed by " · port " and its link port (#14: a list of chosen and learnt addresses per peer, and one UDP port for every machine, so no port is shown)

> r[viewmodel.machines.row.where]
> Under its label, a row says the peer's platform and its address, joined by " · ", such as
> "Mac · 192.0.2.10", or "Mac · No address" when the peer has no address.
>
> - parity: Python's Mac app joined the platform and the address with two spaces, and both apps added " · port 24820" (#16: one viewmodel for every machine, with the Windows app's separator; #14: no port)

### A peer's state

Each row shows one state for its peer: a word, and a sentence under it. Each state is shown as
live, waiting or a fault; how each looks is `spec/ui.md`'s.

> r[viewmodel.machines.state.order]
> A row shows the first of these states that holds for its peer: dormant; not in use; driving this
> machine; input here; linked; waking; and then, by what the peer's link last reported, looking,
> refused, version, unreachable or dropped. A peer that is driving this machine while its link is
> up shows driving this machine, not linked.

> r[viewmodel.machines.state.dormant]
> A dormant peer (`pairing.dormant.forgotten`) shows "Needs re-pair" and the sentence "The other
> machine no longer has this pairing.", as waiting, whatever its link reports.
>
> - parity: Python also said "The saved pairing now identifies a different machine." for a peer whose link answered under another machine id, and "This pairing needs to be set up again." for any other reason (ADR 0003: a machine id is derived from the pinned key, so a peer is dormant for one reason only)

> r[viewmodel.machines.state.not-in-use]
> A peer that is not in use shows "NOT IN USE" and the sentence "{label} is not in use on this
> Mac." on a Mac or "{label} is not in use on this PC." on a PC, as waiting.

> r[viewmodel.machines.state.driving]
> A peer driving this machine shows "Driving this Mac" or "Driving this PC", and the sentence
> "{label}'s keyboard and pointer are on this Mac. Push the pointer back through the edge it
> arrived by to send them home.", with "this PC" for "this Mac" on a PC, as live.

> r[viewmodel.machines.state.input-here]
> The peer this machine's input is on shows "Input here" and the sentence "This Mac's keyboard and
> pointer are on {label}. Do the same again to bring them home.", with "This PC's" for "This
> Mac's" on a PC, as live.

> r[viewmodel.machines.state.linked]
> A peer whose link is up (`link.report.up`) shows "Linked" and the sentence "Connected to
> {label}.", as live, whichever of the two machines dialled it.

> r[viewmodel.machines.state.waking]
> A peer being woken (`discovery.wake.status`) shows "Waiting" and the sentence "Waking {label}…",
> as waiting.
>
> - parity: Python's Mac app showed a peer being woken as it showed any peer whose link was down (#16: one viewmodel for every machine; the Windows app's)

> r[viewmodel.machines.state.looking]
> A peer whose link has reported nothing yet, or last reported down because it was replaced, this
> machine quit, this machine forgot the peer or put it out of use, or the peer's pinned key changed
> (`link.report.down`), shows "Waiting" and the sentence "Looking for {label}. It connects on its
> own once Beamer is open there.", as waiting.
>
> - parity: Python showed "Waiting for {label} to connect." for a peer this machine did not dial because it could not drive it (`link.dial.every-peer`: every peer in use is dialled, whatever either may drive of the other)

> r[viewmodel.machines.state.refused]
> A peer whose link last reported down because another machine answered, the peer no longer has
> this pairing, or the peer does not use this machine now (`link.report.down`) shows "Refused", as
> a fault, with the sentence the status gives that reason.

> r[viewmodel.machines.state.version]
> A peer whose link last reported down because the two machines run different versions of Beamer
> (`link.version.mismatch`) shows "Version", as a fault, with the sentence the status gives that
> reason.
>
> - parity: Python also showed "Version" for a peer that had never linked and closed its first link, as a Beamer of version 5 did (#11 drops the v5 refusal path)

> r[viewmodel.machines.state.unreachable]
> A peer whose link last reported down because the peer is unreachable (`link.dial.deadline`)
> shows "Unreachable", as a fault, with the sentence the status gives that reason.
>
> - parity: Python showed "Not listening" for a dial the peer's machine refused, and the Mac app "Blocked" for its macOS 27 tunnel (#14: one QUIC handshake over UDP, in which a refused dial looks unreachable; #11 drops the tunnel, F42)

> r[viewmodel.machines.state.dropped]
> A peer whose link last reported down for any other reason (`link.report.down`), such as the peer
> stopped answering or closed it, shows "Dropped", as a fault, with the sentence the status gives
> that reason, less its final full stop, followed by ". Beamer keeps trying to reconnect.".

### Renaming a peer

> r[viewmodel.machines.rename.open]
> Each row has a "Rename" button. Pressing it puts in place of the label a field holding the
> peer's base name (`viewmodel.machines.rename.start`), with the note "Only on this Mac" on a Mac or
> "Only on this PC" on a PC, unless machine names are among the settings shared across the
> cluster, when there is no note.

> r[viewmodel.machines.rename.save]
> Pressing Return in the rename field asks Core to give the peer the field's text as its label,
> closes the field, and shows the new label (`viewmodel.machines.label.name`): "  Work Mac  " is
> saved and shown as "Work Mac".

> r[viewmodel.machines.rename.cancel]
> Pressing Escape in the rename field closes it and changes nothing. Opening it again shows the
> saved label, not the text that was discarded.

> r[viewmodel.machines.rename.start]
> Wherever Beamer lets the user rename a peer, on its row or on the Crossing page, the field starts
> with the peer's base name (`viewmodel.machines.label.name`), never with the id tail
> `viewmodel.machines.label.shared` adds: a peer shown as "Studio (BBBw)" because another peer
> shares its label "Studio" is renamed starting from "Studio", and saving it unchanged saves
> "Studio".

### Kept off the crossing map

> r[viewmodel.machines.kept-off.button]
> A row has a "Leave out" button while its peer is on the crossing map, and a "Put back" button
> while the peer is kept off it, named "Leave out {label}" and "Put back {label}" to assistive
> technology. Pressing "Leave out" asks Core to keep the peer off the crossing map
> (`crossing.derive.kept-off`), and pressing "Put back" asks Core to restore it.

> r[viewmodel.machines.kept-off.note]
> While its peer is kept off the crossing map, a row says "Left out of the layout. The shortcut and
> the menu still reach it.", and otherwise it does not.

### In use

> r[viewmodel.machines.in-use.switch]
> Each row has an "In use" switch, on while the peer is in use. Turning it off asks Core to put the
> peer out of use, and turning it on asks Core to put it in use again (`link.dial.not-in-use`,
> `link.dial.in-use-again`). On a Mac its tip says "Off: no input or settings link to {label}.
> Pairing, directions, ways and jump key are kept. Turn it back on here to reconnect.", and on a
> PC it starts "Off on this PC:" in place of "Off:".

### Pinning an address

> r[viewmodel.machines.pin.switch]
> Each row has a "Pin address" switch, on while the peer has a chosen address
> (`discovery.address.chosen-only`), and disabled while the peer has no address. Its tip says
> "Keeps this address through reconnects and re-pairing. Turn off to follow address changes
> automatically."
>
> - parity: Python pinned or released the one address it kept for each peer (#14: a peer has a list of chosen and learnt addresses)

> r[viewmodel.machines.pin.toggle]
> Turning "Pin address" off asks Core to remove each of the peer's chosen addresses, which stay
> learnt addresses (`discovery.address.unchosen`). Turning it on asks Core to make the address the
> row shows a chosen address (`discovery.address.choose-learnt`). Either changes that peer alone.

### May drive and may be driven by

> r[viewmodel.machines.directions.disclosure]
> Each row has a "Directions" button, closed at first, under which its may drive and may be driven
> by switches stay hidden. Pressing it shows them and makes it read "Hide directions"; pressing it
> again hides them. Each row's button opens its own row only.

> r[viewmodel.machines.directions.switches]
> Under "Directions", a row's switch "This Mac drives it" is on while this machine may drive the
> peer, and its switch "It drives this Mac" is on while the peer may drive this machine
> (`pairing.peers.permissions`). On a PC they read "This PC drives it" and "It drives this PC".

> r[viewmodel.machines.directions.one-peer]
> Turning one of a row's two switches asks Core to change that one permission of that one peer,
> to the switch's new value, and no other permission or peer.

> r[viewmodel.machines.directions.home-first]
> Turning off may drive, or in use, for the peer this machine's input is on, whether for that peer
> alone or for every peer at once, asks Core to bring the input home before it asks Core for the
> change. Turning may drive off for another peer, turning it on, or turning may be driven by off,
> leaves the input where it is.

### Removing a peer

> r[viewmodel.machines.remove.ask]
> Each row has a "Remove" button. Pressing it changes nothing yet: the row's other controls give
> way to the question "Remove {label}? It stays paired on that machine until you remove this one
> there too." and two buttons, "Remove" and "Keep".

> r[viewmodel.machines.remove.keep]
> Pressing "Keep" puts the row back as it was, and changes nothing.

> r[viewmodel.machines.remove.focus]
> Pressing a row's "Remove" gives the keyboard's focus to the question's "Keep", so that pressing
> Space twice never removes a peer. Pressing "Keep" gives it back to the row's "Remove".

> r[viewmodel.machines.remove.confirm]
> Pressing "Remove" under the question asks Core to remove the peer (`pairing.remove.peer`,
> `pairing.remove.link`). Once it is removed, its row leaves the list and every other row stays.

> r[viewmodel.machines.remove.failed]
> When Core reports that a removal could not be saved (`pairing.peers.unreadable`), the Machines
> list says "Beamer could not save the removal. Try again in a moment.", as a fault, and the peer
> keeps its row.
>
> - parity: Python's Mac app said "Could not remove it: {error}", with the system's error, where the Windows app said this sentence (#16: one viewmodel for every machine, and ADR 0004: Core reports a typed failure, never the system's text)

### Dormant peers

> r[viewmodel.machines.dormant.re-pair-button]
> The row of a dormant peer has a "Re-pair" button, named "Re-pair {label}" to assistive
> technology. No other row has one.

> r[viewmodel.machines.dormant.re-pair]
> Pressing "Re-pair" for a dormant peer, on its row or wherever else Beamer offers it, shows the
> Overview page and asks to pair again with that peer, which opens the pairing sheet as
> `viewmodel.pairing-sheet.pair-again` says, with the peer's address filled in whether or not
> addresses are hidden.
>
> - parity: Python's Windows app, while addresses were hidden, left the address empty and chose instead the machine heard at that address, when there was one (#16: one viewmodel for every machine; the Mac app's, which fills the address in and hides it)

## Pairing sheet

The pairing sheet is where the user pairs this machine with another: its first half shows a code
on this machine, its second enters the code another machine shows, picked from the machines heard
or by a typed address. What pairing itself does is `spec/pairing.md`'s; what discovery hears is
`spec/discovery.md`'s. Below, "this Mac" is said on a Mac and "this PC" on a PC, and `•••` is the
mark that stands for an address while Hide addresses is on.

Dropped, so no rule here (#11): pair nearby (F3), pairing a phone and the QR code (#14), the
pairing version 2 exchange and its "Older Beamer" machines, and the portable build (F23).

### Opening and closing

> r[viewmodel.pairing-sheet.open]
> Pressing "Pair a machine" on the Machines page opens the pairing sheet, headed "Pair a machine".
> While the sheet is open, that button says "Cancel", and pressing it closes the sheet.

> r[viewmodel.pairing-sheet.open-without-peers]
> On a Mac, while this machine has no peers, the pairing sheet shows on the Machines page without
> being opened, and the button above it says "Pair a machine". On a PC the sheet shows only once
> opened.

> r[viewmodel.pairing-sheet.close-ends-code]
> Closing the pairing sheet while it shows a code asks Core to cancel the code
> (`pairing.code.cancel`).

> r[viewmodel.pairing-sheet.close-clears]
> Closing the pairing sheet clears the code typed and the selected machine, and its status line
> goes back to "Choose a machine, or type its address, then type the code it shows."
>
> - parity: Python's PC kept the code typed, the selected machine and the last status words when the sheet closed (one behaviour on both machines; the Mac's, so a sheet opened again starts afresh)

> r[viewmodel.pairing-sheet.window-closed]
> Closing the settings window while the pairing sheet shows a code asks Core to cancel the code,
> and the sheet is closed when the window opens again.
>
> - parity: Python's PC left a code up while its window was hidden, still able to pair (one behaviour on both machines; the Mac's, since a code nobody can see should not accept a pairing)

### Showing a code

> r[viewmodel.pairing-sheet.show-half]
> The sheet's first half is headed "Show a code on this Mac" on a Mac and "Show a code on this PC"
> on a PC, and offers "Show a code". On a Mac it also says "On the other machine, open Beamer,
> choose this Mac in its list and type the code."

> r[viewmodel.pairing-sheet.show-code]
> Pressing "Show a code" asks Core to show a code (`pairing.code.one-at-a-time`) and clears the
> words of the last code's outcome. While the code is shown, "Cancel code" takes the place of
> "Show a code".
>
> - parity: Python's PC said "Cancel" for "Cancel code" (one wording on both machines; the Mac's, beside the sheet's own "Cancel")

> r[viewmodel.pairing-sheet.code-shown]
> While a code is shown, the sheet shows its six digits in two groups of three, as "482 913".
>
> - parity: Python's Mac showed the six digits one to a box, with no space, and both apps also showed the code as a QR code for a phone (one wording on both machines, the PC's; #14 removes the QR code, and phone peers are out of scope, #11)

> r[viewmodel.pairing-sheet.seconds-left]
> While a code is shown, the sheet says "Expires in 60 s", with the whole seconds the code has
> left as `pairing.code.seconds-left` reports them.

> r[viewmodel.pairing-sheet.own-address]
> While a code is shown, the sheet gives the address to type on the other machine
> (`discovery.own-address.offered`) as "This Mac's address: 192.0.2.9" on a Mac and "This PC's
> address: 192.0.2.9" on a PC, and says "No network address found." when Core gives none.
>
> - parity: Python showed its pairing port beside the address ("192.0.2.9 · port 24821" on the Mac, "This PC's address: 192.0.2.9 · port 24821" on the PC), and the PC showed nothing when it had no address (`pairing.by-address.dial` dials the one UDP port, #14, so no port is given; one wording on both machines)

> r[viewmodel.pairing-sheet.own-address-hidden]
> While Hide addresses is on, the sheet shows `•••` in place of this machine's address: "This Mac's
> address: •••".

> r[viewmodel.pairing-sheet.cancel-code]
> Pressing "Cancel code" asks Core to cancel the code (`pairing.code.cancel`). The digits, the
> seconds left and the address go, "Show a code" comes back, and the sheet says no outcome.

> r[viewmodel.pairing-sheet.code-ended]
> When a code ends other than by cancelling, its digits, seconds left and address go, "Show a
> code" comes back, and the sheet says the code's outcome (`pairing.outcome.showing`) under "Show a
> code" until the user shows a new code, cancels or closes the sheet.

> r[viewmodel.pairing-sheet.showing-paired]
> When another machine pairs with the code shown, the pairing sheet closes and the window's
> message line says "Paired with Laptop.", naming the new peer by its label, which is its name
> until the user renames it.
>
> - parity: Python's PC kept the sheet open and said it under "Show a code" (one behaviour on both machines, the Mac's, as entering a code does)

> r[viewmodel.pairing-sheet.showing-wrong-code]
> When Core reports that a wrong code was entered, the sheet says "A wrong code was tried, so this
> code was cancelled. Show a new one to try again."

> r[viewmodel.pairing-sheet.showing-expired]
> When Core reports the code expired, the sheet says "The code expired. Show a new one."

> r[viewmodel.pairing-sheet.showing-known]
> When Core reports the other machine already paired (`pairing.known.showing`), naming a peer, the
> sheet says "Studio is already paired here, since 24-09-2026. Remove it from the list first to pair
> again.", with the peer's label and the day it was paired as DD-MM-YYYY in this machine's time
> zone.

> r[viewmodel.pairing-sheet.showing-own-key]
> When Core reports that the other machine gave this machine's own key (`pairing.known.showing`),
> the sheet says "That machine gave this machine's own identity, so nothing was paired. Show a new
> code to try again."

> r[viewmodel.pairing-sheet.showing-known-there]
> When Core reports that the other machine may already be paired here, or the code was wrong
> (`pairing.known.there`), the sheet says "That machine may already be paired here, or the code
> was wrong. Show a new code to try again."

> r[viewmodel.pairing-sheet.showing-full]
> When Core reports this machine full, as the outcome of a code or when the user presses "Show a
> code" (`pairing.capacity.showing`), the sheet says "This machine already has 32 machines paired,
> the most Beamer keeps. Remove one, then show a new code."
>
> - parity: Python's Mac, pressing "Show a code" while full, said "Either this machine or this machine already has 32 machines paired, the most Beamer keeps. Remove one, then try again." (a bug: the words for entering a code, naming this machine twice; the PC's words)

> r[viewmodel.pairing-sheet.showing-not-saved]
> When Core reports the pairing not saved, the sheet says "Beamer could not save the pairing on
> this machine, so it did not complete. Show a new code to try again."

> r[viewmodel.pairing-sheet.showing-no-version-outcome]
> A code's outcome is never about the other machine's version: a machine that runs another version
> of Beamer leaves the code shown (`pairing.connection.version`).
>
> - parity: Python ended the code and said "A machine on an older Beamer tried to pair. Update Beamer on it to 1.5.0, then show a new code." (#14 refuses another version in the pairing connection's handshake, before the code is touched)

### Machines heard

> r[viewmodel.pairing-sheet.enter-half]
> The sheet's second half is headed "Enter a code from another machine". It lists the machines
> heard under "On this network", offers an address field under "Not listed? Type its address.",
> six digits under "Code shown on the other machine", and "Pair". Until the user acts, its status
> line says "Choose a machine, or type its address, then type the code it shows."
>
> - parity: Python's PC said nothing in its status line until the user acted (one behaviour on both machines, the Mac's)

> r[viewmodel.pairing-sheet.heard-showing-only]
> The list shows each machine heard whose latest beacon says a code is shown
> (`discovery.heard.list`), in the order Core reports them. A machine heard that shows no code is
> not listed.
>
> - parity: Python's Mac also listed machines heard showing no code, and said "Laptop is not showing a code yet. Show one in Beamer on it first." when one was selected (the PC's behaviour; #14's beacon carries no name, so a machine showing no code is only an address, and nothing can pair with it)

> r[viewmodel.pairing-sheet.heard-item]
> Each machine in the list shows the address its beacons come from, "192.168.50.22", and two
> machines heard are never shown alike while Hide addresses is off.
>
> - parity: Python showed each machine by the name and platform its beacon gave, "Studio · Windows · showing a code", adding the address only when another machine heard had the same name, compared ignoring case and surrounding blanks (#14: a beacon carries no name and no platform, only that a code is shown)

> r[viewmodel.pairing-sheet.heard-item-hidden]
> While Hide addresses is on, each machine in the list shows `•••` in place of its address.

> r[viewmodel.pairing-sheet.heard-empty]
> With no machine listed, the sheet says "No other machine is showing a code yet."
>
> - parity: Python's Mac said "Looking for machines running Beamer. Open it on the other machine and it appears here; across a VPN or on guest Wi-Fi it may not, so type its address below." (one wording on both machines, the PC's, since only machines showing a code are listed)

> r[viewmodel.pairing-sheet.heard-failure]
> While this machine lacks its UDP port and hears no beacons (`discovery.beacon.port-failure`), the
> sheet says "Beamer cannot look for machines: address already in use" in place of the list, with
> the error Core reports.
>
> - parity: Python's PC said nothing of it (one behaviour on both machines, the Mac's)

> r[viewmodel.pairing-sheet.select]
> Pressing a machine in the list selects it, and only it. Selecting a machine clears an address
> typed, leaves the code typed as it is, and the status line says "Six digits, as shown on that
> machine."
>
> - parity: Python said "Six digits, as shown on Laptop.", naming the machine by its beacon's name; the Mac cleared the code typed, and the PC said nothing (#14: a beacon carries no name; one behaviour on both machines: the Mac's words, and the PC's kept code, so six digits typed before a machine is selected can be paired at once)

> r[viewmodel.pairing-sheet.selection-by-address]
> A selected machine stays selected for as long as a machine showing a code is heard at its
> address, whatever its beacons say meanwhile.
>
> - parity: Python kept the selection by the beacon's address and pairing port, and paired with the pairing id of the latest beacon heard there (#14: a beacon carries neither, and one UDP port serves each machine)

> r[viewmodel.pairing-sheet.heard-gone]
> When the selected machine leaves the list, nothing is selected and the code typed stays. Unless
> an attempt is in progress or a failure is said, the status line goes back to "Choose a machine, or
> type its address, then type the code it shows."
>
> - parity: Python's Mac kept a machine that stopped showing its code selected, saying "Laptop is not showing a code yet. Show one in Beamer on it first.", and kept the status line's words when a machine left (only machines showing a code are listed, `viewmodel.pairing-sheet.heard-showing-only`)

> r[viewmodel.pairing-sheet.status-kept]
> While an attempt is in progress, and after one has failed, the status line keeps its words when
> the machines heard change.

### Typing an address

> r[viewmodel.pairing-sheet.address-unselects]
> Typing in the address field unselects the selected machine.
>
> - parity: Python's Mac unselected it only when the user pressed Return in the field (one behaviour on both machines, the PC's)

> r[viewmodel.pairing-sheet.address-first]
> Pressing "Pair" with an address typed pairs by address with it, whatever machine is selected.

> r[viewmodel.pairing-sheet.address-masked]
> While Hide addresses is on, the address field shows each character typed as a dot.
>
> - parity: Python's PC showed the address typed (one behaviour on both machines, the Mac's, since Hide addresses hides every address in the window)

> r[viewmodel.pairing-sheet.pair-again]
> Asking to pair again with a peer opens the pairing sheet with no machine selected and the
> address field filled in with the address `pairing.by-address.pair-again` gives, ready for the
> code.

### The code typed

> r[viewmodel.pairing-sheet.code-digits]
> The code typed keeps only the digits of what is typed or pasted, and at most six: pasting
> "482 913" or "482-913" gives 482913.

> r[viewmodel.pairing-sheet.pair-pressable]
> "Pair" can be pressed only while a machine is selected or an address is typed, the code typed
> has six digits, and no attempt is in progress.
>
> - parity: Python's Mac let "Pair" be pressed with fewer than six digits typed (one behaviour on both machines, the PC's)

> r[viewmodel.pairing-sheet.short-code]
> Asking to pair, by pressing Return in the code, with fewer than six digits typed asks Core for
> nothing, and the status line says "The code is 6 digits."
>
> - parity: Python's PC did nothing and said nothing (one behaviour on both machines, the Mac's)

### Pairing

> r[viewmodel.pairing-sheet.pair-heard]
> Pressing "Pair" with a machine selected, and no address typed, asks Core to pair with the
> machine heard at that address, with the code typed (`pairing.by-beacon.dial`).

> r[viewmodel.pairing-sheet.pair-typed]
> Pressing "Pair" with an address typed asks Core to pair by address, with that address and the
> code typed (`pairing.by-address.dial`).

> r[viewmodel.pairing-sheet.in-progress]
> While an attempt is in progress, the status line says "Pairing with 192.0.2.40…", with the address
> dialled, and "That machine" in its place while Hide addresses is on.
>
> - parity: Python's Mac named a selected machine by its beacon's name, and the PC said "Pairing…" on the button instead (#14: a beacon carries no name; one behaviour on both machines, the Mac's)

> r[viewmodel.pairing-sheet.entering-name]
> The words for an attempt that failed name the other machine by the address dialled,
> "192.0.2.40", and as "That machine" while Hide addresses is on.
>
> - parity: Python named a selected machine by its beacon's name; for a typed address the Mac said "That machine" and the PC the address, hidden as `•••` (#14: a beacon carries no name; one behaviour on both machines)

> r[viewmodel.pairing-sheet.entering-failed]
> When an attempt fails, the status line says why (`pairing.outcome.entering`), "Pair" can be
> pressed again, and the selected machine, the address typed and the code typed stay as they were.

> r[viewmodel.pairing-sheet.entering-paired]
> When an attempt pairs, the code typed, the address typed and the selection are cleared, the
> pairing sheet closes, and the window's message line says "Paired with Laptop. Connecting…",
> naming the new peer as the Machines page names it, from the name the exchange proved.
>
> - parity: Python's PC kept the sheet open and said "Paired with Laptop." in its status line (one behaviour on both machines, the Mac's, since a peer is linked with at once, `pairing.peers.at-once`)

> r[viewmodel.pairing-sheet.entering-not-showing]
> When Core reports that the other machine is not showing a code, the status line says
> "192.0.2.40 is not showing a code. Show one in Beamer on it, then try again."

> r[viewmodel.pairing-sheet.entering-not-accepted]
> When Core reports that the code was not accepted, the status line says "That code was not
> accepted, and 192.0.2.40 has cancelled it. Show a new one there to try again."

> r[viewmodel.pairing-sheet.entering-version]
> When Core reports that the two machines run different versions of Beamer
> (`pairing.connection.version`), the status line says "192.0.2.40 runs a different version of
> Beamer. Install the same version on both machines, then pair again."
>
> - parity: Python said "Studio runs an older Beamer. Update Beamer on it to 1.5.0, then pair again." (#14: an exact ALPN match cannot tell an older Beamer from a newer one, and no compatibility with the Python apps is kept, #11)

> r[viewmodel.pairing-sheet.entering-no-answer]
> When Core reports that the other machine did not answer (`pairing.connection.no-answer`), the
> status line says "192.0.2.40 did not answer. Check both machines are on the same network, then
> try again."

> r[viewmodel.pairing-sheet.entering-full]
> When Core reports that one of the two machines is full, the status line says "Either this
> machine or 192.0.2.40 already has 32 machines paired, the most Beamer keeps. Remove one, then try
> again."

> r[viewmodel.pairing-sheet.entering-known]
> When Core reports the other machine already paired here (`pairing.known.entering`), naming a
> peer, the status line says "Studio is already paired with this machine, since 24-09-2026. Remove
> it from the list first to pair again.", with the peer's label and the day it was paired as
> DD-MM-YYYY in this machine's time zone.

> r[viewmodel.pairing-sheet.entering-itself]
> When Core reports that the other machine is this machine itself (`pairing.known.entering`), the
> status line says "That is this machine, not another one."

> r[viewmodel.pairing-sheet.entering-known-there]
> When Core reports that the other machine already has this one paired, the status line says
> "192.0.2.40 already has this machine paired. Remove this machine from its list there, then pair
> again."

> r[viewmodel.pairing-sheet.entering-not-saved]
> When Core reports the pairing not saved, the status line says "Pairing finished, but Beamer
> could not save it on this machine. Try again in a moment."

> r[viewmodel.pairing-sheet.entering-busy]
> When Core reports that another pairing is still finishing (`pairing.exchange.one-attempt`), the
> status line says "Another pairing is still finishing. Try again in a moment."

## Crossing page

The Crossing page's own lines and its settings for the crossing methods other than a zone push.
The crossing map drawn on it is the map canvas's (`viewmodel.map`), and each peer's row of edge,
third, corner and notch choices is the ledger's (`viewmodel.ledger`). Below, **this machine's word**
is "this Mac" on a Mac and "this PC" on a PC.

### Introduction

> r[viewmodel.crossing.intro.own-zones]
> While own zones on every machine is on, the Crossing page's introduction says "Drag a machine on
> the map to where it sits. The ledger says how each one is reached; click a row to pick it on the
> map."

> r[viewmodel.crossing.intro.profile]
> While own zones on every machine is off, the Crossing page's introduction says "Click a machine on
> the map to set its ways. The ledger says how it reaches each other machine."

> r[viewmodel.crossing.intro.pc-purpose]
> On a PC, the Crossing page's purpose, above its introduction, says "Choose how the pointer or a key
> moves input to each machine, and how hard the edge pushes back first." On a Mac, the introduction
> is the page's only purpose line.

> r[viewmodel.crossing.scope.pc]
> On a PC, while own zones on every machine is on and "Same on all machines" is off, the Crossing
> page shows under its purpose "Ways below lead to the selected machine. Its side is shared with that
> machine. Resistance, drag protection and Shortcut apply to this PC's input for every destination."
> While own zones on every machine is off, it shows no such line. A Mac's Crossing page never shows
> it.

### What the page shows

> r[viewmodel.crossing.settings.shown]
> The Crossing page shows its Resistance section and its Shortcut section, the Shortcut switch,
> the shortcut key, "How you press it" and "Shortcut goes to" included, whatever the selected peer's
> zone choices are and whether the shortcut is on or off.
>
> - parity: Python's Mac hid Resistance while the selected peer had no edge, third, corner or notch on, and hid the whole Shortcut section, its own switch included, while the shortcut was off, unless a shortcut key clash was being reported, so the shortcut could not be turned on again there (#16: one viewmodel for every platform; the PC's, which showed both always)

> r[viewmodel.crossing.settings.drag-guard]
> The Crossing page's switch "Don't cross while dragging with this Mac's pointer", on a PC "Don't
> cross while dragging with this PC's pointer", is on while blocking crossings while dragging is on
> (`crossing.push.drag`), and can be changed only while at least one peer in use has a zone choice
> on other than the shortcut: an edge, a third, a corner or the notch. A peer not in use never
> counts. Changing it asks Core to turn
> blocking crossings while dragging on or off (`crossing.push.drag-off`).
>
> - parity: Python's Mac counted only the selected peer's zone choices, so the switch could not be changed while a peer without zone choices was selected (#16: one viewmodel for every platform; the PC's)

> r[viewmodel.crossing.settings.resistance]
> The Crossing page's Resistance section shows the resistance (`crossing.push.resistance`) as
> "{value} px", and setting it there asks Core to set the resistance to that whole number of px, from
> 0 to 500.

> r[viewmodel.crossing.settings.resistance-zero]
> At a resistance of 0, the note under the resistance says "Switches the moment this Mac's pointer
> touches the edge. Each screen supplies its own zones.", with "this PC's" for "this Mac's" on a PC.

> r[viewmodel.crossing.settings.resistance-edge]
> At a resistance above 0, while the selected peer's displays touch an edge of this machine's
> displays on the crossing map, the note under the resistance says "Follows this Mac's mouse or
> trackpad on every screen; each screen supplies its own zones. This screen's {edge} edge lights to
> show the resistance.", {edge} being that edge's name in lowercase ("left", "right", "top" or
> "bottom"), with "this PC's" for "this Mac's" on a PC.

> r[viewmodel.crossing.settings.resistance-no-edge]
> At a resistance above 0, while the selected peer's displays touch no edge of this machine's
> displays, or no peer is selected, the note under the resistance says "Follows this Mac's mouse or
> trackpad on every screen; each screen supplies its own zones.", with "this PC's" for "this Mac's"
> on a PC.

### The crossing line and Pause crossing

The Overview page's Keyboard and mouse section has a Pause crossing button and, beside it, the
crossing line: why nothing can cross, or else what the zones are doing.

> r[viewmodel.crossing.pause.button]
> The Pause crossing button reads "Pause crossing", and "Resume crossing" during a crossing pause.
> Pressing it asks Core to pause crossing, or to resume it during a crossing pause
> (`crossing.pause.zones`).

> r[viewmodel.crossing.pause.shown]
> The Pause crossing button shows only while this machine has at least one zone, leading to any
> peer. The crossing line shows while this machine has a zone or while the line gives one of the
> reasons nothing can cross (`viewmodel.crossing.pause.not-paired`,
> `viewmodel.crossing.pause.not-driving`, `viewmodel.crossing.pause.no-link`), the shortcut alone
> being on included. With no zone and none of those reasons, neither shows.

> r[viewmodel.crossing.pause.first-reason]
> The crossing line gives the first of these that holds, and nothing else: no peer; no peer this
> machine may drive; no link with any peer; a crossing pause; a full-screen pause; and otherwise that
> crossing is on. A reason nothing can cross is given whether or not this machine has a zone.

> r[viewmodel.crossing.pause.not-paired]
> With no peer, the crossing line says "Not paired yet, so no edge or shortcut moves input until you
> pair a machine on Overview." on a Mac, and "Not paired yet, so no edge or shortcut moves input
> until you pair a machine above." on a PC.
>
> - parity: Python's PC said "Waiting to hear from {name}. This PC cannot push into it until it has connected once." next, while no peer had an address, as a peer carried over from a 1.4.x settings file had none; every peer has an address from its pairing (#11: no compatibility with the Python apps and their settings files)

> r[viewmodel.crossing.pause.not-driving]
> With peers but none this machine may drive, the crossing line says "No machine is driven by this
> Mac, so edges and the shortcut do nothing." on a Mac, and "This PC does not drive {name}, so edges
> and the shortcut do nothing." on a PC.

> r[viewmodel.crossing.pause.no-link]
> With a peer this machine may drive but no link with any peer, the crossing line says "Not connected
> to any machine, so edges and the shortcut do nothing yet." on a Mac, and "Not connected to {name},
> so edges and the shortcut do nothing yet. Check that it is switched on and lets this PC drive it."
> on a PC.

> r[viewmodel.crossing.pause.name]
> In the PC's crossing line, {name} is the one peer's name as the machines list shows it when there is
> exactly one peer, and "your machines" when there are several.

> r[viewmodel.crossing.pause.paused]
> With peers, a peer this machine may drive and a link, during a crossing pause the crossing line says
> "Pauses pointer crossing through this screen, including another machine's pointer. Shortcuts and
> jump keys still work. Resets when Beamer restarts."

> r[viewmodel.crossing.pause.full-screen]
> With peers, a peer this machine may drive and a link, during a full-screen pause
> (`crossing.full-screen.zones`) and no crossing pause, the crossing line says "Held: an app is full
> screen."

> r[viewmodel.crossing.pause.on]
> With peers, a peer this machine may drive, a link, no crossing pause and no full-screen pause, the
> crossing line says "On. Pause it to lean on an edge without switching."

### Jump keys

Each peer's row in the ledger has a Keys column: the peer's tick for the shortcut's cycle
(`viewmodel.shortcut.cycle-ledger`) and, captioned "Jump", the peer's jump key.

> r[viewmodel.crossing.jump-key.empty]
> A peer with no jump key shows "Set a key" under "Jump" in its ledger row, and no Clear button.

> r[viewmodel.crossing.jump-key.shown]
> A peer's jump key shows in its ledger row in this machine's words: on a Mac, the chord's modifiers
> as their symbols, ⌃ for `ctrl`, ⌥ for `alt`, ⇧ for `shift` and ⌘ for `cmd`, then a space and the
> key's name (`viewmodel.shortcut.key-names-mac`); on a PC, the modifiers as "Ctrl" for `ctrl`,
> "Alt" for `alt`, "Shift" for `shift` and "Win" for `cmd`, then the key, a single character in
> capitals or else the key's name (`viewmodel.shortcut.key-names-pc`), all separated by spaces. A
> Clear button, named "Clear jump key" for accessibility, shows beside it.

> r[viewmodel.crossing.jump-key.tooltip]
> A peer's jump key is described as "Press {key} to send input to {name}; press again to bring it
> home.", and while it has none, "Choose a key combination to jump to {name} and back.", {name} being
> the peer's name as the ledger shows it.

> r[viewmodel.crossing.jump-key.arm]
> Clicking a peer's jump key in its ledger row starts recording one: the key reads "Press a key
> combination…" and a hint under it, hidden until then, says "Click again to cancel". Core is told
> that a jump key is being recorded (`crossing.jump-key.recording`) until the recording ends.
> Clicking again, or pressing Esc alone, ends the recording and leaves the jump key as it was.

> r[viewmodel.crossing.jump-key.record]
> A chord pressed while a peer's jump key is being recorded ends the recording and asks Core to make
> it that peer's jump key. Clear asks Core to clear the peer's jump key (`crossing.jump-key.clear`).

> r[viewmodel.crossing.jump-key.refused]
> When Core refuses a recorded chord, the window's message line says why, and the peer's jump key
> stays as it was: "choose a modifier and one non-modifier key" for a chord that is not a jump key's
> (`crossing.jump-key.chord`), "that key is the trigger shortcut's: pick a chord without it" for one
> that holds the shortcut key (`crossing.jump-key.shortcut-clash`), and "that key is already
> assigned to another machine" for another peer's chord (`crossing.jump-key.unique`).
>
> - parity: Python's PC said it only to a screen reader, in the map canvas's announcement, while its Mac showed it in the window's message line (#16: one viewmodel for every platform; the Mac's)

### Way home

> r[viewmodel.crossing.shake.switch]
> The Crossing page's Way home section has a switch "Shake to come home", on while the shake is on
> (`crossing.shake.defaults`). Changing it asks Core to turn the shake on or off
> (`crossing.profile.shake.in-profile`, `crossing.profile.shake.own`).

> r[viewmodel.crossing.shake.sensitivity]
> Under "Shake sensitivity", the Way home section shows the shake sensitivity as "{value} %" and
> lets it be set to a whole number from 50 to 200, which asks Core to set the shake sensitivity
> (`crossing.shake.sensitivity-range`).

> r[viewmodel.crossing.shake.note]
> Under the shake sensitivity, the Way home section says "Shake the mouse or trackpad left and right a
> few times while you drive another machine, and input comes straight back to the machine you are
> using."

## Crossing map

The crossing map (F6) as the Crossing page shows it, and what the user's clicks, drags, drops, keys
and resizes on it ask of `Core`. Where footprints sit, their sizes, contacts, thirds, snap points,
zones and what a drop, resize or recentre does to the crossing map are `spec/crossing.md`'s; these
rules say only what the map shows from `Core`'s snapshot and which command each gesture asks for.
How the map is drawn, its scale and fit, its sizes, colours and opacities, and where a press lands
in points are `spec/ui.md`'s. The ledger under the map is `viewmodel.ledger`'s, the rest of the
Crossing page `viewmodel.crossing`'s and `viewmodel.shortcut`'s, and the map while own zones on
every machine is off `viewmodel.profiles`'s.

"This Mac" on a Mac and "This PC" on a PC name this machine wherever a rule quotes "This Mac". The
**peer selected on the map** is the peer whose footprint the map shows as selected, the one the
ledger and the rest of the page show.

### What the map shows

> r[viewmodel.map.members]
> The map shows one footprint for this machine and one for every active machine on the crossing map
> (`crossing.map.members.this`, `crossing.map.members.peers`, `crossing.map.members.third-party`),
> each where the crossing map has it. A peer with no footprint yet is shown where crossing places it
> by default (`crossing.map.place.facing`, `crossing.map.place.unplaced`), and a footprint the
> crossing map keeps off is never shown.

> r[viewmodel.map.inactive-hidden]
> An inactive peer (not in use, kept off the crossing map, or dormant) has no footprint on the map,
> and no seam, pad or wrap-around seam there; it keeps its row in the ledger. Keeping a peer off the
> crossing map takes its footprint off the map, and restoring it, or putting it in use again, shows
> it where the crossing map has it. Nothing on the map presses, drags, resizes or renames an
> inactive peer.

> r[viewmodel.map.offline-shown]
> A peer with no link that is in use, not kept off the crossing map and not dormant, as one that is
> asleep or unreachable, keeps its footprint on the map, where the crossing map has it
> (`crossing.derive.no-link`), and its outcome says "Offline" (`viewmodel.map.outcome`).

> r[viewmodel.map.follows]
> The map follows `Core`'s snapshot whenever the crossing map, the zone choices, the paired list or
> a link changes, on this machine or on another: a peer just paired gets its footprint, and a change
> to the crossing map arriving from another machine shows each footprint, seam and outcome as the
> changed map gives them.

> r[viewmodel.map.displays]
> Each footprint shows every display of its machine, where `crossing.map.displays.fill` and
> `crossing.map.displays.apart` place it, a display moved apart included even where it lies outside
> its footprint. A machine whose displays are not known shows its footprint alone.

> r[viewmodel.map.title]
> Each footprint is titled with its machine's name: "This Mac" for this machine, for a peer the name
> the Machines page gives it, and for a machine that is not a peer the name the crossing map carries
> with its footprint (`crossing.map.members.name`). Where the name does not fit the footprint, the footprint's tooltip
> and accessible name still carry it whole.

> r[viewmodel.map.status-own]
> This machine's footprint has the status line "{name} · macOS" on a Mac and "{name} · Windows" on a
> PC, {name} being this machine's name, or, while it has none, the first part of its host name, up
> to the first ".". It never says "Offline".
>
> - parity: the PC's footprint for this machine showed its name alone, or "Windows" while it had none (#16 and ADR 0004: one viewmodel for both machines, taking the Mac's words)

> r[viewmodel.map.status-peer]
> A peer's footprint has the status line "{platform} · Linked" while a link to it is up
> (`link.report.up`) and "{platform} · Offline" otherwise, {platform} being "macOS" for a Mac peer,
> "Windows" for a PC peer and "Machine" for a peer of any other platform, as the ledger names them.
>
> - parity: Python's Mac map said "Other" and its PC map "Computer" for a peer of another platform, both named a Linux peer "Linux", and a peer running a Beamer without the crossing map said "Beamer {version} · only sees which side" (one wording on both machines and with the ledger; Linux is out of scope, #1; no compatibility with the Python apps, #11)

> r[viewmodel.map.outcome]
> A peer's footprint shows one outcome, the first of these that holds: "Offline" while no link to it
> is up; "Crosses" while a corner zone, a wrap-around zone or the notch zone leads to it; "Edge off"
> while its edge is off, or while none of its picked thirds touches this machine's display
> (`crossing.derive.share.unpicked`); "Overlaps" while it is in an overlap
> (`crossing.map.overlap.reported`); "Drag to connect" while its footprint touches none of this
> machine's displays, or it is reported as crossing between two peers (`crossing.derive.between`);
> "Crosses" while an edge or thirds zone leads to it; and "Edge off" otherwise. This machine's
> footprint shows no outcome.
>
> - parity: the PC's footprint said "Set elsewhere" for a peer crossing between two peers (#16 and ADR 0004: one viewmodel for both machines, taking the Mac's words, which dropped that label)

### Contacts, wrap-around zones and corners

> r[viewmodel.map.seam-lit]
> While an edge or thirds zone leads to a peer through a contact along an edge
> (`crossing.derive.share.zone`), the map lights that contact on this machine's footprint: the whole
> contact while the peer crosses there by its whole edge, and otherwise only the stretches of the
> contact inside the thirds of this machine's display edge that the peer picked and that went to it
> (`crossing.derive.share.claim`). On a display 160 by 90 map units, a contact over its whole right
> edge with the start third alone lights 0 to 30; a contact over its whole bottom edge with the
> start and end thirds lights 0 to 53⅓ and 106⅔ to 160; a contact over 0 to 30 of the right edge
> with the end third alone lights nothing. No contact of a peer is lit while its edge is off or no
> edge or thirds zone leads to it there, and a contact at a corner is never lit as a seam.

> r[viewmodel.map.tracks]
> For the peer selected on the map, while its edge is on, each third of each of its contacts along an
> edge that is not lit shows as an unlit track, which a click turns on (`viewmodel.map.third-click`).
> Other peers' unlit thirds show nothing.

> r[viewmodel.map.wrap-seam]
> For each edge of this machine on which a wrap-around zone leads to an active peer
> (`crossing.derive.wrap.zone`), the map shows a seam along that edge of this machine's footprint and,
> inside the footprint, the label "wraps to {name}", {name} being the peer's name on the map. Each
> edge shows once for each peer.

> r[viewmodel.map.wrap-kept]
> For each edge of this machine whose state for an active peer is `kept`
> (`crossing.derive.offer.edge`), the map shows a faint dashed seam along that edge of this
> machine's footprint, labelled "wrap kept, Edge off". An edge on which a wrap-around zone to that
> peer is derived never also shows as kept.

> r[viewmodel.map.wrap-label-merged]
> Wrap labels that would say the same words on several edges of this machine's footprint are shown
> as one label: the words, " · " and the edges named in the order top, left, right, bottom, separated
> by ", ", as in "wraps to Bee · top, left"; where that does not fit the footprint, the words, " · "
> and the number of edges, as in "wraps to Bee · 3 sides"; and where neither fits, the words alone.

> r[viewmodel.map.corner-pad]
> For each corner zone that leads to an active peer (`crossing.derive.corner.contact`,
> `crossing.derive.corner.chosen`), the map shows a pad at that corner of this machine's footprint:
> one pad for every corner in the zones derived, never for a corner only chosen. A peer reached only
> at a corner shows its pad and no lit seam.

> r[viewmodel.map.notch-mark]
> On a Mac, while an active peer has the notch chosen (`crossing.derive.notch.one`), the map shows a
> notch mark at the top of this machine's footprint, over the notch's stretch of the display that has
> it, or at the middle of the footprint's top edge while its displays are not known.

> r[viewmodel.map.between]
> Where the footprints of two peers that are reported as crossing between each other
> (`crossing.derive.between`) touch, the map marks their contact apart from this machine's lit
> contacts.

> r[viewmodel.map.hint]
> While own zones on every machine is on, directly under the map, the Crossing page says "Option-click a display to split or group · drag to
> move · click a third of a lit edge to leave it out · drag a corner to resize" on a Mac, and
> "Alt-click a display to split or group · drag to move · click a third of a lit edge to leave it out
> · drag a corner to resize" on a PC.

> r[viewmodel.map.legend]
> Under the map's hint, the Crossing page shows a legend of two entries, "Crosses" and "Offline,
> ways kept".

### Selecting

> r[viewmodel.map.select]
> A click on a peer's footprint, or pressing its accessible element, selects that peer on the map,
> and the Crossing page shows that peer as its chosen peer.

> r[viewmodel.map.select-default]
> When the Crossing page opens, or the chosen peer leaves the peer list, the first peer in the peer
> list's order becomes the chosen peer, and the map shows its footprint selected; with one peer, that
> peer is selected. A peer chosen in the ledger or on the map stays selected on the map until another
> is chosen.

> r[viewmodel.map.select-own]
> A click on this machine's footprint changes no selection: the peer selected on the map stays
> selected, and nothing is asked of `Core`.

> r[viewmodel.map.tab-order]
> While the map has keyboard focus, Tab selects the next peer's footprint and Shift-Tab the previous
> one, in the reading order `crossing.shortcut.cycle-rows` gives the centres of the footprints shown,
> without this machine's. Tab on the last, or Shift-Tab on the first, moves keyboard focus off the
> map.

### Moving a footprint

> r[viewmodel.map.drag-drop]
> Releasing a drag of a peer's footprint that moved it asks `Core` for one drop of that footprint at
> the place it was released, in whole map units (`crossing.map.drop.change`), or at the snap preview
> while one is shown (`viewmodel.map.snap-preview`). Nothing is asked while the drag lasts, and a
> press released without moving the footprint asks nothing.

> r[viewmodel.map.own-fixed]
> This machine's footprint never moves from the map: dragging it or pressing an arrow key with it
> selected moves nothing and asks nothing of `Core`, and a drop the page is handed that would move it
> is refused without asking `Core`. It can still be resized (`viewmodel.map.resize-own`).

> r[viewmodel.map.snap-radius]
> A drop asked by a drag carries the snap radius of `crossing.map.snap.drop-radius` in map units: 12
> divided by the map's scale, in points per map unit, as drawn when the drag began; on a map drawn
> at half a point per map unit it is 24. A drop asked by an arrow key, by a recentre or by a resize
> carries no snap radius.
>
> - parity: the PC's arrow keys and Recentre asked for their drops with the snap radius too (`crossing.map.recentre` and `crossing.map.resize.taken` take no snap radius; #16: one viewmodel for both machines)

> r[viewmodel.map.snap-preview]
> While a peer's footprint is dragged, the map shows a snap preview at the free snap point
> `crossing.map.snap.choice` picks with the snap radius, only while that point differs from where the
> footprint is and lies within the snap radius of it, or within 1.5 times the snap radius for a
> centred point; the dragged footprint itself stays under the pointer. With the snap radius 12, a
> footprint 16 map units right of a centred point shows a preview there, and one 19 units right shows
> none. No snap preview is shown while a footprint is resized, while a display is dragged, or for
> this machine's footprint.

> r[viewmodel.map.drop-result]
> After `Core` takes a drop, the map shows the dropped footprint where the crossing map then has it,
> which may be a snap point other than where it was released (`crossing.map.drop.overlap`).

> r[viewmodel.map.drop-refused]
> When `Core` refuses a drop (`crossing.map.drop.overlap`, `crossing.map.drop.crowded`,
> `crossing.map.drop.inactive`), the map shows every footprint where the crossing map still has it,
> and says "Refused: that position is already occupied." on a Mac and "That position cannot connect
> this machine." on a PC.

> r[viewmodel.map.keep-off-drag]
> Releasing a drag of a peer's footprint below the bottom edge of the map asks `Core` to keep that
> peer off the crossing map (`crossing.derive.kept-off`) instead of a drop.

> r[viewmodel.map.arrow-move]
> While the map has keyboard focus and a peer's footprint is selected, each arrow key moves that
> footprint by a third of this machine's footprint, rounded to a whole map unit: its width for left
> and right, its height for up and down. Each press asks `Core` for one drop there. With this
> machine's footprint 160 by 100, Down moves a peer's footprint 33 map units down.
>
> - parity: the Mac moved a footprint by a third of its own width or height (#16: one viewmodel for both machines, taking the PC's, whose third is a third of this machine's edge)

> r[viewmodel.map.recentre]
> While a peer's footprint is selected and is not centred against an edge of this machine's
> footprint, the map offers "Recentre", whose accessible name is "Recentre {name}". Pressing it, or C
> while the map has keyboard focus, asks `Core` once to recentre that peer (`crossing.map.recentre`);
> once it is centred, the map no longer offers it. Nothing is recentred while a drag lasts.

> r[viewmodel.map.split-display]
> Option-clicking on a Mac, or Alt-clicking on a PC, one of a peer's displays without moving it asks
> `Core` to move that display apart from its machine at the offset (0, 0)
> (`crossing.map.displays.apart`), and on a display already apart, to rejoin it
> (`crossing.map.displays.together`). This machine's displays are not split from the map.

> r[viewmodel.map.display-drag]
> Dragging a peer's display that is apart, or Option-dragging (Mac) or Alt-dragging (PC) one that is
> not, moves that display alone, by whole map units, and leaves its machine's footprint where it
> was. Its release asks `Core` for one drop of the footprint with that display's new offset.

### Resizing a footprint

> r[viewmodel.map.resize-offered]
> The map offers a resize handle on the footprint that is selected and on the footprint under the
> pointer, for every footprint it shows, this machine's and a peer's with a display apart included,
> and takes it away when the pointer leaves the map.
>
> - parity: the Mac offered no resize for a peer with a display moved apart (#16: one viewmodel for both machines, taking the PC's; `crossing.map.resize.proportional` scales the offsets)

> r[viewmodel.map.resize-drag]
> Dragging a footprint's resize handle resizes it as `crossing.map.resize.drag` and
> `crossing.map.resize.clamp` say, the pointer's travel turned into map units at the map's scale as
> drawn when the drag began. Nothing is asked while the drag lasts; its release asks `Core` for one
> drop of the resized footprint (`crossing.map.resize.taken`), and nothing when its size did not
> change. Dragging the handle of a 160 by 100 footprint 80 map units right gives 240 by 150, 32 right
> and 20 down gives 192 by 120, and far enough out 480 by 300 or in 40 by 25. A resize released
> below the map never keeps the peer off the crossing map.

> r[viewmodel.map.resize-keys]
> While the map has keyboard focus and a footprint is selected, Option-Right or Option-Down on a Mac,
> Alt-Right or Alt-Down on a PC, asks `Core` for one resize step up, and Option-Left or Option-Up,
> Alt-Left or Alt-Up, one step down (`crossing.map.resize.step`), its top-left where it was. A footprint
> 160 by 100 steps to 180 by 112 and to 140 by 88. A step that leaves the size as it was, at 40 or
> 480 map units, asks nothing.

> r[viewmodel.map.resize-own]
> This machine's footprint can be resized by its handle and by the keys like a peer's, and stays
> where it is; its displays' offsets scale with it (`crossing.map.resize.proportional`). Resizing it
> leaves the peer selected on the map as it was.

> r[viewmodel.map.press-priority]
> A press within a third of the selected peer's contact goes to that third even where it falls on a
> resize handle, and a press on a footprint's tick for the shortcut's cycle toggles the tick and
> starts neither a resize nor a drag.

### Thirds

> r[viewmodel.map.third-click]
> A press and release on the same third of a contact along an edge between this machine's display and
> the selected peer's display asks `Core` once to toggle that third of the peer's edge
> (`crossing.derive.edge.toggle-third`), on every edge: clicking a lit third leaves it out, and
> clicking it again turns it on. The press starts no drag: no footprint moves and the crossing map is
> not changed, wherever the pointer goes before the release. Another peer's contact, and a contact at
> a corner, toggle nothing.

> r[viewmodel.map.third-click-cancel]
> A press on a third released over another third, or off the thirds, toggles nothing.

> r[viewmodel.map.third-tooltip]
> A third of the selected peer's contact has the tooltip "{third} third: click to turn it off" while
> it is picked, with "click to leave it out" in place of "click to turn it off" on a PC, and "{third}
> third: click to turn it on" while it is not. {third} is "Top", "Middle" or "Bottom" along a left or
> right edge, and "Left", "Middle" or "Right" along a top or bottom edge.

### Renaming

> r[viewmodel.map.rename-start]
> Double-clicking a peer's footprint, or pressing Return or F2 while the map has keyboard focus and a
> peer's footprint is selected, starts renaming that peer: a rename field opens holding the peer's
> label, or its name when it has no label.

> r[viewmodel.map.rename-own]
> Double-clicking this machine's footprint renames nothing and says "Rename this Mac on Overview." on
> a Mac and "Rename this computer on Overview." on a PC.

> r[viewmodel.map.rename-finish]
> While a peer is being renamed from the map, Return asks `Core` to rename the peer to the field's
> text, as renaming does on the Machines page, an empty text included, which clears the peer's label.
> Escape closes the field and asks nothing.

### The tick for the shortcut's cycle

> r[viewmodel.map.cycle-tick]
> While the shortcut's destination is next among ticked, each peer's footprint shows a tick, ticked
> while the peer is in the shortcut's cycle (`crossing.shortcut.cycle-ticks`), with the tooltip "In
> the shortcut's cycle". This machine's footprint has none, and with any other destination no
> footprint has one.

> r[viewmodel.map.cycle-toggle]
> A click on a peer's tick, or Space while the map has keyboard focus and the peer's footprint is
> selected, asks `Core` once to tick that peer for the shortcut's cycle if it was unticked, or to
> untick it if it was ticked (`crossing.shortcut.cycle-ticks`).

### Announcements and accessibility

> r[viewmodel.map.announce-move]
> After `Core` takes a drop asked by a drag, an arrow key or Recentre, the map announces "{name},
> {position} This Mac, {third} third. {outcome}." while exactly one third of this machine's edge
> crosses to that peer, and "{name}, {position} This Mac. {outcome}." otherwise. {position} is
> "right of" when the footprint starts at or right of this machine's footprint's right edge, else
> "left of" when it ends at or left of its left edge, else "below" when it starts at or below its
> bottom edge, else "above". {third} is the third named in lower case as the tooltip names it
> (`viewmodel.map.third-tooltip`). {outcome} is "Overlaps" while the peer is in an overlap, "Drag to
> connect" while its footprint touches none of this machine's displays and no zone leads to it or it
> crosses between two peers, "Edge off" while no zone leads to it otherwise, and "Crosses" while one
> does. Moving a peer from touching this machine's right edge one third to the right announces
> "Bee, right of This Mac. Drag to connect.".
>
> - parity: the Mac named the third "start", "middle" or "end"; the PC said "{name}, {edge} of This PC, {third} third. {result}" with "top of" and "bottom of" for above and below and "at a corner of This PC", named the third its footprint's centre faced when no third was picked, and always ended "Crosses." (#16: one viewmodel for both machines; the PC's ending was never set from the outcome)

> r[viewmodel.map.announce-resize]
> After `Core` takes a resize, the map announces "{name} resized.". A refused resize or drop announces
> no success.

> r[viewmodel.map.description]
> Until the map announces something, its accessible description is, for each footprint in the map's
> order: "{name}, {machine name}, this Mac." for this machine, or "{name}, this Mac." while it has no
> name, with "this PC." in place of "this Mac." on a PC; "{name}, {outcome}." for a peer, {outcome} as `viewmodel.map.outcome` gives it; then, for each
> footprint that can be resized, "{name}: drag a corner to resize, or use Option and the arrow keys."
> on a Mac and "{name}: drag a corner to resize, or use Alt and the arrow keys." on a PC; and, while
> the map shows ticks for the shortcut's cycle, "{name} is in the shortcut's cycle." or "{name} is out
> of the shortcut's cycle." for each peer. With no footprint it is "No machines paired.".
>
> - parity: the PC described the map as its machines' names joined by "; ", then this PC's status line and "Tile size can be changed by dragging a corner or pressing Alt+arrow." (#16 and ADR 0004: one viewmodel for both machines, taking the Mac's)

> r[viewmodel.map.footprint-access]
> Each footprint is an accessible button named with its machine's name, whose value is "Selected"
> while it is selected and whose help, while it can be resized, is "Drag a corner to resize, or use
> Option and the arrow keys." on a Mac and "Drag a corner to resize, or use Alt and the arrow keys."
> on a PC. While the map shows ticks for the shortcut's cycle, each peer's tick is an accessible
> checkbox named "{name}, In the shortcut's cycle", checked while the peer is ticked, which pressing
> toggles as `viewmodel.map.cycle-toggle` does.
>
> - parity: the PC's map exposed no accessible element for each footprint or tick, only its description (#16 and ADR 0004: one viewmodel for both machines, taking the Mac's)

> r[viewmodel.map.read-only]
> While the map is read-only, as when it shows another machine's crossing map that this machine may
> not edit, no press, drag or key on it asks anything of `Core`: no drop, resize, toggled third or
> tick, rename or recentre. It offers no resize handle and no "Recentre".

## Crossing ledger

The crossing ledger is the table on the Crossing page, under the crossing map, with one row per
peer. Each row shows and changes that peer's zone choices (`crossing.derive.edge.form`): its edge
and thirds, its wrap-around edges, its corners and, on a Mac, the notch and return through the
notch; and in its Keys cell, the peer's tick for the shortcut's cycle and its jump key. What each
choice does to the zones is `spec/crossing.md`'s; a row only shows the choices and asks `Core` to
change them. How the ledger is laid out, at any width, and how a state looks (dimmed, lit,
dashed) are `ui`'s.

Below, {name} is the row's peer's shown name, as every page names a peer (the `viewmodel.machines`
topic gives it); "this Mac" is said on a Mac and "this PC" on a PC; {Edge} is "Left", "Right",
"Top" or "Bottom", the edge of this machine a chip stands for; and a peer is "set aside" while it
is kept off the crossing map or not in use. Each edge of this machine has, for each peer, one of
the states `contact`, `on`, `kept`, `free`, `taken` and `unavailable`, and each corner one of
`on`, `free`, `taken` and `unavailable`, as `crossing.derive.offer.edge` and
`crossing.derive.offer.corner` give them.

Dropped, so no rule here (#11): phone peers, which had no row; the status "Beamer {version} · only
sees which side" of a peer running Beamer 1.5.0 or 1.5.1, and the single edge and corner it could
take, since there is no compatibility with the Python apps; and a read-only ledger for a machine
that could not take profile edits, since every machine can.

### Rows

> r[viewmodel.ledger.rows]
> The crossing ledger has one row for each peer, an inactive peer included, and no other row.
> Pairing a machine adds its row, and removing a pairing removes that peer's row and leaves every
> other row as it was.

> r[viewmodel.ledger.empty]
> With no peer, the crossing ledger shows no row and no column header.

> r[viewmodel.ledger.order]
> Rows are in the order of their peers' footprints on the crossing map, or of the footprint a peer
> with none would be given (`crossing.derive.offer.unplaced`): by the footprint's left edge, left
> to right, then by its top edge, top to bottom, and in the peer list's order where both are
> equal. A peer set aside keeps its row in that order, by its kept footprint: with Sea's footprint
> at x = -160 and Bee's at x = 160, Sea's row comes first, and stays first when Sea is kept off the
> crossing map.

> r[viewmodel.ledger.name]
> Each row is headed by {name}, which is also the row's accessible name. The name's tooltip and
> accessible name are always the whole name, however much of it the row has room to show. When the
> peer's name changes, its row shows the new name.

> r[viewmodel.ledger.status]
> Under {name}, a row says "{platform} · {status}". {platform} is "macOS" for a Mac peer,
> "Windows" for a PC peer and "Machine" for a peer of any other platform. {status} is "Left out"
> while the peer is set aside, otherwise "Linked" while this machine has a link with it
> (`link.report.up`), and otherwise "Offline, ways kept".
>
> - parity: Python's PC said "Computer" for a peer of an unknown platform, both apps said "Linux" for a Linux peer, and the PC said "Left out" only for a peer kept off the crossing map, never for one not in use (one wording on both machines, the Mac's; Linux is out of scope, #1)

> r[viewmodel.ledger.dimmed]
> A row is dimmed while its peer is set aside or has no link with this machine, and only then.

> r[viewmodel.ledger.offline-live]
> A row dimmed only because its peer has no link keeps every control enabled, as it is with a
> link, and a click on one asks `Core` for the same change as with a link
> (`crossing.derive.no-link`): with the peer's thirds all picked, clicking the middle third of an
> offline peer's row leaves the start and end thirds picked.

> r[viewmodel.ledger.select]
> Clicking a row anywhere but on one of its controls makes its peer the Crossing page's selected
> machine: that row is marked selected, every other row unselected, and the crossing map selects
> the same machine. Selecting a row writes nothing and asks `Core` for no change.

> r[viewmodel.ledger.selected-row]
> Whichever way a peer becomes the Crossing page's selected machine, in the ledger or on the
> crossing map, its row is marked selected and every other row unselected. A dormant peer's row
> can be selected like any other.

> r[viewmodel.ledger.columns]
> On a Mac, the crossing ledger's columns are headed "Machine", "Edges", "Corners", "Notch" and
> "Keys", in that order. On a PC they are headed "Machine", "Edges", "Corners" and "Keys".

> r[viewmodel.ledger.table-name]
> The crossing ledger is presented to assistive technology as a table, with a row for each of its
> rows, and its accessible name is "How each machine is reached".
>
> - parity: Python's Mac named it "Machines and crossing ways" (one wording on both machines, the PC's)

### Rename

> r[viewmodel.ledger.rename.offer]
> Each row offers "Rename", whose accessible name is "Rename {name}".
>
> - parity: Python's PC named it "Rename" alone (one wording on both machines, the Mac's, which says which machine)

> r[viewmodel.ledger.rename.field]
> Pressing "Rename" puts a text field in the place of the row's name, holding the peer's label, or
> its name when it has no label, and says "Return saves, Escape cancels" under it.

> r[viewmodel.ledger.rename.save]
> Return in the rename field asks `Core` to rename the peer to the text in it, as renaming does on
> the Machines page, closes the field, and the row then shows the new name. Escape closes the field
> and asks for nothing, and the row keeps its name.

> r[viewmodel.ledger.rename.scope]
> While the rename field is open, the row also says "Only on this Mac" on a Mac and "Only on this
> PC" on a PC, unless this machine shares machine names with its peers through settings sync.
>
> - parity: Python's Mac hid the note whenever "Same on both" was on, whatever it shared, and the PC always showed it, joined to the hint as "Return saves, Escape cancels · Only on this PC" (one behaviour on both machines: the note shows exactly when a new name stays on this machine)

### Setting a peer aside

> r[viewmodel.ledger.leave.offer]
> A row whose peer is not set aside offers "Leave out", whose accessible name is "Leave out
> {name}" and whose tooltip is "Leave this machine out of pointer crossings".
>
> - parity: Python's PC named it "Leave out" alone, with the tooltip "Leave this machine out of crossings" (one wording on both machines, the Mac's: the shortcut, the menu and jump keys still reach a machine left out)

> r[viewmodel.ledger.leave.restore-offer]
> A row whose peer is set aside offers "Put back" instead, whose accessible name is "Put back
> {name}" and whose tooltip is "Left out machines stay paired and linked. The shortcut, menu and
> clipboard still reach them."
>
> - parity: Python's PC named it "Put back" alone (one wording on both machines, the Mac's)

> r[viewmodel.ledger.leave.press]
> Pressing "Leave out" asks `Core` to keep the peer off the crossing map
> (`crossing.derive.kept-off`), and its zone choices, jump key and tick for the shortcut's cycle
> stay as they were. Pressing "Put back" asks `Core` to restore the peer
> (`crossing.map.place.restored`) and, when it is not in use, to put it in use again; its zone
> choices are again as they were.
>
> - parity: Python's PC did not count a peer not in use as left out, so its "Put back" only restored a peer kept off the crossing map (one behaviour on both machines, the Mac's)

> r[viewmodel.ledger.leave.inert]
> While a row's peer is set aside, the row's edge chips, thirds, corners and notch ticks are
> disabled, and a click on one asks `Core` for nothing. Each chip, third and corner then has the
> tooltip "Left out: Put back to change its ways". The tick for the shortcut's cycle, the jump key,
> "Rename" and "Put back" stay enabled.

> r[viewmodel.ledger.leave.kept-shown]
> While a row's peer is set aside, the row answers its edges and corners as if the peer were
> restored and in use (`crossing.derive.offer.kept-off`), and its Edges cell shows only the chips
> of `contact`, `on` and `kept` edges: a peer touching the right edge and wrapping round on the
> top edge shows a lit "Right" chip and a "Top, wraps" chip that is on, and its corners on stay on.

### Edges

> r[viewmodel.ledger.edge.chips]
> A row's Edges cell has a chip for each edge of this machine whose state for the peer is not
> `unavailable`: first the edges the peer touches (`contact`), then the others, each group in the
> order left, right, top, bottom. With the peer touching the top edge, the top edge's chip comes
> first.

> r[viewmodel.ledger.edge.no-contact]
> A row whose peer touches no edge of this machine's displays, touching only at a corner or not at
> all, has no `contact` chip and no thirds, so the ledger offers no way to toggle that peer's edge,
> and every corner is `unavailable` for it (`crossing.derive.offer.corner`). Nothing shown in the
> row, and no click in it, moves a footprint on the crossing map.

> r[viewmodel.ledger.edge.none]
> When a row shows no edge chip, its Edges cell says "Place it on the map".

> r[viewmodel.ledger.edge.contact]
> The chip of a `contact` edge reads "{Edge}" and is on while the peer's edge is on, in either
> form, and off while it is off (`crossing.derive.edge.form`). Its tooltip is "{Edge} edge:
> touches {name}; click to turn it off" while on, and "{Edge} edge: touches {name}; click to turn
> it on" while off. Its accessible name is "{Edge} edge, touches {name}, on" while on, and "{Edge}
> edge, touches {name}, off" while off.

> r[viewmodel.ledger.edge.contact-click]
> Clicking a `contact` chip asks `Core`, at once, to toggle the peer's edge
> (`crossing.derive.edge.toggle-edge`), and for nothing else: on, in either form, it turns off and
> keeps its thirds, so the chip goes off and no third is lit; off, it turns on as the whole edge,
> with all three thirds lit. No other peer's zone choices change.

> r[viewmodel.ledger.edge.thirds]
> A `contact` chip carries the edge's three thirds, in the order start, middle, end. A third is lit
> while the peer's edge is on and that third is picked, so the lit thirds are always the picked
> thirds of `crossing.derive.edge.form`, for each of its seven values. A lit third's tooltip is
> "{Third} third: click to leave it out" and an unlit one's "{Third} third: click to turn it on",
> and its accessible name is "{Edge} edge, {third} third, on" when lit and "{Edge} edge, {third}
> third, off" when not, where {Third} is the third's name (`viewmodel.ledger.thirds.names`) and
> {third} the same in lower case: "Top edge, left third, on".

> r[viewmodel.ledger.edge.third-click]
> Clicking a third asks `Core` to toggle that third (`crossing.derive.edge.toggle-third`). With
> only the start third lit, clicking it turns the peer's edge off, so the chip goes off and no third
> is lit; clicking the middle third then turns the edge on with the middle third alone, lit.

> r[viewmodel.ledger.edge.free]
> The chip of a `free` edge reads "{Edge}" and is off. Its tooltip is "{Edge} edge: click to wrap
> round to {name}", and its accessible name "{Edge} edge, wraps round, off".

> r[viewmodel.ledger.edge.free-click]
> Clicking a `free` chip asks `Core` to add that edge to the peer's wrap-around edges and, when the
> peer's edge is off, to turn it on with the thirds it kept (`crossing.derive.edge.off-keeps-thirds`):
> with the start and end thirds kept and the edge off, clicking the bottom edge's `free` chip makes
> the bottom edge wrap round and lights the start and end thirds.

> r[viewmodel.ledger.edge.on]
> The chip of an `on` edge reads "{Edge}, wraps" and is on. Its tooltip is "{Edge} edge wraps round
> to {name}; click to remove", and its accessible name "{Edge} edge, wraps round, on".

> r[viewmodel.ledger.edge.on-click]
> Clicking an `on` chip asks `Core` to remove that edge from the peer's wrap-around edges, and
> changes no other zone choice: the peer's other wrap-around edges, edge, thirds and corners stay.

> r[viewmodel.ledger.edge.kept]
> The chip of a `kept` edge reads "{Edge}, wraps" and is off, and its accessible name is "{Edge}
> edge, wraps round, kept". Its tooltip is "{Edge} edge wraps round to {name} once Edge is on;
> click to turn Edge on" while the peer's edge is off, and "{Edge} edge: none of it is free just
> now; click to remove" while it is on.

> r[viewmodel.ledger.edge.kept-click]
> Clicking a `kept` chip while the peer's edge is off asks `Core` to turn the edge on, with the
> thirds and wrap-around edges it kept, so a `kept` edge whose wrap-around zone can be derived
> becomes `on`. Clicking it while the edge is on asks `Core` to remove that edge from the peer's
> wrap-around edges.

> r[viewmodel.ledger.edge.taken]
> The chip of an edge `taken` by another peer reads "{Edge}, {holder}", where {holder} is that
> peer's shown name, and is disabled. Its tooltip is "{Edge} edge: {holder}'s", and its accessible
> name "{Edge} edge, held by {holder}". Clicking it asks `Core` for nothing.

### Thirds

> r[viewmodel.ledger.thirds.names]
> The thirds of a left or right edge are named, in the order start, middle, end, "Top", "Middle"
> and "Bottom"; those of a top or bottom edge "Left", "Middle" and "Right".

> r[viewmodel.ledger.thirds.phrase]
> A sentence names the picked thirds of an edge as "the {thirds} of the {edge} edge", where {edge}
> is "left", "right", "top" or "bottom" and {thirds} the picked thirds' names in lower case, in the
> order start, middle, end, joined as "top", "top and bottom" or "left, middle and right". When no
> picked third is known, it names the middle: "the middle of the top edge". With the end and start
> thirds of a left edge picked, it says "the top and bottom of the left edge".

### Corners

> r[viewmodel.ledger.corner.buttons]
> A row's Corners cell has four corner buttons, in the order top left, top right, bottom left,
> bottom right. Each is on while its corner is `on` for the peer, and off otherwise; a `taken` or
> `unavailable` corner's button is disabled.

> r[viewmodel.ledger.corner.kept-removable]
> A corner in the peer's zone choices whose state is `unavailable`, as when the peer's footprint
> has moved away from this machine's, shows as `on` and stays enabled, so that clicking it removes
> it.

> r[viewmodel.ledger.corner.names]
> A corner button's accessible name is "{Corner} corner, on" while `on`, "{Corner} corner, off"
> while `free`, "{Corner} corner, held by {holder}" while `taken` by the peer named {holder}, and
> "{Corner} corner, unavailable" while `unavailable`, where {Corner} is "Top left", "Top right",
> "Bottom left" or "Bottom right".

> r[viewmodel.ledger.corner.tooltips]
> A corner button's tooltip is "{Corner} corner: click to turn it off" while `on`, "{Corner}
> corner: click to turn it on" while `free`, "{holder}'s corner" while `taken`, and "Place {name}
> beside this Mac first" on a Mac, "Place {name} beside this PC first" on a PC, while
> `unavailable`.

> r[viewmodel.ledger.corner.click]
> Clicking an `on` corner button asks `Core` to remove that corner from the peer's corners, and a
> `free` one to add it, keeping the peer's other corners (`crossing.derive.corner.independent`),
> so the top-left and bottom-right corners can both be on. Clicking a `taken` or `unavailable`
> button asks `Core` for nothing.

### Notch

> r[viewmodel.ledger.notch.tick]
> On a Mac whose platform backend declares a notch (`crossing.zone.notch`), each row's Notch cell
> has a tick titled "Notch", ticked while the notch is chosen for the peer. Its accessible name is
> "Notch, on" while ticked and "Notch, off" while not, and its tooltip "Push into the notch to reach
> {name}".

> r[viewmodel.ledger.notch.choose]
> Ticking a row's "Notch" asks `Core` to choose the notch for that peer, which clears it for every
> other peer (`crossing.derive.notch.one`); unticking it asks `Core` to clear it for that peer.

> r[viewmodel.ledger.notch.none]
> On a Mac whose platform backend declares no notch, each row's notch tick has no title and is
> disabled, its accessible name is "Notch, no notch", and the Notch cell says "No notch".

> r[viewmodel.ledger.notch.return]
> On a Mac with a notch, a row whose peer has the notch chosen also has a tick "Return through the
> notch", ticked while return through the notch is on for that peer. Its accessible name is "Return
> through the notch, on" while ticked and "Return through the notch, off" while not, and its
> tooltip "Coming back from {name}, arrive just below the notch". A row whose peer does not have the
> notch shows no such tick.

> r[viewmodel.ledger.notch.return-choose]
> Ticking a row's "Return through the notch" asks `Core` to turn return through the notch on for
> that peer (`crossing.landing.notch-return`), and unticking it to turn it off.

### Keys

> r[viewmodel.ledger.keys.cycle-tick]
> A row's Keys cell has a tick titled with the shortcut key's name, as the shortcut settings name
> it, such as "Right Ctrl". It is ticked while the peer is ticked for the shortcut's cycle
> (`crossing.shortcut.cycle-ticks`), and its tooltip is "In the shortcut's cycle". Toggling it asks
> `Core` to tick or untick the peer, and changes no zone choice.

> r[viewmodel.ledger.keys.jump-shown]
> Beside the caption "Jump", a row's jump key control reads "Set a key" while the peer has no jump
> key. With one, it shows its chord: on a Mac, the symbols of its modifiers in the order ⌃ ⌥ ⇧ ⌘,
> with no space between them, then a space and the key's name, as "⌘ F18"; on a PC, the names of
> its modifiers, "Ctrl", "Win", "Alt" and "Shift", then the key's name, each apart from the next by
> a space, as "Alt F18". A key's name is one character in upper case, or the key's name with each
> word capitalised, as "F18" or "Page Down".

> r[viewmodel.ledger.keys.jump-words]
> With a jump key, the control's tooltip is "Press {key} to send input to {name}; press again to
> bring it home." and its accessible name "Jump straight here key, {key}", where {key} is the chord
> in words: on a Mac with the modifiers named "Control", "Option", "Shift" and "Command", as
> "Command F18"; on a PC as the control shows it. With none, the tooltip is "Choose a key
> combination to jump to {name} and back." and the accessible name "Jump straight here key, Choose
> a key combination".
>
> - parity: Python's Mac's tooltip with no jump key said "Press Choose a key combination to send input to {name}; press again to bring it home." (one wording on both machines, the PC's)

> r[viewmodel.ledger.keys.jump-arm]
> Clicking a row's jump key control starts recording: the control reads "Press a key combination…"
> and says "Click again to cancel". Clicking it again stops recording and leaves the jump key as it
> was. Recording goes on through any change the ledger shows meanwhile, such as a third clicked in
> the same row.

> r[viewmodel.ledger.keys.jump-one]
> Only one row records at a time: starting to record in a row stops recording in any other row,
> leaving that row's jump key as it was.

> r[viewmodel.ledger.keys.jump-recording]
> While a row records, the ledger tells `Core` that a jump key is being recorded
> (`crossing.jump-key.recording`), and once no row records, that none is. Moving the recording from
> one row to another tells `Core` nothing in between.

> r[viewmodel.ledger.keys.jump-record]
> While a row records, pressing a chord that `crossing.jump-key.chord` allows stops recording and asks
> `Core` to set it as the peer's jump key, which then shows in the control. A press that makes no
> such chord is ignored, and recording goes on. Escape pressed with no modifier stops recording and
> leaves the jump key as it was.

> r[viewmodel.ledger.keys.jump-refused]
> When `Core` refuses a recorded chord, the peer keeps its jump key, and the Crossing page warns:
> "that key is the trigger shortcut's: pick a chord without it" for a chord with the shortcut key
> (`crossing.jump-key.shortcut-clash`), "that key is already assigned to another machine" for
> another peer's chord (`crossing.jump-key.unique`), and "choose a modifier and one non-modifier
> key" for any other chord refused.
>
> - parity: Python's PC only announced the refusal, to assistive technology (one behaviour on both machines, the Mac's, so the refusal is seen)

> r[viewmodel.ledger.keys.clear]
> While the peer has a jump key and the row is not recording, the row offers "Clear", whose
> accessible name is "Clear jump key". Pressing it asks `Core` to clear the peer's jump key
> (`crossing.jump-key.clear`), and the control then reads "Set a key" and "Clear" goes.
>
> - parity: Python's Mac named it "Clear" alone (one wording on both machines, the PC's)

### Another machine's zones

While the Crossing page shows another machine's zones from the active profile, which machine it
shows being the `viewmodel.profiles` topic's, the ledger shows that machine's zone choices and
the states its edges and corners have for each peer (`crossing.profile.view.other-zones`).

> r[viewmodel.ledger.other.cells]
> While the Crossing page shows another machine's zones, the ledger's rows offer no "Rename", no
> "Leave out" or "Put back", and no Keys cell, and any rename or recording in progress stops. Its
> columns are headed "Machine", "Edges", "Corners" and "Notch" when that machine is a Mac, and
> "Machine", "Edges" and "Corners" otherwise.

> r[viewmodel.ledger.other.edits]
> While the Crossing page shows another machine's zones, a click on a chip, third, corner or notch
> tick asks `Core` to change that machine's zone choices (`crossing.profile.choices.edit-other`)
> as the same click does for this machine's, and this machine's zone choices stay as they were.

> r[viewmodel.ledger.other.notch]
> While the Crossing page shows a Mac's zones, each row's Notch cell has the "Notch" tick and, for
> the peer with the notch, the "Return through the notch" tick, as on that Mac, without "No notch".
>
> - parity: Python's PC titled the second tick "Return here" (one wording on both machines, the Mac's)

## Shortcut

The Crossing page's Shortcut section, and the ticks for the shortcut's cycle. When and where the
shortcut moves input is crossing's (`crossing.shortcut.defaults` and the rules after it).

### The Shortcut switch

> r[viewmodel.shortcut.switch-first]
> The Shortcut section's first control, under its heading "Shortcut", is a switch titled "Shortcut",
> on while the shortcut is on.

> r[viewmodel.shortcut.switch-own]
> Turning the Shortcut switch off or on asks Core to turn the shortcut off or on for this machine
> (`crossing.shortcut.method-off`), whichever peer is selected on the page, and changes no peer's
> zone choices and no peer's tick for the shortcut's cycle. Selecting another peer afterwards leaves
> the switch as it was.

> r[viewmodel.shortcut.detail]
> The Shortcut switch's detail, shown under its title and given as its description for
> accessibility, is "{key}, to where input last went" with the shortcut destination where input last
> went, "{key}, next in order" with next in order, and "{key}, next among ticked" with next among
> ticked, {key} being the shortcut key's name (`viewmodel.shortcut.key-names-pc`,
> `viewmodel.shortcut.key-names-mac`): with the default shortcut key and the destination next in
> order, "Right Ctrl, next in order" on a PC and "Right Option, next in order" on a Mac. It follows a new shortcut key or destination as soon as Core has it, also one chosen
> on this page.

### The shortcut key

> r[viewmodel.shortcut.key-names]
> Every key the viewmodel names by its key name, the shortcut key and the key of a jump key's chord
> included, is named in this machine's words: on a PC as `viewmodel.shortcut.key-names-pc` says, on a
> Mac as `viewmodel.shortcut.key-names-mac` says. So `ctrl_r` is "Right Ctrl" on a PC and "Right
> Control" on a Mac, `alt_r` is "Right Alt" on a PC and "Right Option" on a Mac, `f18` is "F18" and
> `page_down` is "Page Down" on both.

> r[viewmodel.shortcut.key-names-pc]
> On a PC, the shortcut key is named "Left Ctrl" and "Right Ctrl" for `ctrl` and `ctrl_r`, "Left
> Windows" and "Right Windows" for `cmd` and `cmd_r`, "Left Alt" and "Right Alt" for `alt` and
> `alt_r`, "Left Shift" and "Right Shift" for `shift` and `shift_r`, "Caps Lock", "F1" to "F20",
> "Left", "Up", "Right", "Down", "Insert", "Delete", "Home", "End", "Page Up", "Page Down", "Num Lock",
> "Scroll Lock", "Pause" and "Menu", by its key name (`input.key-name.names`, `input.modifier.pc-names`).

> r[viewmodel.shortcut.key-names-mac]
> On a Mac, the shortcut key is named "Left Control" and "Right Control" for `ctrl` and `ctrl_r`,
> "Left Command" and "Right Command" for `cmd` and `cmd_r`, "Left Option" and "Right Option" for `alt`
> and `alt_r`, "Left Shift" and "Right Shift" for `shift` and `shift_r`, "Caps Lock", and any other
> key by its key name with each word capitalised and `_` as a space: "F13", "Page Up", "Num Lock".

> r[viewmodel.shortcut.key-recorder]
> The Shortcut section shows the shortcut key's name, with the hint "Click, then press the key".
> Clicking it starts recording a key: it reads "Press a key…" and the hint "Click again to cancel",
> until a key is pressed or it is clicked again. Its name for accessibility is "Shortcut key, {key}".

> r[viewmodel.shortcut.key-recorded]
> A key pressed while the shortcut key is being recorded ends the recording and asks Core to make it
> the shortcut key. When Core takes it, the Shortcut section names the new key at once.
>
> - parity: Python's PC read "Press a key or button…" while recording, though only a key could be recorded (#16: one viewmodel for every platform; the Mac's)

> r[viewmodel.shortcut.key-refused]
> A key that can never be the shortcut key (`crossing.shortcut.key-allowed`,
> `crossing.shortcut.key-typing`) ends the recording and leaves the shortcut key as it was. On a PC,
> the Shortcut section then says, under the key, "That key cannot be the shortcut. Choose a modifier
> such as Right Ctrl, a function key, or a key like Insert or Scroll Lock."; a Mac says nothing.

> r[viewmodel.shortcut.key-clash]
> When Core refuses a new shortcut key because it clashes with a jump key
> (`crossing.shortcut.key-jump-clash`), the Shortcut section shows Core's message "That shortcut
> clashes with the jump key <chord>; the jump key has been kept.", on a Mac in place of the hint under
> "How you press it" and on a PC under the key, until a shortcut key is next taken.

> r[viewmodel.shortcut.order]
> The Shortcut section shows, from top to bottom: the Shortcut switch, the shortcut key, "How you
> press it", "Shortcut goes to", the note under it, "Time between taps" (in double-tap style only),
> and the hint under the style (`viewmodel.shortcut.style-hint`).

### How you press it

> r[viewmodel.shortcut.style]
> Under "How you press it", the Shortcut section offers "Double-tap" and "Hold", for the double-tap
> and the long press (`crossing.shortcut.long-press`), with the style in force chosen. Choosing one
> asks Core to set the shortcut's style.

> r[viewmodel.shortcut.style-hint]
> Under the style, the Shortcut section says "Tap twice to switch; tap twice again to come back." in
> double-tap style, and "Input is on another machine for as long as the key is held. If you jump
> while holding the shortcut, releasing it brings input home." in long-press style.

> r[viewmodel.shortcut.double-tap-window]
> In double-tap style only, the Shortcut section shows "Time between taps" with the double-tap window
> as "{value} ms", and setting it there asks Core to set the double-tap window
> (`crossing.shortcut.window-range`). In long-press style it is hidden.

### Shortcut goes to

> r[viewmodel.shortcut.destination]
> Under "Shortcut goes to", the Shortcut section offers "Last machine", "Next in order" and "Next
> among ticked", for the shortcut destinations where input last went, next in order and next among
> ticked, with the destination in force chosen, also after a restart. Choosing one asks Core to set
> the shortcut destination.

> r[viewmodel.shortcut.destination-last]
> With the destination where input last went, the note under "Shortcut goes to" says "With Hold,
> input goes to the last machine."

> r[viewmodel.shortcut.destination-next-long-press]
> With the destination next in order or next among ticked, in long-press style, the note under
> "Shortcut goes to" starts "With Hold, input goes to the first ready machine in order."

> r[viewmodel.shortcut.destination-next]
> With the destination next in order or next among ticked, in double-tap style, the note under
> "Shortcut goes to" starts "The order follows the tiles from top to bottom, then left to right. This
> Mac is always in the cycle.", with "This PC" for "This Mac" on a PC.

> r[viewmodel.shortcut.destination-ticked]
> With the destination next among ticked, the note under "Shortcut goes to" ends, after a space,
> "Choose which machines join the cycle with the tick boxes on their tiles."

### The shortcut's cycle

> r[viewmodel.shortcut.cycle-machines]
> With the destination next among ticked, each peer's row in the machines list shows a tick "In the
> shortcut's cycle", ticked while the peer is ticked for the shortcut's cycle
> (`crossing.shortcut.cycle-ticks`); with any other destination it does not show. It shows for every
> peer, and keeps showing when the peers' states are read again, as after a link comes up or goes
> down.

> r[viewmodel.shortcut.cycle-machines-in-use]
> A peer's "In the shortcut's cycle" tick in the machines list can be changed only while the peer is
> in use and not dormant.
>
> - parity: Python's Mac let the tick of a dormant peer in use be changed (#16: one viewmodel for every platform; the PC's)

> r[viewmodel.shortcut.cycle-ledger]
> Each peer's row in the ledger shows, whatever the destination, a tick titled with the shortcut key's
> name and described as "In the shortcut's cycle", ticked while the peer is ticked for the shortcut's
> cycle, a peer kept off the crossing map included.

> r[viewmodel.shortcut.cycle-untick]
> Unticking a peer's tick for the shortcut's cycle, in the machines list or in the ledger, asks Core
> to untick the peer for the shortcut's cycle (`crossing.shortcut.cycle-ticks`), and ticking it asks
> Core to tick it, also for a peer kept off the crossing map. Both ticks, and the map canvas, then
> show the peer's new tick.

## Profiles

Profiles (F7) as the Crossing page and the tray menu show them. What a profile is, which one is
active, own zones on every machine, keeping its own zones, saved zones and profile status are
`spec/crossing.md`'s (its Profiles and Own zones sections); these rules say only what the screens
show, enable and validate, and which command each action asks `Core` for. The Crossing page's map,
ledger and sections otherwise are `viewmodel.map`'s, `viewmodel.ledger`'s and `viewmodel.crossing`'s;
the rest of the tray menu is `viewmodel.tray`'s.

The **viewed machine** is the machine whose zone choices the Crossing page shows and edits: this
machine, or, while own zones on every machine is off, a machine picked on the map. "This Mac" on a
Mac and "This PC" on a PC name this machine wherever a rule quotes "This Mac".

### Own zones on every machine

> r[viewmodel.profiles.switch.shows]
> The Crossing page shows a switch "Own zones on every machine", on while own zones on every
> machine is on (`crossing.own-zones.every-machine`).
>
> - parity: Python showed the inverse switch, "Same ways on every machine", off by default (#154 makes the setting own zones on every machine, on until the user turns it off)

> r[viewmodel.profiles.switch.toggle]
> Turning the switch on or off asks `Core` to turn own zones on every machine on or off
> (`crossing.own-zones.every-machine`), and nothing else.

> r[viewmodel.profiles.switch.note]
> While own zones on every machine is on, the page shows under the switch "Turn this off to set
> every machine's ways from this screen. Turning it off changes nothing until you do; each
> machine's own ways are kept." While it is off, the note is not shown.
>
> - parity: Python said "Set every machine's ways from this screen. Turning it on changes nothing until you do; each machine's own ways are kept.", for its inverse switch (#154 inverts the setting)

> r[viewmodel.profiles.switch.hidden-while-on]
> While own zones on every machine is on, the page shows no profile tabs, no profile actions, no
> profile word on any footprint and no own-zones band, and the map does not pick the viewed
> machine.

> r[viewmodel.profiles.switch.on-returns-home]
> When own zones on every machine turns on while another machine is viewed, this machine becomes the
> viewed machine, and the ledger, the map and the page's sections are this machine's again.

> r[viewmodel.profiles.intro]
> While own zones on every machine is off, the Crossing page's introduction says "Click a machine on
> the map to set its ways. The ledger says how it reaches each other machine." and the hint under the
> map says "Click a machine to set its ways · Tab and Return pick one from the keyboard".

### Profile tabs

> r[viewmodel.profiles.tabs.listed]
> While own zones on every machine is off, the page shows one tab for each profile `Core` lists
> (`crossing.profile.listed`), oldest made first, and between profiles made at the same stamp, the
> lower profile id first.
>
> - parity: Python listed profiles in the order this machine first held them, which differs between machines (an order every machine shares, from what the store holds, #14)

> r[viewmodel.profiles.tabs.label]
> A profile's tab, its item in the Delete… menu and its item in the tray menu are labelled with its
> name; when two or more listed profiles have the same name, each of them is labelled "{name} ·
> {date}", the date it was made in this machine's local time as DD-MM-YYYY.

> r[viewmodel.profiles.tabs.active]
> The active profile's tab is marked as chosen, and no other tab is.

> r[viewmodel.profiles.tabs.click]
> Choosing a tab that is not the active profile's asks `Core` to make that profile active
> (`crossing.profile.active.shared`). Choosing the active profile's tab asks nothing.

> r[viewmodel.profiles.tabs.add]
> After the tabs, the page shows a "+" action whose accessible name is "New profile", which opens the
> profile name prompt for a new profile.

> r[viewmodel.profiles.tabs.menu]
> Each tab has a menu with "Rename", which opens the profile name prompt for that profile, and
> "Remove", which asks `Core` to remove it (`crossing.profile.removed`). "Remove" is enabled only
> while two or more profiles are listed and the tab's profile is not the active one.
>
> - parity: Python's PC titled the menu's second item "Delete" (the Mac's word is kept for both)

> r[viewmodel.profiles.tabs.switch-note]
> While own zones on every machine is off and two or more profiles are listed, the page says
> "Switching moves every machine to this profile's places and ways." under the map.

### Profile actions

> r[viewmodel.profiles.actions.shown]
> While own zones on every machine is off, the page shows three actions: "New profile…", "Rename" and
> "Delete…".

> r[viewmodel.profiles.actions.new]
> "New profile…" opens the profile name prompt for a new profile.

> r[viewmodel.profiles.actions.rename]
> "Rename" opens the profile name prompt for the active profile.

> r[viewmodel.profiles.actions.delete-enabled]
> "Delete…" is enabled only while two or more profiles are listed.

> r[viewmodel.profiles.actions.delete-menu]
> "Delete…" opens a menu with one item for each listed profile but the active one, in tab order.
> Choosing an item asks `Core` to remove that profile (`crossing.profile.removed`).

### The profile name prompt

> r[viewmodel.profiles.name.new]
> The prompt for a new profile is titled "New profile", with an empty field named "Profile name",
> and the actions "Add" and "Cancel".
>
> - parity: Python's PC showed an untitled field under the profile actions, with "Save" and "Cancel" (the Mac's prompt is kept for both)

> r[viewmodel.profiles.name.rename]
> The prompt for renaming a profile is titled "Rename profile", with the field "Profile name" holding
> the profile's name, and the actions "Save" and "Cancel".

> r[viewmodel.profiles.name.limit]
> The "Profile name" field takes at most 32 characters.

> r[viewmodel.profiles.name.submit]
> "Add", "Save", or Return in the field, asks `Core` to make the profile, or to rename it, with the
> field's text as it stands (`crossing.profile.make`, `crossing.profile.rename`). The prompt closes
> when `Core` takes the change, and stays open when it refuses.
>
> - parity: Python's Mac closed the prompt on any answer and asked nothing for a blank name (the PC's behaviour is kept for both)

> r[viewmodel.profiles.name.cancel]
> "Cancel" closes the prompt and asks `Core` for nothing.

> r[viewmodel.profiles.name.invalid]
> When `Core` refuses a profile name (`crossing.profile.name`), the page note says "Give the profile a
> name of 1 to 32 characters."

> r[viewmodel.profiles.name.full]
> When `Core` refuses to make a profile because 8 exist (`crossing.profile.make-limit`), the page
> note says "The desk is full. Remove an unused profile before adding more ways."
>
> - parity: Python's PC said "This desk is full. Remove an unused profile before adding more ways." (the Mac's words are kept for both)

> r[viewmodel.profiles.refused]
> When `Core` refuses any other profile change from the page, the page note says "These ways could
> not be changed."

> r[viewmodel.profiles.not-saved]
> When `Core` reports that a profile change could not be saved, the page note says "These ways could
> not be saved. Try again."

> r[viewmodel.profiles.note-cleared]
> The page note left by a refused profile change is gone once `Core` takes the next profile change
> from the page.

### Profile words on the map

> r[viewmodel.profiles.word.this]
> While own zones on every machine is off, this machine's footprint carries the profile word "Desk"
> while its zones come from the active profile, and "Own" while it keeps its own zones.

> r[viewmodel.profiles.word.offline]
> While own zones on every machine is off, a peer's footprint carries the profile word "Offline"
> while it has no link.

> r[viewmodel.profiles.word.peer]
> While own zones on every machine is off, a linked peer's footprint carries the profile word "Own"
> when its profile status says its zones come from its own zone choices
> (`crossing.profile.status.record`), and "Desk" otherwise.

> r[viewmodel.profiles.word.beside-outcome]
> A footprint's profile word takes the place of its platform and link status, and is shown beside its
> crossing outcome, never in its place: a peer whose zones cross shows "Crosses" and its profile
> word.

> r[viewmodel.profiles.word.description]
> The map's accessible description names each footprint's profile word after that footprint's own
> description.

### Picking the viewed machine on the map

> r[viewmodel.profiles.picker.click]
> While own zones on every machine is off, a click on a machine's footprint, this machine's
> included, makes that machine the viewed machine. It chooses no peer in the ledger, moves no
> footprint and asks `Core` for nothing.

> r[viewmodel.profiles.picker.click-third]
> While own zones on every machine is off, a click on a peer's footprint where it shows a third of a
> lit edge makes that peer the viewed machine and toggles no third.

> r[viewmodel.profiles.picker.cycle-tick]
> While own zones on every machine is off and this machine is viewed, a click on a footprint's tick
> for the shortcut's cycle toggles that tick and leaves the viewed machine as it was.

> r[viewmodel.profiles.picker.resize]
> While own zones on every machine is off, a drag of a footprint's resize handle, however short,
> resizes that footprint as on the Crossing page, asking `Core` for one change to the crossing map
> when it ends, and leaves the viewed machine as it was.

> r[viewmodel.profiles.picker.off]
> While own zones on every machine is on, this machine is always the viewed machine, and a click on a
> peer's footprint chooses that peer in the ledger, as on the Crossing page without profiles.

> r[viewmodel.profiles.picker.inactive]
> An inactive peer of this machine can never be the viewed machine: asking to view it leaves this
> machine viewed.

> r[viewmodel.profiles.picker.gone]
> When the viewed machine leaves the machines a profile covers (`crossing.profile.view.machines`),
> this machine becomes the viewed machine.

### The map's keyboard while it picks

> r[viewmodel.profiles.keys.order]
> While own zones on every machine is off, Tab and Shift-Tab move keyboard focus over every footprint,
> this machine's included, in reading order: rows of footprints top to bottom, and left to right
> within a row. Moving focus views no machine.

> r[viewmodel.profiles.keys.leave]
> Tab on the last footprint in reading order, or Shift-Tab on the first, moves keyboard focus off the
> map and views no machine.

> r[viewmodel.profiles.keys.pick]
> While own zones on every machine is off, Return or Space on the focused footprint makes its machine
> the viewed machine. It renames no machine, toggles no tick for the shortcut's cycle and moves no
> footprint.

### Viewing another machine

> r[viewmodel.profiles.other.ledger-heading]
> While own zones on every machine is off, the ledger's heading is "{name}'s ways", named for the
> viewed machine.

> r[viewmodel.profiles.other.header]
> While another machine is viewed, the ledger shows "Editing {name}'s ways ·" followed on the same line
> by the action "Back to This Mac", which makes this machine the viewed machine again.

> r[viewmodel.profiles.other.notes]
> While another machine is viewed, the ledger shows, in this order under the header: the viewed
> machine's status line, "Changes go to {name} at once." and "Jump keys and permissions are set on
> {name}."

> r[viewmodel.profiles.other.changes-note]
> "Changes go to {name} at once." is shown only while the viewed machine has a link.
>
> - parity: Python's PC showed it while the viewed machine was offline too (the Mac's behaviour is kept for both: the status line says an offline machine takes changes when it next connects)

> r[viewmodel.profiles.other.sections]
> While another machine is viewed, the page shows neither the pointer, resistance and shortcut
> sections nor the own-zones band; they return when this machine is viewed again.

> r[viewmodel.profiles.other.rows]
> While another machine is viewed, the ledger has one row for each machine on the viewed machine's
> map but itself, this machine's row named "This Mac", each showing the viewed machine's zone
> choices toward that machine (`crossing.profile.view.other-zones`).
>
> - parity: Python's PC named this machine's row with this machine's own name (the Mac's words are kept for both)

> r[viewmodel.profiles.other.no-local-controls]
> While another machine is viewed, the ledger's rows have no jump key, rename or keep-off control, and
> the ledger has no column for jump keys.

> r[viewmodel.profiles.other.notch]
> While another machine is viewed, the ledger shows the notch and return-through-the-notch controls
> only when the viewed machine is a Mac, as its profile status says or, without one, as its pairing
> recorded.

> r[viewmodel.profiles.other.edit]
> While another machine is viewed, a change made in the ledger or on the map to its zone choices
> toward a machine, the notch and return through the notch included, asks `Core` to edit the viewed
> machine's zone choices toward that machine in the active profile
> (`crossing.profile.choices.edit-other`), and never this machine's own zone choices.

> r[viewmodel.profiles.other.offline-edit]
> Changes to the zone choices of a viewed machine with no link are asked of `Core` as for a linked one.

> r[viewmodel.profiles.other.row-click]
> While another machine is viewed, a click on a ledger row chooses that row's machine as the ledger's
> chosen peer, and the viewed machine stays as it was.

> r[viewmodel.profiles.other.map-own]
> While another machine is viewed, the map shows the crossing map as the viewed machine's zones see
> it: the viewed machine's footprint stands where this machine's would, with the viewed machine's own
> link state and platform, and this machine's footprint shows "This Mac" with its own.

> r[viewmodel.profiles.other.inactive]
> While another machine is viewed, a peer this machine has kept off its crossing map, or that is
> inactive here, has no footprint and blocks no other footprint (`crossing.profile.view.other-zones`).

> r[viewmodel.profiles.other.set-elsewhere]
> While another machine is viewed, a footprint that does not touch the viewed machine's footprint
> shows the outcome "Set elsewhere" instead of a crossing outcome.

> r[viewmodel.profiles.other.drag]
> While another machine is viewed, dragging a footprint moves it on the crossing map as on the
> Crossing page, and leaves the viewed machine as it was.

> r[viewmodel.profiles.other.arrows]
> While another machine is viewed, the arrow keys move the selected footprint, the viewed machine's
> included, as on the Crossing page, and never move this machine's own footprint.

> r[viewmodel.profiles.other.no-machine-actions]
> While another machine is viewed, the map shows no tick for the shortcut's cycle, and renaming a
> machine or keeping it off the crossing map from the map does nothing.

> r[viewmodel.profiles.other.back]
> After "Back to This Mac", the ledger shows this machine's rows with their jump key, rename and
> keep-off controls, and the pointer, resistance and shortcut sections are shown again.

### The viewed machine's status line

> r[viewmodel.profiles.status.shown]
> While own zones on every machine is off, the ledger shows a status line for the viewed machine, this
> machine included; while it is on, none.

> r[viewmodel.profiles.status.offline]
> The status line of a viewed peer with no link says "Offline. It takes these when it next connects".

> r[viewmodel.profiles.status.sending]
> The status line of a linked viewed machine says "Sending…" while it has no profile status, or while
> its profile status names another profile than the active one or another stamp than that of its zone
> choices in the active profile (`crossing.profile.status.record`).

> r[viewmodel.profiles.status.last-heard]
> The status line of a linked viewed peer says "Last heard {time}" while no profile status has arrived
> from it since its link came up and the one this machine has was written before then, the time
> being that status's, in this machine's local time as DD-MM-YYYY HH:MM.

> r[viewmodel.profiles.status.own]
> The status line of a viewed machine whose profile status says its zones come from its own zone
> choices says "Keeps its own ways. The desk's are saved for when it uses them".

> r[viewmodel.profiles.status.waiting]
> The status line of a viewed machine whose profile status names peers it is not paired with says
> "Needs pairing with {name}", naming them as one list (`viewmodel.words.joined`).

> r[viewmodel.profiles.status.may-not-drive]
> When this machine is viewed and its zone choices in the active profile name peers it may not drive,
> its status line says "{name} is off on that machine", naming those peers as one list
> (`viewmodel.words.joined`).

> r[viewmodel.profiles.status.uses-desk]
> Otherwise, the status line says "Uses the desk's ways".

> r[viewmodel.profiles.status.order]
> The status line's cases apply in the order: no link, "Sending…" for no profile status, "Last
> heard", own zone choices, peers not paired, "Sending…" for another profile or stamp, peers it may
> not drive, "Uses the desk's ways".

### The own-zones band

> r[viewmodel.profiles.own.keep]
> While own zones on every machine is off, this machine is viewed and it does not keep its own zones,
> the page shows "Keep this machine's own ways", which asks `Core` to have this machine keep its own
> zones (`crossing.own-zones.this-machine`).

> r[viewmodel.profiles.own.kept]
> While own zones on every machine is off and this machine keeps its own zones, the page says "This Mac
> keeps its own ways" and shows "Use the desk's ways", which asks `Core` to have it stop keeping its
> own zones (`crossing.own-zones.stop`).

> r[viewmodel.profiles.own.share]
> While own zones on every machine is off and this machine keeps its own zones, the page shows "Share
> mine with the desk" only when its own zone choices differ from its zone choices in the active
> profile. It asks `Core` to share its own zones (`crossing.own-zones.share`).

> r[viewmodel.profiles.own.hidden]
> While own zones on every machine is on, or another machine is viewed, the page shows none of the
> own-zones band's actions, saved zones or toggle.

### Saved zones

> r[viewmodel.profiles.saved.toggle]
> While own zones on every machine is off, this machine is viewed and it has saved zones, the page
> shows the toggle "Earlier ways ▾", closed, which opens the list of saved zones and then reads
> "Earlier ways ▴"; choosing it again closes the list.

> r[viewmodel.profiles.saved.empty]
> With no saved zones, the page shows neither the toggle nor the list.

> r[viewmodel.profiles.saved.entry]
> The open list shows each set of saved zones, newest first, as "{date} · {reason}", the date it was
> saved in this machine's local time as DD-MM-YYYY, followed by the actions "Restore" and "Delete".

> r[viewmodel.profiles.saved.restore]
> "Restore" asks `Core` to restore that set of saved zones (`crossing.profile.saved.restore`).

> r[viewmodel.profiles.saved.delete]
> "Delete" asks `Core` to delete that set of saved zones (`crossing.profile.saved.delete`).

> r[viewmodel.profiles.saved.reason-edit]
> Zone choices saved by an edit made on this machine (`crossing.profile.saved.edit`) give the reason
> "Before you changed it".

> r[viewmodel.profiles.saved.reason-replaced]
> Zone choices saved because another machine changed them or made another profile active
> (`crossing.profile.saved.replaced`, `crossing.profile.saved.active-change`) give the reason "Before
> {name} changed it", naming the machine named in the saved zones, this machine by its own name, and a
> machine with no name here as "another machine".
>
> - parity: Python's Mac named this machine "This Mac" there (the PC's words are kept for both)

> r[viewmodel.profiles.saved.reason-restore]
> Zone choices saved by a restore (`crossing.profile.saved.restore`) give the reason "Before a
> restore".

> r[viewmodel.profiles.saved.reason-every-on]
> Zone choices saved because own zones on every machine turned on
> (`crossing.own-zones.every-machine-on`) give the reason "When Own zones on every machine went on".
>
> - parity: Python said "When Same ways went off" (#154 makes the setting own zones on every machine, the inverse)

> r[viewmodel.profiles.saved.reason-every-off]
> Zone choices saved because own zones on every machine turned off
> (`crossing.own-zones.every-machine-off`) give the reason "When Own zones on every machine went
> off".
>
> - parity: Python said "When Same ways went on" (#154 makes the setting own zones on every machine, the inverse)

### Way home

> r[viewmodel.profiles.home.shake]
> The Crossing page's "Way home" section shows the switch "Shake to come home", on while the shake is
> on in the shake settings in force (`crossing.profile.shake.in-profile`, `crossing.profile.shake.own`).
> Turning it on or off asks `Core` to turn the shake on or off.

> r[viewmodel.profiles.home.sensitivity]
> The "Way home" section shows "Shake sensitivity" as a ruler from 50 to 200 with the value in force
> written "{value} %". Setting the ruler asks `Core` to set the shake sensitivity to
> its value.

> r[viewmodel.profiles.home.sensitivity-keys]
> Each arrow key on the shake sensitivity ruler changes it by 10, or by 1 with Option held on a Mac or
> Alt held on a PC, never past 50 or 200, and asks `Core` to set the new value.

> r[viewmodel.profiles.home.shortcut]
> The "Way home" section says "Shortcut: {style} {key} ·", the style being "double-tap" or "hold"
> and the key the shortcut key's name as the shortcut section shows it, marked as a key, in the order
> the translated sentence puts them, followed by the action "Same → Shortcut".
>
> - parity: Python's PC wrote "Shortcut: double-tap {key} (set in Same → Shortcut)" as one sentence with a link (the Mac's line is kept for both)

> r[viewmodel.profiles.home.shortcut-open]
> "Same → Shortcut" opens the Overview page with keyboard focus on the shortcut's item of the settings
> shared with other machines.

> r[viewmodel.profiles.home.no-way-back]
> The "Way home" section shows one line "{name} has no way back to {machine} except the shortcut and a
> shake." for each machine the viewed machine's zones lead to that has no zone leading back to it
> (`crossing.profile.no-zone-home`), {machine} being the viewed machine's name, and shows nothing
> there when there is none.

### Profiles in the tray menu

> r[viewmodel.profiles.tray.shown]
> The tray menu has a "Profile" submenu only while own zones on every machine is off and two or more
> profiles are listed.

> r[viewmodel.profiles.tray.items]
> The "Profile" submenu has one item for each listed profile, in tab order and labelled as its tab is,
> and ticks the active profile's item only.

> r[viewmodel.profiles.tray.choose]
> Choosing an item of the "Profile" submenu that is not ticked asks `Core` to make its profile active
> (`crossing.profile.active.shared`).

> r[viewmodel.profiles.tray.unreadable]
> While the settings cannot be read, the tray menu has no "Profile" submenu.

### When the settings cannot be read

> r[viewmodel.profiles.unreadable]
> While the settings cannot be read, the Crossing page shows own zones on every machine on and "Shake
> to come home" on at 100 %, its switch for own zones on every machine and its shake controls are
> disabled, and it asks `Core` for nothing.

## Identify displays

What numbers the displays get, which machines show them, and for how long, are `display`'s; how
a number is drawn is `ui`'s.

> r[viewmodel.identify.button]
> The Crossing page offers "Identify screens", with the tooltip "Show these numbers on every linked
> machine's screens for 3 seconds.". It is enabled whatever this machine's peers and links, with
> no peer included.

> r[viewmodel.identify.press]
> Pressing "Identify screens" asks `Core` to identify displays, here and on the peers it has a link
> with, and changes no setting.

> r[viewmodel.identify.number]
> While displays are identified, each of this machine's displays shows its number in decimal
> digits, and no other words.

## Same on all machines

The Overview page's Settings module, and the lines it puts on the pages it keeps in step. Which
values each group keeps in step across the cluster, as shared entries, and what Revert restores, is
the settings domain's (#60); this is what the window shows and asks.

> r[viewmodel.settings.groups]
> Overview's Settings module says "Same on all machines" and offers four tick boxes in this order,
> "Crossing", "Design", "Shortcut" and "Names", each ticked while that group is kept in step, then
> "Everything", ticked while all four are.

> r[viewmodel.settings.groups-note]
> Under the four groups the Settings module says "Crossing shares resistance and drag protection.
> Design shares effects, colour, length, size and arrival animation. Shortcut shares shortcut on/off,
> trigger key and style, and destination. Names shares this machine's name and paired-machine
> labels."

> r[viewmodel.settings.tick]
> Ticking or unticking one group asks Core to keep in step exactly the groups then ticked, in the
> order Crossing, Design, Shortcut, Names. Ticking Everything asks for all four; unticking it asks for
> none.

> r[viewmodel.settings.everything-note]
> While all four groups are kept in step, "Everything" carries the note "Shares every group and can
> replace local values on this machine."; otherwise the note is not shown.

> r[viewmodel.settings.unpaired]
> On a PC with no peer, the five tick boxes cannot be changed.

> r[viewmodel.settings.state-note]
> Under the tick boxes the Settings module says "Off: each machine keeps its current values." while no
> group is kept in step, and otherwise "Shares {groups} with compatible paired machines.", with the
> groups kept in step named as their tick boxes are, in their order, joined by ", ": "Shares Crossing,
> Design with compatible paired machines."

> r[viewmodel.settings.state-detail]
> The state note's tooltip says "Connected machines with compatible Beamer versions update now; others
> catch up when they reconnect. Zones, sides, jump keys, full-screen hold, animation enable and
> appearance stay on each machine." and, on a Mac, adds "Notch and trackpad controls stay on this
> Mac."

> r[viewmodel.settings.own-zones-line]
> The Settings module says, on a Mac under the state note and on a PC under "Same on all machines"
> above the tick boxes, "Own zones on every machine: set on the Crossing page · On" while own zones on every machine is on (`crossing.own-zones.every-machine`), and "Own
> zones on every machine: set on the Crossing page · Off" while it is off.
>
> - parity: Python said "Ways: set on the Crossing page · On" while Same ways on every machine was on, and "· Off" while it was off (#154: own zones on every machine is the inverse of Same ways, so the line names it and its state)

> r[viewmodel.settings.revert]
> The Settings module shows a "Revert" button while this machine keeps the values that Same on all
> machines last replaced, and for at most 30 days after it saved them. Pressing it asks Core to put
> those values back.

> r[viewmodel.settings.scope-own]
> Under each of these pages' purpose, a line says whose settings they are:
>
> | Page | On a Mac | On a PC |
> |---|---|---|
> | Crossing | "Ways below lead to the selected machine. Its side is shared with that machine. Resistance, drag protection and Shortcut apply to this Mac's input for every destination." | "Ways below lead to the selected machine. Its side is shared with that machine. Resistance, drag protection and Shortcut apply to this PC's input for every destination." |
> | Design | "Used on this screen. Shared animation settings may follow another machine; Animate, window appearance, notch and trackpad controls stay on this Mac." | "Used on this screen. Shared animation settings may follow another machine; Animate and window appearance stay on this PC." |
> | Keyboard | "Modifier keys and Stays here belong to this Mac's input. Pointer speed and scrolling belong to this screen when another machine drives it. Every machine keeps its own." | "Modifier keys and Stays here belong to this PC's input. Pointer speed and scrolling belong to this screen when another machine drives it. Every machine keeps its own." |
>
> The Keyboard page's line never changes; the Crossing and Design pages' lines give way to
> `viewmodel.settings.scope-crossing` and `viewmodel.settings.scope-design`.

> r[viewmodel.settings.scope-crossing]
> While the Crossing group or the Shortcut group is kept in step, the Crossing page's line says
> "{shared} stay in step. Ways and jump keys below are local; each side is shared with its paired
> machine.", {shared} being "Resistance, drag protection and Shortcut" while both groups are,
> "Resistance, drag protection" while only Crossing is, and "Shortcut" while only Shortcut is.

> r[viewmodel.settings.scope-design]
> While the Design group is kept in step, the Design page's line says "Style, colour, length, size and
> landing animation stay in step. Animation enable and window appearance stay local." and, on a Mac,
> adds "Notch and trackpad controls stay local."

> r[viewmodel.settings.own-row]
> While any group is kept in step, each row that stays this machine's own carries the note "This Mac
> only." on a Mac or "This PC only." on a PC: Overview's "Hold this Mac's edges while an app on it is
> full screen" or "Hold this PC's edges while an app on it is full screen", and the Design page's
> Animate crossings, Appearance and, on a Mac, Trackpad. While no group is, the note is not shown.

## Keyboard page

The Keyboard page: keyboard stays here, the modifier style, the stays-here list and the titles of
its entries, and pointer and scroll speed. What each setting does to input is `spec/input.md`'s.

> r[viewmodel.input.keyboard-stays-here]
> The Keyboard page's "Keyboard sharing" module offers the switch "Mouse only", on while keyboard
> stays here is on (`input.keyboard-stays-here.keys`), with the note "Keep this machine's keyboard
> here. Pointer and clipboard sharing stay on." Turning it on or off asks Core to save keyboard stays
> here so. A change of keyboard stays here that arrives in a snapshot moves the switch and asks
> nothing of Core.

> r[viewmodel.input.keyboard-stays-here-refused]
> When Core reports that a change to keyboard stays here could not be saved, the switch shows the
> saved state again.

> r[viewmodel.input.modifier-choices]
> The Keyboard page's "Modifier keys" module offers the modifier styles as a choice, "Same
> shortcuts" for Semantic and "Same positions" for Positional and, on a PC only, "Mac keyboard
> layout" for Mac keyboard layout, the saved style chosen (`input.modifier.default-semantic`).
> Choosing one asks Core to save it as this machine's modifier style.

> r[viewmodel.input.modifier-note]
> Under the choice, the module says, for the saved style:
>
> | Style | On a Mac | On a PC |
> |---|---|---|
> | Semantic | "On a PC, Command arrives as Control, so Command-C copies there too, and Control arrives as the Windows key. Between two Macs every key arrives as itself." | "On a Mac, Ctrl arrives as Command and the Windows key as Control, so Ctrl+C copies there too. Between two PCs every key arrives as itself." |
> | Positional | "On a PC, each key arrives as the key in its place, so Command arrives as the Windows key. Between two Macs every key arrives as itself." | "On a Mac, each key arrives as the Mac key in the same place: Ctrl as Control, the Windows key as Command. Between two PCs every key arrives as itself." |
> | Mac keyboard layout | "Custom: the key map in settings.json is kept as it is." | "To a Mac: Alt becomes Command and Win becomes Option; Ctrl stays Control." |
> | any other, or a Mac's hand-written key map (`input.modifier.hand-map`) | "Custom: the key map in settings.json is kept as it is." | no other style is offered |

> r[viewmodel.input.stays-here-module]
> The Keyboard page's stays-here module is headed "Stays on this Mac" on a Mac and "Stays on this PC"
> on a PC, and lists the entries of this machine's stays-here list (`input.stays-here.list`) in the
> list's order, one row each, with the entry's title and a "Remove" button whose spoken name is
> "Remove {title}".

> r[viewmodel.input.stays-here-note]
> Above the list, a Mac says "These keys and buttons keep working on this Mac while its input is on
> another machine: a mouse's back button for this Mac's browser, say, or a volume key for its
> speakers." and, while the list is empty, adds under it "Nothing yet. Every key and button goes to
> the machine that has input." A PC says "These keep working on this PC while its input is on another
> machine: a mouse's back button for this PC's browser, say, or a volume key for its speakers." while
> the list has entries, and "Nothing yet. Every key and button goes to the machine that has input. Add
> one to keep it here: a mouse's back button for this PC's browser, say, or a volume key for its
> speakers." while it is empty.

> r[viewmodel.input.stays-here-recorder]
> Under the list, the button "Add a key or button" with the hint "Click, then press it" starts
> recording: it then reads "Press a key or button…" with the hint "Click again to cancel", and the next
> press it records (`viewmodel.input.record-mac-key` to `viewmodel.input.record-pc-button`) ends the
> recording. Clicking it again while recording ends the recording and records nothing.

> r[viewmodel.input.stays-here-add]
> A recorded entry that is not on the stays-here list yet is added at the end, and the page asks Core
> to save the list. A recorded entry already on the list changes nothing.

> r[viewmodel.input.stays-here-shortcut-key]
> Recording the shortcut key adds nothing, ends the recording, and the module says "That key is the
> shortcut; it always stays with Beamer."

> r[viewmodel.input.stays-here-remove]
> Pressing an entry's "Remove" takes that entry off the list, keeps the others in their order, and
> asks Core to save the list.

> r[viewmodel.input.stays-here-refused]
> When Core reports that a stays-here list could not be saved (`input.stays-here.refused`), as one of
> 65 entries is, the page shows the list as last saved and, on a PC, says "That could not be saved, so
> the list is unchanged." in the amber tone.

> r[viewmodel.input.record-mac-key]
> On a Mac, a recorded key press is a key entry for its Mac key code: Right Option records key 0x3D.
> A modifier key whose release arrives while recording, because it was already held when recording
> began, is not recorded, and recording goes on. Caps Lock is recorded at each of its toggles, as a
> press.

> r[viewmodel.input.record-mac-button]
> On a Mac, a recorded right button press is the button entry `right`; other buttons 2, 3 and 4, by
> macOS's numbering from 0, are `middle`, `back` and `forward`, and any higher other button is its
> number counting the left button as 1: other button 5 is `6`.

> r[viewmodel.input.record-mac-media]
> On a Mac, a recorded press of a media key or a brightness key is the entry for its key name
> (`input.media-key.mac-codes`): volume up is `volume_up`. Its release, and a system key Beamer gives
> no key name, are not recorded, and recording goes on.

> r[viewmodel.input.record-pc-key]
> On a PC, a recorded key press is a key entry for the virtual-key code the PC's capture reports for
> it, with its side for a modifier (`input.stays-here.pc-sided-modifier`): Right Ctrl records 0xA3.

> r[viewmodel.input.record-pc-left-ctrl]
> On a PC, a recorded Left Ctrl press records nothing yet: let go with no other key pressed, it
> records Left Ctrl, 0xA2; when another key is pressed while it is held, that key is recorded instead,
> so AltGr, which Windows reports as Left Ctrl then Right Alt, records Right Alt, 0xA5.

> r[viewmodel.input.record-pc-button]
> On a PC, a recorded press of the right, middle, back or forward button is the button entry `right`,
> `middle`, `back` or `forward`. A left click ends the recording and records nothing.

> r[viewmodel.input.title-mac]
> On a Mac, a stays-here entry's title is, for a key with a key name (`input.key-name.mac-named`,
> `input.modifier.mac-names`, `input.keypad.mac-codes`, `input.key-name.input-method-names`), its
> name's title from this table, and otherwise its key name's words with each underscore as a space
> and each word capitalised (`f13` is "F13", `numpad_enter` is "Numpad Enter"):
>
> | Key name | Title | Key name | Title |
> |---|---|---|---|
> | `alt` | Left Option | `alt_r` | Right Option |
> | `cmd` | Left Command | `cmd_r` | Right Command |
> | `ctrl` | Left Control | `ctrl_r` | Right Control |
> | `shift` | Left Shift | `shift_r` | Right Shift |
> | `caps_lock` | Caps Lock | `esc` | Escape |
> | `hangul` | Han/Yeong | `hanja` | Hanja |
> | `page_up` | Page Up | `page_down` | Page Down |
>
> For any other key, it is the character the Mac's keyboard layout types for that key unshifted, in
> upper case ("A" for key code 0x00 on a US layout), or "Key {code}" when the layout types none
> ("Key 9999").

> r[viewmodel.input.title-button]
> On a Mac and a PC, a button entry's title is "Right button", "Middle button", "Back button" or
> "Forward button" for `right`, `middle`, `back` and `forward`, and "Button {number}" for a numbered
> one ("Button 6").

> r[viewmodel.input.title-mac-media]
> On a Mac, a media key entry's title is "Volume Up", "Volume Down", "Mute", "Play/Pause", "Next Track"
> or "Previous Track" for `volume_up`, `volume_down`, `volume_mute`, `media_play_pause`, `media_next`
> and `media_prev`, and its key name as it is for any other (`brightness_up`).

> r[viewmodel.input.title-pc]
> On a PC, a key entry's title is Windows' own word for its virtual-key code:
>
> | Code | Title | Code | Title | Code | Title |
> |---|---|---|---|---|---|
> | 0x08 | Backspace | 0x09 | Tab | 0x0D | Enter |
> | 0x13 | Pause | 0x14 | Caps Lock | 0x15 | Han/Yeong |
> | 0x19 | Hanja | 0x1B | Esc | 0x20 | Space |
> | 0x21 | Page Up | 0x22 | Page Down | 0x23 | End |
> | 0x24 | Home | 0x25 | Left | 0x26 | Up |
> | 0x27 | Right | 0x28 | Down | 0x2C | Print Screen |
> | 0x2D | Insert | 0x2E | Delete | 0x5B | Left Windows |
> | 0x5C | Right Windows | 0x5D | Menu | 0x6A | Num * |
> | 0x6B | Num + | 0x6D | Num - | 0x6E | Num . |
> | 0x6F | Num / | 0x90 | Num Lock | 0x91 | Scroll Lock |
> | 0xA0 | Left Shift | 0xA1 | Right Shift | 0xA2 | Left Ctrl |
> | 0xA3 | Right Ctrl | 0xA4 | Left Alt | 0xA5 | Right Alt |
> | 0xA6 | Browser Back | 0xA7 | Browser Forward | 0xA8 | Browser Refresh |
> | 0xA9 | Browser Stop | 0xAA | Browser Search | 0xAB | Browser Favourites |
> | 0xAC | Browser Home | 0xAD | Mute | 0xAE | Volume Down |
> | 0xAF | Volume Up | 0xB0 | Next Track | 0xB1 | Previous Track |
> | 0xB2 | Stop | 0xB3 | Play/Pause | 0xB4 | Mail |
> | 0xB5 | Media Select | 0xB6 | Launch App 1 | 0xB7 | Launch App 2 |
> | 0xBA | ; | 0xBB | = | 0xBC | , |
> | 0xBD | - | 0xBE | . | 0xBF | / |
> | 0xC0 | ` | 0xDB | [ | 0xDC | \ |
> | 0xDD | ] | 0xDE | ' | | |
>
> and 0x70 to 0x87 are "F1" to "F24", 0x60 to 0x69 are "Num 0" to "Num 9", 0x30 to 0x39 and 0x41
> to 0x5A are the digit or capital letter of that code ("A" for 0x41), and any other code is "Key
> {code}" ("Key 254"). Every key name the PC's capture sends has a title, apart from the
> undifferentiated Shift, Control and Alt, 0x10, 0x11 and 0x12, which never enter the list
> (`input.stays-here.pc-sided-modifier`).

> r[viewmodel.input.speed]
> The Keyboard page's "Another machine's pointer here" module offers "Pointer speed" and "Scroll speed"
> as two controls from 25 % to 400 % (`input.speed.range`), each showing its value as a whole
> percentage with "%": "100%". Moving either asks Core to save this machine's pointer speed or scroll
> speed so.

> r[viewmodel.input.speed-note]
> Under pointer speed the module says "Pointer speed for another machine's mouse or trackpad while it
> drives this Mac." on a Mac and "For the trackpad or mouse of whichever machine drives this PC." on a
> PC.

> r[viewmodel.input.reverse]
> The module offers the switch "Reverse other machines' scrolling" on a Mac and "Reverse scrolling
> from other machines" on a PC, on while reverse scrolling is on (`input.speed.reverse`). Turning it on
> or off asks Core to save reverse scrolling so.

## Admin mode

A PC's admin mode switch, showing what `platform.admin.*` reports. A Mac has no admin mode
(`platform.capability.unsupported-os`) and shows none.

> r[viewmodel.input.admin-switch]
> On a PC, Overview's "Keyboard and mouse" module offers the switch "Control administrator windows",
> on while admin mode is on and off by default (`platform.admin.opt-in`), followed by the note "Use
> Windows permission prompts locally."

> r[viewmodel.input.admin-note]
> On a PC, under the switch, a note says why admin mode cannot be changed, and the switch cannot be
> changed while it does:
>
> | Admin mode reports | Words |
> |---|---|
> | unsupported, not the installed Beamer (`platform.admin.installed`) | "Install Beamer to control administrator windows." |
> | unsupported, not the signed-in account (`platform.login.pc-account`, `platform.login.pc-no-account`) | "This setting needs Beamer to run as the signed-in account." |
> | unsupported, a standard account (`platform.admin.administrator`) | "This setting needs a Windows administrator account." |
> | unsupported, User Account Control off (`platform.admin.uac`) | "Enable Windows User Account Control to use this setting." |
> | unsupported, rights not checked (`platform.admin.unchecked`) | "Windows could not check this account's administrator permission." |
> | its state could not be read | "Beamer could not read this PC's administrator setting. Restart Beamer to try again." |
>
> Otherwise the note says "Beamer will run as administrator on this PC. Changing this restarts
> Beamer; Windows may ask for permission."
>
> - parity: Python's note for a Beamer not run from its installed path added "This setting is unavailable in portable mode." (#11: the portable build, F23, is dropped)

> r[viewmodel.input.admin-change]
> On a PC, turning the switch on or off asks Core to change admin mode so. Until Core reports the
> outcome, the switch shows admin mode's earlier state and cannot be changed, and the note says
> "Waiting for Windows permission…".

> r[viewmodel.input.admin-cancelled]
> On a PC, when the change reports that it was cancelled (`platform.admin.cancelled`), the switch
> shows admin mode's earlier state and can be changed again, and the note says "Administrator
> permission was cancelled. Your previous setting has been kept." Beamer goes on running.

> r[viewmodel.input.admin-failed]
> On a PC, when the change reports that it failed or was refused (`platform.admin.task-refused`,
> `platform.admin.rollback`), the switch shows admin mode's earlier state and can be changed again,
> and the note says "The administrator setting could not be changed. Your previous setting has been
> kept."

> r[viewmodel.input.admin-same]
> On a PC, when the change reports that admin mode already had the state chosen
> (`platform.admin.same`), the switch and the note show admin mode as it is, and Beamer goes on
> running.

> r[viewmodel.input.blocker-words]
> On a PC, an input problem (`platform.blocker.reported`) is said in these words, {app} being the
> input blocker's name (`platform.blocker.name`) or "the foreground window", for its kind
> (`platform.blocker.kind`):
>
> | Kind | Words |
> |---|---|
> | GameInput's system window | "Windows blocked Beamer's input because {app} is GameInput's system window. Click a normal window on this PC to continue." |
> | a system-level window | "Windows blocked Beamer's input because {app} is a system-level window. Use it locally and click a normal window on this PC to continue." |
> | a window running as administrator | "Windows blocked Beamer's input because {app} is running as administrator. Click a normal window on this PC to continue." |
> | a window Beamer was not allowed to inspect | "Windows blocked Beamer's input while {app} was in front. Windows did not allow Beamer to inspect this window's process. Click a normal window on this PC to continue." |
> | an ordinary window | "Windows blocked Beamer's input while {app} was in front. Click a normal window on this PC to continue." |

> r[viewmodel.input.blocker-elevated]
> On a PC whose Beamer runs with administrator rights, an input blocker running as administrator is
> said in the ordinary window's words, naming it, never "is running as administrator".

## Permissions page

A Mac's Permissions page (F38). A PC has no Permissions page. The permissions themselves, asking for
them and relaunching are `platform.permissions.*`.

> r[viewmodel.permissions.mac-only]
> Only a Mac's settings window has a Permissions page.

> r[viewmodel.permissions.rows]
> The Permissions page has two rows, in this order: "Accessibility", with "Lets Beamer move this Mac's
> pointer and type on it when another machine drives.", and "Input Monitoring", with "Lets Beamer read
> this keyboard and trackpad, to send them to another machine."

> r[viewmodel.permissions.state]
> Each row says "Granted" in the signal tone while its permission is granted and "Required" in the
> amber tone while it is not, as `platform.permissions.mac-input` reports it.

> r[viewmodel.permissions.grant]
> While a row's permission is not granted, the row shows a "Grant" button; pressing it asks Core to
> ask macOS for that permission only (`platform.permissions.ask`). While it is granted, the row shows
> no button.

> r[viewmodel.permissions.login-note]
> Under the two rows, the page says, headed "At login": "On Overview, turn on Start Beamer when you log
> in. If macOS needs approval, that page shows the status and opens Login Items settings."

> r[viewmodel.permissions.capture-relaunch]
> While both permissions are granted but were not both granted when this run of Beamer started
> (`platform.permissions.relaunch-needed`), the page says "Both granted. Relaunch Beamer so macOS
> passes it the keyboard." in the amber tone and shows the button "Relaunch Beamer". In every other
> state it shows no such button.

> r[viewmodel.permissions.relaunch]
> Pressing "Relaunch Beamer" asks Core to relaunch Beamer for its permissions
> (`platform.permissions.relaunch`).

> r[viewmodel.permissions.capture-ready]
> While input capture runs, the page says "Input capture ready." in the signal tone.

> r[viewmodel.permissions.capture-retrying]
> While both permissions are granted, no relaunch is needed and input capture is not running, the
> page says "Retrying input capture…" in the fault tone (`input.capture.retry`).
>
> - parity: Python added macOS's own words for why capture failed, "Retrying input capture… {detail}" (#16: the OS's words reach only the log, `platform.capability.error-is-state`)

> r[viewmodel.permissions.capture-missing]
> While either permission is not granted, the page says "Beamer needs both, granted separately. macOS
> only delivers keyboard events to a process started after the grant, so relaunch Beamer once you have
> given them."

> r[viewmodel.permissions.follows]
> A permission granted or taken away while the Permissions page is shown changes its row, its button
> and the capture line as soon as Core reports it (`platform.permissions.change-seen`).

## Connection page

The Connection page (F39) and Hide addresses. The firewall check and its repair are
`platform.firewall.*`; the address to pair by is `discovery.own-address.offered`.

> r[viewmodel.connection.modules]
> The Connection page shows, in this order, on a Mac: Hide addresses, then this Mac's address; on a
> PC: the "Windows Firewall" module, the "Addresses" module with Hide addresses, then the "This PC"
> module with this PC's address. It offers no language choice, no port and no address to type.
>
> - parity: Python offered a language choice with "Relaunch Beamer" (#11: English only, F34); the Mac's page edited the first paired machine's address and port, showed its Wake-on-LAN hardware address, and had a "Connect" button, and the PC's took this PC's listening port with "Save and reconnect" (#14: one link port, `link.transport.port`, and a peer's chosen addresses set for each peer, `discovery.address.choose`)

> r[viewmodel.connection.address]
> The Connection page shows "This Mac's address" on a Mac and "This PC's address" on a PC, with the
> address Core gives to pair with this machine by address (`discovery.own-address.offered`), or "Not
> known yet" when it gives none.
>
> - parity: Python's Mac showed the first paired machine's address instead, and the PC showed the address it faced its peers on (#14: a peer's addresses are kept for each peer; the address shown is the one another machine types to pair)

> r[viewmodel.connection.hide-switch]
> The Connection page offers the switch "Hide addresses", off until the user turns it on, with the
> note "Hides every IP and hardware address in this window and its menu." on a Mac and "Hides every IP
> and hardware address in this window and the tray." on a PC. Turning it on or off asks Core to save it
> as this machine's own setting, which no peer receives. The switch works, and hides addresses, whether
> or not any peer is paired.

> r[viewmodel.connection.hide-text]
> While Hide addresses is on, every text that the settings window, the menu bar menu or the tray
> menu, their tooltips and Beamer's notifications show has each IPv4 address in it (four groups of one
> to three digits joined by dots) and each hardware address in it (six groups of two hexadecimal
> digits joined by `:` or `-`) replaced by "•••", and nothing else changed: "Desktop PC at
> 192.168.1.20:24820, woken by 02:1A:2B:3C:0D:4E" reads "Desktop PC at •••:24820, woken by •••", and
> "port 24820 and version 1.4.0" stays as it is.

> r[viewmodel.connection.hide-off]
> While Hide addresses is off, every text shows its addresses as they are.

> r[viewmodel.connection.hide-applies]
> A change to Hide addresses, made on the switch or by reloading the settings, shows at once on every
> page, menu and readout of an open settings window, without it being closed and opened again.

> r[viewmodel.connection.hide-typed]
> While Hide addresses is on, a field in which the user types an address shows what was typed as
> dots, and can still be edited. Turning Hide addresses off shows the typed text again, unchanged.

> r[viewmodel.connection.firewall-checking]
> On a PC, until the first firewall check reports, the "Windows Firewall" module says "Checking
> Windows Firewall…". While a check or a repair runs, its button reads "Checking…" and cannot be
> pressed.

> r[viewmodel.connection.firewall-check-again]
> On a PC, once a check reports, the "Windows Firewall" module offers the button "Check again", which
> asks Core to check the firewall again (`platform.firewall.check-times`).

> r[viewmodel.connection.firewall-ready]
> On a PC whose firewall reports ready (`platform.firewall.ready`), the module says "Windows Firewall
> lets your other machines reach Beamer on port 24820 on this private network, and lets a new machine
> discover this PC." while the LAN is on the Private profile, and the same with "this network" in
> place of "this private network" otherwise.

> r[viewmodel.connection.firewall-blocked]
> On a PC whose firewall reports a block rule (`platform.firewall.blocked`), the module says "Windows
> Firewall has a block rule affecting Beamer's input or pairing traffic; ask your administrator to
> review Beamer's inbound block rules in Windows Firewall."

> r[viewmodel.connection.firewall-missing]
> On a PC whose firewall reports Beamer's rule missing (`platform.firewall.missing`), on any profile,
> the module says "Beamer's firewall rule is missing. Repair adds it; Windows asks for administrator
> permission." and offers the button "Repair", which asks Core to repair the firewall
> (`platform.firewall.repair`).
>
> - parity: Python said "Beamer's firewall rules need attention; run the Beamer installer again to repair them for private networks.", on a domain network asked for an administrator's Domain-profile rules, and on a port other than 24820 asked for that port to be allowed (#21: the app's Repair runs the elevated setup, whose rule covers the Domain profile; #14: the port is always 24820)

> r[viewmodel.connection.firewall-public]
> On a PC whose firewall reports the LAN on the Public profile (`platform.firewall.network-public`),
> the module says "Windows treats this network as public; if you trust it, change its network profile
> to Private in Windows Settings." and offers no Repair.

> r[viewmodel.connection.firewall-failed]
> On a PC whose firewall check failed (`platform.firewall.check-failed`), the module says "Beamer could
> not check Windows Firewall. Check again; if the problem continues, run the Beamer installer again to
> repair its rules." in the fault tone.

> r[viewmodel.connection.firewall-permission]
> On a PC whose firewall check was refused for lack of permission (`platform.firewall.check-permission`)
> while Beamer runs without administrator rights, the module says "Beamer cannot check Windows
> Firewall without administrator permission. Repair asks for it." in the fault tone and offers
> "Repair". While Beamer runs with administrator rights, the same refusal is said in the words of
> `viewmodel.connection.firewall-failed`, never asking for a permission Beamer has.
>
> - parity: Python said "Beamer cannot check Windows Firewall without administrator permission; run the Beamer installer again to repair its rules." and offered only Check again (#21: the app's Repair runs the elevated setup)

## Start at login

The Overview page's Start at login module, showing the state `platform.login.*` reports.

> r[viewmodel.settings.login-mac-switch]
> On a Mac, Overview's "At login" module offers the switch "Start Beamer when you log in", on while
> Beamer's login item is enabled or waiting for the user's approval (`platform.login.mac-state`). The
> switch can be changed only while Beamer.app is in /Applications or ~/Applications, or while the
> login item is enabled or waiting for approval (`platform.login.mac-applications`,
> `platform.login.mac-moved`).

> r[viewmodel.settings.login-mac-status]
> Under the switch, the module says the login item's status, the first that holds:
>
> | State | Words |
> |---|---|
> | waiting for approval, Beamer.app in Applications | "Login item status: Requires approval. Allow Beamer in System Settings, General, Login Items." |
> | waiting for approval, elsewhere | "Login item status: Requires approval. Move Beamer to Applications and reopen it before allowing it in Login Items." |
> | Beamer.app not in Applications | "Login item status: {status}. Move Beamer to Applications and open it there to start at login.", {status} being "Not registered", "Enabled" or "Not found" |
> | not found | "Login item status: Not found. Check Beamer is installed in Applications, then turn it on again." |
> | enabled | "Login item status: Enabled. Beamer will start when you log in." |
> | not registered | "Login item status: Not registered. Turn on Start Beamer when you log in." |

> r[viewmodel.settings.login-mac-approval]
> On a Mac, the button "Open Login Items settings" shows only while Beamer's login item waits for
> approval, and pressing it opens Login Items in System Settings (`platform.login.mac-approval`).

> r[viewmodel.settings.login-pc-switch]
> On a PC, Overview's "At sign-in" module offers the switch "Start Beamer when you sign in", on while
> Start at login is on (`platform.login.pc-trigger`). It can be changed only while Beamer runs from
> its installed path and either runs as the signed-in account or Start at login is on.

> r[viewmodel.settings.login-pc-hint]
> On a PC, under the switch, the module says the first that holds:
>
> | State | Words |
> |---|---|
> | Windows names no signed-in account (`platform.login.pc-no-account`) | "Windows could not identify the signed-in account, so start at sign-in is unavailable." |
> | Beamer runs as another account (`platform.login.pc-account`) | "Start at sign-in needs Beamer to run as the signed-in account." |
> | on, admin mode on | "Starts in the tray when you sign in, as administrator." |
> | on, admin mode off | "Starts in the tray when you sign in, without an administrator prompt." |
> | off, admin mode on | "When enabled, Beamer starts in the tray as administrator." |
> | off, admin mode off | "When enabled, Beamer starts in the tray with your normal permissions." |
> | not run from its installed path | "Only the installed app can start at sign-in." |

> r[viewmodel.settings.login-pc-changed]
> On a PC, once a change to Start at login succeeds, the module says "In the tray, with no window."

## Design page

The Design page: how a crossing looks and feels on this machine, and design follow. How the edge
glow and the landing animation are drawn is `ui`'s. The crossing effects (F30) and the notch island
and beam (F31) come later (#11), so the page offers the Light styles only.

> r[viewmodel.design.modules]
> The Design page shows, in this order: "On screen", "Style", "Colour", on a Mac "Trackpad", then
> "Appearance" and "Match design with".

> r[viewmodel.design.animate]
> "On screen" offers the switch "Animate crossings on this Mac" on a Mac and "Animate crossings on this
> PC" on a PC, with the note "Turns crossing and landing animations on or off on this screen. Saved
> style choices stay in place." Turning it off keeps the chosen style and colour.

> r[viewmodel.design.landing]
> "On screen" also offers the switch "Show where the pointer lands", with the note "For shortcut, menu
> and jump arrivals; requires Animate crossings on this screen."

> r[viewmodel.design.styles]
> "Style" offers one group of styles, headed "Light": "Glow", with the detail "Quiet", and "Beam",
> with the detail "Medium". Under the tiles the chosen style's name is followed by what it does:
> "A band of light that deepens the harder you push." for Glow, "A thin line with a comet of light
> running along it." for Beam.
>
> - parity: Python offered Aperture in the Light group and the Membrane, Sparks, Instrument, Folio and Selvedge groups of crossing effects, each Quiet, Medium and Showpiece, falling back to Glow and Beam only when the effects could not load (#11: the crossing effects, F30, are Later)

> r[viewmodel.design.style-hint]
> "Style" says "Hover to preview" above its tiles.

> r[viewmodel.design.style-for]
> "Style" offers "Style for": "Crossing" or "Shortcut and menu", Crossing when the page opens. Each
> choice shows only its own tiles: the crossing styles for Crossing, the switch styles for Shortcut
> and menu, every time it is chosen.

> r[viewmodel.design.switch-styles]
> Under Shortcut and menu, "Style" offers one group, headed "Simple": "Same as crossing", with the
> detail "Plays Glow", and "Ring", with the detail "Closes onto the pointer", and says "What plays
> around the pointer for shortcut, menu and jump arrivals. Same as crossing uses this screen's
> crossing style."
>
> - parity: Python also offered Aperture and the crossing effects' groups as switch styles (#11: the crossing effects, F30, are Later)

> r[viewmodel.design.one-choice]
> Choosing a style tile chooses that style for the "Style for" choice shown, and the tile is shown
> chosen. Setting the chosen style from a snapshot shows it chosen without asking Core for a change.

> r[viewmodel.design.show-at]
> "Style" offers "Show at": "Edge" and "Corner", and on a Mac "Notch" too. Every tile shows its style
> at the place chosen.

> r[viewmodel.design.preview-place]
> When the Design page opens, and whenever this machine's zone choices change, "Show at" chooses the
> first place a zone of this machine crosses at: "Edge" when any peer's edge is on, whole or by its
> thirds; else "Corner" when any corner is on; else, on a Mac, "Notch" when the notch is on; else
> "Edge". Once the user chooses a place by hand, it stays chosen for the rest of the run.

> r[viewmodel.design.place-note]
> Under "Show at", while no zone of this machine crosses at the place chosen, the page says "{Place}
> is not one of your ways in, so this only shows how it would look. Turn it on under Crossing." on a
> Mac and "{Place} is not one of this PC's ways in, so this only shows how it would look. Turn it on
> under Crossing." on a PC, {Place} being "The edge", "A corner" or "The notch". While one does, it
> says nothing there. `viewmodel.design.place-no-notch` comes before this rule.
>
> - parity: Python's Mac, with the notch chosen, added "At the notch, Glow and Beam do not draw: the notch style you choose here plays instead, and their tiles show it." (#11: the notch island and beam, F31, are Later)

> r[viewmodel.design.place-no-notch]
> On a Mac whose displays have no notch, with "Notch" chosen under "Show at", the page says "This Mac
> has no notch, so this only shows how it would look.", whatever its zones.

> r[viewmodel.design.colours]
> "Colour" offers two groups of five colours: "Light", with "Signal", "Colourful", "Ocean", "Sunset"
> and "Aurora", and "Mono", with "White", "Silver", "Graphite", "Ink" and "Warm grey". One colour is
> chosen across both groups. The module says "Any colour works with any style." on a Mac and "For the
> crossing and the shortcut alike" on a PC.
>
> - parity: Python also offered the Membrane, Sparks, Instrument, Folio, Selvedge and Bright colour packs, and the Mac's note added "The notch's Outline uses it too; Island keeps Beamer's own cyan." (#11: the colour packs, F30, and the notch island and beam, F31, are Later)

> r[viewmodel.design.frame-rate]
> "Style" offers "Frame rate": "Match display", "Half" or "Quarter", in this order, the saved one
> chosen. Choosing one asks Core to save it.

> r[viewmodel.design.trackpad]
> On a Mac, "Trackpad" offers the switch "Tick as the push builds", "Ticks at": "Every quarter",
> "Halfway" or "Only when through", and the button "Try it" (`platform.haptics.tick`).

> r[viewmodel.design.appearance]
> "Appearance" offers "This window": "System", "Light" or "Dark", with the note "System follows your
> Mac's light or dark setting." on a Mac and "System follows Windows' light or dark setting." on a PC.
> Choosing one asks Core to save it as this machine's own setting.

## Design follow

"Match design with": this machine following a peer's design. Which design values follow, and when,
is the settings domain's (#60).

> r[viewmodel.design.follow-choices]
> "Match design with" offers "None", then each peer that has a link now, in the order of the peer
> list, and the followed peer also while it has no link, each by its name. Its chosen entry is the
> followed peer, or None.

> r[viewmodel.design.follow-unavailable]
> A peer whose design this machine has not received yet is listed but cannot be chosen, with the
> tooltip "Design not available yet."
>
> - parity: Python's words were "Design not available yet: connect to this machine with Beamer 1.5.0 beta.6 or later.", and a peer of an older version was listed as "{name} — Needs beta.6 or later" (#11: no compatibility with other versions; a peer of another version has no link, `link.version.alpn`)

> r[viewmodel.design.follow-choose]
> Choosing a peer asks Core to follow that peer's design; choosing None asks Core to stop following
> and keep this machine's own design.

> r[viewmodel.design.follow-note]
> Under the choice, the module says the first that holds:
>
> | State | Words |
> |---|---|
> | following, the Design group of Same on all machines on | "Following {name}, paused while Same on all machines is on." |
> | following a peer that has no link | "Keeping the last design received from {name} until it reconnects." |
> | following a peer that has a link | nothing |
> | following none, no peer has a link | "No paired machine is connected." |
> | following none, no listed peer can be chosen | "Design not available yet." |
> | following none | "None: this machine keeps its own Design." on a Mac, "None: this PC keeps its own Design." on a PC |
>
> - parity: Python said "A connected machine is unavailable until it has beta.6 or later." or "Design not available yet: connect to this machine with Beamer 1.5.0 beta.6 or later." when no listed peer could be chosen (#11: no compatibility with other versions)

> r[viewmodel.design.follow-lock]
> While this machine follows a peer's design and the Design group of Same on all machines is off, the
> style, the switch style, the colour, the frame rate and "Show where the pointer lands" cannot be
> changed, and "Style" shows the banner "Following {name}'s design" with the button "Stop following",
> which asks Core to stop following. Otherwise those can be changed and no banner shows.

> r[viewmodel.design.follow-follows]
> "Match design with" follows the snapshot: a peer whose link comes up is added, one whose link goes
> down is taken off unless it is followed, a peer's design arriving makes it choosable, and a peer
> renamed shows its new name.

## Audio page

The Audio page (F43) shows every machine's microphones and outputs, the shared microphone, and
where each machine's sound plays. It is drawn from `Core`'s audio state: the audio routes, each
machine's devices, and the status of each audio route. Until `spec/audio.md` (#62) names those
states, the rules below describe them in glossary words. The Camera section and the camera's marks
on the audio map are `viewmodel.camera`'s; the page's place in the page list is
`viewmodel.pages`'s.

### Sections and words

> r[viewmodel.audio.sections]
> The Audio page shows, from top to bottom: the "Microphone" section, the Audio page's notices, the
> Camera section, the "Machines" section with the audio map, and the selected machine's section,
> headed by that machine's name.

> r[viewmodel.audio.unavailable]
> While `Core` reports that audio is not running on this machine yet, the Audio page shows only an
> "Audio" section saying "Audio is not running in this Beamer yet. Microphones and speakers show
> here once it is.", and shows the other sections as soon as audio runs.
>
> - parity: the Mac's page showed every section with this machine marked "No audio" instead; only the PC's page showed this sentence (#16: one viewmodel for both machines, taking the PC's words)

> r[viewmodel.audio.names.own]
> The Audio page names this machine "This Mac" on a Mac and "This PC" on a PC, wherever it names a
> machine.

> r[viewmodel.audio.names.peer]
> The Audio page names every other machine by its label (`viewmodel.machines.label.name`,
> `viewmodel.machines.label.shared`, `viewmodel.machines.label.no-name`,
> `viewmodel.machines.label.unknown`): a machine it has no name for is "Machine · " followed by its
> id tail, the last four characters of its machine id in unpadded URL-safe base64.
>
> - parity: Python's Audio page used the last six characters of the machine id (one naming on every page, `viewmodel.machines.label.no-name`)

> r[viewmodel.audio.names.list]
> Where the Audio page names several machines in one phrase, it joins their names as
> `viewmodel.words.joined` says: one name alone, two as "A and B", and three or more as "A, B and C".

> r[viewmodel.audio.tone]
> Each status line and notice of the Audio page carries a tone: quiet, plain, live, waiting or
> fault.

### The shared microphone

> r[viewmodel.audio.mic.label]
> The Microphone section's selector is labelled "Microphone your apps hear", and under it the page
> says "Apps use 'Beamer Microphone' on a Mac and 'CABLE Output' on a PC."

> r[viewmodel.audio.mic.choices]
> The microphone selector lists "Off" first, then one heading per machine whose audio devices
> `Core` knows, this machine first and the others in the audio map's reading order. Under each
> heading come "Default (<name of that machine's default microphone>)", or "Default microphone"
> when it has none, then each of its microphones by name.

> r[viewmodel.audio.mic.heading-not-a-choice]
> A machine's heading in the microphone selector and the output selector names the machine and
> cannot be chosen: choosing it asks nothing of `Core`.

> r[viewmodel.audio.mic.no-virtual]
> The microphone selector and the selected machine's microphones leave out every microphone whose
> name starts with "Beamer Microphone" or "CABLE Output": those are the virtual microphone's other
> end.

> r[viewmodel.audio.mic.offered]
> A machine's entries in the microphone selector can be chosen only while that machine is in the
> cluster and its audio is running or starting; otherwise they are listed but disabled. "Off" can
> always be chosen.

> r[viewmodel.audio.mic.selected]
> The microphone selector marks exactly one entry: "Off" while no microphone is shared, the
> machine's default entry while the audio route shares that machine's default microphone, and the
> microphone's own entry while it shares a named microphone.

> r[viewmodel.audio.mic.found-by-name]
> When no microphone of the shared microphone's machine has the audio route's device key, the
> selector marks the microphone of that machine with the audio route's device name, and it can be
> chosen.

> r[viewmodel.audio.mic.unplugged]
> While the shared microphone is a named microphone its machine no longer lists, the selector shows
> it under that machine's heading as "<name> (not connected)", disabled and still marked.

> r[viewmodel.audio.mic.unknown-machine]
> While the shared microphone's machine is not among the machines whose devices `Core` knows, the
> selector ends with that machine's heading and one disabled, marked entry: the microphone's name,
> or "Default microphone".

> r[viewmodel.audio.mic.closed-title]
> The closed microphone selector shows the marked entry as "<machine> · <entry>", such as "This
> Mac · Default (MacBook Air Microphone)", and "Off" as "Off".

> r[viewmodel.audio.mic.stable-entries]
> Each entry of the Audio page's selectors keeps one identity, made from its machine and device,
> across every new state `Core` reports, so the marked entry remains marked while the audio route is
> unchanged. No two entries of one selector share an identity.

> r[viewmodel.audio.mic.choose-off]
> Choosing "Off" in the microphone selector asks `Core` to share no microphone.

> r[viewmodel.audio.mic.choose-default]
> Choosing a machine's default entry in the microphone selector asks `Core` to share that machine's
> default microphone, whichever it is when the audio route is used.

> r[viewmodel.audio.mic.choose-device]
> Choosing a microphone in the microphone selector asks `Core` to share that microphone of that
> machine, named by its device key and its name.

> r[viewmodel.audio.mic.meter]
> The Microphone section shows the shared microphone's level while its machine lists that
> microphone, and no level meter otherwise.

### The microphone's status line

> r[viewmodel.audio.mic.listeners]
> The machines that hear the shared microphone are those `Core`'s status of the microphone's audio
> route names; when it names none, they are the machines a microphone stream from it runs or starts
> to, followed by the microphone's own machine while an app there records its virtual microphone.
> Each machine counts once.

> r[viewmodel.audio.mic.line-off]
> While no microphone is shared, or the microphone's audio route is off, the Microphone section says
> "No microphone is shared: apps that record it hear silence." in the quiet tone.

> r[viewmodel.audio.mic.line-live]
> While the shared microphone streams and at least one machine hears it, the Microphone section says
> "<microphone> on <machine> is live for <listeners>." in the live tone, such as "MacBook Air
> Microphone on This Mac is live for lebig and This Mac."

> r[viewmodel.audio.mic.line-ready]
> While the shared microphone is ready, or streams to no machine, the Microphone section says
> "<microphone> on <machine> is ready. It turns on when an app starts recording." in the plain tone.

> r[viewmodel.audio.mic.line-unplugged]
> While the shared microphone waits for its device, the Microphone section says "<microphone> is
> not connected to <machine>. Beamer uses it again as soon as it is back." in the waiting tone.

> r[viewmodel.audio.mic.line-machine-offline]
> While the shared microphone waits for its machine, the Microphone section says "<machine> is
> offline. Its microphone comes back with it." in the waiting tone.

> r[viewmodel.audio.mic.line-unreachable]
> While a machine recording the shared microphone has no direct link to the microphone's machine,
> the Microphone section says "<listeners> cannot hear <machine>: they are not linked to it
> directly.", or "A machine recording it is not linked to <machine> directly." when no listener is
> known, in the waiting tone.

> r[viewmodel.audio.mic.line-no-virtual]
> While a machine recording the shared microphone has no virtual microphone, the Microphone section
> says "<listeners> cannot hear <machine>: no virtual microphone there.", or "A machine recording
> it has no virtual microphone." when no listener is known, in the waiting tone.

> r[viewmodel.audio.mic.line-failed]
> While the shared microphone could not start, the Microphone section says "<microphone> on
> <machine> could not start: <reason>." in the fault tone, or "<microphone> on <machine> could not
> start." when `Core` gives a reason the page has no words for.

> r[viewmodel.audio.reasons]
> The Audio page words the reasons an audio route could not start as: the device is gone "the
> device is gone"; a device Beamer leaves alone "Beamer does not use that device"; no virtual
> microphone "no virtual microphone there"; the device taken by another app "another app holds the
> device"; permission refused "the system does not allow it"; not supported "that machine cannot do
> it"; the port taken "the network port is taken"; audio stopped on the machine "the audio helper
> stopped"; the output muted "its output is muted there"; the machine's sound cannot be recorded
> "that machine cannot capture its sound".

> r[viewmodel.audio.mic.device-name]
> In the Microphone section's line and on the audio map, the shared microphone is named by the
> device it resolves to on its machine, else by the audio route's device name, else as "its default
> microphone".

### The audio map

> r[viewmodel.audio.map.machines]
> The audio map shows this machine and every machine whose audio devices `Core` knows, each once,
> and no other machine.

> r[viewmodel.audio.map.place]
> Each machine on the audio map sits at its footprint on the crossing map, in map units.

> r[viewmodel.audio.map.unplaced]
> Machines without a footprint sit in one line below the placed ones, this machine first and the
> others in the order `Core` lists them: the first at the smallest left of the placed footprints and 24 map units below
> their lowest bottom, each the median width and height of the placed footprints (the larger middle
> one for an even count), 24 map units apart. With nothing placed, they start at 0, 0 and are
> 160 by 100.

> r[viewmodel.audio.map.reading-order]
> The audio map's reading order is by lines from top to bottom, each from left to right. Taking the
> machines by their centres from top to bottom, a machine joins the current line when its centre is
> at most half the smaller of the two heights below the centre of that line's first machine, and
> otherwise starts a new line.

> r[viewmodel.audio.map.select-default]
> The selected machine is this machine until the user selects another on the audio map, and becomes
> this machine again when the selected one leaves the audio map.

> r[viewmodel.audio.map.select-click]
> Clicking a machine on the audio map selects it: the selected machine's section then shows that
> machine.

> r[viewmodel.audio.map.select-keys]
> On the audio map, an arrow key selects the nearest machine in that direction: among the machines
> whose centre lies further along that direction, the one with the least distance along it plus
> twice the distance across it. With none in that direction, Left and Up select the previous machine in
> reading order and Right and Down the next, and Home and End select the first and the last.
>
> - parity: the Mac's map stepped through reading order only, and had no Home or End (#16: one viewmodel for both machines, taking the PC's keys)

> r[viewmodel.audio.map.hint]
> Under the audio map the page says "Click a machine, or use the arrow keys, to see its devices and
> choose where its sound plays."
>
> - parity: the Mac's page said "Click a machine to see its devices and choose where its sound plays." although its arrow keys worked too (#16: one viewmodel for both machines)

> r[viewmodel.audio.map.legend]
> The audio map's legend reads "Sound", "Microphone" and "Waiting", with "Camera" between
> "Microphone" and "Waiting" while the Camera section shows.

> r[viewmodel.audio.map.title]
> Each machine on the audio map shows its name.

> r[viewmodel.audio.map.subtitle-own]
> Under its name, this machine on the audio map shows "<its own name> · <platform>", such as
> "MacBook Air · macOS", or the platform alone when it has no name; the platform is "macOS" or
> "Windows".
>
> - parity: Python also had "Linux" and, for an unknown platform, "Computer" (Linux is out of scope, #1)

> r[viewmodel.audio.map.subtitle-peer]
> Under its name, every other machine on the audio map shows "<platform> · Linked" while it is in
> the cluster and "<platform> · Offline" while it is not.

> r[viewmodel.audio.map.subtitle-audio]
> A machine's line under its name says instead "Audio starting…" while its audio starts, or while
> this machine's audio runs or starts but has not listed its devices yet; "Audio unavailable" while
> its audio has failed or is missing; and "No audio" while `Core` knows none of its devices
> otherwise.

> r[viewmodel.audio.map.mic-line]
> On the audio map, the shared microphone's machine shows the shared microphone's name; each other
> machine that hears it while it streams shows "Hears <machine>" in the live tone; any other machine
> shows the name of its own default microphone in the quiet tone.

> r[viewmodel.audio.map.output-line]
> Each machine on the audio map shows where its sound plays: "Sound plays here" (plain) while it
> plays on that machine, and while it is sent to another machine "Sound → <machine>" (live) while it
> streams, "Sound → <machine>, blocked" (fault) while that output is muted there, and "Sound →
> <machine>, waiting" (waiting) otherwise.

> r[viewmodel.audio.map.offline-quiet]
> A machine on the audio map that is not in the cluster shows its sound line in the quiet tone.

> r[viewmodel.audio.map.recording]
> A machine on the audio map on which an app records its virtual microphone carries a recording
> mark, whose tooltip says "An app here is recording Beamer's microphone."; other machines carry
> none.

> r[viewmodel.audio.map.meter]
> Each machine on the audio map shows one level: on the shared microphone's machine that
> microphone's level, or none while the machine does not list it; on any other machine the level of
> its default output, or none when it has no default output.

> r[viewmodel.audio.map.description]
> Each machine on the audio map has one spoken sentence, which is also its tooltip: "<machine>,
> online." or "<machine>, offline.", then "Its <microphone> is the shared microphone." on the shared
> microphone's machine, then its sound line followed by ".", then the recording sentence while an
> app there records. The recording sentence appears in it once.

> r[viewmodel.audio.map.spoken]
> The audio map is named "Machines and where their sound goes" for a screen reader, and its spoken
> value is every machine's sentence in reading order, then every arrow's sentence.
>
> - parity: the PC's map was named "Machines" (#16: one viewmodel for both machines, taking the Mac's name)

> r[viewmodel.audio.map.mic-arrows]
> The audio map draws a microphone arrow from the shared microphone's machine to each other machine
> on the map that hears it, live while the microphone streams and waiting otherwise, spoken as
> "<machine> hears the microphone of <machine>.". No arrow leads from a machine to itself.

> r[viewmodel.audio.map.sound-arrows]
> The audio map draws a sound arrow from each machine whose sound is sent to another machine on the
> map to that machine, live while that sound streams and waiting otherwise, spoken as "<machine>'s
> sound plays on <machine>.".

### The selected machine's sound

> r[viewmodel.audio.output.label]
> The selected machine's section labels its output selector "Sound from <machine> plays on".

> r[viewmodel.audio.output.here]
> The output selector lists first "Here, on <name of the selected machine's default output>", or
> "Here" when it has none.

> r[viewmodel.audio.output.closed-title]
> The closed output selector shows the marked entry as "<machine> · <entry>", such as "This Mac ·
> Default (MacBook Air Speakers)", and "Here" as "<selected machine> · <its default output>", or
> the selected machine's name alone when it has no default output.

> r[viewmodel.audio.output.choices]
> After "Here", the output selector lists, in reading order, one heading per machine the selected
> machine is linked to directly and whose audio devices `Core` knows, each followed by "Default
> (<name of its default output>)", or "Default output" when it has none, then each of its outputs
> by name. For this machine those are its direct peers; for another machine, the machines it lists
> as linked, and this machine when the two are linked directly.

> r[viewmodel.audio.output.offered]
> Another machine's outputs can be chosen only while the selected machine is in the cluster and can
> send its sound to another machine, and the other machine is in the cluster with its audio running
> or starting; otherwise they are listed but disabled.

> r[viewmodel.audio.output.selected]
> The output selector marks "Here" while the selected machine's sound plays on it, the other
> machine's default entry while its sound is sent to that machine's default output, and the
> output's own entry while it is sent to a named output, found by device key and else by name.

> r[viewmodel.audio.output.unplugged]
> While the selected machine's sound is sent to a named output its machine no longer lists, the
> output selector shows it under that machine's heading as "<name> (not connected)", disabled and
> still marked. When that machine has no heading in the selector, the selector ends with its heading
> and that one disabled, marked entry ("<name> (not connected)", or "Default output").

> r[viewmodel.audio.output.can-open]
> The output selector can be opened while the selected machine's sound is sent elsewhere, so "Here"
> can always be chosen again, and otherwise only while at least one other entry can be chosen.

> r[viewmodel.audio.output.choose-here]
> Choosing "Here" in the output selector asks `Core` to play the selected machine's sound on that
> machine.

> r[viewmodel.audio.output.choose]
> Choosing another machine's default entry, or one of its outputs, in the output selector asks
> `Core` to send the selected machine's sound to that machine's default output, or to that output
> named by its device key and its name.

> r[viewmodel.audio.output.mute-note]
> While the selected machine's sound is sent to another machine, its section says "Muted on
> <machine> while it plays elsewhere."; otherwise it says nothing there.

> r[viewmodel.audio.output.line-here]
> While the selected machine's sound plays on it, its section says "Plays on <name of its default
> output>." in the plain tone, or nothing when it has no default output.

> r[viewmodel.audio.output.line-cannot-send]
> While the selected machine is in the cluster with its devices known but cannot send its sound to
> another machine, and the page has no other sound line for it, its section says "<machine> cannot
> send its sound to another machine."

> r[viewmodel.audio.output.line-streaming]
> While the selected machine's sound streams to another machine, its section says "Playing on
> <output> on <machine>." in the live tone.

> r[viewmodel.audio.output.line-unplugged]
> While the output the selected machine's sound is sent to is unplugged, its section says "<output>
> is not connected to <machine>. <selected machine> plays its sound itself until it is back." in the
> waiting tone.

> r[viewmodel.audio.output.line-offline]
> While the machine the selected machine's sound is sent to is offline, or `Core` has no status for
> that audio route yet, the section says "<machine> is offline. <selected machine> plays its sound
> itself until it is back." in the waiting tone.

> r[viewmodel.audio.output.line-unreachable]
> While the selected machine and the machine its sound is sent to are not linked directly, the
> section says "<selected machine> and <machine> are not linked directly, so <selected machine>
> plays its sound itself." in the waiting tone.

> r[viewmodel.audio.output.line-muted-there]
> While the output the selected machine's sound is sent to is muted because that machine sends its
> own sound elsewhere, the section says "<machine> sends its own sound elsewhere, which mutes
> <output> there. Choose another output on <machine>." in the fault tone.

> r[viewmodel.audio.output.line-failed]
> While the selected machine's sound could not be sent, its section says "<selected machine>'s
> sound could not be sent to <machine> (<reason>). It plays on <selected machine>." in the fault
> tone, or without the reason in brackets when the page has no words for it.

> r[viewmodel.audio.output.device-name]
> In the selected machine's sound line and the Audio page's notices, the output a machine's sound
> is sent to is named by the device it resolves to on that machine, else by the audio route's
> device name, else as "its default output".

### The selected machine's devices

> r[viewmodel.audio.devices.headings]
> The selected machine's section lists its microphones under "Microphones" and its outputs under
> "Outputs", each by name, in the order `Core` reports them.
>
> - parity: the Mac's page headed the microphones "Inputs" (GLOSSARY.md: an input is never an audio device)

> r[viewmodel.audio.devices.detail]
> Under each device's name the section says "Default · <kind>" for the machine's default device
> and "<kind>" for any other, the kind being how the device is attached: "Built-in", "USB",
> "Bluetooth", "Virtual", "HDMI" or "Other".

> r[viewmodel.audio.devices.active-mic]
> The shared microphone's entry among the selected machine's microphones says "Active
> microphone".

> r[viewmodel.audio.devices.plays]
> An output of the selected machine that plays other machines' sound, streaming, says "Plays
> <machines>", such as "Plays lebig".

> r[viewmodel.audio.devices.empty]
> Where the selected machine lists no microphone, or no output, its section says "<machine> has not
> listed its devices yet." while `Core` knows none of its devices, "<machine> is offline." while it
> is not in the cluster, and else "No microphones." or "No outputs.".

> r[viewmodel.audio.devices.usable]
> The selected machine's Test buttons and Headset toggles can be used only while it is in the
> cluster, its audio is running, and it is this machine or linked to it directly.

> r[viewmodel.audio.devices.test]
> Each microphone of the selected machine has a "Test" button, whose tooltip says "Show this
> microphone's level for ten seconds."; pressing it asks `Core` to test that microphone of that
> machine.

> r[viewmodel.audio.devices.headset]
> Each output of the selected machine has a "Headset" toggle, on while the output counts as
> headphones or a headset, whose tooltip says "A headset cannot be heard by a microphone, so Beamer
> leaves its echo alone."

> r[viewmodel.audio.devices.headset-set]
> Turning an output's Headset toggle on asks `Core` to count that output as a headset, and turning
> it off asks `Core` to count it as speakers, whatever the OS says about the device.

> r[viewmodel.audio.devices.meters]
> Each device of the selected machine shows its own level.

> r[viewmodel.audio.devices.spoken]
> For a screen reader, a level meter is named "Level", the Microphone section's meter "Shared
> microphone level", and a device's meter "<device> level"; a Test button is named "Test
> <device>" and a Headset toggle "<device> is a headset".
>
> - parity: the Mac's page named only the meters, and the PC's page only the buttons and toggles, beside "Level" (#16: one viewmodel for both machines)

### Low-latency virtual microphone

> r[viewmodel.audio.low-latency.shown]
> The selected machine's section shows the "Low-latency virtual microphone" toggle only while the
> selected machine is this machine, this machine is a PC, and VB-CABLE is installed on it; it never
> shows on a Mac or for another machine.

> r[viewmodel.audio.low-latency.note]
> Under the low-latency toggle the section says "About 35 ms instead of 70 ms, but no other app can
> play into CABLE Input meanwhile.", followed by " This version of the audio helper cannot do it."
> while the toggle is on, this machine's audio runs, and its audio reports it cannot render the
> virtual microphone exclusively.

> r[viewmodel.audio.low-latency.state]
> The low-latency toggle is on while this machine's low-latency virtual microphone setting is on,
> and showing that setting asks nothing of `Core`.

> r[viewmodel.audio.low-latency.set]
> Turning the low-latency toggle on or off asks `Core` to set this machine's low-latency virtual
> microphone setting to on or off; while the toggle is not shown, no action reaches `Core`.

### Levels

> r[viewmodel.audio.level.scale]
> A level meter shows a level in dBFS as a fraction of its length: -60 dBFS and below empty, 0 dBFS
> full, and linear in between, so -30 dBFS fills half; it shows the average and the peak alike.

> r[viewmodel.audio.level.none]
> A level meter for which `Core` reports no level shows no reading.

> r[viewmodel.audio.level.watch]
> While the Audio page is visible, the viewmodel asks `Core` to report audio levels, and once it is
> hidden, or another page is shown, it asks `Core` to stop; it asks once per change.

> r[viewmodel.audio.menu-open]
> While a selector of the Audio page is open, the page keeps showing what it showed when the
> selector opened, and shows `Core`'s newest audio state as soon as the selector closes.

### Notices

> r[viewmodel.audio.notice.order]
> The Audio page's notices come in this order: audio stopped, the virtual microphone's install,
> VB-CABLE missing, microphone permission, system sound permission, muted outputs, feedback, the
> shared microphone's echo, echo on listening machines, choosing the virtual microphone, and
> machines without audio.

> r[viewmodel.audio.notice.audio-stopped]
> While this machine's audio has stopped and did not start again, the Audio page says "Beamer's
> audio helper stopped and did not start again." in the fault tone, with a "Retry" button that asks
> `Core` to start this machine's audio again.

> r[viewmodel.audio.notice.vmic-missing]
> On a Mac whose virtual microphone is not installed, the Audio page says "Beamer Microphone is not
> installed, so apps on this Mac cannot hear another machine's microphone." in the waiting tone,
> followed by " Every app on this Mac goes quiet for about a second while it installs." and an
> "Install Beamer Microphone" button while no audio stream runs or starts.

> r[viewmodel.audio.notice.vmic-busy]
> While an audio stream runs or starts, the virtual microphone's install or update notice says
> instead " Beamer is sending or playing sound now; it can be installed once that stops." and has
> no button.

> r[viewmodel.audio.notice.vmic-not-carried]
> When this Beamer does not carry the virtual microphone, its install notice ends with " This build
> of Beamer does not carry it." and has no button.

> r[viewmodel.audio.notice.vmic-outdated]
> On a Mac whose virtual microphone is older than the one this Beamer carries, the Audio page says
> "This Beamer carries a newer Beamer Microphone." in the plain tone, followed by the same sentence
> and button as the install notice, the button reading "Update Beamer Microphone".

> r[viewmodel.audio.notice.vmic-install]
> Pressing the install or update button asks `Core` to install the virtual microphone, asking the
> user for administrator rights with "Beamer wants to install Beamer Microphone, the microphone
> your apps hear from your other machines."; until it ends, the button reads "Installing…" and is
> disabled, and pressing it again asks nothing.

> r[viewmodel.audio.notice.vmic-install-result]
> When the virtual microphone's install ends, the button reads as before, and under the notices the
> page says "Sound started streaming before the install could begin. Try again once it stops." if
> a stream started first, "This build of Beamer does not carry Beamer Microphone." if it was not
> carried, "Beamer Microphone could not be installed. The log has the details." if it failed, and
> nothing if it was installed or the user cancelled.

> r[viewmodel.audio.notice.vbcable-missing]
> On a PC without VB-CABLE, the Audio page says "To use another machine's mic here, install
> VB-CABLE (vb-audio.com), then choose 'CABLE Output' as the microphone in your apps." in the
> waiting tone.

> r[viewmodel.audio.notice.mic-permission]
> While the OS refuses Beamer this machine's microphones, the Audio page says, in the fault tone, on
> a Mac "Beamer may not use this Mac's microphones. Allow it in System Settings, Privacy & Security,
> Microphone." and on a PC "Beamer may not use this PC's microphones. Allow it in Settings, Privacy
> & security, Microphone.", with an "Open privacy settings" button.

> r[viewmodel.audio.notice.mic-silent]
> While a microphone of this machine delivers only silence, the microphone permission notice says
> instead "A microphone here sends only silence. Check that Beamer may use the microphone."

> r[viewmodel.audio.notice.mic-permission-open]
> The microphone permission notice's "Open privacy settings" button opens the OS's microphone
> privacy settings: `x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone` on
> a Mac and `ms-settings:privacy-microphone` on a PC.

> r[viewmodel.audio.notice.system-permission]
> While the OS refuses Beamer the recording of this Mac's sound, or that recording delivers only
> silence, the Audio page says "Beamer may not record this Mac's sound, so it cannot play on another
> machine. Allow it in System Settings, Privacy & Security, Screen & System Audio Recording." in the
> fault tone, with an "Open privacy settings" button that opens
> `x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture`.

> r[viewmodel.audio.notice.muted-output]
> For each machine whose sound is sent to an output that is muted because its machine sends its own
> sound elsewhere, in machine id order, the Audio page says "<machine>'s sound cannot play on
> <output> on <machine>: <machine> sends its own sound elsewhere, which mutes that output." in the
> fault tone.

> r[viewmodel.audio.notice.feedback]
> For each sound stream in which `Core` reports feedback, once per pair of machines, the Audio page
> says, in the waiting tone and without a button, "Beamer heard what sounds like feedback in
> <machine>'s sound playing on <machine>, and turned it down." followed by one of the advice
> sentences of `viewmodel.audio.notice.feedback-advice`.

> r[viewmodel.audio.notice.feedback-advice]
> The feedback notice's advice depends on whether the sound plays on headphones or a headset on the
> receiving machine, and on whether the sending machine lists a device whose name contains "elgato"
> or "wave link", in any case. On headphones with such a device it says " If Wave Link's monitor
> plays a microphone into the system output on <sender>, turn it off."; on headphones without, " If
> <sender> plays a microphone through its own sound, stop it."; on speakers with such a device, " If
> a microphone on <sender> can hear <receiver>'s speakers, use headphones on <receiver>, or turn off
> Wave Link's monitor to the system output on <sender>."; on speakers without, " If a microphone on
> <sender> can hear <receiver>'s speakers, use headphones on <receiver>, or stop <sender> from
> playing that microphone through its own sound."

> r[viewmodel.audio.notice.speakers]
> A machine's speakers are its default output while its own sound plays on it, and every output on
> which another machine's sound streams to it, leaving out every output that counts as headphones or
> a headset.

> r[viewmodel.audio.notice.echo-cancelled]
> While the shared microphone streams to another machine and its machine has speakers, and Beamer
> cancels echo there, the Audio page says "Beamer cancels the echo of <machine>'s speakers. A
> headset on <machine> sounds better." in the plain tone.

> r[viewmodel.audio.notice.echo-not-cancelled]
> While the shared microphone streams to another machine and its machine has speakers, and Beamer
> does not cancel echo there, the Audio page says "<machine>'s microphone can hear its speakers, and
> Beamer does not cancel that echo there. Use a headset on <machine>." in the waiting tone.

> r[viewmodel.audio.notice.echo-cancelling]
> Beamer counts as cancelling echo on the shared microphone's machine while that machine's audio
> can cancel echo, its echo cancellation is on automatic when it is this machine, and its default
> output plays something: it does not while the machine's own sound streams to another machine and
> no other machine's sound streams to that output.

> r[viewmodel.audio.notice.echo-listener]
> For each machine that hears the streaming shared microphone and plays its own sound on a default
> output that is not headphones or a headset, the Audio page says, in the waiting tone, "If
> <listener>'s speakers can be heard by <microphone>, people in a call on <listener> hear
> themselves. Use a headset on <listener>." while Beamer does not cancel echo on the microphone's
> machine.

> r[viewmodel.audio.notice.echo-listener-send]
> While Beamer cancels echo on the shared microphone's machine, the listening machine's echo notice
> says instead "If <listener>'s speakers can be heard by <microphone>, people in a call on
> <listener> hear themselves. Use a headset, or send <listener>'s sound to <microphone's machine>,
> where Beamer cancels it.", with a "Send <listener>'s sound to <microphone's machine>" button
> while the listener can send its sound to another machine and is linked directly to the
> microphone's machine.

> r[viewmodel.audio.notice.echo-listener-action]
> Pressing "Send <listener>'s sound to <microphone's machine>" asks `Core` to send the listener's
> sound to the default output of the microphone's machine.

> r[viewmodel.audio.notice.pick-virtual-mic]
> While a microphone is shared and its audio route is neither off nor waiting for its machine, and
> this machine has its virtual microphone but no app records it, the Audio page says in the plain
> tone "In your call app on this Mac, choose Beamer Microphone as the microphone." on a Mac and
> "In your call app on this PC, choose CABLE Output as the microphone." on a PC.

> r[viewmodel.audio.notice.peers-without-audio]
> While peers this machine has a link to report no audio capability, the Audio page says in the
> plain tone "<name> runs a Beamer without audio. Update Beamer there to use its microphones and
> speakers." for one, and "<names> run a Beamer without audio. Update Beamer there to use their
> microphones and speakers." for several, naming only the peers that have a name, and shows no such
> notice when none has one.

> r[viewmodel.audio.notice.peers-unknown]
> A peer whose capabilities this machine does not know yet is not named in the notice of peers
> without audio.

## Camera

The Camera module of the Audio & Camera page on a Mac and on a PC, the camera's marks on that
page's machine map, and the camera items of the Mac's menu bar menu (F44). They show Core's camera
state, which `spec/camera.md` (#63) defines. Until it does, these rules name that state in plain
words:

- the **camera route**: off, or the machine whose camera the others see, with one chosen camera
  of that machine or its default camera followed, and a quality, 720p or 1080p;
- the **camera's state**, reported once overall, with the machines it concerns, and once for each
  machine with the virtual camera: *off*, *idle*, *starting*, *streaming*, *reduced*, *sharing
  off*, *waiting for permission*, *denied*, *waiting for the camera* (with the camera's name),
  *no camera*, *waiting for the machine* (offline, or starting its camera), *unreachable*, *no
  direct path*, *not installed*, *outdated*, *damaged*, *no virtual camera*, or *error* with a
  reason;
- each machine's camera report in its machine record: whether it can share its camera (a Mac) or
  has the virtual camera (a PC), its cameras by name and which one the OS prefers, its camera
  access (allowed, not allowed, restricted, not asked yet or unknown), its camera sharing, whether
  its camera list is ready, starting or failed, its virtual camera's install state (installed,
  missing, outdated, damaged, or unsupported here) and whether an app has the virtual camera open,
  with the app's name when the OS gives it.

The virtual camera's product name, which every string here uses, is Beamer Camera. A line's
**tone** is quiet, plain, live, waiting or fault; `ui` gives each its colour.

### Naming machines

> r[viewmodel.camera.names.this-machine]
> Inside a sentence of the Camera module, this machine is "this Mac" on a Mac and "this PC" on a
> PC, and a peer is its label (`viewmodel.machines.label.name`, `viewmodel.machines.label.shared`).

> r[viewmodel.camera.names.capital]
> A camera status line that starts with a machine's name or with "an app" starts with a capital
> letter: "This Mac and lebig are not linked directly.", "An app on lebig is using it.".

> r[viewmodel.camera.names.apps]
> The camera status lines name each machine that has the virtual camera open as "{app} on
> {machine}" when the OS gave the app's name, and as "an app on {machine}" when it did not. Two or
> more are joined as a list: "Microsoft Teams on lebig and Zoom on asusrog".

### The module

> r[viewmodel.camera.module.shown]
> The Audio & Camera page shows its Camera module, and the map legend's "Camera" item, while at
> least one machine in the camera state can share its camera or has the virtual camera, whatever
> its install state. With none, both are hidden and the map shows no camera marks.

> r[viewmodel.camera.module.place]
> The Camera module, headed "Camera", sits on the Audio & Camera page after the page's notices and
> before the machine map.

> r[viewmodel.camera.module.hint]
> Under its status line the Camera module always says "Apps on a PC choose 'Beamer Camera'. Macs
> cannot show another machine's camera yet."

> r[viewmodel.camera.module.help]
> On a PC whose virtual camera is installed, the Camera module shows the line "One app at a time
> can use Beamer Camera unless Windows allows several: Settings > Bluetooth & devices > Cameras >
> Beamer Camera." with a "Camera settings" button, which opens Windows' camera settings
> (`ms-settings:camera`). A Mac, and a PC whose virtual camera is not installed, show neither.

### The camera your apps see

> r[viewmodel.camera.selector.choices]
> The Camera module's "Camera your apps see" list offers "Off" first. Then, for each machine that
> can share its camera, this machine first and the others by name, ignoring case, it shows the
> machine's name as a heading that cannot be chosen, then "Default camera ({name})", naming the
> camera that machine's OS prefers, or "Default camera" when it prefers none, then each of its
> cameras by name. A machine that cannot share its camera has no heading.

> r[viewmodel.camera.selector.offered]
> A camera in the "Camera your apps see" list can be chosen only while its machine is this one or
> in the cluster, and its camera list is ready or starting. Otherwise it is listed but cannot be
> chosen.

> r[viewmodel.camera.selector.selected]
> The "Camera your apps see" list selects "Off" while the camera route is off, the machine's
> "Default camera" entry while the camera route follows that machine's default camera, and the
> named camera while the camera route names one.

> r[viewmodel.camera.selector.unplugged]
> While the camera route names a camera its machine no longer lists, the "Camera your apps see"
> list adds "{camera} (not connected)" under that machine, which cannot be chosen, and keeps it
> selected.

> r[viewmodel.camera.selector.unknown-machine]
> While the camera route names a machine whose cameras this machine has no list of, the "Camera
> your apps see" list adds a heading with that machine's name and, under it, the camera route's
> camera by name, or "Default camera" when it follows the default, which cannot be chosen, and
> keeps it selected.

> r[viewmodel.camera.selector.title]
> The closed "Camera your apps see" list shows "{machine} · {camera}", as "This Mac · Default
> camera (MacBook Air Camera)", or "Off".

> r[viewmodel.camera.selector.choose]
> Choosing "Off" in the "Camera your apps see" list asks Core to turn the camera route off.
> Choosing a machine's "Default camera" asks Core to set the camera route to that machine, following
> its default camera, and choosing a named camera asks Core to set the camera route to that machine
> and camera. A heading, or an entry that cannot be chosen, asks nothing.

### Quality

> r[viewmodel.camera.quality.choices]
> The Camera module's "Quality" choice offers "720p" and "1080p", in that order, and shows the
> camera route's quality, or "720p" when the camera route names neither.

> r[viewmodel.camera.quality.off]
> The "Quality" choice can be changed only while the camera route names a machine. While it is
> off, the choice is disabled.

> r[viewmodel.camera.quality.choose]
> Choosing "720p" or "1080p" asks Core to set the camera route's quality to it. No other quality is
> ever asked for.

### Status line

> r[viewmodel.camera.status.off]
> While the camera route is off, or the camera's state is *off*, the Camera module's status line
> says "Off. Apps that open Beamer Camera see a 'camera off' picture." in the quiet tone.

> r[viewmodel.camera.status.idle]
> While the camera's state is *idle*, the status line says "Ready. No app on {machines} is using
> Beamer Camera.", naming the machines whose state is *idle*, or "Ready. No app is using Beamer
> Camera." when it names none, in the plain tone.

> r[viewmodel.camera.status.starting]
> While the camera's state is *starting*, the status line says "Starting for {apps}…", naming the
> apps on the machines whose state is *starting*, in the live tone.

> r[viewmodel.camera.status.streaming]
> While the camera's state is *streaming*, the status line says "{apps} is using it." for one
> machine and "{apps} are using it." for two or more, naming the apps on the machines whose state
> is *streaming* or *reduced*, or "An app is using it." when it names none, in the live tone.

> r[viewmodel.camera.status.reduced]
> While the camera's state is *reduced*, or any machine the streaming line names is *reduced*, the
> status line adds " Reduced quality: the network is slow." to the streaming line, and its tone is
> waiting.

> r[viewmodel.camera.status.sharing-off]
> While the camera's state is *sharing off*, the status line says "Camera sharing is off on this
> Mac." when the camera route names this machine, and "Turned off on {machine}." otherwise, in the
> quiet tone.

> r[viewmodel.camera.status.permission]
> While the camera's state is *waiting for permission*, the status line says "Allow camera access
> on this Mac." when the camera route names this machine, and "Allow camera access on {machine}."
> otherwise, in the waiting tone.

> r[viewmodel.camera.status.denied]
> While the camera's state is *denied*, the status line says "This Mac does not allow Beamer to use
> its camera." when the camera route names this machine, and "{machine} does not allow Beamer to
> use its camera." otherwise, in the fault tone.

> r[viewmodel.camera.status.waiting-device]
> While the camera's state is *waiting for the camera*, the status line says "{camera} is not
> connected to {machine}.", the camera being the one the state names, else the camera route's
> camera by name (`viewmodel.camera.map.badge`), in the waiting tone.

> r[viewmodel.camera.status.no-camera]
> While the camera's state is *no camera*, the status line says "This Mac has no camera." when the
> camera route names this machine, and "{machine} has no camera." otherwise, in the waiting tone.

> r[viewmodel.camera.status.waiting-machine]
> While the camera's state is *waiting for the machine*, the status line says "{machine} is
> starting its camera side." while that machine is starting its camera, and "{machine} is
> offline." otherwise, in the waiting tone.

> r[viewmodel.camera.status.unreachable]
> While the camera's state is *unreachable*, the status line says "{machine} and {machines} are not
> linked directly.", naming the camera route's machine, then the machines whose state is
> *unreachable*, or "the machine using it" when it names none, in the waiting tone.

> r[viewmodel.camera.status.no-direct-path]
> While the camera's state is *no direct path*, the status line says "Video needs a direct network
> path between {machine} and {machines}.", naming the camera route's machine, then the machines
> whose state is *no direct path*, or "the machine using it" when it names none, in the waiting
> tone.

> r[viewmodel.camera.status.install]
> While the camera's state is *not installed*, *outdated* or *damaged*, the status line says
> "Beamer Camera is not installed on {machines}.", "Beamer Camera needs an update on {machines}."
> or "Beamer Camera is damaged on {machines}.", naming the machines in that state, in the waiting
> tone.

> r[viewmodel.camera.status.no-virtual-camera]
> While the camera's state is *no virtual camera*, the status line says "No linked PC can show it:
> apps on a PC use Beamer Camera." in the waiting tone.

> r[viewmodel.camera.status.error]
> While the camera's state is *error*, the status line says "The camera stopped on {machine}:
> {reason}." in the fault tone, the reason in these words: the camera is gone or missing, "the
> camera is not available"; camera access refused, "camera access is not allowed"; the virtual
> camera missing, "Beamer Camera is not installed"; the virtual camera outdated, "Beamer Camera
> needs an update"; the machine cannot share its camera, "that machine cannot send its camera";
> the video failed to decode, "the picture could not be shown"; the video never reached the
> machine using it, "the picture did not arrive"; the camera choice changed under it, "the camera
> choice changed"; another app holds the camera, "another app holds the camera"; a fault in
> Beamer, "something went wrong in Beamer". An error without a reason says "The camera stopped on
> {machine}.".
>
> - parity: Python also said "Beamer's media helper stopped" for a fault of its separate media process (ADR 0004: Beamer is one process, with no media helper)

> r[viewmodel.camera.status.retry]
> While the camera's state is *error*, the status line carries a "Retry" button, which asks Core to
> set the camera route again to the same machine and camera. In every other state it carries no
> button.
>
> - parity: Python's Retry, when the fault was its media helper and the camera was this machine's, restarted the helper instead (ADR 0004: there is no media helper)

> r[viewmodel.camera.status.detail]
> While the camera's state is *streaming* or *reduced*, the line under the status says the first
> running stream's size, frame rate and delay, as "1280x720 · 30 fps · 72 ms", with the frame rate
> and delay rounded to whole numbers and any part the stream does not report omitted. With no
> running stream, and in every other state but *denied*, the line is empty and hidden.
>
> - parity: Python added " · through the Beamer link" when the stream had turned from its own UDP flow to the link (#14: media always travels on the link, `link.class.media-datagrams`)

> r[viewmodel.camera.status.denied-detail]
> While the camera's state is *denied*, the line under the status says where the camera
> machine's user allows it: "Allow it in System Settings > Privacy & Security > Camera." when the
> camera route names this machine, "Allow it on {machine} in System Settings > Privacy &
> Security > Camera." when it names a peer that is a Mac, and "Allow it in the system settings on
> {machine}." when it names a peer that is not.

### This machine

> r[viewmodel.camera.this-machine.heading]
> Under a divider, the Camera module lists lines about this machine, the first headed "This Mac"
> on a Mac and "This PC" on a PC. Each line has a label, a value with a tone, and at most one
> button. A machine that can share its camera shows the Mac's lines, one that has the virtual
> camera shows the PC's line, and a machine with neither shows no lines, no heading and no
> divider.

> r[viewmodel.camera.this-machine.sharing]
> The Mac's first line is "Camera sharing": "On", plain, with a "Stop Sharing" button that asks
> Core to turn this Mac's camera sharing off, while sharing is on or unknown; "Off", quiet, with a
> "Share the Camera" button that asks Core to turn it on, while it is off.

> r[viewmodel.camera.this-machine.access]
> The Mac's second line is "Camera access": "Allowed", plain, while macOS allows it; "Not
> allowed", fault, with an "Open Privacy Settings" button that opens macOS's Camera privacy
> settings (`x-apple.systempreferences:com.apple.preference.security?Privacy_Camera`), while
> macOS refuses or restricts it; "Unknown", quiet, while Beamer cannot tell.

> r[viewmodel.camera.this-machine.not-asked]
> While macOS has not asked about camera access yet, the "Camera access" line says "Not asked yet":
> in the waiting tone and with no button while Beamer needs camera access, because the notice then
> carries the button (`viewmodel.camera.notice.permission`); in the plain tone with an "Allow
> Camera Access…" button otherwise. That button asks Core to have macOS ask for camera access.

> r[viewmodel.camera.this-machine.test-image]
> While this Mac shares a test image in place of its camera, the Mac's lines end with "Test
> source": "On, sent instead of the camera", in the waiting tone, and the map names the camera
> "Test source" (`viewmodel.camera.map.badge`).

> r[viewmodel.camera.this-machine.pc-installed]
> The PC's line is "Beamer Camera": "Installed", plain, with a "Remove…" button while the virtual
> camera is installed. While a video stream is opening, starting or running on this PC, the line
> has no button.

> r[viewmodel.camera.this-machine.pc-restart]
> While the virtual camera Windows has loaded is older than the one installed, both versions being
> known, the PC's "Beamer Camera" line says "Close apps that use the camera to finish updating
> Beamer Camera." in the waiting tone, with no button.

> r[viewmodel.camera.this-machine.pc-unsupported]
> While this version of Windows cannot have the virtual camera, the PC's "Beamer Camera" line says
> "This version of Windows cannot show it." in the quiet tone, with no button.

> r[viewmodel.camera.this-machine.pc-missing]
> While the virtual camera is missing, outdated or damaged, the PC's "Beamer Camera" line says "Not
> installed", "Needs an update" or "Damaged", in the waiting tone, with an "Install Beamer
> Camera…", "Update Beamer Camera…" or "Repair Beamer Camera…" button. While a video stream is
> opening, starting or running on this PC, the line has no button.

### Installing the virtual camera

> r[viewmodel.camera.install.ask]
> Pressing "Install Beamer Camera…", "Update Beamer Camera…" or "Repair Beamer Camera…" asks Core to
> install the virtual camera, one and the same request for the three. While one install or removal
> runs, no other starts.

> r[viewmodel.camera.install.remove-confirm]
> Pressing "Remove…" first asks "Remove Beamer Camera?", saying "Apps on this PC will no longer
> list Beamer Camera. Windows asks for administrator permission, and every camera on this PC may
> pause for about a second.", with "Remove" and "Cancel", Cancel being the default. Only "Remove"
> asks Core to remove the virtual camera.

> r[viewmodel.camera.install.running]
> While an install or a removal runs, the PC's "Beamer Camera" line says under its value "Waiting
> for Windows to install Beamer Camera…" or "Waiting for Windows to remove Beamer Camera…", and its
> button is disabled.

> r[viewmodel.camera.install.refused]
> When an install or a removal ends without success, the PC's "Beamer Camera" line says under its
> value "Installation was cancelled." or "Removal was cancelled." when the user refused Windows'
> administrator prompt, and "Beamer Camera could not be installed. Try again, or see Beamer's log."
> or "Beamer Camera could not be removed. Try again, or see Beamer's log." for any other failure,
> including one that could not start. Its button can be pressed again.
>
> - parity: Python also said "Beamer's media helper is missing, so Beamer Camera cannot be installed." (or removed) when its separate media helper was missing (ADR 0004: there is no media helper)

> r[viewmodel.camera.install.note-kept]
> The note under the PC's "Beamer Camera" line remains while the line shows the same value and
> button, and goes as soon as the virtual camera's install state changes what the line shows.

> r[viewmodel.camera.install.done]
> When an install or a removal succeeds, the note under the PC's "Beamer Camera" line goes, and the
> page asks Core to read the virtual camera's install state again.

### Notices

> r[viewmodel.camera.notice.order]
> The Camera module's notices come in this order: the camera store notice, the permission notice,
> the notice of another app on this Mac, then one notice for each PC whose virtual camera is not
> ready. In the ordinary states, *off*, *idle*, *streaming*, *reduced*, *sharing off* and *denied*
> on a Mac whose camera choices were read and saved, there is none.

> r[viewmodel.camera.notice.store-unreadable]
> While this machine can share its camera and Beamer could not read its saved camera choices, so
> that camera sharing started off, the Camera module shows "Camera sharing is off because Beamer
> could not read its saved camera settings." in the waiting tone, with no button: the "Camera
> sharing" line carries "Share the Camera".

> r[viewmodel.camera.notice.save-failed]
> While Beamer could not save this machine's camera choices, the Camera module shows "Beamer
> could not save the camera settings. After a restart, they may not be as you set them." in the
> fault tone.

> r[viewmodel.camera.notice.permission]
> While Beamer needs camera access on this Mac, because the camera route names it, its camera
> sharing is on and macOS has not asked yet, the Camera module shows "Beamer needs camera access to
> share this Mac's camera." in the waiting tone, with an "Allow Camera Access…" button that asks
> Core to have macOS ask for camera access. A peer's page never shows it.

> r[viewmodel.camera.notice.shared-use]
> While this Mac's camera is streaming or reduced and another app on this Mac uses the camera at
> the same time, the Camera module shows "Another app on this Mac uses the camera at the same
> time." in the plain tone.

> r[viewmodel.camera.notice.peer-install]
> While the camera route names a machine, the Camera module shows, for each peer in the cluster
> that has the virtual camera and whose virtual camera is missing, outdated or damaged, in name
> order: "{machine}: Beamer Camera is not installed there. Install it in Beamer on {machine}.",
> "{machine}: Beamer Camera needs an update there. Update it in Beamer on {machine}." or
> "{machine}: Beamer Camera is damaged there. Repair it in Beamer on {machine}.", in the waiting
> tone, with no button. This machine's own virtual camera is never one: its "Beamer Camera" line
> says it.

### The machine map

> r[viewmodel.camera.map.off]
> While the camera route is off, the Audio & Camera page's machine map shows no camera marks.

> r[viewmodel.camera.map.badge]
> While the camera route names a machine, that machine's card on the map shows the camera's name:
> the camera the camera route names, or the default camera its machine's OS prefers when the
> camera route follows it; the camera route's camera by name when its machine no longer lists it;
> "Default camera" when neither is known. The name is marked live while at least one camera arrow
> is live. The card's accessible description adds "Its {camera} is the shared camera.".

> r[viewmodel.camera.map.watching]
> While the camera route names a machine, the card of each other machine whose virtual camera is
> installed says "{app} uses the camera", or "An app uses the camera" when the OS gave no app name,
> while an app there has the virtual camera open, and "Camera ready" while none has and that
> machine's state is *idle*. The card's description adds the same words, ending with a full stop.

> r[viewmodel.camera.map.arrows]
> While the camera route names a machine, the map draws a camera arrow from that machine to each
> other machine whose virtual camera is installed and open in an app: live while that machine's
> state is *streaming* or *reduced*, waiting otherwise. Its description is "{machine} sees the
> camera of {machine}.", the machine seeing first, this machine named "This Mac" or "This PC".

> r[viewmodel.camera.map.on-map]
> A camera arrow is drawn only between two different machines that both have a card on the map.

### The Mac's menu bar menu

> r[viewmodel.camera.menu.shown]
> The Mac's menu bar menu shows its camera items only while this Mac can share its camera. A PC's
> menu has none.

> r[viewmodel.camera.menu.line]
> While the camera route names this Mac and the camera's state is *starting*, *streaming* or
> *reduced*, the Mac's menu shows under the drive status "Camera: {users}", each machine using it
> named "{app} on {machine}", or "in use on {machine}" when the OS gave no app name, joined as a
> list. Otherwise the line is hidden.

> r[viewmodel.camera.menu.tooltip]
> While the Mac's menu shows its camera line, the menu bar icon's tooltip ends with the same line,
> on a line of its own.

> r[viewmodel.camera.menu.sharing]
> The Mac's menu shows "Stop Sharing the Camera" while this Mac's camera sharing is on and "Share
> the Camera" while it is off, whatever the camera route.

> r[viewmodel.camera.menu.sharing-click]
> Choosing "Stop Sharing the Camera" asks Core to turn this Mac's camera sharing off, and choosing
> "Share the Camera" asks Core to turn it on, whatever the camera state says at that moment. The
> item's words turn to the other action at once.

> r[viewmodel.camera.menu.sharing-pending]
> After the sharing item is chosen, it keeps the words it turned to until the camera state agrees
> with what was asked, or for 2 seconds, whichever comes first, even when a newer camera state
> still says the old one. Then the camera state decides its words again.

> r[viewmodel.camera.menu.allow]
> The Mac's menu shows "Allow Camera Access…" only while Beamer needs camera access on this Mac
> (`viewmodel.camera.notice.permission`). Choosing it asks Core to have macOS ask for camera
> access.

## Tray menu

The tray icon (its icon in the Mac's menu bar, or in the PC's notification area) and its menu, F27.

> r[viewmodel.tray.drive-status]
> The tray menu's first item, dimmed, says where input is: "Driving {name}" while this machine's
> input is on a peer, naming it; else "Driven by {name}" while a peer drives this machine, naming
> its driver; else "Idle". A machine with no label is "another machine"
> (`viewmodel.words.unknown-machine`).

> r[viewmodel.tray.tooltip]
> The tray icon's tooltip is the drive status. After a refusal notification
> (`viewmodel.notice.refused`), it is the drive status, a line break, and that notification's text,
> until the drive status next changes. While the tray menu shows a camera line
> (`viewmodel.camera.menu.tooltip`), that line ends the tooltip, on a line of its own.

> r[viewmodel.tray.order]
> The tray menu lists, in order: the drive status; on a Mac, the camera line, when shown
> (`viewmodel.camera.menu.line`); a separator; "Beamer {version}", dimmed; a separator; the input
> items (`viewmodel.tray.send-one`, `viewmodel.tray.send-each`); the pause item; a separator; the two
> direction items; on a Mac, the camera sharing item and "Allow Camera Access…", each when shown
> (`viewmodel.camera.menu.shown`, `viewmodel.camera.menu.sharing`, `viewmodel.camera.menu.allow`);
> the "Profile" submenu, when shown (`viewmodel.profiles.tray.shown`); one submenu per shared
> display (`viewmodel.tray.display-menu`); a separator; "Settings…"; on a Mac, "Show in Dock";
> "Reload configuration"; "Open log folder"; a separator; "About Beamer"; and "Quit Beamer".
>
> - parity: Python's update check turned the version item into "Beamer {version} is available…", a link to the download, and its Mac added the Profile submenu at the menu's end, after the shared displays' submenus (#11, F22: no update check; one view-model for every platform, the Windows app's order)

> r[viewmodel.tray.send-one]
> With fewer than two peers in use that this machine may drive, the tray menu has one input item:
> "Bring input back to this Mac" ("this PC" on a PC) while this machine's input is on a peer; else
> "Send input to {name}", naming the shortcut's machine (`crossing.shortcut.machine`); else "Send
> input", dimmed. Choosing it brings input home, or crosses to the shortcut's machine by the menu.
> The window's button that moves input says the same and does the same.
>
> - parity: Python's Mac said "Return input to Mac" and named the status peer, and its Windows app said "Send input across" when it could name no machine (one view-model for every platform)

> r[viewmodel.tray.send-each]
> With two or more peers in use that this machine may drive, the tray menu has one item for each of
> them, in the peer list's order, in place of the single input item, and none for a peer that is not
> in use or that this machine may not drive: "Bring input back" on the peer this machine's input is
> on, never dimmed, even while its link is down; "Send input to {name}" on a peer whose link is up;
> and "{name} is unavailable", dimmed, on a peer whose link is down. Choosing "Bring input back"
> brings input home; choosing another peer's item crosses to it by the menu, from wherever input is,
> straight from the peer input is on when it is on one.

> r[viewmodel.tray.pause]
> The tray menu's pause item says "Pause crossing", and "Resume crossing" while a crossing pause is
> on. Choosing it starts or ends the crossing pause (`crossing.pause.zones`).

> r[viewmodel.tray.directions]
> The tray menu's two direction items speak of may drive and may be driven by
> (`pairing.peers.permissions`). With one peer they say "This Mac drives {name}" and "{name} drives
> this Mac"; with several, "This Mac drives every machine" and "Every machine drives this Mac"; with
> none, "This Mac drives other machines" and "Other machines drive this Mac", both dimmed. A PC says
> "this PC" and "This PC" for "this Mac" and "This Mac".
>
> - parity: Python's Mac left the two items undimmed with no peer, and its Windows app said "This PC drives machines" and "Machines drive this PC" then (one view-model for every platform)

> r[viewmodel.tray.directions-tick]
> Each direction item is ticked while every peer has its permission on and unticked otherwise.
> While some peers but not all have it on, its words end " (some enabled)".
>
> - parity: Python's Mac showed a mixed tick for some, with no words for it (one view-model for every platform: the Windows app's, which any tray menu can show)

> r[viewmodel.tray.directions-choose]
> Choosing a direction item turns its permission on for every peer when not all have it on, and
> off for every peer when all have it on. When that turns may drive off for the peer this machine's
> input is on, input comes home first.

> r[viewmodel.tray.items]
> "Settings…" opens Beamer's window, "Open log folder" opens the folder Beamer logs to, "About
> Beamer" shows Beamer's About panel and "Quit Beamer" quits Beamer.

> r[viewmodel.tray.click]
> A primary click on the tray icon opens Beamer's window. A secondary click, or a Control-click on a
> Mac, opens the tray menu.

> r[viewmodel.tray.icon]
> The tray icon shows one of three states: away, while this machine's input is on a peer or a peer
> drives this machine; paused, while a crossing pause or a full-screen pause is on and this machine
> has a zone; and here, otherwise. Away comes before paused.

> r[viewmodel.tray.icon-words]
> The tray icon's accessibility description is "Input on this Mac" when here, "Input held on this
> Mac; crossing is off" when paused, and "Input on another machine" when away, with "this PC" for
> "this Mac" on a PC.

> r[viewmodel.tray.badge]
> On a PC, the tray icon carries a badge: linked while some peer's link is up; waiting while none
> is; and fault while this PC cannot take links on its port (`link.transport.port-taken`), when the
> icon is also dimmed.
>
> - parity: Python's badge showed its receiver's state, connected, waiting, error or stopped, and dimmed the icon for error and stopped (#14: one link per pair, and no receiver apart from it)

> r[viewmodel.tray.show-in-tray]
> On a PC, the tray icon is shown while Show in tray is on, as it is until the user turns it off.
> Turning it off, from its control on the window or by a reload of the configuration, takes the
> icon down at once; turning it on puts it up.

> r[viewmodel.tray.close]
> On a PC, closing Beamer's window quits Beamer while Show in tray is off. While it is on, closing
> the window hides it and Beamer keeps running.

> r[viewmodel.tray.hidden-start]
> On a PC, a start asked to start hidden, as a start at login is, shows Beamer's window while Show in
> tray is off.

### Shared displays in the tray menu

> r[viewmodel.tray.display-menu]
> The tray menu has one submenu for each display of this machine that is a shared display with a
> peer whose link is up, or that has a display owner, in the order this machine lists its displays.
> Its title is "Screen {name}: {owner}", {owner} being the display owner, "This Mac" ("This PC" on a
> PC) when it is this machine and the peer's label otherwise, or "Screen {name}: not managed" while
> the display has no display owner. {name} is the display's own name, else the name a peer gives
> it, else its key.

> r[viewmodel.tray.display-choices]
> A shared display's submenu lists this machine as "This Mac" ("This PC" on a PC), then each peer
> whose link is up and that has the display, in machine id order, then the display owner when it
> is none of those, then "Not managed". The display owner's item is ticked, and "Not managed" is
> ticked while the display has no display owner.

> r[viewmodel.tray.display-choose]
> Choosing a machine's item in a shared display's submenu asks Core to make that machine the
> display owner, which brings a handover; choosing "Not managed" asks Core to leave the display
> with no display owner.

## Dock and menu bar

A Mac's two presence settings, Show in Dock and Show in menu bar, F27.

> r[viewmodel.dock.defaults]
> On a Mac, Show in Dock and Show in menu bar are on until the user turns one off, and each keeps
> the user's choice across restarts.

> r[viewmodel.dock.presence]
> On a Mac, Beamer is in the Dock while Show in Dock is on or its window is open, and leaves the
> Dock when its window closes with Show in Dock off. Turning Show in Dock off while the window is
> open leaves Beamer in the Dock until the window closes, and a start at login with it off
> (`platform.login.mac-hidden`) never puts Beamer in the Dock.

> r[viewmodel.dock.menu-bar]
> On a Mac, the tray icon is in the menu bar while Show in menu bar is on, and while Show in Dock is
> off whatever Show in menu bar says. Turning Show in menu bar off with Show in Dock on takes the
> tray icon out of the menu bar at once and keeps Show in Dock on.

> r[viewmodel.dock.never-both]
> On a Mac, the Show in menu bar control is dimmed while Show in Dock is off, and the Show in Dock
> control while Show in menu bar is off. Asked to turn off the one still on while the other is off,
> Beamer keeps it on.

> r[viewmodel.dock.both-off-stored]
> On a Mac whose stored settings have Show in Dock and Show in menu bar both off, the Show in menu
> bar control shows on and the tray icon is in the menu bar. Turning Show in Dock on from there
> stores Show in menu bar on as well.

> r[viewmodel.dock.tray-item]
> On a Mac, the tray menu's "Show in Dock" item is ticked while Show in Dock is on. Choosing it
> turns Show in Dock off when it is on, and on when it is off, as its control on the window does.

> r[viewmodel.dock.reopen]
> On a Mac, a click on Beamer's icon in the Dock opens Beamer's window, also while Show in menu bar
> is off.

## Notifications

What each notification says, and what raises each one, F36. Delivering them is
`spec/platform.md`'s (`platform.notify.mac-centre`, `platform.notify.pc`).

> r[viewmodel.notice.title]
> Every notification is titled "Beamer", unless its rule gives another title.

> r[viewmodel.notice.refused]
> When a take or a pass on is refused as driven by another machine (`driving.take.refused`), a
> notification says "Can't cross to {name}: it is being driven from {other}.", naming the machine
> in the way, or "Can't cross to {name}: another machine is driving it." when the refusal names no
> machine or one that is not a peer. Refused as driving another machine, it says "Can't cross to
> {name}: it is driving {other}." or "Can't cross to {name}: it is driving another machine.".

> r[viewmodel.notice.refused-once]
> A refusal notification whose text is the same as one raised in the last 30 s is not raised again,
> though it still becomes the tray icon's tooltip (`viewmodel.tray.tooltip`). A change of the drive
> status lets the same text raise a notification again at once.

> r[viewmodel.notice.came-home]
> When input comes home because of the machine it was on (`driving.home.reasons`), a notification
> says why, naming that machine: refused as not allowed, "{name} does not accept input from this
> Mac" ("this PC" on a PC); refused because that machine sent the driver home, "{name} sent the
> input back"; refused as malformed, "{name} could not read the request to take it"; no answer to
> the take, "{name} did not answer"; taken over (`driving.home.taken-over`), "{other} took over
> {name}", or "Another machine took over {name}" when the new driver is not a peer; and the link
> lost, "Lost the link to {name}". Input brought home by this machine's own crossing method, by a
> send home, or because capture stopped raises none.
>
> - parity: Python's Mac raised nothing when refused because it had sent the driver home, said "Input returned to this Mac" when its own queue toward the link failed, and its Windows app said "{name} would not take the input" for a malformed take (one view-model for every platform)

> r[viewmodel.notice.cannot-cross]
> When Core reports that this machine cannot cross to a peer (`driving.take.no-link`,
> `crossing.shortcut.machine-nothing`), a notification says "Cannot switch — {sentence}": for a peer
> with no link, the sentence of the link status that peer's last down reason gives
> (`viewmodel.status.order`); for a peer that does not let this machine drive it, "{name} does not
> accept input from this Mac" ("this PC" on a PC); with no peer to cross to, the link status's
> sentence. A peer this machine may not drive raises none.
>
> - parity: Python's Mac said "{name} does not accept input from this Mac" without "Cannot switch — ", and the link's own last status text stood where the sentence is (one view-model for every platform: the Windows app's form, with the link status's words)

> r[viewmodel.notice.driver-unreachable]
> When Core reports that this driven machine cannot cross because its driver could not be reached
> (`driving.role.drive-while-driven-unreachable`), a notification says "Cannot switch — {name}
> could not be reached while this Mac is being driven" ("this PC" on a PC), naming the peer it was
> crossing to.
>
> - parity: Python's Mac logged it and raised no notification (one view-model for every platform: the Windows app's)

> r[viewmodel.notice.skipped]
> Each peer the shortcut passes over as not connected (`crossing.shortcut.skipped`) raises a
> notification "Skipped {name}: not connected.", in the order reported.

> r[viewmodel.notice.shake]
> A shake that brings input home (`crossing.shake.home`) raises a notification "Shook home from
> {name}. Turn this off in Crossing → Way home.", naming the peer input was on.

> r[viewmodel.notice.wake]
> A wake raises "Waking {name}…" when its packet is sent (`discovery.wake.crossing`), then "{name}
> is awake — switch again to send input" when the peer's link comes up, or "{name} did not wake"
> when 60 s pass without it (`discovery.wake.waiting`).

> r[viewmodel.notice.wake-not-sent]
> A wake packet that cannot be sent (`discovery.wake.send-failed`) raises "Could not send the
> wake-up packet".

> r[viewmodel.notice.permissions]
> On a Mac without both of its input permissions (`platform.permissions.mac-input`), choosing a
> peer's input item in the tray menu moves no input and raises "Grant both Mac permissions first.".

> r[viewmodel.notice.not-saved]
> When a change made from the tray menu cannot be saved, a notification says "Could not save the
> change: {error}", and the tray menu and the window show what is stored again.

> r[viewmodel.notice.reload]
> Choosing "Reload configuration" raises a notification titled "Configuration reloaded" saying
> "Beamer is using the config on disk." when the settings read, and one titled "Configuration not
> reloaded" whose body is the error when they do not.
>
> - parity: Python's Windows app raised nothing on success, and "Could not reload the configuration: {error}" titled "Beamer" on failure (one view-model for every platform: the Mac app's)

> r[viewmodel.notice.input-problem]
> On a PC, an input problem that raises a notification (`platform.blocker.notify`) says, by its
> kind (`platform.blocker.kind`), with {app} its input blocker's name (`platform.blocker.name`),
> or "the foreground window" when it has none: GameInput's system window, "Windows blocked Beamer's
> input because {app} is GameInput's system window. Click a normal window on this PC to
> continue."; a system-level window, "Windows blocked Beamer's input because {app} is a system-level
> window. Use it locally and click a normal window on this PC to continue."; a window running as
> administrator, "Windows blocked Beamer's input because {app} is running as administrator. Click a
> normal window on this PC to continue."; a window Beamer was not allowed to inspect, "Windows
> blocked Beamer's input while {app} was in front. Windows did not allow Beamer to inspect this
> window's process. Click a normal window on this PC to continue."; and an ordinary window,
> "Windows blocked Beamer's input while {app} was in front. Click a normal window on this PC to
> continue."

> r[viewmodel.notice.addresses-hidden]
> While Hide addresses is on, every address in a notification's title and body, in the tray menu
> and in the tray icon's tooltip is hidden, as `viewmodel.connection.hide-text` says: a peer whose
> label is its address reads "Send input to •••" in the tray menu.
