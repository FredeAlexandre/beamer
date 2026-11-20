# Camera

A Mac's camera goes where the user sends it: the apps on every PC of the cluster see one shared
camera, from a Mac, through the virtual camera, Beamer Camera. It is F44 of #11's inventory,
which ships. The camera route says which Mac's camera the PCs see, and the
camera quality says at what size; both are shared entries. A camera stream runs only while an app
on a PC has its virtual camera open, so the Mac's camera is on only while an app wants it.

The rules come from the Python app at `archive/python` (the route, the plan and the statuses in
`core/camera/model.py`, `core/camera/planner.py` and `core/camera/part.py`, the limits in
`core/camera/wire.py`, the sharing switch in `core/camera/store.py`, the placeholder words in
`core/camera/texts.py`, the virtual camera's install in `win_app/vcam_install.py`, and the OS's part
in the media helper's video backends under `native/audio/mac/` and `native/audio/win/` and in the
virtual camera's source under `native/audio/vcam-win/`), mined through its tests in
`spec/ledger/camera.txt`, and from the measured design in `dev/camera/PHASE2.md`. They take #14's
store, in which the camera route and the camera quality are shared entries and each machine's
cameras, camera access, camera sharing and virtual camera are parts of its machine record, #14's
media traffic class (`link.class.media-datagrams`), ADR 0004's one process with no media helper,
#9's contract with the virtual camera, which stays the C++ source Frame Server loads, and #21's
install of it.

What stays elsewhere: how shared entries and machine records are stamped, merged, exchanged and
kept, and how a value that breaks its schema is ignored, are `spec/store.md`'s (`store.entry.*`,
`store.record.*`, `store.sync.*`); how camera's messages and media travel, and how a message that
does not decode is dropped, are `spec/link.md`'s (`link.class.control`,
`link.class.media-datagrams`, `link.frame.undecodable`); what is inside a camera stream (its media
framing, the H.264 codec and its settings, loss recovery, the playout delay and lip sync with a
microphone stream) is `spec/media.md`'s (#64); which machines count as in the cluster for media is
`spec/audio.md`'s (`audio.cluster.in`, `audio.cluster.links`); how a capability reports its state
is `spec/platform.md`'s (`platform.capability.*`); the `--elevated` command line, `beamer install`,
`beamer uninstall` and the prompt for administrator rights are `spec/app.md`'s (`app.elevated.*`,
`app.install.*`, `app.uninstall.*`); where `local.json` is kept and when it is written are
`spec/runtime.md`'s (`runtime.files.*`); how a machine is named is `spec/viewmodel.md`'s
(`viewmodel.machines.label.*`); and every word the Audio & Camera page and the tray menu show is
`spec/viewmodel.md`'s (`viewmodel.camera.*`).

