# Media

What is inside an audio stream and a camera stream: the media packets that carry them on the link,
how sound is coded and played out smoothly against two machines' clocks, the echo canceller and
the feedback guard on the machine that sends, how a camera's picture is coded, sent, repaired and
shown, and the time map that keeps a camera stream in step with a microphone. It serves F43 and F44
of #11's inventory, "Audio and microphone follow input" and "Camera follows input", which ship.

The rules come from the Python app's media engine at `archive/python`, which is C++
(`native/audio/engine/`: the packets in `packet.h` and `video_packet.cpp`, Opus in `codec.cpp`,
the jitter buffer in `jitter.cpp`, drift in `resampler.cpp`, the streams in `streams.cpp`,
`video_send.cpp` and `video_recv.cpp`, the pacer in `pacer.cpp`, lip sync in `sync.cpp`, the echo
canceller in `aec.cpp`, the feedback guard in `howl.cpp`; the Mac's encoder in
`native/audio/mac/videoenc_mac.mm`), checked by its own suites (`native/audio/tests/`), and from the
measured designs in `dev/audio/PHASE2.md` §4 and §7, `dev/camera/PHASE2.md` §4, `dev/audio/STATUS.md`
and `dev/camera/STATUS.md`. They take #14's media traffic class: media goes as datagrams on the link,
and the media header (stream, sequence number, timestamp, codec), H.264 fragments of at most 1200
bytes, NACK, PLI and the time map stay inside the datagram, with Opus's settings and bitrates
unchanged. The codecs stay C, behind `beamer-opus-sys` and `beamer-speexdsp-sys` (#16, ADR 0004).

What stays elsewhere: how media datagrams travel, that the link never sends a lost one again, that
media never delays input, and how a datagram that does not decode closes the link, are
`spec/link.md`'s (`link.class.media-datagrams`, `link.class.media-and-input`, `link.frame.datagram`,
`link.frame.undecodable`, #146); which audio streams run, between which machines, when echo is
cancelled and against which output, and how feedback is reported, are `spec/audio.md`'s
(`audio.stream.*`, `audio.mic.*`, `audio.echo.when`, `audio.echo.idle`, `audio.feedback.*`, #164);
which camera streams run, how the receiving machine asks for and ends one, its start and failure
waits, and how a reduced stream is reported, are `spec/camera.md`'s (`camera.stream.*`,
`camera.status.reduced`, #165); what the virtual camera shows when no picture arrives is
`camera.placeholder.*`'s; and every latency budget of audio and camera is `spec/perf.md`'s (#70).

Gone with the link's design, so no rule here (#14, ADR 0003): the media engine's own UDP flow, its
offer and answer, its keys, nonces and replay windows, its hello, ping, pong and bye packets, its
fallback through the link and the lower Opus bitrates it set meanwhile, and the outbox limits that
kept that fallback from starving input. ADR 0004's one process has no media helper, so its
command lines, its JSON on stdin and stdout, and its statistics events go too. The dev-only test
signals, packet loss switches and time map logs go with the `audio-dev-*` files (#11). Nothing in
this domain comes later. Linux has no media here (#1).

One parser is the media domain's own: a media packet read from a datagram (`media-frame`). None of
#15's eight state machines is media's. A Mac is a machine running macOS, and a PC one running
Windows. Every rule is `ci` unless it says otherwise: the codecs, the echo canceller and the
feedback guard run on hosted runners over recorded or generated signals, and two machines' clocks
and the network between them are simulated. A rule that only the real cameras, speakers and
microphones of the Mac and `lebig` can show is `machine`.

## Media packets

> r[media.packet.kinds]
> A media packet is one of: an audio frame, a fragment of a video frame, a receiver report, a
> request to send packets again (NACK), a request for a key frame (PLI), or a time map. Each names
> the stream it belongs to, but a time map, which speaks for every stream its sender sends to that
> machine.
>
> - fuzz: media-frame

> r[media.packet.one-datagram]
> Each media packet travels alone in one datagram on the link, at most 1200 bytes long with its
> encoding.
>
> - parity: Python's media packets carried up to 1400 bytes of payload behind a 24-byte header and a 16-byte tag (#14: payloads of at most 1200 bytes, so a packet fits any path the link takes)
> - fuzz: media-frame

> r[media.packet.round-trip]
> Every media packet decodes to the packet that was encoded.
>
> - fuzz: media-frame

> r[media.packet.unknown-stream]
> A media packet naming a stream this machine does not take from that peer, or feedback naming a
> stream it does not send to that peer, is ignored, and the link stays up.
>
> - fuzz: media-frame

> r[media.packet.out-of-range]
> A media packet that decodes but whose values are out of range (an audio frame of 0 samples or of
> more than 5760, or of other than 1 or 2 audio channels, a fragment whose index is not below its
> frame's fragment count) is ignored, and the link stays up.
>
> - fuzz: media-frame

## Stream ids

> r[media.id.unique]
> A machine gives each audio stream and camera stream it starts an id that is not 0 and that no
> other stream it sends or takes has while that stream runs, whatever its kind: an audio stream
> never takes the id of a running camera stream, nor a camera stream that of a running audio
> stream.

r[media.id.shared-microphone-loop]
The shared microphone played into its own machine's virtual microphone (`audio.mic.own-machine`)
has an id too, which no audio stream or camera stream of that machine takes while it plays.

## Audio frames

r[media.audio.rate]
Every audio stream is sampled at 48 kHz and sent as one Opus frame per media packet.

> r[media.audio.timestamp]
> An audio frame's timestamp counts samples at 48 kHz: each frame's is the previous frame's plus
> the previous frame's length in samples, counted modulo 2^32. The receiving machine orders frames by
> timestamp across that wrap, never by arrival.
>
> - fuzz: media-frame

r[media.audio.silent]
An audio frame says it is silent when the sound it codes has a root mean square below 0.003 of full
scale (about −50 dBFS).

r[media.audio.mapping]
Sound goes between a device's audio channels and a stream's by copying them when the counts match,
by giving every channel the one channel of a mono signal, by averaging every channel into a mono
stream, and otherwise by taking the first channels in order and filling any left over with silence.

## Opus

> r[media.opus.microphone]
> A microphone stream is coded with Opus for voice: mono, 32 kb/s, frames of 10 ms, in-band
> forward error correction on for an expected loss of 10 %, discontinuous transmission off, and
> complexity 5.

> r[media.opus.sound]
> A sound stream is coded with Opus for music: stereo, 96 kb/s, frames of 10 ms, forward error
> correction off, discontinuous transmission off, and complexity 9.
>
> - parity: Python capped it at 48 kb/s while the stream went through the link because its UDP flow failed (#14: media always travels as datagrams on the link, and Opus bitrates are unchanged)

r[media.opus.encode-error]
A frame the encoder cannot code is not sent, and the stream's next frame keeps the timestamp it
would have had, so the receiving machine conceals the gap as a lost frame.

## The jitter buffer

The receiving machine plays each audio stream from a jitter buffer. Its level is how much sound,
in ms, it holds ahead of what it plays: the frames received and not yet played, from the next one
due to the newest one, and what the resampler holds. Its aim is the level it steers towards.

r[media.jitter.waits]
Before it plays, the jitter buffer plays silence until the frames it holds span its aim, less half
of what the output asks for at a time. It then plays from the oldest frame it holds.

> r[media.jitter.aim]
> The jitter buffer's aim is the sum of: what the output asks for at a time, but at least 10 ms;
> half a frame; the 97th percentile of the transit excess over the last 10 s; and 2 ms, kept
> between 10 ms and 120 ms. The transit excess of a frame is its arrival time less its timestamp's
> time, less the lowest such value over the last 10 s, and an excess of 150 ms or more is left out
> as an outage. With fewer than 10 frames in the last 10 s, the percentile counts as 10 ms. It is
> worked out again at most every 20 ms. A new stream's aim starts at 25 ms.
>
> - fuzz: media-frame

r[media.jitter.aim-moves]
A higher aim is taken at once. A lower one is approached gradually: at each pull, the aim moves
towards it by the pull's length over 2 s of the distance between them.

r[media.jitter.spikes]
When at least 2 transit excesses of more than the larger of 15 ms and the 97th percentile plus
5 ms, and of less than 150 ms, arrived in the last 10 s (spikes less than 0.2 s apart counting as
one, at its largest), the aim's percentile is at least the largest of them.

r[media.jitter.order]
The jitter buffer plays frames in timestamp order. A frame whose time has already been played is
dropped as late, and a second copy of a frame it holds is dropped.

r[media.jitter.lost]
When a frame's turn comes and it is missing but a newer frame has arrived, the frame is lost: it is
concealed from the next frame's forward error correction when that frame is there and the stream
has forward error correction on, and otherwise by Opus's packet loss concealment, and play moves on.

r[media.jitter.late-frame]
When a frame's turn comes and no newer frame has arrived, Opus's packet loss concealment plays a
frame in its place without moving play on, and the frame keeps its turn. After more than 3 such
frames in a row, the jitter buffer stops playing and waits again as `media.jitter.waits` says.

> r[media.jitter.concealed-once]
> A frame concealed while nothing newer had arrived, which then proves lost (a newer frame
> arrives and it never does), is not concealed a second time: the concealment already played stood
> in for it. A frame that arrives late after such a concealment is played.

r[media.jitter.flush]
When the jitter buffer starts or starts again, and whenever its level is above its aim by more than
the larger of 40 ms and the aim, it discards frames, oldest first, until it is within one frame of
its aim.

r[media.jitter.drop-silent]
While the level, averaged over 1 s, is above the aim by more than the larger of 5 ms and three
quarters of a frame, the next frame due is discarded instead of played when it is silent and a newer
frame has arrived.

r[media.jitter.drop-sound]
A frame that is not silent is discarded only after the averaged level has stayed above the aim, as
`media.jitter.drop-silent` says, for 2 s, and while it is above it by more than the larger of 30 ms
and a frame.

r[media.jitter.decoded]
Every frame the jitter buffer receives in time is decoded, including the ones it discards, so the
decoder stays in step with the encoder.

r[media.jitter.decode-error]
A frame that does not decode plays as silence of its length.

> r[media.jitter.jump]
> A frame whose timestamp is 512 frames or more past the next frame due starts the jitter buffer
> again from that frame, discarding what it holds. Before it plays, a frame 512 frames or more older
> than the newest one is dropped as late.
>
> - fuzz: media-frame

r[media.jitter.frame-length]
The jitter buffer takes each frame's length from the frame itself, up to 120 ms, so a stream may
change it.

## Drift

The two machines' clocks run at slightly different rates, and the jitter buffer makes up for it by
playing slightly faster or slower than 48 kHz, through a resampler.

r[media.drift.steer]
The jitter buffer plays at 1 + p × 10⁻⁶ times the stream's rate, where p, in ppm, is 40 times the
error plus the drift integral, kept within ±500. The error is the level, averaged over 1 s, less the
aim, in ms. The drift integral grows by 0.1 times the error for each second, and stays within ±200.

r[media.drift.catch-up]
While the error is above 4 ms, the jitter buffer plays faster: at least 160 + 200 × (error − 4)
ppm, and at most 2000 ppm.

r[media.drift.clock-ratio]
Once the ratio of the two clocks, measured from frame timestamps against arrival times and play
times, spans at least 60 s with a standard error of at most 5 ppm, the drift integral follows that
ratio, kept within ±200 ppm and moving at most 2 ppm per second, instead of growing with the error.
A step in the measurement (the sender's capture or the output starting again) starts a new segment
of it without discarding the older ones, and the integral keeps its value while the ratio is
measured again.

r[media.drift.bounded]
Over 10 minutes of a stream whose sender's clock runs 100 ppm fast, with 2 % of frames lost at
random and 0 to 20 ms of jitter, or 100 ppm slow, with 2 % lost in bursts and 0 to 10 ms of jitter:
from 30 s on, the level averaged over each 10 s stays within 10 ms of the aim, the level never
reaches 200 ms, every frame decodes, and at the end the drift integral is within 10 ppm of the
clocks' offset.

r[media.drift.resampler]
The resampler interpolates with a windowed sinc of 16 taps in 512 phases (a Kaiser window, β 8,
cutoff 0.97 of the Nyquist frequency), each phase's taps summing to 1.

## Receiver reports

> r[media.report.while-receiving]
> While a machine takes an audio stream from a peer that sent it media in the last 1 s, it sends
> that peer a receiver report every 10 ms, unless it sent that peer a media packet in the last 9 ms.
> One report goes per 10 ms per peer, cycling through the audio streams it takes from that peer
> that sent media in the last 1 s.
>
> - parity: Python sent them on its own UDP flow, never through the link (#14: media goes as datagrams on the link)

r[media.report.carries]
An audio stream's receiver report carries the newest timestamp received and the jitter buffer's
level, in tenths of a ms up to 6553.5 ms, or 0 while it does not play.

## The shared microphone into its own machine

The shared microphone played into its own machine's virtual microphone (`audio.mic.own-machine`)
goes through a short buffer of its own, with no codec.

r[media.loop.aim]
Its aim is what the microphone delivers at a time, plus what the virtual microphone asks for at a
time, plus 2 ms, kept between 5 ms and 60 ms. A higher aim is taken at once, and a lower one
approached as `media.jitter.aim-moves` says.

r[media.loop.waits]
It plays silence until its level reaches its aim, and again from the first time it runs dry, which
it fills with silence.

r[media.loop.flush]
When its level is above its aim by more than the larger of 40 ms and the aim, it discards the
oldest sound until it is back at its aim.

r[media.loop.drift]
It steers its rate on its level averaged over 1 s, as `media.drift.steer` and `media.drift.catch-up`
say, so that the microphone's clock and the virtual microphone's may differ.

## The echo canceller

When `audio.echo.when` says a microphone stream has its echo cancelled, the sending machine
removes from it what its default output plays, the reference, before the encoder.

r[media.echo.reference]
The reference is everything the output plays, Beamer's own playback included, captured from the
output itself.

r[media.echo.frames]
The echo canceller works on 10 ms mono frames at 48 kHz, with an adaptive filter of 100 ms: it
removes echo that reaches the microphone up to 100 ms after the reference played it.

r[media.echo.aligned]
The reference is lined up with the microphone by the OS's capture times of both, so the delay the
filter must cover is only the sound's way from the speakers to the microphone, plus 1 ms.

r[media.echo.drift]
When the microphone and the output run on different clocks, the reference is resampled to stay in
line with the microphone, steered by its position error (a second-order loop of 0.2 rad/s); a
position error of more than 20 ms lines it up again at once. With the output's clock 100 ppm fast
or slow, the canceller still finds the echo within 4 ms of its true delay and removes it by at
least 20 dB.

r[media.echo.waits]
A microphone frame waits for its reference until 30 ms of microphone sound has queued behind it,
and then goes with silence for the reference it lacks. While no reference has arrived for 100 ms
of microphone sound, frames do not wait.

r[media.echo.no-reference]
When the reference cannot be captured, the microphone stream runs without its echo cancelled, and
Core reports it so (`audio.echo.reported`).

r[media.echo.removes]
With a reference whose echo reaches the microphone within 100 ms, the echo in the microphone
stream is at least 20 dB lower than without the canceller 5 s after the echo starts, and a voice
at the microphone, alone or over the echo, changes by less than 1 dB.

> r[media.echo.no-path]
> Until an echo path shows, the microphone passes the canceller with only the filter's DC notch: an
> echo path shows once, in at least 12 of the last 100 frames, the filter removed at least 8 dB of
> the microphone's sound, and it stays shown for 15 s after the last such frame. A reference the
> microphone does not hear changes the microphone stream by less than 0.5 dB, and adds nothing of
> its own above −40 dB of the microphone.

r[media.echo.weight]
Once an echo path shows, the share of the filter's estimate removed follows how much of that
estimate the microphone really holds, measured over about 1 s while the reference plays, moving at
most from none to all in 100 ms. It drops to none when, in 30 of the last 100 frames, removing the
estimate made the microphone at least 1 dB louder.

r[media.echo.residual]
While at least half of the estimate is removed, the residual echo is suppressed by up to 40 dB, and
up to 15 dB while the near end talks too. Nothing else of the microphone's sound is changed: no
noise suppression, gain control, voice detection or dereverberation.

> r[media.echo.real]
> With the Mac's speakers playing a test signal at the volume Alex uses and the Mac's microphone
> sent to `lebig` with its echo cancelled, the signal reaches `lebig`'s virtual microphone at least
> 15 dB lower than with the canceller off.
>
> - tier: machine (only the Mac's real speakers and microphone in a room make the echo)

## The feedback guard

Acoustic feedback can loop through two machines: one machine's sound plays on another's speakers,
a microphone on the first hears them, and an app there plays that microphone into its own sound,
which its sound stream sends again. The feedback guard watches each sound stream just before its
encoder and turns it down when feedback builds up.

r[media.feedback.sound-only]
Every sound stream has a feedback guard, and no microphone stream does.

r[media.feedback.untouched]
While the feedback guard does not turn the stream down, it passes the sound bit for bit.

> r[media.feedback.tone]
> The guard looks, every 512 samples, at a Hann-windowed 1024-point spectrum of the stream's mono
> mix for a tone: a peak, with the two bins on each side, of 800 Hz to 16 kHz, that holds at least
> 40 % of the power from 140 Hz to 16 kHz, with no partial at half, twice or three times its
> frequency and no other spectral line within 15 dB of it.

r[media.feedback.rising]
Feedback is such a tone whose level, at a frequency stable to within one bin, rose by at least 6 dB
over 0.26 s, 0.77 s or 2.3 s, with the means of all four quarters of that span never falling by
more than 0.5 dB and each half carrying at least 35 % of the rise, and which ends at −45 dBFS or
louder. A tone that steps up and then stays level is not feedback.

r[media.feedback.duck]
On feedback, the guard turns the stream down by 20 dB over 20 ms, and keeps it down for at least
2 s, doubled for each earlier time within the last minute, up to 30 s.

r[media.feedback.release]
Once the tone has fallen by 12 dB from where it triggered, or gone, for 0.3 s and the least time
down has passed, the guard brings the stream back up over 1 s. Once the tone has fallen at least
once, the guard brings it back up 2 s after the least time down in any case.

r[media.feedback.own-tone]
A tone that has not fallen at all, and stays within 6 dB of where it triggered, 4 s after the guard
turned the stream down is the sound's own: the guard brings the stream back up, and ignores tones
at that frequency for 30 s.

r[media.feedback.passes]
Music and voices pass the guard untouched: chords, a swelling chord, sine sweeps, held and
swelling flute-like notes, a tone switched on, voices with drifting pitch, vibrato, formants from
380 Hz to 2.2 kHz, swelling syllables and a sung note.

r[media.feedback.caught]
A simulated loop between two machines, of loop gain 1.06 to 4 at 1.2 kHz or 7.1 kHz with a delay of
100 to 180 ms, with or without music under it, is turned down within 9 s, at its frequency within
200 Hz, with no false alarm.

r[media.feedback.reported]
The guard reports each change, turned down or back up, with the tone's frequency in Hz, to Core
(`audio.feedback.report`).

<!-- VIDEO -->
