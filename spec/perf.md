# Perf

Beamer's budgets: how long input, a crossing, the clipboard, the window, sound and the camera may
take on the Mac and `lebig`, each as a bound at named percentiles. Every rule here is `machine`
tier: `cargo xtask machine` checks it against the real, deployed apps, driven from outside by the
machine agents (ADR 0005), and its run's report gives the rule's p50, p95 and p99 (#22).

The budgets come from the link protocol's decision (#14), set against the Python app's performance
baseline (#4). The media budgets come from the measured designs in `dev/audio/` and `dev/camera/`,
and the window's from the Python app's frame-time tests at `archive/python`
(`mac_app/tests/test_design_offscreen.py`, `mac_app/tests/test_audio_page.py`,
`mac_app/tests/test_camera_section.py`), which other domains' miners moved to this domain's
ledger slice. What each budget measures is behaviour other domains state: input's round trip is
`spec/link.md`'s (`link.input.round-trip`), a crossing is `spec/crossing.md`'s and
`spec/driving.md`'s, the clipboard is `spec/clipboard.md`'s, sound is `spec/audio.md`'s, the camera
is `spec/camera.md`'s, and what is inside their streams is `spec/media.md`'s. That media never
delays input, and input delays media by at most one datagram, is `link.class.media-and-input`'s,
checked at `ci`; `perf.input.with-media` checks its effect on the real machines.

Each budget gates the milestone that first ships what its run needs (#24). A run that carries an
audio or camera stream waits for M3, which brings them (F43, F44):

| Milestone | Rules |
|---|---|
| M1 | `perf.own.driver`, `perf.own.driven`, `perf.input.round-trip-wifi`, `perf.input.round-trip-cable`, `perf.input.loss`, `perf.crossing.wifi`, `perf.crossing.cable` |
| M2 | `perf.clipboard.text-wifi`, `perf.clipboard.text-cable`, `perf.clipboard.image-wifi`, `perf.clipboard.image-cable`, `perf.ui.resize`, `perf.ui.scroll` |
| M3 | `perf.input.with-media`, `perf.ui.audio-levels`, `perf.ui.audio-machine`, `perf.ui.camera-status`, `perf.ui.tray-menu`, every `perf.audio.*` and `perf.camera.*` rule |

No part of this domain is Later or dropped (#11), and no rule here is about a state machine or a
parser, so none carries `model:` or `fuzz:`.

## How a budget is measured

- **A sample** is one measurement of a rule's quantity, and a run gives the rule its samples. A
  percentile is taken over every sample of the run, by nearest rank: the p95 is the smallest
  sample that at least 95 % of the samples do not exceed. A bound is met when the run's percentile
  is at most the bound.
- **Every run stands alone.** A rule whose run is made once with each machine driving, in each
  direction, or on each machine, is met only when each of those runs meets it on its own samples.
  Samples are never pooled across runs.
- **A sample that never ends.** A sample whose end has not come by its run's next step, or by the
  end of the run, counts as a sample above every bound: a crossing refused or never placed, a
  clipboard never set, a virtual camera that never gives a picture. Two kinds are dropped instead:
  a test signal's marker never recorded, and a camera picture never got by the app. A run that
  drops more than 1 % of the markers or pictures it made fails. Beamer's own time, round trips and
  frame times are logged by the app as each one ends, so none of them is left without an end.
- **Enough samples.** A run keeps at least 20 samples, at least 100 when its rule bounds a p99,
  and at least 1000 when its rule bounds a p99.9. A run with fewer fails.
- **A void run.** A run is void, and gives its rule no verdict, when during it a link between the
  two machines goes down, either app quits or starts, or the clock tie discards more than 10 % of
  its samples. The run is made again, and a rule whose run is void 3 times in a row fails.
- **Each machine's part is read from its own log**: the JSON-lines log lines that
  `spec/observability.md` defines (#71), never a hook inside the app (ADR 0005). Lines about
  what happens many times a second are counts and histograms written once a second, as #16
  decided, and percentiles merge those histograms.
- **Two machines' clocks are never subtracted.** The PC's clock wanders against the Mac's by
  hundreds of milliseconds (#4). A quantity that starts on one machine and ends on the other is
  either read as half the sum of a trip out and the trip home that follows it, each started on the
  clock of the machine it leaves and ended on the clock of the machine it reaches, so the offset
  cancels; or, where no trip home exists, through the clock tie.
- **The clock tie.** Through a run that needs it, the two machine agents make an out-and-back pair
  every 100 ms. The first agent stamps a datagram on its clock as it sends it (t1), the second
  stamps its answer on its clock as it sends it (t2), and the first stamps the answer as it
  arrives (t3). The pair's round trip is t3 − t1, and it reads the second clock as ahead of the
  first by t2 − (t1 + t3) / 2, uncertain by half its round trip. A sample is tied by the pair with
  the smallest round trip of those made within 10 s before or after it, and its uncertainty is
  half that round trip. A sample is discarded when its uncertainty is over 3 ms, or when either
  machine's clock stepped between the sample and its pair: a step is a change of more than 1 ms,
  from one pair to the next, in how far that machine's clock is ahead of its monotonic clock. The
  3 ms is #4's own error, half the round trip of its clock pings
  (`docs/research/perf-baseline.md` on `research/perf-baseline`, "error ≤ RTT/2 ≈ 3 ms"), and is
  small against the audio and camera bounds of 66 ms and more. The smallest of the 200 round
  trips around a sample is near the radio's floor, not #4's 6 ms p50: datagram pings between the
  two machines had their smallest round trip at 2.5 to 3.4 ms over whole 5 to 10 minute runs
  (`docs/research/perf-baseline/audio-camera-existing.md` on `research/perf-baseline`), an
  uncertainty of 1.25 to 1.7 ms, so a 20 s window's smallest round trip still ties its samples up
  to 6 ms, 2.6 ms above the highest of those floors. WP10's harness tied the clocks the same way,
  by the smallest of its SSH pings, 4.1 ms, which this cut-off keeps at 2.05 ms
  (`dev/audio/reports/wp10.md`). A radio that cannot keep 90 % of the samples voids the run
  rather than failing it.
- **On Wi-Fi** means both machines on the same 5 GHz Wi-Fi, as #4 measured. **On the cable**
  means both machines wired by Ethernet to one router, with Wi-Fi off. Each rule names which.
- **Input** in a run is real OS input posted by the machine agent of the machine it starts on.
  Its pointer moves and Shift presses act on no app.

## Beamer's own time

> r[perf.own.driver]
> On a driver, the time from an input message being ready to send to the first datagram carrying
> it being handed to the OS to send is at most 0.5 ms at p50, p95 and p99. An input message is
> ready to send at the later of two moments: its capture getting from the OS the newest input it
> carries, which for a merged move (`link.input.merge`) is the newest move merged into it; and the
> link being free to send it, which is when the last wait a rule makes it take ends: the pace
> (`link.input.pace`), room in the window (`link.input.window`), the answer to a pass on that
> queues it (`driving.pass-on.take`), or the clipboard sent to a machine ahead of it
> (`clipboard.take.sent`). Those waits are timing the link's and driving's rules set on purpose,
> not the time Beamer takes to deal with an input, which is what #14 bounds here.
>
> Run: for 10 minutes on Wi-Fi, the driver's machine agent moves the pointer in circles at 125
> moves a second and taps Shift every 100 ms, with the input on the peer. It runs once with the
> Mac driving and once with the PC driving, and each must meet the bound.
>
> Baseline: the Python app took 0.03 ms to queue an input and 0.07 ms to write it, at the mean, on
> the PC (#4). #14 sets this budget.
>
> - tier: machine (only the real capture on each OS gives the time from the OS to the wire)

> r[perf.own.driven]
> On a driven machine, the time from a datagram carrying an input message arriving from the OS to
> the OS taking the injection of that message (`input.injection.each-kind`) is at most 0.5 ms at
> p50, p95 and p99. An input message that waits for a missing one (`link.input.once-in-order`)
> counts from the arrival of the one it waited for, and a pointer move merged on arrival
> (`link.input.receive-merge`) from the arrival of the newest move merged into it.
>
> Run: the run of `perf.own.driver`, read on the driven machine.
>
> Baseline: the Python app on the Mac took 0.09 ms to decode an input, 0.22 ms to dispatch it and
> 0.18 ms to inject it, at the mean (#4). #14 sets this budget.
>
> - tier: machine (only the real injection on each OS gives the time from the wire to the OS)

## Input

> r[perf.input.round-trip-wifi]
> On Wi-Fi, the input round trip, each round-trip sample of `link.input.round-trip`, is at most
> 6.3 ms at p50 and 10.5 ms at p95, in each direction. Its p99 is reported and not bounded: on
> Wi-Fi the radio's stalls set the tail, which no transport can mend (#14), and
> `perf.input.loss` bounds what Beamer adds to it.
>
> Run: the run of `perf.own.driver`, read on the driver.
>
> Baseline: 6.3 ms at p50 and 10.5 ms at p95 with the PC driving the Mac, over 138,954 inputs;
> 6.1 ms and 9.1 ms with the Mac driving the PC, over 102 (#4). #14 sets the first as
> the bound in both directions.
>
> - tier: machine (only the real machines on a real Wi-Fi give its round trip)

> r[perf.input.round-trip-cable]
> On the cable, the input round trip, each round-trip sample of `link.input.round-trip`, is at
> most 6.3 ms at p50 and 10.5 ms at p95, and at most 50 ms at p99 and at p99.9, in each
> direction.
>
> Run: the run of `perf.own.driver`, on the cable.
>
> Baseline: nothing was measured wired (#4). #14 sets the p99.9, keeps the Wi-Fi
> baseline's p50 and p95 on the cable too, and leaves tighter cable bounds to the cable's first
> measurement.
>
> - tier: machine (only the real machines on a real cable give its round trip)

> r[perf.input.loss]
> On Wi-Fi, with 2 % of the link's datagrams dropped at random in each direction, the input round
> trip of input sent one message at a time is at most the round trip without loss, over the same
> run, plus the cost of a loss: its p50 and its p95 at most the p50 and the p95 without loss plus
> that cost, and its p99 at most the p99 without loss plus twice that cost, in each direction. The
> cost of a loss is the larger of 15 ms and twice the p50 without loss.
>
> Run: for 20 minutes on Wi-Fi, the driver's machine agent presses Shift and releases it 50 ms
> later, every 100 ms, with no other input and the input on the peer. The minutes alternate: in
> the first, third and every odd minute, no datagram is dropped; in every even minute, the Mac's
> packet filter discards 2 % of the UDP datagrams to and from port 24820 at random in each
> direction. The round trip without loss is that of the odd minutes, and the round trip with loss
> that of the even minutes, so the radio's own tail falls on both. It runs once with each machine
> driving.
>
> Baseline: #4 measured no loss on purpose, and #14 bounds the Wi-Fi tail by the cost of
> recovering from a loss, measured by dropping datagrams on purpose. Each input message here is
> acknowledged at once and alone: it is dealt with about 50 ms after the one before it, so more
> than 15 ms after the last acknowledgement (`link.input.ack-timing`), and outside the radio's
> stalls its round trip, a loss's wait included, ends before the next one is made. One loss costs
> it one wait. A dropped input datagram is sent again max(2 × QUIC's RTT, 5 ms) after it was first
> sent (`link.input.resend`), so its sample grows by that wait. A dropped acknowledgement is
> answered by acknowledging the datagram sent again, on the same timing (`link.input.ack-repeat`):
> at the later of its arrival and 15 ms after the dropped one, with a delay of 0, so its sample
> grows by the larger of 15 ms and the resend's wait. Twice the p50 without loss stands for twice
> QUIC's RTT: at #4's 6 ms p50 the cost is 15 ms. A sample made longer by at most the cost raises
> each percentile by at most the cost. About 4 % of the samples meet one loss, out of two
> datagrams each dropped 2 % of the time, and about 0.1 % meet two, which can raise the p99 by
> twice the cost. At #4's Wi-Fi the p95 without loss plus the cost, about 25 ms, is near the
> link's p99 of 24 to 32 ms, so the samples without loss above it are about 1 %, which leaves room
> for that 0.1 % at the p95.
>
> - tier: machine (only the real machines on a real Wi-Fi give the cost of a loss on top of the
>   radio's)

> r[perf.input.with-media]
> On Wi-Fi, while a camera stream at 1080p runs from the Mac to the PC, the PC's sound plays on
> the Mac and the microphone's audio route sends the Mac's microphone to the PC, the input round
> trip is at most 6.3 ms at p50 and 10.5 ms at p95, in each direction. Its p99 is reported and not
> bounded, as in `perf.input.round-trip-wifi`.
>
> Run: the run of `perf.own.driver`, with those three streams running from 30 s before it starts
> to after it ends, each carrying what the media budgets' runs carry. The camera route names the
> Mac's camera that the Mac's machine agent feeds with pictures, as in the camera budgets, and
> the PC's machine agent keeps the virtual camera open as an app does. The PC's machine agent
> plays the audio budgets' test signal as the PC's sound, without pause. The Mac's machine agent
> plays it, without pause, into the microphone the audio route names, as in
> `perf.audio.mic-mac-to-pc`, and the PC's machine agent records the PC's virtual microphone as
> an app does.
>
> Baseline: the bounds of `perf.input.round-trip-wifi` (#4). #14 promises that media never delays
> input (`link.class.media-and-input`).
>
> - tier: machine (only the real machines on a real Wi-Fi carry real media beside input)

## Crossing

> r[perf.crossing.wifi]
> On Wi-Fi, a crossing takes at most 4.5 ms at p50 and 10 ms at p95. A crossing runs from the
> push that crosses (`crossing.push.breakthrough`) on the machine the input leaves to the pointer
> placed at the landing point on the machine the input reaches: on the driven machine
> (`driving.accept.lands`) going out, and at home coming home through a zone of the driven
> machine. A sample is half the sum of a crossing out and the crossing home that follows it. Its
> p99 is reported and not bounded: on Wi-Fi the radio's stalls set the tail (#14).
>
> Run: on Wi-Fi, 200 times, the driver's machine agent pushes the pointer through a zone to the
> peer, waits 1 s, pushes it through the peer's zone that leads home, and waits 1 s. A crossing
> placed after its 1 s wait has no end (How a budget is measured). It runs once with each machine
> driving.
>
> Baseline: a crossing out and the crossing home summed took 9 ms at p50, 37 ms at p95 and 40 ms
> at most, over 52 pairs, so a crossing about 4.5 ms at p50 and 18 ms at p95 (#4). #14 sets the
> p95 at 10 ms.
>
> - tier: machine (only the real capture and the real machines on a real Wi-Fi give a crossing's
>   time)

> r[perf.crossing.cable]
> On the cable, a crossing, as `perf.crossing.wifi` measures it, takes at most 4.5 ms at p50 and
> 10 ms at p95. Its p99 is reported and not bounded until the cable's first measurement (#14).
>
> Run: the run of `perf.crossing.wifi`, on the cable.
>
> Baseline: the p50 and the p95 of `perf.crossing.wifi`. #14 sets the p95 on both Wi-Fi and the
> cable, and leaves every other cable bound but input's p99.9 to the cable's first measurement.
>
> - tier: machine (only the real capture and the real machines on a real cable give a crossing's
>   time)

## Clipboard

> r[perf.clipboard.text-wifi]
> On Wi-Fi, a text on the clipboard of the machine the input leaves is on the clipboard of the
> machine it reaches, ready to paste, at most 15 ms at p50 and at p95 after the push that crosses
> (`crossing.push.breakthrough`). Going out, the trip starts at the push on the driver's clock
> and ends as the driven machine sets its clipboard from the clipboard message its driver sent it
> (`clipboard.take.sent`, `clipboard.travel.order`), on the driven machine's clock. Coming home,
> it starts at the push through the driven machine's zone, on its clock, and ends as the driver
> sets its clipboard from the machine let go (`clipboard.arrive.set`), on the driver's clock. A
> sample is half the sum of a crossing out and the crossing home that follows it, each carrying a
> new text. Its p99 is reported and not bounded: on Wi-Fi the radio's stalls set the tail (#14).
>
> Run: on Wi-Fi, 100 times, the driver's machine agent puts a new text of 100 characters on the
> driver's clipboard and pushes the pointer through a zone to the peer; then, 1 s later, the
> peer's machine agent puts another on the peer's clipboard, and the driver's pushes the pointer
> through the peer's zone that leads home, and waits 1 s. A clipboard set after its 1 s wait has no
> end. It runs once with each machine driving.
>
> Baseline: 8 to 36 ms over 5 texts, too few for percentiles (#4). #14 sets the p95 at 15 ms.
>
> - tier: machine (only the real OS clipboards and the real machines on a real Wi-Fi give the time
>   to paste)

> r[perf.clipboard.text-cable]
> On the cable, a text follows a crossing, as `perf.clipboard.text-wifi` measures it, within 15 ms
> at p50 and at p95. Its p99 is reported and not bounded until the cable's first measurement
> (#14).
>
> Run: the run of `perf.clipboard.text-wifi`, on the cable.
>
> Baseline: #14's p95 bound. #14 leaves every other cable bound but input's p99.9 to the cable's
> first measurement.
>
> - tier: machine (only the real OS clipboards and the real machines on a real cable give the time
>   to paste)

> r[perf.clipboard.image-wifi]
> On Wi-Fi, a PNG of 8 MiB on the clipboard of the machine the input leaves is on the clipboard of
> the machine it reaches, ready to paste, within twice the raw time at p50 and at p95, measured
> from the push that crosses as `perf.clipboard.text-wifi` measures a text. A sample is half the
> sum of a crossing out and the crossing home that follows it, each carrying a new PNG, divided by
> the raw time. Its p99 is reported and not bounded: on Wi-Fi the radio's stalls set the tail
> (#14).
>
> The raw time is the time 8 MiB takes one way between the two machine agents on the same link,
> over one TCP stream. One copy pair is timed on the driver's clock: its machine agent writes
> 8 MiB of random bytes on the stream; the peer's machine agent reads all of them, then writes
> them back; the copy pair's time runs from the first byte written to the last byte read back.
> The raw time is half the median of 5 copy pairs, made just before the samples.
>
> Run: as `perf.clipboard.text-wifi`, 20 times, each machine agent putting a new PNG of noise of
> between 7.5 and 8 MiB on the clipboard in place of the text, and each wait 5 s in place of 1 s.
> Each PNG fits (`clipboard.payload.image-limit`, `clipboard.png.size`).
>
> Baseline: 113 to 345 ms over 3 images of sizes not recorded (#4). #14 sets the bound at twice
> the time the raw bytes take.
>
> - tier: machine (only the real OS clipboards and the real machines on a real Wi-Fi give the time
>   to paste)

> r[perf.clipboard.image-cable]
> On the cable, a PNG of 8 MiB follows a crossing, as `perf.clipboard.image-wifi` measures it,
> within twice the raw time at p50 and at p95, the raw time measured on the cable. Its p99 is
> reported and not bounded until the cable's first measurement (#14).
>
> Run: the run of `perf.clipboard.image-wifi`, on the cable.
>
> Baseline: #4 measured no image wired. #14 sets the bound at twice the time the raw bytes take,
> and leaves every other cable bound but input's p99.9 to the cable's first measurement.
>
> - tier: machine (only the real OS clipboards and the real machines on a real cable give the time
>   to paste)

## The window

A frame's time is the time Beamer takes to make one frame of its window, from starting on what
the frame shows to handing the frame to the OS to show.

> r[perf.ui.resize]
> While the window is resized on the Design page, each frame's time is under 16 ms at p50 and p95,
> and under 25 ms at p99 and at its largest.
>
> Run: on each machine, the machine agent drags the window's right edge from 760 to 1900 points
> wide and then to 760 again, over 10 s, 5 times.
>
> - tier: machine (only a real window on a real display gives frame times)
> - parity: Python bounded, on the Mac only, the time it took to deal with each new size and lay the page out, 95 % of them under 16 ms and all under 25 ms, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.scroll]
> While the Design page scrolls, each frame's time is under 16 ms at p50, p95 and p99.
>
> Run: on each machine, the machine agent scrolls the Design page with the scroll wheel from top
> to bottom and up to the top again, 10 times.
>
> - tier: machine (only a real window on a real display gives frame times)
> - parity: Python bounded only the cost of AppKit's cursor-area resets that scrolling made, five passes over the Design page's controls under 20 ms in all, on the Mac (Rune keeps no cursor areas, ADR 0001; the frame drawn while scrolling is what the user sees)

> r[perf.ui.audio-levels]
> While the Audio & Camera page shows the levels of running audio streams, each frame's time is
> under 16 ms at p50, p95 and p99, and at its largest.
>
> Run: on each machine, for 60 s, the Audio & Camera page is shown while the PC's sound plays on
> the Mac and the microphone's audio route sends the Mac's microphone to the PC, each carrying the
> test signal of the audio budgets below.
>
> - tier: machine (only a real window on a real display with real audio streams gives frame times)
> - parity: Python bounded, on the Mac only, each level update at 20 a second and each status update every 2 s to under 16 ms, from fake levels and statuses, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.audio-machine]
> While the Audio & Camera page is made to show again, one after the other, machines whose devices
> it showed before, every frame's time, the largest included, is under 16 ms.
>
> Run: on each machine, the machine agent clicks the other machine on the Audio & Camera page and
> then this machine, so the page has shown both, and waits 2 s before the run's samples start.
> Then it clicks the other machine and this machine in turn, one click a second, 40 clicks in all,
> so every click chooses a machine shown before. The run's samples are every frame made from its first click to
> 1 s after its last, and each of them meets the bound: the frame times the app writes once a
> second do not tell the frames that follow a click from the others, so the bound covers them all.
>
> - tier: machine (only a real window on a real display gives frame times)
> - parity: Python bounded, on the Mac only, the time to deal with the click and lay out the kept devices, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.camera-status]
> While the camera's status changes and the Audio & Camera page shows it, each frame's time is
> under 16 ms at p50, p95 and p99, and at its largest.
>
> Run: for 20 minutes, with the camera route naming the Mac's camera and the Audio & Camera page
> shown on both machines at once for the whole run, the PC's machine agent opens the virtual
> camera as an app does for 2 s, then closes it for 6 s. The stream keeps running for 3 s after
> each close (`camera.demand.hold`) and then stops, so every cycle starts and stops it, and the
> camera's status changes 300 times, which the page on each machine shows. Each machine's frames
> are a run of their own, with the 20 minutes' frames, at least 100 of them (Enough samples).
>
> - tier: machine (only a real window on a real display with a real camera stream gives frame
>   times)
> - parity: Python bounded, on the Mac only, each update of the camera's status at 4 a second and each level update at 20 a second to under 16 ms, from fake statuses, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.tray-menu]
> Each update of the tray menu takes under 16 ms, at p50, p95 and p99, and at its largest, from
> starting on what the menu shows to handing it to the OS.
>
> Run: on the Mac, the run of `perf.ui.camera-status`, whose 300 changes each show or hide the
> camera line of the Mac's tray menu (`viewmodel.camera.menu.line`). The PC's tray menu has no
> camera items (`viewmodel.camera.menu.shown`), so on the PC it is the run of
> `perf.crossing.wifi` with the PC driving, whose 200 crossings out and 200 home each change the
> drive status the menu shows first (`viewmodel.tray.drive-status`). Each machine's updates are a
> run of their own, at least 100 of them (Enough samples).
>
> - tier: machine (only a real tray menu on a real OS gives its update times)
> - parity: Python bounded, on the Mac only, its menu's status refresh every 0.4 s to under 16 ms, from fake camera statuses (the tray menu is Beamer's on both machines; Python's PC had no such bound)

## Audio

Each audio budget is the one-way time of a marked test signal: the sending machine's machine agent
plays it, into a microphone the audio route names or as the machine's sound, and the receiving
machine's machine agent records it, from the virtual microphone as an app does or from the output
it plays on. A sample is one marker, from the moment it is played to the moment it is recorded,
tied by the clock tie. Every run lasts 10 minutes on Wi-Fi, with a marker every 250 ms, and starts
once the stream has run for 30 s. The baselines are the Python app's last full measurement, WP16 in
`dev/audio/reports/wp16.md`, which #4 gathers.

WP16 timed a microphone differently: its test signal was made inside the audio helper, after the
capture, and timed from there (`dev/audio/reports/wp10.md`), so no capture device was in its path.
The helper is gone (ADR 0004), and ADR 0005 puts no hook inside the app, so a microphone run here
plays the test signal into a loopback device: an output the machine agent plays into, and the
microphone it feeds, which the audio route names. Two things make up the difference. A
microphone sample is the marker's time less the device's own time: before the run, the sending
machine's agent plays markers into the device and records them from its microphone, for 60 s on
its one clock, and the median of those is the device's own time. That loop also holds one capture
of the device by the agent, which Beamer's capture of the device replaces in the run; the two may
differ by up to one capture period, so each microphone bound is WP16's figure plus a margin of
10 ms. The margin is this spec's choice, not a measured figure. The PC captured in packets of 480 frames, 10 ms at 48 kHz, in
every run (`dev/audio/reports/wp14.md`), but on the Mac each process sets its own capture period:
the audio helper asked for 128 frames and a process that asks for none got 512
(`dev/audio/reports/wp11.md`), and nothing yet fixes Beamer 2.0's.

In every microphone run, the headset choice of the default output of the machine whose
microphone is shared is headset, written in the seeded state before the apps start (ADR 0005).
That output's form is then a headset (`audio.form.choice`, `audio.form.applied`), so the stream's
echo is not cancelled, whatever that output plays (`audio.echo.when`).

> r[perf.audio.mic-pc-to-mac]
> The PC's microphone reaches the Mac's virtual microphone within 66.5 ms at p50 and 72 ms at
> p95, a sample less the device's own time. Its p99 is reported and not bounded: no baseline
> measured one (#4).
>
> Run: the microphone's audio route names the PC and its loopback device's microphone, which the
> PC's machine agent plays the test signal into. The Mac's machine agent records the Mac's virtual
> microphone. The headset choice of the PC's default output is headset, so the stream's echo is
> not cancelled (`audio.echo.when`).
>
> Baseline: 56.5 ms at p50 and 62 ms at p95 in WP16's a_1; 41 to 56 ms at p50 over WP16's runs
> (#4). The bounds add the 10 ms margin above.
>
> - tier: machine (only real sound through the real OS audio and a real Wi-Fi gives its delay)

> r[perf.audio.sound-pc-to-mac]
> The PC's sound plays on the Mac's output within 65.1 ms at p50 and 84.9 ms at p95. Its p99 is
> reported and not bounded: no baseline measured one (#4).
>
> Run: the PC's sound route names the Mac's default output. The PC's machine agent plays the test
> signal as the PC's sound, and the Mac's records what the Mac's output plays.
>
> Baseline: 65.1 ms at p50 and 84.9 ms at p95, over a 31-minute run (#4).
>
> - tier: machine (only real sound through the real OS audio and a real Wi-Fi gives its delay)

> r[perf.audio.sound-mac-to-pc]
> The Mac's sound plays on the PC's output within 69.5 ms at p50 and 80.2 ms at p95. Its p99 is
> reported and not bounded: no baseline measured one (#4).
>
> Run: the Mac's sound route names the PC's default output. The Mac's machine agent plays the test
> signal as the Mac's sound, and the PC's records what the PC's output plays.
>
> Baseline: 69.5 ms at p50 and 80.2 ms at p95 (#4).
>
> - tier: machine (only real sound through the real OS audio and a real Wi-Fi gives its delay)

> r[perf.audio.mic-mac-to-pc]
> With the low-latency virtual microphone setting off, the Mac's microphone reaches the PC's
> virtual microphone within 182.4 ms at p50 and 195 ms at p95, a sample less the device's own
> time. Its p99 is reported and not bounded: no baseline measured one (#4).
>
> Run: the microphone's audio route names the Mac and its loopback device's microphone, which the
> Mac's machine agent plays the test signal into. The PC's machine agent records "CABLE Output" as
> an app does. The headset choice of the Mac's default output is headset, so the stream's echo is
> not cancelled (`audio.echo.when`).
>
> Baseline: 172.4 ms at p50 and 185 ms at p95 in WP16's d_1, whose echo was not cancelled either
> (#4). The bounds add the 10 ms margin above.
>
> - tier: machine (only real sound through the real OS audio, a real VB-CABLE and a real Wi-Fi
>   gives its delay)

> r[perf.audio.mic-mac-to-pc-low-latency]
> With the PC's low-latency virtual microphone setting on (`audio.low-latency.pc`), the Mac's
> microphone reaches the PC's virtual microphone within 130.3 ms at p50 and 176.7 ms at p95, a
> sample less the device's own time. Its p99 is reported and not bounded: no baseline measured
> one (#4).
>
> Run: the run of `perf.audio.mic-mac-to-pc`, with the setting on. The headset choice of the
> Mac's default output is headset there too, so the stream's echo is not cancelled
> (`audio.echo.when`).
>
> Baseline: 120.3 ms at p50 and 166.7 ms at p95 in WP16's d_ex1 (#4), whose echo was cancelled
> (WP16, miss 4); here it is not, so this run differs from `perf.audio.mic-mac-to-pc`'s only in
> the setting. The bounds add the 10 ms margin above.
>
> - tier: machine (only real sound through the real OS audio, a real VB-CABLE and a real Wi-Fi
>   gives its delay)

## Camera

Each camera budget is measured with a camera on the Mac that the Mac's machine agent feeds with
pictures carrying their number and the time they were made, and the PC's machine agent opening
the virtual camera as an app does and reading each picture's number and time. A sample is one
picture, from the time it was made to the time the PC's machine agent got it, tied by the clock
tie. The camera route names that camera, on Wi-Fi, with
no microphone stream to the PC.

> r[perf.camera.latency-720p]
> At 720p, a picture of the Mac's camera reaches an app that has the PC's virtual camera open
> within 70 ms at p50, 90 ms at p95 and 140 ms at p99.
>
> Run: the camera quality is 720p, and the PC's machine agent keeps the virtual camera open for
> 10 minutes. Samples start with the stream's first picture.
>
> Baseline: the Python app was never measured; its prototype took 57.5 ms at p50, 60.3 ms at p95
> and 65.3 ms at p99 the same way, over 17,992 pictures (#4). The bounds are the camera design's
> own pass marks for this run, `cam_latency` in `dev/camera/PHASE2.md`.
>
> - tier: machine (only a real camera stream into the real virtual camera on a real Wi-Fi gives
>   its delay)

> r[perf.camera.latency-1080p]
> At 1080p, a picture of the Mac's camera reaches an app that has the PC's virtual camera open
> within 70 ms at p50, 90 ms at p95 and 140 ms at p99.
>
> Run: the run of `perf.camera.latency-720p`, with the camera quality at 1080p.
>
> Baseline: the Python app was never measured; its prototype took 56.8 ms at p50, 63.3 ms at p95
> and 106.6 ms at p99, the p99 from one Wi-Fi stall, over 17,975 pictures (#4). The bounds are
> `cam_latency`'s in `dev/camera/PHASE2.md`.
>
> - tier: machine (only a real camera stream into the real virtual camera on a real Wi-Fi gives
>   its delay)

> r[perf.camera.first-picture]
> An app that opens the PC's virtual camera gets the first picture from the Mac's camera within
> 450 ms at p50 and 600 ms at p95 of opening it. Its p99 is reported and not bounded: no baseline
> measured one (#4).
>
> Run: at 720p, the PC's machine agent opens the virtual camera as an app does, keeps it open
> 10 s, and closes it for 10 s, 20 times. A sample is the time from an open to the first picture
> that is not a placeholder (`camera.placeholder.*`).
>
> Baseline: the Python app was never measured; its prototype took 260 ms at p50 and 308 ms at p95
> over 20 opens (#4). The bounds are the camera design's pass marks, `cam_start` in
> `dev/camera/PHASE2.md`.
>
> - tier: machine (only a real camera stream into the real virtual camera gives its start)