Dropped, so no rule here (#11): the `audio-dev-*` files and the dev keys the camera read from them,
with the synthetic test source and the forced refusals they set, and the camera's end-to-end
harness under `dev/camera/e2e/`, which drove them. Compatibility with the Python apps' `camera_*`
messages, the `camera1` and `video1` feature words, their UDP media flow, its keys and its fallback
through the link goes too (#14, ADR 0003). Linux has no camera here (#1). Nothing in this domain
comes later.

One parser is the camera domain's own: what Beamer reads back from the shared memory the virtual
camera writes (`camera-shm`). Camera's messages decode as `link.frame.*` says (`link-frame`), its
store parts as `store.file.*` says (`store-file`), and a camera stream's media as `spec/media.md`
says (`media-frame`). The camera route, its statuses and the camera streams they start are #15's
`camera` state machine.

A Mac is a machine running macOS, and a PC one running Windows. Every rule is `ci` unless it says
otherwise. Behaviour across machines is tested through `beamer-sim`, with fake cameras and a fake
virtual camera. A rule that only a real camera, a real Frame Server or a person's eyes can show is
`machine` or `human`.

## A machine's camera

> r[camera.state.kinds]
> A Mac's record says it can share its camera, and a PC's record says it has the virtual camera.
> A machine has a camera when its record says either, as a Mac and a PC do. A machine without a
> camera is never a camera machine and never takes a camera stream.

> r[camera.state.reported]
> Each machine's record says whether its camera is starting, running or failed. A Mac's comes from
> its camera devices capability (`platform.capability.states`): starting until that capability first
> reports, running while it is ready or needs permission, and failed while it is refused or
> unsupported here. A PC captures no camera, and its camera never fails: it is starting until Beamer
> has first read its virtual camera's install state (`camera.virtual.state`), and running from then
> on.
>
> - parity: Python's PC reported its camera failed while its media helper had failed or ran without video (ADR 0004: there is no media helper, and a PC's virtual camera's install state says what it can do)

> r[camera.state.failed]
> When a machine's camera fails, nothing of its camera runs: every camera stream it sends or takes
> stops, each peer that took one stops showing it, and it sends and takes none while its camera
> stays failed.
>
> - parity: Python's camera ran inside the media helper, whose restart closed every camera stream with the reason "helper" and sent the machine's list again as starting (ADR 0004: Beamer is one process, with no media helper to restart)
> - model: camera

> r[camera.state.recovers]
> When a machine's camera runs again after it failed, the camera streams the camera route calls for
> to and from that machine start again, with nothing set again.
>
> - model: camera

> r[camera.peer.gated]
> This machine sends camera's messages (asking a peer to take a camera stream, its answer, and the
> end of a stream) and camera media to a peer only while both machines have a camera
> (`camera.state.kinds`), and takes none from a peer without one. A peer without a camera gets no
> camera message from any machine.
>
> - parity: Python sent `camera_*` messages only to a peer whose link had the `audio` capability and whose audio hello carried the word `camera1`, which a machine announced only when its media helper had video (#14: capabilities become facts in each machine's own record)
> - model: camera

> r[camera.message.malformed]
> A camera message from a peer that names a camera stream this machine does not know, a camera key
> that is empty or longer than 200 characters, or a name longer than 128 characters is ignored, and
> the link stays up.
>
> - fuzz: link-frame

## Cameras

> r[camera.devices.record]
> Each Mac's record lists its cameras, in the order macOS reports them. Each has its camera key,
> its name, and how it is attached: built in, USB, Continuity, virtual or other. The record also
> names the camera macOS prefers, or none.
>
> - model: camera

r[camera.devices.key]
A camera's key is the unique id macOS gives the capture device, and the camera macOS prefers is
its system preferred camera.

> r[camera.devices.mac]
> On a Mac, the cameras listed are the built-in wide-angle, external and Continuity video devices
> macOS reports. A Continuity camera is attached as Continuity; otherwise a device whose transport
> is built in, USB or virtual is attached as that, a built-in wide-angle device is built in, and
> any other is other.
>
> - tier: machine (only a real Mac reports its capture devices)

r[camera.devices.limits]
A Mac lists at most 16 cameras, the first macOS reports. It cuts a camera's name to 128 characters
and its key to 200, leaves out a camera with no key, and lists a camera key once. A camera macOS
prefers whose key is longer than 200 characters is named as none.

r[camera.devices.rewritten]
A machine writes its record's camera part again each time one of its parts changes: its cameras,
the camera macOS prefers, its camera access, its camera sharing, its capture, its camera state or
its virtual camera. Nothing else writes it again (`store.record.unchanged`).

> r[camera.devices.kept]
> A machine's camera part stays known on every machine as long as its record does
> (`store.record.away`), and goes with it when that machine is forgotten (`store.forget.record`).
>
> - parity: Python held the camera lists of at most 32 other machines, making room by dropping the one offline longest, and dropped a list once its machine had been offline for 30 days (#14: a machine record is dropped only when its machine is forgotten)

## Capture

> r[camera.capture.record]
> Each Mac's record says its camera's capture: idle, starting, running, interrupted, or failed with
> why (the camera is gone, macOS does not allow it, or it failed); which of macOS's video effects
> are on (Center Stage, Portrait, Studio Light, Background, Reactions, Edge Light); and whether
> another app uses the camera too.
>
> - model: camera

> r[camera.capture.which]
> A Mac's capture is the first of these that holds: failed because the camera is gone, when the
> camera of a stream it sends went away; running, or interrupted when macOS interrupts it, while
> any stream it sends runs; starting while any stream it sends starts; failed with the reason of a
> stream it sends that failed because the camera is gone, macOS does not allow it, or the camera
> failed; else idle.
>
> - model: camera

> r[camera.capture.mac]
> On a Mac, the effects and whether another app uses the camera are read from the camera once a
> second while it captures. When macOS interrupts the capture, it is tried again every 2 s and runs
> again once macOS ends the interruption. A camera unplugged while it captures is gone.
>
> - tier: machine (only a real camera reports its effects, interruptions and unplugging)

> r[camera.capture.size]
> A Mac captures each camera stream at the camera quality's size: in a format of that size when the
> camera offers one, else in its smallest larger format, centre-cropped to 16:9 and scaled. It
> scales one picture per camera, whatever the number of streams.
>
> - tier: machine (only a real camera offers its formats)

## The camera route

> r[camera.route.entry]
> The camera route is one shared entry: off, which is its default, or a machine and either the
> camera that machine prefers, followed, or one named camera of it, named by its camera key and its
> name. The camera it names is the shared camera, and that machine is the camera machine.
>
> - model: camera

> r[camera.route.quality]
> The camera quality is one shared entry: 720p, which is its default, or 1080p.
>
> - model: camera

> r[camera.route.any-machine]
> Any machine may write the camera route and the camera quality. A write made on any machine
> replaces the value every machine holds, whatever their clocks say (`store.stamp.after-seen`).
> Writing the camera route off names no camera.
>
> - model: camera

> r[camera.route.apart]
> Two machines writing the camera route and the camera quality at once both keep their write on
> every machine.
>
> - model: camera

> r[camera.route.schema]
> A camera route or camera quality arriving from a peer is ignored (`store.entry.schema`) when it
> names a machine by a malformed machine id, names a camera with an empty key, a key longer than
> 200 characters or a name longer than 128 characters, or holds a quality other than 720p or 1080p.
>
> - parity: Python's route message carried up to 8 entries it did not know, of at most 1 KiB each, which it kept, saved and sent on, and it dropped on its own an entry stamped more than 600 s ahead of its clock (#14: the protocol version must match exactly, so a key no domain declares is ignored, `store.entry.schema`, and a stamp ahead is taken, `store.stamp.ahead`)
> - model: camera

> r[camera.route.found]
> A camera route naming a camera finds it on its Mac by its camera key, else as the one camera of
> that Mac with its name, as when a USB camera moved to another port. With no camera of that key,
> and none or several of that name, the camera is gone.
>
> - model: camera

> r[camera.route.preferred]
> A camera route following the camera a Mac prefers uses whichever camera macOS prefers when the
> stream starts, and moves to the new one when macOS's choice changes while the stream runs.
>
> - model: camera

> r[camera.route.off]
> Writing the camera route off, on any machine, stops every camera stream, and the camera
> machine's camera stays off until the route names a camera again.
>
> - model: camera

## The camera quality

r[camera.quality.size]
A camera stream at 720p is 1280 by 720 pixels at 30 frames a second and 2.5 Mbit/s, and at 1080p
1920 by 1080 pixels at 30 frames a second and 5 Mbit/s.

> r[camera.quality.whatever-the-app]
> Every camera stream is sent at the camera quality, whatever size and frame rate the app on the PC
> asked the virtual camera for.
>
> - model: camera

> r[camera.quality.change]
> When the camera quality changes, each running camera stream changes to the new size and goes on
> running: it is not stopped or started again.
>
> - model: camera

## The cluster

> r[camera.cluster.in]
> For the camera, a machine is in the cluster when audio counts it there (`audio.cluster.in`), and
> it has a link up with the camera machine when the camera machine is this machine and has one, or
> when the camera machine's record lists it (`audio.cluster.links`).
>
> - model: camera

> r[camera.cluster.same-status]
> Once their stores hold the same entries and records, every machine of the cluster reports the same
> status for each machine and the same status for the camera route, apart from steps 7 and 8 of
> `camera.status.machine`. Those rest on a camera stream's own state, which only its two ends see:
> only the camera machine reports step 7, for the machine a stream goes to, and only the machine
> taking a stream reports step 8, for itself; every other machine reports step 9 for that machine.
> So failed while a stream cannot start (`camera.status.failed`) is reported only on the camera
> machine, and reduced (`camera.status.reduced`) only on a stream's two ends. Where step 10 of
> `camera.status.route` builds the route's status from machine statuses that differ this way, the
> route's status may differ too.
>
> - model: camera

## Camera sharing

> r[camera.sharing.setting]
> Each Mac has a camera sharing setting, on by default, which only that Mac changes. It is kept in
> `local.json`.
>
> - parity: Python kept it in its own `camera.json`, with the camera route and the lists of every machine (#16: settings that never sync are kept in `local.json`)

> r[camera.sharing.record]
> A Mac's record says its camera sharing. A machine's camera sharing is read only from its own
> record, and only when that record says it can share its camera: a record that says it cannot
> says nothing of sharing, and a record of this machine arriving from a peer never changes its
> camera sharing (`store.record.own`).
>
> - model: camera

> r[camera.sharing.off]
> While a Mac's camera sharing is off, it never turns its camera on for a camera stream, whatever the
> camera route says and whichever machine wrote it: every stream it sends stops at once, telling
> the receiving machine that sharing is off, and the camera route's status is sharing off on every
> machine. No other machine can turn it on.
>
> - model: camera

> r[camera.sharing.restart]
> After a restart, a Mac's camera sharing is as it was, and with it off no camera stream starts, not
> even before its first record is written.
>
> - model: camera

> r[camera.sharing.refused]
> When `local.json` is on disk but refused at startup (`runtime.files.refused-defaults`), this Mac's
> camera sharing starts off, not at its default, and `Core` reports that it did, until the user
> sets camera sharing again. When `local.json` is missing, as on a first run, camera sharing starts
> on.

> r[camera.sharing.written]
> Turning camera sharing off or on is written to `local.json` as any change to it is
> (`runtime.files.debounced`).
>
> - parity: Python wrote camera sharing at once, not after a delay, so a forced quit right after turning it off kept it off (#16: `Core` debounces its writes, every change alike)

> r[camera.sharing.save-failed]
> While the last write of `local.json` failed (`runtime.files.write-failed`), `Core` reports that
> camera sharing may not be kept across a restart, until a write of it succeeds.
>
> - parity: Python tried a failed save of `camera.json` again 5 s later (#16: a failed write is tried again with the next change and at Quit, `runtime.files.write-retried`)

## Camera access

r[camera.permission.record]
Each Mac's record says its camera access, as macOS reports it to the camera devices capability:
allowed, not allowed, restricted, not asked yet, or unknown.

> r[camera.permission.needed]
> `Core` reports that this Mac needs camera access while it is the camera machine, its camera
> sharing is on and its camera access is not asked yet.
>
> - model: camera

> r[camera.permission.local]
> macOS's camera prompt is asked for only by an action on that Mac: writing the camera route to
> name that Mac from that Mac while its camera access is not asked yet, or asking for camera access
> there. A camera route written on another machine, an app using the virtual camera, or anything
> else never asks for it. Asking for camera access on a PC does nothing.
>
> - model: camera

> r[camera.permission.prompt]
> Asking for camera access on a Mac shows macOS's camera prompt for Beamer.
>
> - tier: human (a macOS permission prompt is seen on the Mac's display)
> - do: On the Mac, run `tccutil reset Camera uk.co.kalkman.beamer`, relaunch Beamer, then choose this Mac's camera on the Audio & Camera page and allow it
> - see: macOS shows its camera prompt naming Beamer, and once allowed the page no longer asks for camera access

> r[camera.permission.granted]
> Camera streams that waited for camera access start once it is allowed, with nothing set again.
>
> - model: camera

## Demand

> r[camera.demand.hold]
> A camera stream keeps running for 3 s after the record of the machine it goes to stops saying that
> an app has its virtual camera open, and stops then, saying that no app uses it, unless an app
> opens it again meanwhile.
>
> - model: camera

> r[camera.demand.live-end]
> Only an end seen while the machine is in the cluster with its camera running starts the 3 s: when
> the link to it goes down, it leaves the cluster, or its camera is starting or failed, its demand
> ends at once.
>
> - model: camera

> r[camera.demand.fresh]
> A record held from before this machine started, or from before the link to its machine last went
> down, never starts a camera stream: only a record taken since says that an app has a virtual
> camera open.
>
> - model: camera

## Statuses

> r[camera.status.route]
> The camera route has one status, the first of these that holds:
>
> 1. off, while the route is off;
> 2. waiting for the camera machine, while it is not in the cluster, no record of it is known, or
>    its camera is starting;
> 3. failed, because the camera machine's camera failed (`camera.state.reported`);
> 4. no camera, when the camera machine cannot share its camera;
> 5. sharing off (`camera.sharing.off`);
> 6. no camera, when the route follows the camera the camera machine prefers and it prefers none
>    that it lists;
> 7. waiting for the camera, when the camera the route names is gone (`camera.route.found`),
>    naming it by the route's name;
> 8. denied, while the camera machine's camera access is not allowed or restricted;
> 9. waiting for camera access, while it is not asked yet or unknown;
> 10. then, from the statuses this machine reports for the machines (`camera.status.machine`):
>     streaming, naming every machine streaming; else starting, naming every machine starting; else
>     failed, naming the first failed machine in machine id order, with its reason; else
>     unreachable, naming every machine unreachable; else idle, naming every machine idle; else not
>     installed, outdated or damaged, in that order, naming every machine in that state; else no
>     virtual camera.
>
> - model: camera

> r[camera.status.machine]
> The camera route has one status for each machine in the cluster whose record says it has the
> virtual camera, and, while the route is not off, for each other machine with a camera in the
> cluster too. It is the first of these that holds:
>
> 1. off, while the route is off;
> 2. no virtual camera, when it is the camera machine, has no virtual camera, or its virtual
>    camera is unsupported here (`camera.virtual.state`);
> 3. not installed, outdated or damaged, as its virtual camera's install state says, whether or not
>    an app has it open;
> 4. the route's status from steps 2 to 9 of `camera.status.route`;
> 5. idle, while no app has its virtual camera open, nor did within the last 3 s
>    (`camera.demand.hold`);
> 6. unreachable, while it has no link up with the camera machine (`camera.cluster.in`);
> 7. on the camera machine only, while it has a stream to that machine, one that stopped included
>    until the route no longer calls for it or it is asked for again (`camera.status.retry`): failed
>    while that stream failed (`camera.status.failed`), idle when that machine ended it as
>    `camera.status.quiet` says, streaming while it runs (or reduced, `camera.status.reduced`) and
>    starting while it opens or starts;
> 8. on that machine itself only, while it takes a stream from the camera machine: streaming while
>    it runs (or reduced), starting otherwise;
> 9. else failed, while the camera machine's capture failed, with its reason; streaming while it
>    runs or is interrupted; starting otherwise.
>
> - parity: Python also reported no direct path, after idle and unreachable, when the camera machine had no UDP path to that machine, as over loopback (#14: media goes as datagrams on the link, so a machine with a link up is reached)
> - model: camera

> r[camera.status.failed]
> When a camera stream cannot start or stops on a failure, the camera machine reports the status of
> the machine it goes to as failed, with why: that machine refused it (its camera route does not
> name this camera, its virtual camera is not installed or outdated, or it cannot take it), the
> picture did not reach it (`camera.stream.no-receiver`), the camera is gone, macOS does not allow
> it, or the camera failed (step 7 of `camera.status.machine`). The machine the stream went to does
> not take it, or no longer does, and it and every other machine report that machine's status from
> the other steps: failed only while the camera machine's record says its capture failed
> (`camera.capture.which`).
>
> - model: camera

> r[camera.status.quiet]
> A camera stream that the machine it goes to ends because its camera route moved, because no app
> uses its virtual camera any more, or because it quits is no failure: the camera machine's camera
> turns off, and that machine's status is idle there.
>
> - model: camera

> r[camera.status.retry]
> A camera stream that stopped on a failure or a refusal is asked for again only when the camera
> route or the camera quality changes, when the record of the machine it goes to changes, when the
> link to that machine comes up again, or after the waits of `camera.stream.no-receiver`. Nothing
> else asks for it again.
>
> - model: camera

> r[camera.status.reduced]
> While `spec/media.md` sends a running camera stream reduced, the machines that send and take it
> report it as reduced, not streaming.
>
> - parity: Python also called a stream reduced while it went through the link because its UDP flow failed (#14: media always travels on the link)

r[camera.status.streams]
`Core` reports each camera stream this machine sends or takes: the camera machine, the machine it
goes to, whether it is opening, starting, running or failed, and its size. A route whose status is
streaming has its streams among those its camera machine reports.

## Camera streams

> r[camera.stream.on-demand]
> The camera machine streams the shared camera to each other machine that is in the cluster, has a
> link up with it, has its virtual camera installed, and on which an app has the virtual camera
> open or did within the last 3 s (`camera.demand.hold`), one stream each, and to no other machine.
> It streams into that machine's virtual camera.
>
> - model: camera

> r[camera.stream.at-once]
> The camera machine asks a machine to take a camera stream, and turns its camera on for it, in the
> same `Core` step in which it takes the record or the change that calls for the stream, without
> waiting for the answer.
>
> - model: camera

> r[camera.stream.ask]
> To start a camera stream, the camera machine asks the receiving machine to take it, naming the
> camera it sends: the camera the route names, or, when the route follows the camera it prefers,
> that camera's key and name when it lists it, and no camera otherwise.
>
> - model: camera

> r[camera.stream.route-first]
> A machine asked to take a camera stream holds every write of the camera route that the camera
> machine had taken before it asked (`link.class.control`, `store.sync.change`).
>
> - parity: Python flooded the route and asked for streams as separate messages, so a machine could be asked before the route naming the asking machine reached it, refuse, and send its list again once the route caught up, for one more try (#14: the store's changes and camera's messages travel in order on the control stream)
> - model: camera

> r[camera.stream.take]
> A machine takes a camera stream only when its own camera route names the asking machine and,
> when the route names a camera, the camera asked for has that camera key or that name, and only
> while its virtual camera is installed. Otherwise it refuses, saying which: its route names
> another camera, its virtual camera is not installed (missing or damaged), its virtual camera is
> outdated, or it has no virtual camera.
>
> - model: camera

> r[camera.stream.one]
> A machine takes at most one camera stream at a time. A newly taken one replaces the one it had,
> which it ends, telling its camera machine that its route moved.
>
> - model: camera

> r[camera.stream.receiver-ends]
> The machine taking a camera stream ends it, and tells the camera machine why, when its camera
> route no longer names that machine or that camera (its route moved), when its virtual camera is
> no longer installed, or when it cannot show the stream.
>
> - model: camera

> r[camera.stream.refused]
> When the receiving machine refuses a camera stream or ends it, the camera machine stops that
> stream and, when no other stream needs it, turns its camera off, in the same `Core` step.
>
> - model: camera

> r[camera.stream.no-receiver]
> A camera stream that the receiving machine does not start showing within 3 s, or from which it
> sends nothing back for 5 s while it runs (`spec/media.md`), stops, failed because the picture did
> not reach that machine. While an app there still has the virtual camera open, it is asked for
> again 10 s after that failure, then 30 s after the next and 60 s after each later one. A stream
> that runs, and the app closing the virtual camera, start the waits again from 10 s.
>
> - model: camera

> r[camera.stream.bound]
> The camera machine's camera is never on for a stream for longer than 3.5 s, or 5.5 s once that
> stream has run, without a machine that wants it: one that takes the stream and on which an app
> has the virtual camera open or did within the last 3 s.
>
> - model: camera

> r[camera.stream.change]
> When the camera route names another machine or another camera, every camera stream it called for
> stops and, when the route still calls for streams, new ones start. A write of the camera route or
> the camera quality with the value it has, and a change of the camera quality
> (`camera.quality.change`), stop no stream.
>
> - model: camera

> r[camera.stream.reasons]
> When the camera machine ends a camera stream, it tells the receiving machine why: the route moved,
> no app uses the virtual camera any more, camera access is not allowed, camera sharing is off,
> the camera is gone, the picture did not reach it, or Beamer quits.
>
> - model: camera

## Links and restarts

> r[camera.link.down]
> When the link to a peer goes down, every camera stream to or from that peer stops at once, and the
> camera machine's camera turns off when no other stream needs it.
>
> - model: camera

> r[camera.link.up]
> When the link to a peer comes up again, the camera streams the camera route calls for between
> the two start again, with nothing set again.
>
> - model: camera

> r[camera.link.restart]
> After either machine restarts, the camera route, the camera quality and camera sharing are as they
> were, and once the link is up again the camera streams they call for start again, with nothing
> set again.
>
> - model: camera

r[camera.link.quit]
When Beamer quits, every camera stream it sends or takes stops, it tells each peer that it quits,
and its camera turns off.

## The virtual camera

The virtual camera is a Windows virtual camera that Frame Server loads from a DLL Beamer carries.
The DLL stays C++ (#9), and Beamer speaks its existing contract: a COM class and an install record
under HKLM, a shared-memory section with two events, and the pictures and beat Beamer writes there.
What needs administrator rights is a step of the PC's elevated setup (`platform.install.one-setup`,
`app.install.steps`) or of an elevated removal (`app.uninstall.removes`), as ADR 0004 and #21
decide.

> r[camera.virtual.pc-only]
> Only a PC has the virtual camera. A Mac's record reports its virtual camera as unsupported here,
> and never says that an app has it open.

> r[camera.virtual.state]
> A PC's virtual camera has one install state, the first of these that holds:
>
> 1. unsupported here, while Windows cannot create a software virtual camera: Media Foundation is
>    missing or does not start, `mfsensorgroup.dll` does not load, it lacks
>    `MFIsVirtualCameraTypeSupported` or `MFCreateVirtualCamera` (as before Windows 11), or
>    `MFIsVirtualCameraTypeSupported` fails or says a software camera source is not supported;
> 2. missing, while neither the COM class nor the install record `HKLM\SOFTWARE\Beamer\Camera`
>    exists, the COM class existing only while the default value of
>    `HKLM\SOFTWARE\Classes\CLSID\{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}\InprocServer32` is a
>    string (REG_SZ), and the install record while its key exists;
> 3. damaged, while one of the two is missing, the install record's `InstallGuid` is empty, longer
>    than 38 characters or holds a character other than a hex digit, `-`, `{` and `}`, its `OwnerSid`
>    is not a SID, its `DllVersion` is missing, not a DWORD, 0, or 0x80000000 or more, the COM class
>    names another DLL path than its `DllPath` (compared without case), or no file is at `DllPath`
>    (a folder there counts as none), where `InstallGuid`, `OwnerSid` and `DllPath` read as empty
>    when they are not strings (REG_SZ);
> 4. outdated, while the DLL version this Beamer carries (`camera.install.carried`) is greater than
>    `DllVersion`;
> 5. else installed, an installed version greater than the carried one included.

> r[camera.virtual.record]
> A PC's record says its virtual camera's install state, whether Windows lists Beamer Camera for
> the signed-in account, and whether an app has it open; while one has, the size and frame rate it
> asks for (`camera.shm.source`) and the app's name when Beamer knows it (`camera.virtual.app`), cut
> to 128 characters.
>
> - model: camera

> r[camera.virtual.listed]
> Windows lists Beamer Camera while it has a present video camera interface of a Windows virtual
> camera device whose friendly name begins with "Beamer Camera", compared without case.
>
> - tier: machine (only Windows 11 with a real Beamer Camera device lists one)

> r[camera.virtual.versions]
> On a PC, `Core` reports three DLL versions of the virtual camera: the one this Beamer carries, the
> installed one (`DllVersion`), and the one Frame Server has loaded (`camera.shm.source`), or none
> while Beamer has not read it.

> r[camera.virtual.created]
> Beamer tries to create Beamer Camera only while the install state is installed, Windows does not
> list Beamer Camera, and no earlier try is still running. A try creates a software virtual camera
> named "Beamer Camera" on the COM class, with system lifetime and access for the signed-in
> account, and then starts it. Beamer tries when it starts and each time it is asked to read the
> install state again (`camera.virtual.reread`), however recent its last try. Each time its virtual
> camera's state changes otherwise, it tries only when its last try was 2 s ago or more, or was for
> another `InstallGuid` or `DllVersion`.
>
> - tier: machine (only Windows 11's Frame Server creates a virtual camera)
> - parity: Python's media helper also tried at once each time Beamer sent it new placeholder words (ADR 0004: no media helper takes the words)

r[camera.virtual.reread]
When asked, as after an install or a removal from the Audio & Camera page
(`viewmodel.camera.install.done`), `Core` reads the virtual camera's install state again and
reports it, with the DLL versions (`camera.virtual.versions`).

> r[camera.virtual.stays]
> After Beamer quits or is killed, Windows still lists Beamer Camera, and an app that opens it sees
> the virtual camera's own picture (`camera.virtual.no-beat`).
>
> - tier: machine (only a real Frame Server keeps a virtual camera with system lifetime)

> r[camera.virtual.offered]
> Beamer Camera offers apps NV12 at 1280x720, 1920x1080 and 640x360, each at 30 fps, in that order.
>
> - tier: machine (only Frame Server shows the media types the installed DLL offers)

> r[camera.virtual.open]
> A PC's record says an app has the virtual camera open as soon as the shared memory says it streams
> (`camera.shm.source`), and says it no longer has 300 ms after the shared memory last said so,
> unless it says so again meanwhile.
>
> - model: camera

> r[camera.virtual.app]
> While an app has the virtual camera open, Beamer names it from Windows' camera consent store for
> the signed-in account: when exactly one app's entry there was last started later than it was last
> stopped, that app's name, and with none or several, no name. A packaged app is named by its
> package's display name, else its package name (its package family name up to its last `_`); an
> app that is not packaged by its program's file description, else its file name. It reads it when
> the virtual camera opens and every 2 s while it stays open.

> r[camera.virtual.no-beat]
> While Beamer's beat in the shared memory is older than 500 ms, is 0, or is more than 1 s ahead
> (`camera.shm.beat`), as when Beamer is not running or is stuck, an app reading Beamer Camera sees
> the virtual camera's own picture: "Beamer Camera" and "Waiting for Beamer", with a moving dot.
>
> - tier: machine (only an app reading the installed DLL through Frame Server shows its picture)

> r[camera.virtual.seen]
> A call app on a PC that chooses Beamer Camera shows the camera the camera route names on another
> machine.
>
> - tier: human (what a call app shows is seen at the desk)
> - do: On the Mac's Audio & Camera page, set the camera route to the Mac's camera; on the PC, open the Windows Camera app or a call app's video preview and choose "Beamer Camera"; then close it
> - see: The preview shows the Mac's camera live; after closing and opening it again, it shows the Mac's camera again

## The virtual camera's picture

While no live picture feeds a PC's virtual camera, it shows a placeholder: a picture with a title
and a line saying why.

> r[camera.placeholder.idle]
> While no camera stream feeds a PC's virtual camera, it shows the placeholder for its status: off,
> sharing off, waiting for camera access or denied while the camera route's status is that; failed
> while it is failed or the camera machine's capture failed; and waiting otherwise.
>
> - model: camera

> r[camera.placeholder.starting]
> While a camera stream a PC took starts and has shown no picture yet, its virtual camera shows the
> starting placeholder.

> r[camera.placeholder.stall]
> When a running camera stream stops delivering pictures, the virtual camera keeps showing the last
> picture for 2 s, then shows the reconnecting placeholder until pictures come again.

> r[camera.placeholder.words]
> Each placeholder has its title and its line, where `<machine>` is the camera machine and
> `<camera>` the shared camera:
>
> | Placeholder | Title | Line |
> |---|---|---|
> | off | "Beamer Camera is off" | "Choose a camera in Beamer" |
> | waiting | "Waiting for `<machine>`" | "`<camera>`" |
> | waiting for camera access | "Waiting for camera access" | "Allow it in Beamer on `<machine>`" |
> | sharing off | "Camera turned off on `<machine>`" | "Turn it on in Beamer on `<machine>`" |
> | denied | "`<machine>` has not allowed the camera" | `camera.placeholder.denied` |
> | failed | "Beamer Camera stopped" | `camera.placeholder.reason` |
> | starting | "Starting `<camera>`" | "on `<machine>`" |
> | reconnecting | "Reconnecting to `<machine>`" | "`<camera>`" |

> r[camera.placeholder.names]
> In a placeholder, the camera machine is named by its label (`viewmodel.machines.label.name`), or
> "another machine" while the camera route is off. The shared camera is the name the route gives
> it; when the route follows the camera its Mac prefers, the name of that camera when the Mac's
> record lists it; else "Default camera".

> r[camera.placeholder.denied]
> The denied placeholder's line is "Allow it on `<machine>` in System Settings > Privacy & Security >
> Camera" when the camera machine is a Mac or its record is not known, and "Allow it in the system
> settings on `<machine>`" otherwise.

> r[camera.placeholder.reason]
> The failed placeholder's line says why, from the reason of the camera route's failed status, else
> of this machine's failed status, else of the camera machine's failed capture: "The camera is not
> available" when the camera is gone, "Camera access is not allowed", "Beamer Camera is not
> installed", "Beamer Camera needs an update", "That machine cannot send its camera" when it cannot
> take or send the stream, "This PC could not show the picture", "The picture did not reach this
> PC", "The camera choice changed" when the route names another camera, and "Something went wrong in
> Beamer" when the camera failed. With no reason, or another, it is "Open Beamer for details".
>
> - parity: Python also had "Beamer's media helper stopped" for a stream its media helper ended (ADR 0004: no media helper)

r[camera.placeholder.translated]
Every placeholder title and line passes through the one translation function (`viewmodel.words.through-tr`),
apart from the machine's and the camera's names, which go into their template untranslated.

> r[camera.placeholder.seen]
> An app on a PC that opens Beamer Camera while no camera stream feeds it shows the placeholder
> with its title and line.
>
> - tier: human (what a camera app shows is seen at the desk)
> - do: On the PC, open the Camera app and choose Beamer Camera with the camera route off; then share the Mac's camera from the Audio & Camera page and turn camera sharing off from the Mac's tray menu
> - see: The Camera app shows "Beamer Camera is off" over "Choose a camera in Beamer", then "Camera turned off on" the Mac's name over "Turn it on in Beamer on" the Mac's name

## Installing the virtual camera

> r[camera.install.carried]
> Each Beamer for Windows carries the virtual camera's DLL, built with it, with one DLL version. The
> elevated setup installs only that DLL.
>
> - parity: Python's installers copied the DLL into a `camera` folder beside its media helper, which looked for it there and then beside itself, and a build without the DLL installed none; its script installer then also removed the `camera` folder an earlier build left, which its Inno Setup installer did not (ADR 0004: there is no media helper; #21: the binary installs itself)

> r[camera.install.differs]
> The virtual camera's step of the elevated setup differs (`platform.install.check`) while the
> install state is missing, outdated or damaged, and never while it is installed or unsupported here.

> r[camera.install.page]
> Asking `Core` to install the virtual camera, the one request for an install, an update and a repair,
> runs `beamer --elevated setup` (`app.elevated.steps`) and reports how it ended: installed when the
> step ends with exit status 0; cancelled when the user declined the UAC prompt; failed for any other
> end, a non-zero exit status, a step that could not start, or a step that has not ended 120 s after
> it started, which is left running. The step's exit status and output go to the log. On a Mac it
> ends as failed and runs nothing.
>
> - parity: Python ran its media helper's `vcam-install --owner-sid <SID> --dll-sha256 <hash>` through the UAC prompt, logged only its exit status, since the prompt hands back no output, and reported "not elevated" (exit 3), "bad arguments" (exit 2) and "no helper" apart from failed (ADR 0004: no media helper; #21: the app's Repair runs `setup`; `viewmodel.camera.install.refused` tells only cancelled from failed; the output is logged so that a failure says why)

> r[camera.install.copies]
> The virtual camera's step of `beamer --elevated setup` copies the carried DLL to
> `%ProgramFiles%\Beamer\Camera\<DLL version>\BeamerCameraSource.dll`, makes
> `%ProgramData%\Beamer\Camera`, writes the COM class
> `HKLM\SOFTWARE\Classes\CLSID\{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}` (its default value "Beamer
> Camera"; under `InprocServer32` the DLL's path as default value and `ThreadingModel` `Both`), and
> writes the install record `HKLM\SOFTWARE\Beamer\Camera`: `InstallGuid`, kept when it is valid
> (`camera.virtual.state`) and a new random GUID otherwise, `OwnerSid` (`camera.install.owner`),
> `DllPath` and `DllVersion` as a DWORD, all in the registry's 64-bit view. A new `InstallGuid` is
> 38 characters, in a form the virtual camera's DLL accepts: `{`, then 36 characters with `-` as the
> 9th, 14th, 19th and 24th and an upper-case hex digit as each other, then `}`, as
> `{0F8E3A2C-5B1D-4E6F-9A7B-3C2D1E0F4A5B}`.

> r[camera.install.owner]
> The install record's `OwnerSid` is the SID of the account the elevated step acts for
> (`app.elevated.other-account`). No account or SID is ever given on the elevated command line.
>
> - parity: Python's unelevated Beamer read its own account's SID and passed it as `--owner-sid`, and failed without a prompt when what it read was not a SID (`app.elevated.fixed`, `app.elevated.own-install`: an elevated step takes no argument and finds its account itself)

> r[camera.install.hash]
> The step registers the DLL only when the SHA-256 of its copy, read back from the version folder,
> is the hash this Beamer was built with. Otherwise it deletes the copy, leaves the COM class and the
> install record as they were, and fails, naming the mismatch in its output.
>
> - parity: Python's unelevated Beamer hashed the DLL before the UAC prompt and passed the hash as `--dll-sha256`, which the helper checked its copy against (`app.elevated.fixed`: the hash is built into the binary instead of given on the command line)

> r[camera.install.safe-folder]
> When `%ProgramFiles%\Beamer`, `%ProgramFiles%\Beamer\Camera`, its version folder,
> `%ProgramData%\Beamer` or `%ProgramData%\Beamer\Camera` exists as a junction, a symbolic link or a
> file, as a folder Administrators cannot open, or as a folder whose owner is neither Administrators
> nor SYSTEM, the step writes nothing into it and fails, naming that path in its output.

> r[camera.install.acl]
> After the step, only Administrators and SYSTEM may write `%ProgramFiles%\Beamer`, its `Camera`
> folder and the version folder, which users, LocalService and app packages may read and run, and
> `%ProgramData%\Beamer`, which users and LocalService may read; LocalService may also write
> `%ProgramData%\Beamer\Camera`, which users may read. None of these folders inherits another ACL.

> r[camera.install.loaded]
> Installing over a DLL that a process has loaded, as Frame Server does, succeeds without stopping
> Frame Server: a copy with the same bytes is kept as it is, and one with other bytes is renamed
> aside, to be deleted at the next restart of Windows, with the new DLL in its place.

> r[camera.install.stale]
> Each run of `beamer --elevated setup` on a PC whose install record names a `DllPath` deletes every
> entry of `%ProgramFiles%\Beamer\Camera` other than that DLL's version folder, apart from one still
> in use, which stays for a later run. Stale entries never stop Frame Server and never make the step
> fail. Without an install record, or without a `DllPath`, it deletes none.
>
> - parity: Python cleared stale versions in its helper's install, which deleted every entry as here, and in its installer's install and repair runs, which deleted only folders and never ran on removal (#21: one elevated setup does both)

## Removing the virtual camera

> r[camera.install.remove-first]
> Removing the virtual camera, by `beamer uninstall` or from the Audio & Camera page, has two
> parts: first Beamer removes Beamer Camera from the signed-in account's cameras
> (`camera.install.remove-account`), and then it runs the elevated removal, whether the first part
> worked, failed or had not ended 30 s after it started. With the install state missing or
> unsupported here, the first part does nothing.
>
> - parity: Python ran its media helper's `vcam-remove` for the first part, skipped when the helper file was missing (ADR 0004: Beamer removes the camera itself)

> r[camera.install.remove-account]
> The first part of a removal runs without administrator rights, and afterwards Windows no longer
> lists Beamer Camera for the signed-in account (`camera.virtual.listed`).
>
> - tier: machine (only Windows 11 makes and removes a virtual camera, and a CI runner runs every process as an administrator)

> r[camera.install.page-remove]
> Asking `Core` to remove the virtual camera runs `beamer --elevated remove-camera`
> (`app.elevated.steps`), which removes the virtual camera alone, not the firewall rule or anything
> else uninstall removes, after the first part (`camera.install.remove-first`), and reports how it
> ended: removed when the step ends with exit status 0, cancelled when the user declined the UAC
> prompt, failed for any other end, a step that has not ended 120 s after it started, which is left
> running, included. The step's exit status and output go to the log.
>
> - parity: Python ran its media helper's `vcam-uninstall` through the UAC prompt, logged only its exit status, and reported "not elevated" (exit 3) apart from failed (ADR 0004: no media helper; `viewmodel.camera.install.refused`; the output is logged so that a failure says why)

> r[camera.install.removes]
> `beamer --elevated remove` and `beamer --elevated remove-camera` delete the COM class, the install
> record, `%ProgramFiles%\Beamer\Camera` and `%ProgramData%\Beamer\Camera` (`app.uninstall.removes`).

> r[camera.install.parents]
> After removing the virtual camera, `HKLM\SOFTWARE\Beamer`, `%ProgramFiles%\Beamer` and
> `%ProgramData%\Beamer` are each deleted when nothing else is left in them, and kept with what else
> is in them otherwise.

r[camera.install.nothing-installed]
Removing the virtual camera from a PC that does not have it deletes nothing but an empty `Beamer`
parent (`camera.install.parents`), does not stop Frame Server, and succeeds, the second of two
removals in a row included.

> r[camera.install.frame-server]
> The removal stops Frame Server only when a file of the virtual camera cannot be deleted because it
> is in use, and then tries again to delete each folder still left, at once and then every 500 ms.
> A folder still left after the 10th try makes the removal fail, naming that folder in its output.
> While no file is in use, Frame Server keeps running.
>
> - tier: machine (only a real Frame Server holds the DLL loaded)
> - parity: Python's page removal also stopped FrameServerMonitor, tried once more, left what remained under `%ProgramFiles%\Beamer` to be deleted at the next restart of Windows, and succeeded, while its installer's removal tried 10 times and failed (one rule for both removals: the installer's, as `camera.install.links` keeps)

> r[camera.install.links]
> Removing the virtual camera or its stale versions never deletes or changes anything a junction or
> symbolic link points to. A link inside the virtual camera's folders, or in place of one of them or
> of a version folder, is deleted as a link. When `%ProgramFiles%\Beamer\Camera`,
> `%ProgramData%\Beamer\Camera` or their `Beamer` parent is reached through a link, nothing is
> deleted through it: a `Beamer` parent that is itself a link stays in place, with what it points
> to, and the removal says so in its output and goes on.
>
> - parity: Python's page removal deleted a `Beamer` parent that was a link as a link, while its installer's removal left it in place (one rule for both removals: the installer's, which its tests cover)

> r[camera.install.other-owner]
> When the install record's `OwnerSid` names an account other than the one the elevated step acts
> for, or is not a SID, the removal leaves the virtual camera installed (COM class, install record
> and folders), does not stop Frame Server, succeeds, and says in its output that the camera stays
> for the account that installed it. SIDs are compared without case. With no `OwnerSid`, the removal
> goes ahead.
>
> - parity: only Python's installer removal checked the owner; its page removal removed the camera whoever installed it (one rule for both removals, for the reason the installer had: the camera is machine-wide while Beamer installs per account)

## The shared memory

> r[camera.shm.names]
> Beamer speaks to the virtual camera through three kernel objects named after the install record's
> `InstallGuid`, as written there: the section `Global\BeamerCamera.<InstallGuid>.Shm` and the
> auto-reset events `Global\BeamerCamera.<InstallGuid>.Frame`, both the DLL's, and
> `Global\BeamerCamera.<InstallGuid>.Demand`, Beamer's. It opens them only while the install state
> is installed or outdated, and when `InstallGuid` changes it closes them and uses the new names.

> r[camera.shm.demand]
> Beamer creates the `.Demand` event so that SYSTEM, LocalService and the install record's
> `OwnerSid` may use it, and nobody else. When it already exists and is owned neither by this
> account nor by this process's default owner, or creating it is refused for want of access, Beamer
> does not use it, reports the virtual camera's error as taken by another process, and tries to
> create it again every 5 s.

> r[camera.shm.owner]
> Beamer writes into the section only when it is owned by LocalService or SYSTEM, and sets the
> `.Frame` event only when it is owned by one of them. A section owned by anyone else is closed
> again at once, and the virtual camera's error says it is taken by another process.

r[camera.shm.open]
While the section is not open, Beamer tries to open it every 1 s, and at once when the `.Demand`
event is set. On opening it, Beamer writes its process id into `helper_pid`.

> r[camera.shm.layout]
> The section is 12,455,936 bytes, little-endian: a header at offset 0, then four picture slots of
> 3,112,960 bytes each, slot `i`'s pixels at `4096 + i * 3112960`. The header's fields are at these
> offsets: `magic` 0, `version` 4, `header_bytes` 8, `slot_count` 12, `state_seq` 16, `streaming`
> 20, `want_w` 24, `want_h` 28, `want_fps` 32, `state_qpc` 40, `dll_version` 48, `source_pid` 52,
> `delivered` 56, `placeholders` 64, `repeats` 72, `rejected` 80, `helper_pid` 88, `beat_qpc` 96,
> `mode` 104, `latest` 108, and the four slot headers of 64 bytes each from 192. A slot header holds
> `seq` at 0, `w` 4, `h` 8, `stride` 12, `written_qpc` 16 and `picture` 24. `state_qpc`,
> `written_qpc`, `beat_qpc` and the four counters are 64-bit, every other field 32-bit.

> r[camera.shm.header]
> Beamer uses a section only when it can map all 12,455,936 bytes of it and its header says `magic`
> 0x4D414342, a `version` from 1 to the highest Beamer speaks, 1, and `slot_count` 4. Otherwise it
> writes nothing into it, and while the install state is installed it reports it as outdated.
> `header_bytes` is never read.
>
> - fuzz: camera-shm

> r[camera.shm.source]
> Beamer reads `streaming`, `want_w`, `want_h`, `want_fps` and `dll_version` when it opens the
> section and each time `state_seq` differs from the value it last read. `streaming` 1 means an app
> has the virtual camera open, and any other value that none has. The size an app asks for is
> `want_w`x`want_h` at `want_fps` when that is one of the sizes Beamer Camera offers
> (`camera.virtual.offered`), and 1280x720 at 30 fps otherwise. `dll_version` from 1 to 100,000 is
> the loaded DLL version, and any other value leaves it unknown.
>
> - fuzz: camera-shm

> r[camera.shm.beat]
> While the section is open, Beamer writes the current QueryPerformanceCounter value into `beat_qpc`
> when it opens the section and at least every 100 ms, from the work that presents the virtual
> camera's picture, so that the beat stops when that work stops.

> r[camera.shm.live]
> Beamer writes each live picture into the live slot after the one it last wrote, the live slots
> being 0 to 2: slot 1 first after it opens the section, then 2, 0, 1 and so on. It keeps that slot
> itself and never reads `latest` back. In the slot, it takes `q`, the slot's `seq` when that is
> even and `seq` plus 1 when it is odd, and sets `seq` to `q + 1`; writes the NV12 picture at the
> slot's pixels, Y first and UV from `stride * h`, with `w` and `h` its display size and `stride`
> equal to `w`; sets `written_qpc` to the current QueryPerformanceCounter value and `picture` to the
> picture count; then sets `seq` to `q + 2`. Only then does it set `latest` to that slot and `mode`
> to 1 (live), and set the `.Frame` event. The picture count is one counter for the four slots,
> which goes up by 1 for each picture Beamer writes into any of them, the first since Beamer started
> being 1. Neither `seq` nor the picture count is ever reset.

> r[camera.shm.placeholder]
> Beamer writes a placeholder picture into slot 3 the same way it writes a live picture into its
> slot, up to setting `seq` to `q + 2` (`camera.shm.live`), at the size the app asks for while one
> has the virtual camera open and 1280x720 otherwise. It leaves `latest` as it is, then sets `mode`
> to 3 (waiting), and sets `.Frame` when it wrote the slot or `mode` was not 3 before. A placeholder
> showing when the section opens is written into it at once.

r[camera.shm.mode]
`mode` is 1 (live) after a live picture, 2 (hold) while Beamer shows the last live picture again,
and 3 (waiting) while slot 3 shows; Beamer never writes another value there.

