# Audio

Sound and the microphone go where the user sends them: every machine's apps hear one shared
microphone, from any machine of the cluster, through the virtual microphone, and each machine's
sound plays on it or on another machine's output. It is F43 of #11's inventory, "Audio and
microphone follow input", which ships. Two audio routes say where things go, and they are shared
entries: the microphone's, and one per machine for its sound. A microphone stream runs only while
an app records a virtual microphone; a sound stream runs while its route sends the sound away.

The rules come from the Python app at `archive/python` (the routes, the plan and the statuses in
`core/audio/model.py`, `core/audio/planner.py` and `core/audio/service.py`, the limits in
`core/audio/wire.py`, the Mac's install in `mac_app/audio_driver.py`, and the OS's part in the audio
helper's backends under `native/audio/mac/` and `native/audio/win/`), mined through its tests in
`spec/ledger/audio.txt`, and from the measured design in `dev/audio/PHASE2.md` and
`dev/audio/STATUS.md`. They take #14's store, in which the audio routes and the headset choices are
shared entries and each machine's devices are parts of its machine record, #14's media traffic
class (`link.class.media-datagrams`), ADR 0004's one process with no audio helper, #9's contract
with the virtual microphone, and #21's install of it.

What stays elsewhere: how shared entries and machine records are stamped, merged, exchanged and
kept, and how a value that breaks its schema is ignored, are `spec/store.md`'s (`store.entry.*`,
`store.record.*`, `store.sync.*`, `store.file.*`); how audio's messages and media travel, and how a
message that does not decode is dropped, are `spec/link.md`'s (`link.class.control`,
`link.class.media-datagrams`, `link.class.media-and-input`, `link.frame.undecodable`); what is
inside an audio stream (its media framing, codec settings, jitter buffer, drift, the echo canceller
and the feedback guard that turns a stream down) is `spec/media.md`'s (#64); how a capability
reports its state is `spec/platform.md`'s (`platform.capability.*`); the `--elevated` command line,
`beamer install`, `beamer uninstall` and the prompt for administrator rights are `spec/app.md`'s
(`app.elevated.*`, `app.install.*`, `app.uninstall.*`); where `local.json` is kept and when it is
written are `spec/runtime.md`'s (`runtime.files.*`); and every word the Audio & Camera page shows
is `spec/viewmodel.md`'s (`viewmodel.audio.*`).

