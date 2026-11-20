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

The input budgets gate M1, the clipboard and window budgets M2, and the media budgets M3 (#24).
No part of this domain is Later or dropped (#11), and no rule here is about a state machine or a
parser, so none carries `model:` or `fuzz:`.

## How a budget is measured

- **A sample** is one measurement of a rule's quantity, and a run gives the rule its samples. A
  percentile is taken over every sample of the run, by nearest rank: the p95 is the smallest
  sample that at least 95 % of the samples do not exceed. A bound is met when the run's percentile
  is at most the bound.
- **Each machine's part is read from its own log**: the JSON-lines log lines that
  `spec/observability.md` defines (#71), never a hook inside the app (ADR 0005). Lines about
  what happens many times a second are counts and histograms written once a second, as #16
  decided, and percentiles merge those histograms.
- **Two machines' clocks are never subtracted.** The PC's clock wanders against the Mac's by
  hundreds of milliseconds (#4). A quantity that starts on one machine and ends on the other is
  either read as half the sum of a trip out and the trip home that follows it, each started and
  ended on the clocks of the machines they run between, so the offset cancels; or, where no trip
  home exists, against the two machines' clocks tied by out-and-back pairs between the machine
  agents, a sample whose tie is uncertain by more than 2 ms being discarded.
- **On Wi-Fi** means both machines on the same 5 GHz Wi-Fi, as #4 measured. **On the cable**
  means both machines wired by Ethernet to one router, with Wi-Fi off. Each rule names which.
- **Input** in a run is real OS input posted by the machine agent of the machine it starts on.
  Its pointer moves and Shift presses act on no app.

## Beamer's own time

> r[perf.own.driver]
> On a driver, the time from its capture getting an input from the OS to the first datagram
> carrying that input being handed to the OS to send is at most 0.5 ms at p50, p95 and p99.
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
> counts from the arrival of the one it waited for.
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
> trip of input made one message at a time is at most 6.3 ms at p50 and 23 ms at p95, in each
> direction. Its p99 is reported and not bounded, as in `perf.input.round-trip-wifi`.
>
> Run: for 10 minutes on Wi-Fi, the Mac's packet filter discards 2 % of the UDP datagrams to and
> from port 24820 at random in each direction, and the driver's machine agent taps Shift every
> 100 ms, with no other input and the input on the peer. It runs once with each machine driving.
>
> Baseline: #4 measured no loss on purpose. The p95 bound is the Wi-Fi baseline's 10.5 ms plus
> one resend after max(2 × 6 ms, 5 ms) (`link.input.resend`), at the 6 ms p50 of #4's link round
> trip. #14 bounds the Wi-Fi tail by the cost of recovering from a loss, measured by dropping
> datagrams on purpose.
>
> - tier: machine (only the real machines on a real Wi-Fi give the cost of a loss on top of the
>   radio's)

> r[perf.input.with-media]
> On Wi-Fi, while a camera stream at 1080p runs from the Mac to the PC, the PC's sound plays on
> the Mac and the microphone's audio route sends the Mac's microphone to the PC, the input round
> trip is at most 6.3 ms at p50 and 10.5 ms at p95, in each direction. Its p99 is reported and not
> bounded, as in `perf.input.round-trip-wifi`.
>
> Run: the run of `perf.own.driver`, with those three streams running from before it starts to after
> it ends.
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
> peer, waits 1 s, pushes it through the peer's zone that leads home, and waits 1 s. It runs once
> with each machine driving.
>
> Baseline: a crossing out and the crossing home summed took 9 ms at p50, 37 ms at p95 and 40 ms
> at most, over 52 pairs, so a crossing about 4.5 ms at p50 and 18 ms at p95 (#4). #14 sets the
> p95 at 10 ms.
>
> - tier: machine (only the real capture and the real machines on a real Wi-Fi give a crossing's
>   time)

> r[perf.crossing.cable]
> On the cable, a crossing, as `perf.crossing.wifi` measures it, takes at most 4.5 ms at p50,
> 10 ms at p95 and 20 ms at p99.
>
> Run: the run of `perf.crossing.wifi`, on the cable.
>
> Baseline: the p50 and the p95 of `perf.crossing.wifi`; the p99 is the largest crossing of #4's
> 52 pairs on Wi-Fi, about 20 ms. #14 sets the p95 on both Wi-Fi and the cable.
>
> - tier: machine (only the real capture and the real machines on a real cable give a crossing's
>   time)

## Clipboard

> r[perf.clipboard.text-wifi]
> On Wi-Fi, a text on the clipboard of the machine the input leaves is on the clipboard of the
> machine it reaches, ready to paste, at most 15 ms at p50 and at p95 after the push that crosses
> (`crossing.push.breakthrough`): written there going out (`clipboard.take.sent`), and set at home
> coming home (`clipboard.arrive.set`). A sample is half the sum of a crossing out and the crossing
> home that follows it, each carrying a new text. Its p99 is reported and not bounded: on Wi-Fi
> the radio's stalls set the tail (#14).
>
> Run: on Wi-Fi, 100 times, the driver's machine agent puts a new text of 100 characters on the
> driver's clipboard and pushes the pointer through a zone to the peer; then, 1 s later, the
> peer's machine agent puts another on the peer's clipboard, and the driver's pushes the pointer
> through the peer's zone that leads home. It runs once with each machine driving.
>
> Baseline: 8 to 36 ms over 5 texts, too few for percentiles (#4). #14 sets the p95 at 15 ms.
>
> - tier: machine (only the real OS clipboards and the real machines on a real Wi-Fi give the time
>   to paste)

> r[perf.clipboard.text-cable]
> On the cable, a text follows a crossing, as `perf.clipboard.text-wifi` measures it, within 15 ms
> at p50 and at p95, and within 36 ms at p99.
>
> Run: the run of `perf.clipboard.text-wifi`, on the cable.
>
> Baseline: #14's p95 bound, and for the p99 the largest of #4's 5 texts on Wi-Fi, 36 ms.
>
> - tier: machine (only the real OS clipboards and the real machines on a real cable give the time
>   to paste)

> r[perf.clipboard.image-wifi]
> On Wi-Fi, a PNG of 8 MiB on the clipboard of the machine the input leaves is on the clipboard of
> the machine it reaches, ready to paste, within twice the raw time at p50 and at p95, measured
> from the push that crosses as `perf.clipboard.text-wifi` measures a text. The raw time is the
> median of 5 copies of 8 MiB of random bytes over one TCP stream between the two machine agents,
> made just before the samples. A sample is half the sum of a crossing out and the crossing home
> that follows it, each carrying a new PNG, divided by the raw time. Its p99 is reported and not
> bounded: on Wi-Fi the radio's stalls set the tail (#14).
>
> Run: as `perf.clipboard.text-wifi`, 20 times, each machine agent putting a new PNG of noise of
> between 7.5 and 8 MiB on the clipboard in place of the text. Each PNG fits
> (`clipboard.payload.image-limit`, `clipboard.png.size`).
>
> Baseline: 113 to 345 ms over 3 images of sizes not recorded (#4). #14 sets the bound at twice
> the time the raw bytes take.
>
> - tier: machine (only the real OS clipboards and the real machines on a real Wi-Fi give the time
>   to paste)

> r[perf.clipboard.image-cable]
> On the cable, a PNG of 8 MiB follows a crossing, as `perf.clipboard.image-wifi` measures it,
> within twice the raw time at p50, p95 and p99.
>
> Run: the run of `perf.clipboard.image-wifi`, on the cable.
>
> Baseline: #4 measured no image wired. #14 sets the bound at twice the time the raw bytes take.
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
> Choosing again, on the Audio & Camera page, a machine whose devices it showed before shows its
> devices in a frame whose time is under 16 ms.
>
> Run: on each machine, the machine agent clicks the other machine on the Audio & Camera page,
> then this machine, then the other machine again, 20 times; every frame made after a click that
> chooses a machine shown before meets the bound.
>
> - tier: machine (only a real window on a real display gives frame times)
> - parity: Python bounded, on the Mac only, the time to deal with the click and lay out the kept devices, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.camera-status]
> While the camera's status changes and the Audio & Camera page shows it, each frame's time is
> under 16 ms at p50, p95 and p99, and at its largest.
>
> Run: for 60 s, with the camera route naming the Mac's camera and the Audio & Camera page shown
> on each machine in turn, the PC's machine agent opens the virtual camera as an app does for 2 s,
> then closes it for 2 s, so the camera's status changes as its stream starts and stops.
>
> - tier: machine (only a real window on a real display with a real camera stream gives frame
>   times)
> - parity: Python bounded, on the Mac only, each update of the camera's status at 4 a second and each level update at 20 a second to under 16 ms, from fake statuses, without drawing (one toolkit, Rune, draws the window on both machines, ADR 0001; the frame drawn is what the user sees)

> r[perf.ui.tray-menu]
> Each update of the tray menu takes under 16 ms, at p50, p95 and p99, and at its largest, from
> starting on what the menu shows to handing it to the OS.
>
> Run: the run of `perf.ui.camera-status`, on each machine.
>
> - tier: machine (only a real tray menu on a real OS gives its update times)
> - parity: Python bounded, on the Mac only, its menu's status refresh every 0.4 s to under 16 ms, from fake camera statuses (the tray menu is Beamer's on both machines; Python's PC had no such bound)

## Audio

Each audio budget is the one-way time of a marked test signal: the sending machine's machine agent
plays it, into a microphone the audio route names or as the machine's sound, and the receiving
machine's machine agent records it, from the virtual microphone as an app does or from the output
it plays on. A sample is one marker, from the moment it is played to the moment it is recorded,
against the two machines' clocks tied by out-and-back pairs. Every run lasts 10 minutes on Wi-Fi,
with a marker every 250 ms, and starts once the stream has run for 30 s. The baselines are the
Python app's last full measurement, WP16 in `dev/audio/reports/wp16.md`, made the same way with
the audio helper's test signal, which #4 gathers; the helper is gone (ADR 0004).

> r[perf.audio.mic-pc-to-mac]
> The PC's microphone reaches the Mac's virtual microphone within 56.5 ms at p50 and 62 ms at
> p95. Its p99 is reported and not bounded: no baseline measured one (#4).
>
> Run: the microphone's audio route names the PC and a microphone of it that the PC's machine
> agent plays the test signal into. The Mac's machine agent records the Mac's virtual microphone.
>
> Baseline: 56.5 ms at p50 and 62 ms at p95; 41 to 56 ms at p50 over WP16's runs (#4).
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
> virtual microphone within 172.4 ms at p50 and 185 ms at p95. Its p99 is reported and not
> bounded: no baseline measured one (#4).
>
> Run: the microphone's audio route names the Mac and a microphone of it that the Mac's machine
> agent plays the test signal into. The PC's machine agent records "CABLE Output" as an app does.
>
> Baseline: 172.4 ms at p50 and 185 ms at p95 (#4).
>
> - tier: machine (only real sound through the real OS audio, a real VB-CABLE and a real Wi-Fi
>   gives its delay)

> r[perf.audio.mic-mac-to-pc-low-latency]
> With the PC's low-latency virtual microphone setting on (`audio.low-latency.pc`), the Mac's
> microphone reaches the PC's virtual microphone within 120.3 ms at p50 and 166.7 ms at p95. Its
> p99 is reported and not bounded: no baseline measured one (#4).
>
> Run: the run of `perf.audio.mic-mac-to-pc`, with the setting on.
>
> Baseline: 120.3 ms at p50 and 166.7 ms at p95 (#4).
>
> - tier: machine (only real sound through the real OS audio, a real VB-CABLE and a real Wi-Fi
>   gives its delay)

## Camera

Each camera budget is measured with a camera on the Mac that the Mac's machine agent feeds with
pictures carrying their number and the time they were made, and the PC's machine agent opening
the virtual camera as an app does and reading each picture's number and time. A sample is one
picture, from the time it was made to the time the PC's machine agent got it, against the two
machines' clocks tied by out-and-back pairs. The camera route names that camera, on Wi-Fi, with
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