Dropped, so no rule here (#11): the `audio-dev-*` files and the dev test signals the audio service
read and wrote, and with them the echo cancellation mode "off", which only a dev file could set.
Compatibility with the Python apps' `audio_*` messages, their UDP media flow and its keys goes too
(#14, ADR 0003). Linux has no audio here (#1). Nothing in this domain comes later.

No parser is the audio domain's own: audio's messages decode as `link.frame.*` says (`link-frame`),
its store parts as `store.file.*` says (`store-file`), and an audio stream's media as
`spec/media.md` says (`media-frame`). The virtual microphone takes no control message from Beamer,
so there is no `hal-control` harness (#9: Beamer reaches it only through public Core Audio calls
and device UIDs). The audio routes, their statuses and the audio streams they start are #15's
`audio` state machine.

A Mac is a machine running macOS, and a PC one running Windows. Every rule is `ci` unless it says
otherwise. Behaviour across machines is tested through `beamer-sim`, with fake audio devices.
Translation code (what a machine makes of what Core Audio or Windows says about a device) is `ci`
on a hosted runner of its own OS (#15); a rule that only real audio devices, the installed virtual
microphone or a person's ears can show is `machine` or `human`.

## A machine's audio

r[audio.state.reported]
Each machine's record says whether its audio is starting, running or failed: starting until its
audio devices capability first reports, running while that capability is ready or needs
permission, and failed while it is refused.

> r[audio.state.reasons]
> When a machine's audio fails, its record says why, as the capability reported it, and nothing of
> its audio runs: every audio stream it sends or takes stops, each peer that took one stops playing
> it, and it sends and takes none while its audio stays failed.
>
> - parity: Python's audio also restarted a crashed audio helper with a growing wait, and gave up after repeated crashes until the user pressed Retry (ADR 0004: Beamer is one process, with no audio helper to restart)

> r[audio.state.recovers]
> When a machine's audio runs again after it failed, the audio streams the routes call for to and
> from that machine start again, with nothing set again.
>
> - model: audio

r[audio.state.has-audio]
A machine has audio when its record reports its audio devices capability in any state but
unsupported here, as a Mac and a PC do. A machine without audio is never one whose devices Core
lists, and an audio route naming it waits for its machine (`audio.mic.machine-gone`,
`audio.sound.waiting`).

> r[audio.peer.gated]
> This machine sends audio's messages (asking a peer to take an audio stream, its answer, the end
> of a stream, watching levels, levels, and testing a microphone) and audio media to a peer only
> while both machines have audio (`audio.state.has-audio`), and takes none from a peer without
> audio. A peer without audio gets no audio message from any machine.
>
> - parity: Python announced audio in each link's handshake capabilities and again in an audio hello, and gave a peer that asked for a newer audio protocol nothing (#14: capabilities become facts in each machine's own record)

> r[audio.message.malformed]
> An audio message from a peer that names an audio stream this machine does not know, a device key
> that is empty or longer than 200 characters, or a name longer than 128 characters is ignored, and
> the link stays up.
>
> - fuzz: link-frame

## Devices

> r[audio.devices.record]
> Each machine's record lists its microphones and its outputs, in the order the OS reports them.
> Each has its device key, its name, how it is attached (built in, USB, Bluetooth, virtual, HDMI or
> other), how many audio channels it has, and its form (microphone, speakers, headphones, headset or
> unknown). The record also names the machine's default microphone and default output, or none.
>
> - model: audio

r[audio.devices.key]
A device's key is its Core Audio UID on a Mac and its endpoint id on a PC.

r[audio.devices.rewritten]
A machine writes its record's audio part again each time one of its parts changes: its devices,
its defaults, its virtual microphone, its audio state, its headset choices or the peers it lists
(`audio.cluster.links`). Nothing else writes it again (`store.record.unchanged`).

r[audio.devices.limits]
A machine lists at most 32 microphones and 32 outputs, the first the OS reports. It cuts a device's
name to 128 characters and its key to 200, leaves out a device with no key, and lists a device key
once.

r[audio.devices.lenient]
In a peer's record, an attachment Beamer does not know reads as other, a form it does not know as
unknown, and a count of audio channels outside 0 to 64 as 0.

r[audio.devices.never-listed]
The virtual microphone and its feed, and VB-CABLE's "CABLE Output", "CABLE Input" and
"CABLE In 16 Ch", are never listed among a machine's microphones and outputs. When the OS's default
microphone or default output is one of them, the record names no default.

> r[audio.devices.left-alone]
> Beamer never lists, opens, mutes or changes a device it leaves alone, and a default that is one of
> them is named as none. On a Mac, it leaves alone a device attached as virtual whose name or
> manufacturer contains "Elgato", in any case, or whose name is exactly one of "Wave Link Stream",
> "Wave Link Monitor", "Wave Link MicrophoneFX", "Wave Link System", "Wave Link Music",
> "Wave Link Browser", "Wave Link Voice Chat", "Wave Link Game", "Wave Link SFX", "Wave Link Aux 1"
> and "Wave Link Aux 2". On a PC, it leaves alone:
>
> - a device attached as virtual whose name, adapter name or manufacturer contains "Elgato";
> - whatever its attachment, a device named "Personal Mix (Elgato Virtual Audio)",
>   "Stream Mix (Elgato Virtual Audio)", "Chat Mix (Elgato Virtual Audio)",
>   "System (Elgato Virtual Audio)" or "Wave 3 (Elgato Virtual Audio)";
> - a device whose adapter's instance id starts with `TUSBAUDIO_ENUM\VID_0FD9&PID_0096` or
>   `USB\VID_0FD9&PID_0096`, or whose adapter, or any device above it, has an instance id starting
>   with `ROOT\ELGATO_ROOT_DEVICE`, so that Wave Link's devices stay left alone when a driver update
>   renames their adapter or attaches them as USB.
>
> On a PC, names and ids are compared without case. A physical Elgato device, such as a Wave:3 on
> USB, is listed like any other.

> r[audio.devices.mac-form]
> On a Mac, a device's microphone and its output each take a form, the first of these that holds.
> A name says headset when it contains "headset", "airpods" or "buds", and names are compared
> without case.
>
> 1. Built in: a microphone is a headset when its data source is the external microphone, and a
>    microphone otherwise; an output is headphones when its data source is headphones, and speakers
>    otherwise.
> 2. Bluetooth: a device whose name contains "speaker" is a microphone when it records and speakers
>    when it plays; else a microphone is a headset, and an output is a headset when the device also
>    records or its name says headset, and headphones otherwise.
> 3. Its terminal type: headphones are headphones, a headset microphone is a headset, a speaker, an
>    LFE speaker or a receiver speaker is speakers, and a microphone or a receiver microphone is a
>    microphone, except that a USB device that both records and plays is never speakers or a
>    microphone by its terminal type.
> 4. USB: a device whose name contains "speaker" is a microphone when it records and speakers when it
>    plays; else a device that both records and plays, or whose name says headset, is a headset; else
>    a device whose name contains "headphone" is headphones; else a microphone is a microphone and an
>    output is unknown.
> 5. HDMI or DisplayPort: an output is speakers and a microphone is unknown.
> 6. Any other device: a headset when its name says headset; else an output whose name contains
>    "headphone" is headphones; else unknown.

> r[audio.devices.pc-form]
> On a PC, a device's form is the first of these that holds. Its description is the endpoint's own
> name without its adapter's, such as "Headphones" in "Headphones (2- Elgato Wave:3)". It says
> headset when its adapter's name contains "headset", or its description contains "headset",
> "micro-casque" or "sur casque"; it says headphones when its description contains "headphone", is
> "casque" or starts with "casque ". All are compared without case.
>
> 1. Its endpoint's form factor is headphones: headphones.
> 2. Its form factor is headset or handset: a headset.
> 3. Its form factor is microphone: a headset when it says headset, and a microphone otherwise.
> 4. It is attached by USB or Bluetooth and says headset: a headset.
> 5. It says headphones: headphones when it is an output, and unknown when it is a microphone.
> 6. Its form factor is speakers or digital display: speakers.
> 7. Else an output is unknown and a microphone is a microphone.

## Headset choices

> r[audio.form.choice]
> Each output of each machine has a headset choice, a shared entry keyed by its machine and its
> device key: headset, speakers, or as the OS says, which is its default. Any machine may write any
> output's choice.
>
> - model: audio

> r[audio.form.applied]
> An output's form in its machine's record is its headset choice when that choice is headset or
> speakers, and the form the OS gives it otherwise. So a choice made on any machine changes the form
> every machine reads in that output's record.
>
> - model: audio

> r[audio.form.kept]
> Every headset choice is kept, however many outputs have one.
>
> - parity: Python kept at most 64 headset choices, dropping the oldest "as the OS says" ones first and then the oldest of the rest (#14: a shared entry is never removed, `store.entry.never-removed`)

## Audio routes

> r[audio.route.mic]
> The microphone's audio route is one shared entry: off, which is its default, or a machine and
> either that machine's default microphone or one named microphone of it, named by its device key
> and its name. The microphone it names is the shared microphone.
>
> - model: audio

> r[audio.route.sound]
> Each machine's sound has its own audio route, a shared entry keyed by that machine: here, which is
> its default, or another machine and either that machine's default output or one named output of
> it, named by its device key and its name.
>
> - model: audio

> r[audio.route.any-machine]
> Any machine may write either audio route, including the sound route of another machine. A
> machine's sound route written to send it to that same machine is written as here.
>
> - model: audio

> r[audio.route.apart]
> Two machines writing the sound routes of two different machines at once both keep their write on
> every machine, and a write of the microphone's route leaves every sound route as it was.
>
> - model: audio

> r[audio.route.schema]
> An audio route or headset choice arriving from a peer is ignored (`store.entry.schema`) when it
> names a machine by a malformed machine id, sends a machine's sound to that same machine, names a
> device with an empty key, a key longer than 200 characters or a name longer than 128 characters,
> or holds a headset choice other than headset, speakers or as the OS says.
>
> - model: audio

> r[audio.route.default-follows]
> An audio route naming a machine's default microphone or default output uses whichever device is
> that machine's default when the stream starts, and moves to the new default when the default
> changes while the stream runs.
>
> - model: audio

> r[audio.route.found]
> An audio route naming a device finds it on its machine by its device key, else as the one device
> of that machine with its name, as when a USB device moved to another port. With no device of that
> key, and none or several of that name, the device is gone.
>
> - model: audio

## The cluster

> r[audio.cluster.links]
> Each machine's record lists the peers it has a link up with that have audio
> (`audio.state.has-audio`).
>
> - model: audio

> r[audio.cluster.in]
> For audio, this machine counts a machine as in the cluster when it is this machine, a peer with
> audio it has a link up with, or a machine that a machine already counted lists
> (`audio.cluster.links`) and that lists it back. A machine one of them lists without being listed
> back is not counted.
>
> - model: audio

> r[audio.cluster.no-timer]
> When a link goes down, the machines reached only through it leave the cluster as soon as the
> records that no longer list it arrive, with no time-out. Their devices stay known, and their
> audio routes and headset choices stay as they were.
>
> - model: audio

> r[audio.cluster.same-status]
> Every machine of the cluster reports the same status for the microphone's route and for each
> sound route, once their stores hold the same entries and records, apart from failed while a
> stream cannot start (`audio.status.failed`). Only the machine that would send that stream reports
> it; every other machine reports the status the route's other steps give.
>
> - model: audio

## The shared microphone

> r[audio.mic.off]
> While the microphone's route is off, no microphone stream runs on any machine, and its status is
> off.
>
> - model: audio

> r[audio.mic.machine-gone]
> While the shared microphone's machine is not in the cluster, or has no audio, no microphone stream
> runs and the status waits for that machine.
>
> - model: audio

> r[audio.mic.device-gone]
> While the route names the default microphone of a machine that has none, or a named microphone
> that is gone there (`audio.route.found`), no microphone stream runs and the status waits for the
> device, naming the microphone by the route's name.
>
> - model: audio

> r[audio.mic.on-demand]
> The shared microphone streams from its machine to each other machine that has a link up with it,
> has its virtual microphone installed, and on which an app records its virtual microphone, and to
> no other machine. It streams into that machine's virtual microphone.
>
> - model: audio

> r[audio.mic.can-tell]
> Each machine's record says whether it can tell when an app records its virtual microphone. A
> machine that cannot is treated as always recording it while a microphone is shared.
>
> - model: audio

> r[audio.mic.keeps-running]
> A microphone stream keeps running for 5 s after the record of the machine it goes to stops saying
> that an app records its virtual microphone, and stops then unless an app records it again
> meanwhile.
>
> - model: audio

> r[audio.mic.direct-only]
> The shared microphone streams only to a machine its machine has a link up with. A machine that
> records it without such a link gets no stream, and the status says it is unreachable, naming
> that machine.
>
> - model: audio

> r[audio.mic.no-virtual]
> A machine counted as recording the virtual microphone, as one that cannot tell is
> (`audio.mic.can-tell`), gets no stream while it has no virtual microphone installed, and the
> status says it has no virtual microphone, naming that machine.
>
> - model: audio

> r[audio.mic.own-machine]
> While an app on the shared microphone's own machine records its virtual microphone, that machine
> plays the shared microphone into its own virtual microphone, with no codec and no echo
> cancellation, so apps can keep the virtual microphone chosen whichever machine's microphone is
> shared.
>
> - model: audio

> r[audio.mic.status]
> The microphone's route has one status, the first of these that holds, whichever other rules of
> this section also hold:
>
> 1. off (`audio.mic.off`);
> 2. waiting for the shared microphone's machine (`audio.mic.machine-gone`);
> 3. waiting for the device (`audio.mic.device-gone`);
> 4. failed, on the shared microphone's machine only, while any of the microphone streams it sends
>    to other machines cannot start, naming every machine whose stream cannot start
>    (`audio.status.failed`); every other machine goes on to the next step;
> 5. streaming, while the shared microphone streams to at least one machine, its own included,
>    naming those machines;
> 6. unreachable, when any machine recording it is (`audio.mic.direct-only`), and else without a
>    virtual microphone (`audio.mic.no-virtual`), naming in either case every machine recording it
>    that gets no stream;
> 7. else ready: chosen, with no app recording it.
>
> - model: audio

> r[audio.mic.many]
> When apps on several machines record the virtual microphone, each of them gets its own stream of
> the shared microphone.
>
> - model: audio

## Echo cancellation

> r[audio.echo.when]
> A microphone stream to another machine has its echo cancelled when the shared microphone's
> machine can cancel echo, as its record says, the form of that machine's default output, its
> headset choice first, is neither headphones nor a headset, and that default output plays
> something (`audio.echo.idle`). Its echo is never cancelled otherwise, and never on a machine's own
> virtual microphone (`audio.mic.own-machine`). The echo is cancelled against what that default
> output plays.
>
> - model: audio

> r[audio.echo.idle]
> A machine's default output plays nothing while its own sound streams to another machine and no
> other machine's sound streams to that output.
>
> - model: audio

r[audio.echo.reported]
Core reports whether echo is cancelled on a running microphone stream of this machine.

> r[audio.echo.restarts]
> When whether a microphone stream's echo is cancelled changes, that stream stops and starts again
> with or without it.
>
> - model: audio

## A machine's sound

> r[audio.sound.here]
> While a machine's sound route is here, that machine plays its own sound, no sound stream leaves
> it, and the route's status is here.
>
> - model: audio

> r[audio.sound.sent]
> While a machine's sound route sends its sound to another machine, and none of
> `audio.sound.waiting`, `audio.sound.unreachable`, `audio.sound.cannot-capture`,
> `audio.sound.device-gone` and `audio.sound.muted-there` stops it, that machine streams its sound
> to the output the route names on the other machine, and plays none of it itself. The route's
> status is streaming, but failed on the sending machine while its stream cannot start
> (`audio.sound.status`). The machine whose sound it is sends the stream, whichever machine wrote
> the route.
>
> - model: audio

> r[audio.sound.status]
> A sound route has one status, the first of these that holds, whichever other rules of this
> section also hold:
>
> 1. here (`audio.sound.here`);
> 2. waiting for the other machine (`audio.sound.waiting`);
> 3. unreachable (`audio.sound.unreachable`);
> 4. failed, because the sending machine cannot capture its sound (`audio.sound.cannot-capture`);
> 5. waiting for the device (`audio.sound.device-gone`);
> 6. the output muted there (`audio.sound.muted-there`);
> 7. failed, on the sending machine only, while its stream cannot start (`audio.status.failed`);
>    every other machine goes on to the next step;
> 8. else streaming (`audio.sound.sent`).
>
> - model: audio

> r[audio.sound.waiting]
> While either machine of a sound route is not in the cluster or has no audio, no stream runs, the
> machine plays its own sound, and the status waits for the other machine.
>
> - model: audio

> r[audio.sound.unreachable]
> While the two machines of a sound route are in the cluster without a link up between them, no
> stream runs, the machine plays its own sound, and the status says they are unreachable.
>
> - model: audio

> r[audio.sound.cannot-capture]
> A machine whose record says it cannot capture its sound sends none: its sound route's status is
> failed, because that machine cannot capture its sound, and it plays its own sound.
>
> - model: audio

> r[audio.sound.device-gone]
> While the output a sound route names is gone on the other machine (`audio.route.found`), or the
> route names that machine's default output and it has none, no stream runs, the machine plays its
> own sound, and the status waits for the device. When the output comes back, by its key or as the
> one output with its name, the stream starts again with nothing set again.
>
> - model: audio

> r[audio.sound.muted-there]
> A PC that sends its own sound to another machine mutes its default output to do so, so it cannot
> play another machine's sound on that output meanwhile. While a sound route names a PC's output
> that is its default output, and that PC's own sound route sends its sound away, no stream runs,
> the machine plays its own sound, and the status says the output is muted there. Another output of
> that PC is not affected.
>
> - model: audio

> r[audio.sound.not-beamer]
> A machine's sound sent to another machine is everything its apps play, apart from what Beamer
> plays: another machine's sound streaming to it, and the shared microphone played into its virtual
> microphone, never go back out in it.
>
> - tier: machine (only a real capture of a machine's sound shows what it holds)

> r[audio.sound.silent-here]
> While a machine's sound streams to another machine, its outputs play none of its apps' sound. On a
> Mac, Beamer's own playing, such as another machine's sound streaming to it, still plays. On a PC
> the default output is muted.
>
> - tier: machine (only a real output shows what it plays)

> r[audio.sound.restored]
> When a machine's sound stops streaming, because its route went back to here, the stream could not
> go on, the link went down or Beamer quit, its output plays its sound again within 1 s. On a PC the
> output's mute and its volumes, each channel's included, are put back exactly as they were, after a
> device change that unplugged it meanwhile once it comes back, and at the next start of Beamer when
> Beamer was killed with the output muted.
>
> - tier: machine (only a real output's mute and volumes show it)
> - parity: Python's PC also put a killed audio helper's mutes back within about 1 s through a guard process of its own (ADR 0004: the watchdog runs no Beamer logic, so a muted output is put back at the start the watchdog relaunches)

> r[audio.sound.user-wins]
> On a PC, while Beamer holds an output muted for a sound stream, a volume change the user makes to
> it is the volume put back when the stream ends, and an unmute the user makes leaves it playing
> until the stream ends, unmuted afterwards.
>
> - tier: machine (only a real output's volume control shows it)

r[audio.sound.no-defaults]
Beamer never changes which microphone or output the OS uses by default, on any machine.

## Statuses

> r[audio.status.failed]
> When an audio stream cannot start, its route's status on the machine that would send it is
> failed, naming the machine it goes to and why: the device is gone, it is a device Beamer leaves
> alone (`audio.devices.left-alone`), another app holds it, the OS does not allow it, or the machine
> cannot do it, on the machine that would send; or, as the machine that would take it answers, its
> output is gone, it has no virtual microphone, its output is muted there, or it cannot do it. While
> several streams of the route cannot start at once, as the shared microphone's can
> (`audio.mic.many`), the status names every machine whose stream cannot start, in machine id
> order, each with its own reason. Every other machine, the one that would take a stream included,
> reports the status the route's other steps give (`audio.mic.status`, `audio.sound.status`).
>
> - parity: Python's failed status named one machine, with its reason: of the streams that could not start, the one opened last (`_compose_snapshot` in `core/audio/service.py` set the status once for each failed stream, in the order the streams were opened, which nothing shows; naming every machine gives a status a test can decide)
> - model: audio

> r[audio.status.retry]
> A failed audio stream is asked for again only when the record of the machine it goes to changes,
> when an audio route or headset choice changes, when a prompt for the permission it waited on is
> answered (`audio.permission.answered`), when the link to that machine comes up again
> (`audio.link.up`), or when the sending machine's audio runs again after it failed
> (`audio.state.recovers`). Nothing else asks for it again.
>
> - model: audio

r[audio.status.streams]
Core reports each audio stream this machine sends or takes, and the one into its own virtual
microphone: whether it is a microphone stream or a sound stream, the machine it comes from and the
machine it goes to, and whether it is starting, running or failed. A route whose status is
streaming has its stream among those its sending machine reports.

## Audio streams

> r[audio.stream.ask]
> To start an audio stream, the sending machine asks the receiving machine to take it, saying
> whether it is a microphone stream or a sound stream, and where it plays: the virtual microphone,
> or an output, its default or one named by device key and name.
>
> - model: audio

> r[audio.stream.take]
> The receiving machine takes an audio stream from a peer it has a link up with when what it plays
> into is there: its virtual microphone installed, or the output found (`audio.route.found`) and not
> muted there (`audio.sound.muted-there`). Otherwise it refuses, saying which.
>
> - model: audio

> r[audio.stream.one-each]
> A machine takes at most one microphone stream and one sound stream from each peer. A new one of
> the same kind from that peer replaces the one it had.
>
> - model: audio

> r[audio.stream.gone-there]
> When what a stream it takes plays into goes away (its output gone, its virtual microphone gone, or
> its output muted there), the receiving machine stops the stream and tells the sending machine, and
> the sending machine plays its own sound again.
>
> - model: audio

> r[audio.stream.once]
> Writing the microphone's route and a sound route together, from either machine, delivered in one
> change or one after the other, starts each stream they call for once, and stops none.
>
> - model: audio

> r[audio.stream.untouched]
> A running audio stream is never stopped or started again by a change that does not change what it
> carries or where it plays: another machine's sound route, a headset choice that does not change
> its echo cancellation, or a write of an audio route with the value it has.
>
> - model: audio

> r[audio.stream.change]
> When the route a running audio stream serves changes what it carries or where it plays, the stream
> stops and, when the route still calls for one, a new one starts. A sound route sent back to here
> stops its stream, and the machine plays its own sound again.
>
> - model: audio

## Links and restarts

> r[audio.link.down]
> When the link to a peer goes down, every audio stream to or from that peer stops at once, a
> machine whose sound streamed there plays it itself again, and this machine's record no longer
> lists that peer (`audio.cluster.links`).
>
> - model: audio

> r[audio.link.up]
> When the link to a peer comes up again, the audio streams the routes call for between the two
> start again, with nothing set again.
>
> - model: audio

> r[audio.link.restart]
> After either machine restarts, the audio routes are as they were, and once the link is up again
> the streams they call for start again, with nothing set again.
>
> - model: audio

r[audio.link.quit]
When Beamer quits, every audio stream it sends or takes stops, and each peer that took one stops
playing it.

## Permissions

r[audio.permission.mic]
While the OS does not allow Beamer to record this machine's microphones, its audio devices
capability says so, and a microphone stream from this machine fails because the OS does not allow
it (`audio.status.failed`).

r[audio.permission.silent]
When a microphone stream of this machine, or the one into its own virtual microphone, delivers
nothing but digital silence for 5 s, Core reports that a microphone here sends only silence. A
sound stream never counts.

> r[audio.permission.mac-sound]
> On a Mac, recording its sound needs the permission to record system audio. While its prompt waits
> for an answer the capability reports needs permission, and once a capture of the Mac's sound has
> delivered anything, silence included, it reports the permission granted. macOS gives no answer
> that says it refused.
>
> - tier: machine (only a real Mac shows its system audio recording prompt and taps)

> r[audio.permission.answered]
> A microphone stream that failed because the OS did not allow it starts once the permission is
> granted, and a sound stream that failed while the prompt for recording the Mac's sound waited
> starts once that prompt is answered.
>
> - model: audio

## Levels

r[audio.levels.watch]
While Core is asked to report audio levels, it reports this machine's, and asks each peer with
audio it has a link up with to send its own, a peer whose link comes up meanwhile included; when
asked to stop, it tells those peers to stop. A request that repeats the one before changes nothing.

r[audio.levels.what]
A level is the root mean square and the peak of one device's samples, over all its channels, in
dBFS to 0.1 dB, at least -100 dBFS. Levels are reported for each microphone and output Beamer has
open on a machine, by machine and device key.

r[audio.levels.rate]
A peer that has asked for levels gets this machine's every 250 ms.

r[audio.levels.coalesced]
However often levels change, Core reports new levels at most once every 50 ms.

r[audio.levels.drop]
When Core stops reporting levels, or the link to a peer goes down, the levels and feedback reports
held from that peer are dropped.

r[audio.levels.test]
Testing a microphone of this machine, or of a peer it has a link up with, opens that microphone
for its level alone for 10 s. Testing a microphone of any other machine does nothing.

## Feedback

> r[audio.feedback.report]
> When the feedback guard of a sound stream turns it down (`spec/media.md`), Core reports feedback
> between the machine whose sound it is and the machine that plays it, with the tone's frequency,
> as active. It reports it on the machine whose sound it is, and on the playing machine while that
> machine reports levels (`audio.levels.watch`).
>
> - model: audio

> r[audio.feedback.ends]
> When the guard lets the stream go, or the stream stops while turned down, the feedback is reported
> as no longer active for 120 s, and then no more.
>
> - model: audio

r[audio.feedback.sound-only]
Only sound streams report feedback: a microphone stream never does.

## The virtual microphone

> r[audio.virtual-mic.record]
> Each machine's record says which virtual microphone it has (Beamer Microphone on a Mac, VB-CABLE on
> a PC), whether it is installed, and whether an app records it.
>
> - model: audio

> r[audio.virtual-mic.mac]
> On a Mac, the virtual microphone is installed while Core Audio has a device with the UID
> `com.beamer.audio.mic`, which apps record as Beamer Microphone. Beamer plays the shared microphone
> into the hidden device with the UID `com.beamer.audio.micfeed`, and an app records it while
> `com.beamer.audio.mic` reports that it runs somewhere.
>
> - tier: machine (only the installed virtual microphone inside coreaudiod has these devices)

> r[audio.virtual-mic.pc]
> On a PC, the virtual microphone is VB-CABLE, installed while Windows has its "CABLE Input" and
> "CABLE Output". Beamer plays the shared microphone into "CABLE Input", and an app records it while
> any other process records "CABLE Output".
>
> - tier: machine (only an installed VB-CABLE has these endpoints)

r[audio.virtual-mic.recording-ends]
A machine's record says an app records its virtual microphone as soon as one starts, and says it no
longer does 3 s after the last one stopped.

> r[audio.virtual-mic.coreaudiod]
> On a Mac, after coreaudiod restarts, as an install of the virtual microphone makes it, the streams
> into the virtual microphone start again with nothing set again.
>
> - tier: machine (only a real coreaudiod restarts)

r[audio.virtual-mic.volume]
Beamer never changes the virtual microphone's own input volume or mute: they are the user's.

r[audio.virtual-mic.vb-cable]
Beamer never installs VB-CABLE: a PC without it has no virtual microphone until the user installs
it.

> r[audio.virtual-mic.heard]
> An app recording the virtual microphone hears the shared microphone of another machine.
>
> - tier: human (whether a call app hears a voice is heard at the desk)
> - do: Share the Mac's microphone from the Audio & Camera page, open a call app's microphone test on the PC with "CABLE Output" chosen, and speak into the Mac; then share the PC's microphone, open a call app's microphone test on the Mac with "Beamer Microphone" chosen, and speak into the PC's microphone
> - see: Each call app's test plays back the voice spoken on the other machine

> r[audio.virtual-mic.sound-heard]
> A machine's sound sent to another machine plays on that machine's output, and not on its own.
>
> - tier: human (where a sound plays is heard at the desk)
> - do: Send the PC's sound to the Mac's default output from the Audio & Camera page, and play a video with sound on the PC; then set it back to here
> - see: The video's sound comes from the Mac and not from the PC, then from the PC again

## Low-latency virtual microphone

> r[audio.low-latency.setting]
> A machine's low-latency virtual microphone setting is off by default. It is kept in `local.json`
> and never leaves the machine.
>
> - parity: Python kept it in its own `audio.json`, with the echo cancellation mode (#16: settings that never sync are kept in `local.json`)

> r[audio.low-latency.pc]
> On a PC with the setting on, Beamer plays into "CABLE Input" exclusively, so no other app can play
> into it meanwhile; with it off, Beamer shares "CABLE Input" with other apps.
>
> - tier: machine (only a real VB-CABLE shows whether another app can open "CABLE Input")

> r[audio.low-latency.change]
> On a PC, changing the setting stops each stream into its virtual microphone and starts it again in
> the new mode, and touches no other stream. Setting the value it has changes nothing. On a Mac the
> setting is kept and changes nothing.
>
> - parity: Python restarted its whole audio helper with or without the low-latency flag, which stopped every audio stream of the machine (ADR 0004: Beamer is one process, with no audio helper)

## Installing the virtual microphone

> r[audio.install.carried]
> Beamer.app carries the virtual microphone in its resources. Core reports, on a Mac, whether this
> Beamer carries it, and whether the installed copy in `/Library/Audio/Plug-Ins/HAL/` is missing,
> the same as the carried one, or different from it.
>
> - parity: Python compared the bundles' version numbers, left an installed copy newer than the carried one alone, and replaced one it could not read or whose identifier was not Beamer's (#21: `beamer install` compares the carried copy's hash with the installed one's)

r[audio.install.busy]
On a Mac, installing the virtual microphone from the Audio & Camera page is refused, with nothing
installed, while an audio stream of this machine runs or starts: the install restarts coreaudiod,
and every app on the Mac goes quiet for about a second.

r[audio.install.page]
Installing the virtual microphone from the Audio & Camera page runs `beamer --elevated
install-driver` (`app.elevated.steps`) and reports how it ended: installed, cancelled when the user
declined the administrator prompt, not carried by this Beamer, refused because a stream started
first, or failed, with the step's output in the log.

> r[audio.install.copies]
> `beamer --elevated install-driver` copies the carried virtual microphone into
> `/Library/Audio/Plug-Ins/HAL/` in place of any copy there, and restarts coreaudiod.
>
> - tier: machine (only a real Mac's coreaudiod loads the copy)

> r[audio.install.pinned]
> `beamer --elevated install-driver` installs nothing, and ends with a non-zero exit status, when the
> carried virtual microphone's bundle identifier is not `com.beamer.audio.driver`, when its signature
> does not check out, or when it is not signed by the pinned certificate.
>
> - tier: machine (only a real Mac's codesign checks a signature)

> r[audio.install.root]
> `beamer --elevated install-driver` run without administrator rights installs nothing and ends
> with a non-zero exit status.
>
> - tier: machine (only a real Mac shows a step run with and without root)

> r[audio.install.remove]
> `beamer --elevated remove` removes the installed virtual microphone and restarts coreaudiod
> (`app.uninstall.removes`).
>
> - tier: machine (only a real Mac's coreaudiod drops the removed device)
