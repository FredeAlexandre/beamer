# Media

What is inside an audio stream and a camera stream: the media packets that carry them on the link,
how sound is coded and played out smoothly against two machines' clocks, the echo canceller and the
feedback guard on the machine that sends, how a camera's picture is coded, sent, repaired and shown,
and the time map that keeps a camera stream in step with a microphone. It serves F43 and F44 of
#11's inventory, "Audio and microphone follow input" and "Camera follows input", which ship.

The rules come from the Python app's media engine at `archive/python`, which is C++
(`native/audio/engine/`: the packets in `packet.h` and `video_packet.cpp`, Opus in `codec.cpp`, the
jitter buffer in `jitter.cpp`, drift in `resampler.cpp`, the streams in `streams.cpp`,
`video_send.cpp` and `video_recv.cpp`, the pacer in `pacer.cpp`, lip sync in `sync.cpp`, the echo
canceller in `aec.cpp`, the feedback guard in `howl.cpp`; the Mac's encoder in
`native/audio/mac/videoenc_mac.mm`), checked by its own suites (`native/audio/tests/`), and from the
measured designs in `dev/audio/PHASE2.md` §4 and §7, `dev/camera/PHASE2.md` §4,
`dev/audio/STATUS.md` and `dev/camera/STATUS.md`. They take #14's media traffic class: media goes as
datagrams on the link, and the media header (stream, packet number, timestamp, codec), H.264
fragments of at most 1200 bytes, NACK, PLI and the time map stay inside the datagram, with Opus's
settings and bitrates unchanged. The codecs stay C, behind `beamer-opus-sys` and
`beamer-speexdsp-sys` (#16, ADR 0004).

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
kept that fallback from starving input. ADR 0004's one process has no media helper, so its command
lines, its JSON on stdin and stdout, and its statistics go too. The dev-only test signals, forced
packet loss and time map logs go with the `audio-dev-*` files (#11). Nothing in this domain comes
later. Linux has no media here (#1).

One parser is the media domain's own: a media packet read from a datagram (`media-frame`). None of
#15's eight state machines is media's. A Mac is a machine running macOS, and a PC one running
Windows. Every rule is `ci` unless it says otherwise: the codecs, the echo canceller and the
feedback guard run on hosted runners over recorded or generated signals, and two machines' clocks
and the link between them are simulated. A rule that only the real cameras, speakers and microphones
of the Mac and `lebig` can show is `machine`.

## Media packets

> r[media.packet.kinds]
> A media packet is one of: an audio frame, a fragment of a picture of a camera stream, a receiver
> report, a request to send packets again (NACK), a request for a key frame (PLI), or a time map.
> Each names the stream it belongs to, but a time map, which speaks for several
> (`media.time-map.carries`).
>
> - fuzz: media-frame

> r[media.packet.one-datagram]
> Each media packet travels alone in one datagram on the link, at most 1200 bytes long with its
> encoding.
>
> - parity: Python's media packets carried up to 1400 bytes of payload behind a 24-byte header and a 16-byte tag (#14: payloads of at most 1200 bytes)
> - fuzz: media-frame

> r[media.packet.number]
> Each audio frame and each fragment carries its stream's packet number: 1 for the stream's first
> packet, and one more for each packet after it. A fragment sent again keeps its number.
>
> - fuzz: media-frame

> r[media.packet.round-trip]
> Every media packet decodes to the packet that was encoded.
>
> - fuzz: media-frame

> r[media.packet.unknown-stream]
> A media packet naming a stream this machine does not take from that peer, feedback naming a stream
> it does not send to that peer, an audio frame naming a camera stream, and a fragment naming an
> audio stream are ignored, and the link stays up.
>
> - fuzz: media-frame

> r[media.packet.out-of-range]
> A media packet that decodes but whose values are out of range is ignored, and the link stays up: a
> fragment whose picture has no fragments or more than 1024, or whose index is not below its
> picture's count; a request to send packets again naming none, or more than 32 runs of them; a time
> map of no stream or of more than 32.
>
> - fuzz: media-frame

## Stream ids

> r[media.id.unique]
> A machine gives each audio stream and camera stream it starts, the shared microphone played into
> its own virtual microphone (`audio.mic.own-machine`) included, an id that is not 0 and that no
> other stream it sends, takes or plays into its own virtual microphone has while that stream runs,
> whatever its kind: an audio stream never takes the id of a running camera stream, nor a camera
> stream that of a running audio stream.

## Audio frames

r[media.audio.rate]
Every audio stream to another machine is sampled at 48 kHz and sent as one Opus frame per media
packet.

> r[media.audio.timestamp]
> An audio frame's timestamp counts samples at 48 kHz: each frame's is the previous frame's plus the
> previous frame's length in samples, counted modulo 2^32. The receiving machine orders frames by
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
> A microphone stream is coded with Opus for voice: mono, 32 kb/s, frames of 10 ms, in-band forward
> error correction on for an expected loss of 10 %, discontinuous transmission off, and complexity
> 5.

> r[media.opus.sound]
> A sound stream is coded with Opus for music: stereo, 96 kb/s, frames of 10 ms, forward error
> correction off, discontinuous transmission off, and complexity 9.
>
> - parity: Python capped it at 48 kb/s while the stream went through the link because its UDP flow failed (#14: media always travels as datagrams on the link, and Opus bitrates are unchanged)

r[media.opus.encode-error]
A frame the encoder cannot code is not sent, and the stream's next frame keeps the timestamp it
would have had, so the receiving machine conceals the gap as a lost frame.

## The jitter buffer

The receiving machine plays each audio stream from another machine from a jitter buffer. Its level
is how much sound, in ms, it holds ahead of what it plays: the frames received and not yet played,
from the next one due to the newest one, and what the resampler holds. Its aim is the level it
steers towards.

r[media.jitter.waits]
The jitter buffer plays silence while it waits: before it first plays, after a jump
(`media.jitter.jump`), and after it stops (`media.jitter.late-frame`). When it has not played since
it started or jumped, or the oldest frame it holds is newer than the next frame due, it waits until
the frames it holds, from the start of the oldest to the end of the newest, span its aim less half
of what the output asks for at a time, and then plays from the oldest frame it holds. Otherwise it
waits until its level reaches its aim, and goes on from the next frame due.

> r[media.jitter.aim]
> The jitter buffer's aim is the sum of: what the output asks for at a time, but at least 10 ms;
> half a frame; the 97th percentile of the transit excess over the last 10 s; and 2 ms, kept between
> 10 ms and 120 ms. The transit excess of a frame is its arrival time less its timestamp's time,
> less the lowest such value over the last 10 s, and an excess of 150 ms or more counts as an
> outage, not jitter. With fewer than 10 frames in the last 10 s, the percentile counts as 10 ms. It
> is worked out again at most every 20 ms. A new stream's aim starts at 25 ms.

r[media.jitter.aim-moves]
A higher aim is taken at once. A lower one is approached gradually: at each pull, the aim moves
towards it by the pull's length over 2 s of the distance between them.

r[media.jitter.spikes]
Once more than 50 frames arrived in the last 10 s, when at least 2 transit excesses of more than the
larger of 15 ms and the 97th percentile plus 5 ms, and of less than 150 ms, arrived in the last 10 s
(spikes less than 0.2 s apart counting as one, at its largest), the aim's percentile is at least the
largest of them.

r[media.jitter.order]
The jitter buffer plays frames in timestamp order. A frame whose time has already been played is
dropped as late, and a second copy of a frame it holds is dropped.

r[media.jitter.lost]
When a frame's turn comes and it is missing but a newer frame has arrived, the frame is lost: it is
concealed from the next frame's forward error correction when that frame is there and the stream has
forward error correction on, and otherwise by Opus's packet loss concealment, and play moves on.

r[media.jitter.late-frame]
When a frame's turn comes and no newer frame has arrived, Opus's packet loss concealment plays a
frame in its place without moving play on, and the frame keeps its turn. After more than 3 such
frames one after another, the jitter buffer stops playing and waits again as `media.jitter.waits`
says.

> r[media.jitter.concealed-once]
> A frame concealed while nothing newer had arrived, which then proves lost (a newer frame arrives
> and it never does), is not concealed a second time: the concealment already played stood in for
> it. A frame that arrives late after such a concealment is played.

r[media.jitter.flush]
When the jitter buffer starts or starts again, and whenever its level is above its aim by more than
the larger of 40 ms and the aim, it discards frames, oldest first, until it is within one frame of
its aim.

r[media.jitter.discard-silent]
While the level, averaged over 1 s, is above the aim by more than the larger of 5 ms and three
quarters of a frame, the next frame due is discarded instead of played when it is silent and a newer
frame has arrived.

r[media.jitter.discard-sound]
A frame that is not silent is discarded only after the averaged level has stayed above the aim, as
`media.jitter.discard-silent` says, for 2 s, and while it is above it by more than the larger of 30
ms and a frame.

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
While the error is above 4 ms, the jitter buffer plays faster: at least 160 + 200 × (error − 4) ppm,
and at most 2000 ppm.

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
> that peer a receiver report every 10 ms, unless it sent that peer a datagram in the last 9 ms. One
> report goes per 10 ms per peer, cycling through the audio streams it takes from that peer that
> sent media in the last 1 s.

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
When its level is above its aim by more than the larger of 40 ms and the aim, it discards the oldest
sound until it is at its aim again.

r[media.loop.drift]
It steers its rate on its level averaged over 1 s, as `media.drift.steer` and `media.drift.catch-up`
say, so that the microphone's clock and the virtual microphone's may differ.

## The echo canceller

When `audio.echo.when` says a microphone stream has its echo cancelled, the sending machine removes
from it what its default output plays, the reference, before the encoder.

r[media.echo.reference]
The reference is everything the output plays, Beamer's own playback included, captured from the
output itself.

r[media.echo.frames]
The echo canceller works on 10 ms mono frames at 48 kHz, with an adaptive filter of 100 ms: it
removes echo that reaches the microphone up to 100 ms after the reference played it.

r[media.echo.aligned]
The reference is lined up with the microphone by the OS's capture times of both, so the delay the
filter must cover is only the sound's travel from the speakers to the microphone, plus 1 ms.

r[media.echo.drift]
When the microphone and the output run on different clocks, the reference is resampled to stay in
line with the microphone, steered by its position error (a second-order loop of 0.2 rad/s); a
position error of more than 20 ms lines it up again at once. With the output's clock 100 ppm fast or
slow, the canceller still finds the echo within 4 ms of its true delay and removes it by at least 20
dB.

r[media.echo.waits]
A microphone frame waits for its reference until 30 ms of microphone sound has queued behind it, and
then goes with silence for the reference it lacks. While no reference has arrived for 100 ms of
microphone sound, frames do not wait.

r[media.echo.no-reference]
When the reference cannot be captured, the microphone stream runs without its echo cancelled, and
Core reports it so (`audio.echo.reported`).

r[media.echo.removes]
With a reference whose echo reaches the microphone within 100 ms, the echo in the microphone stream
is at least 20 dB lower than without the canceller 5 s after the echo starts. A voice at the
microphone alone changes by less than 1 dB. While a voice talks over the echo, it loses less than 3
dB, and the echo is still more than 10 dB lower.

> r[media.echo.no-path]
> Until an echo path shows, the microphone passes the canceller with only the filter's DC notch: an
> echo path shows once, in at least 12 of the last 100 frames, the filter removed at least 8 dB of
> the microphone's sound, and it stays shown for 15 s after the last such frame. A reference the
> microphone does not hear changes the microphone stream by less than 0.5 dB, and adds nothing of
> its own above −40 dB of the microphone.

r[media.echo.weight]
Once an echo path shows, the share of the filter's estimate removed aims at (g − 0.3) / 0.3, kept
between none and all: none while g is 0.3 or less, all while it is 0.6 or more. g is how much of the
estimate the microphone holds: the mean of the microphone times the estimate over the mean square of
the estimate, each mean an exponential average with a time constant of 1 s, updated only while the
reference played above −60 dBFS within the last 100 ms. The share moves towards its aim by at most a
tenth each 10 ms frame. It aims at none when, in 30 of the last 100 frames, removing the estimate
made the microphone at least 1 dB louder.

r[media.echo.residual]
While at least half of the estimate is removed, the residual echo is suppressed by up to 40 dB, and
up to 15 dB while the near end talks too. Nothing else of the microphone's sound is changed: no
noise suppression, gain control, voice detection or dereverberation.

> r[media.echo.real]
> With the Mac's default output on its speakers, neither headphones nor a headset, playing a test
> signal that reaches the Mac's microphone at least 30 dB above the room's noise, and the Mac's
> microphone sent to `lebig` with its echo cancelled (`audio.echo.when`), the signal reaches
> `lebig`'s virtual microphone at least 15 dB below its level at the Mac's microphone.
>
> - tier: machine (only the Mac's real speakers and microphone in a room make the echo)

## The feedback guard

Acoustic feedback can loop through two machines: one machine's sound plays on another's speakers, a
microphone on the first hears them, and an app there plays that microphone into its own sound, which
its sound stream sends again. The feedback guard watches each sound stream just before its encoder
and turns it down when feedback builds up.

r[media.feedback.sound-only]
Every sound stream has a feedback guard, and no microphone stream does.

r[media.feedback.untouched]
While the feedback guard does not turn the stream down, it passes the sound bit for bit.

> r[media.feedback.tone]
> The guard looks, every 512 samples, at a Hann-windowed 1024-point spectrum of the stream's mono
> mix for a tone: a peak, with the two bins above and below it, of 800 Hz to 16 kHz, that holds at
> least 40 % of the power from 140 Hz to 16 kHz, with no partial at half, twice or three times its
> frequency and no other spectral line within 15 dB of it.

r[media.feedback.rising]
Feedback is such a tone whose level, at a frequency stable to within one bin, rose by at least 6 dB
over 0.26 s, 0.77 s or 2.3 s, with the means of all four quarters of that span never falling by more
than 0.5 dB and each half carrying at least 35 % of the rise, and which ends at −45 dBFS or louder.
A tone that steps up and then stays level is not feedback.

r[media.feedback.duck]
On feedback, the guard turns the stream down by 20 dB over 20 ms, and keeps it down for at least 2
s, doubled for each earlier time within the last minute, up to 30 s.

r[media.feedback.lift]
Once the tone has fallen by 12 dB from where it triggered, or gone, for 0.3 s and the least time
down has passed, the guard lifts the stream to its full level over 1 s. Once the tone has fallen at
least once, the guard lifts it 2 s after the least time down in any case.

r[media.feedback.own-tone]
A tone that has not fallen at all, and stays within 6 dB of where it triggered, 4 s after the guard
turned the stream down is the sound's own: the guard lifts the stream, and ignores tones at that
frequency for 30 s.

r[media.feedback.passes]
Music and voices pass the guard untouched: chords, a swelling chord, sine sweeps, held and swelling
flute-like notes, a tone switched on, voices with drifting pitch, vibrato, formants from 380 Hz to
2.2 kHz, swelling syllables and a sung note.

r[media.feedback.caught]
A simulated loop between two machines, of loop gain 1.06 to 4 at 1.2 kHz or 7.1 kHz with a delay of
100 to 180 ms, with or without music under it, is turned down within 9 s, at its frequency within
200 Hz, with no false alarm.

r[media.feedback.reported]
The guard reports each change, turned down or lifted, with the tone's frequency in Hz, to Core
(`audio.feedback.report`).

## Camera streams: coding

A camera stream carries H.264 from the camera machine's encoder. Its timestamps count a 90 kHz
clock, and each coded picture, an access unit, has a frame number.

> r[media.video.encoder]
> A Mac codes each camera stream with the hardware H.264 encoder, in real time with low-latency rate
> control: Constrained High profile at the level the size needs, no B-frames, so pictures leave in
> the order they are decoded, at the stream's average bitrate (`media.video.reduced`) and the camera
> quality's frame rate (`camera.quality.size`), with long-term references on. It never uses a
> software encoder instead.
>
> - tier: machine (only a real Mac's hardware encoder codes the stream)

r[media.video.no-periodic-key]
The encoder sends no key frame on a timer. A key frame goes only first, after the camera stream's
size changes, after the camera changes, and when the receiving machine asks for one
(`media.video.key-request`). Every key frame carries its parameter sets before it.

r[media.video.timestamp]
A camera stream's timestamp counts 90 kHz from the capture time of its first picture, modulo 2^32; a
picture captured before the first counts 0. Every fragment of a picture carries its timestamp.

r[media.video.frame-number]
Each picture the encoder hands on gets the next frame number of its stream, from 0. A number is
never used twice, and a picture dropped after it was numbered leaves a gap.

> r[media.video.fragment]
> A picture is cut, at any byte, into the fewest fragments that each fit one media packet, at most
> 1024, numbered from 0; a picture of 0 bytes is one fragment. Each fragment says its frame number,
> its index, how many fragments the picture has, whether the picture is a key frame, a refresh or a
> long-term reference, whether it was handed on while a key frame of its stream was still being
> sent, and how long the encoder took over it, in tenths of a ms up to 6553.5 ms.
>
> - fuzz: media-frame

r[media.video.order]
A picture's fragments are sent in index order, and pictures in frame number order.

## Camera streams: sending

> r[media.video.pacing]
> The camera machine paces the fragments of every camera stream it sends to one machine together, as
> a bucket of 38,400 bytes, full at first, refilled at 25 Mbit/s. A key frame's fragments go at 4
> times the stream's bitrate, each at its due time counted from the first one's departure, and any
> that fell due while the sender was late go together. Fragments sent again go before any new
> fragment, and are never held to a key frame's schedule. Audio frames, feedback and time maps are
> not paced.

r[media.video.backlog]
While the pictures waiting to leave would take more than 250 ms at 4 times the stream's bitrate, or
6 or more of them wait, the camera machine gives its encoder no new picture. It gives it pictures
again once they would take less than 100 ms and fewer than 2 wait.

r[media.video.overflow]
A picture that is not a key frame and that would make 17 pictures wait, or the waiting pictures
more than 4 MiB, is dropped after it was numbered, and the camera machine then recovers the stream
as on a loss (`media.video.key-loss`). A key frame that would do so is not dropped: it drops every
waiting picture that is not a key frame, and goes, and no recovery for a picture dropped before it
follows.

r[media.video.resend]
The camera machine keeps each fragment it sent for 1 s, or the last 2048 of the stream, whichever is
fewer. When the receiving machine asks for a fragment it still keeps, of a picture it did not drop,
it sends it again as it was, with its packet number. It ignores a request for any other.

r[media.video.reduced]
A camera stream runs at the camera quality's bitrate, at half of it, or at a quarter, and is reduced
while below the full one. On each receiver report, when the reports of the last 2 s span at least
1.5 s and at least 10 pictures were sent over them, the bitrate steps down one level if, over them,
the pictures lost after repair grew by more than 2 % of the pictures sent, or the late pictures by
more than 10 %; the reports so far are then set aside, and the 10 s count starts again, also at a
quarter, where there is no level left to step down to. After 10 s of that count, the bitrate steps
up one level, and the count starts again. Neither changes the stream's size or frame rate, or sends
a key frame.

## Camera streams: key frames

The receiving machine asks for a key frame (PLI) to start a stream, when the stream's size is
refused, and to repair it after a loss, a decoder error or a stall.

r[media.video.starts-on-request]
The camera machine codes nothing for a camera stream before the receiving machine first asks for a
key frame, for whatever reason; the first picture then is a key frame.

r[media.video.no-receiver]
A camera stream whose receiving machine has not asked for a key frame within 3 s of the stream's
start, or from which no receiver report, request to send packets again or request for a key frame
has come for 5 s once it asked, fails because no picture reached that machine
(`camera.stream.no-receiver`), and the camera machine's camera is closed for it at once.

> r[media.video.key-request]
> A request for a key frame to start the stream or after its size was refused gets a key frame,
> unless one is already asked for and its first fragment has not left, or it left less than QUIC's
> RTT for the link (`link.rtt.reported`) plus its pacing time plus 50 ms ago; such a request is a
> duplicate. Other such requests get at most one key frame per 1 s since the last key frame, and one
> inside that time is dropped, never queued.
>
> - parity: Python took the time to the peer and back from its own pings, assuming 20 ms before the first (#14: no pings of media's own; QUIC's RTT for the link takes their place)

r[media.video.key-loss]
A request for a key frame after a loss, a decoder error or a stall gets a refresh, a picture coded
only from long-term references the receiving machine confirmed, when one is confirmed since the last
key frame, and a key frame otherwise. Such a request is ignored while a key frame is asked for and
has not left, and when it names the same last clean picture as the request the last refresh answered
and comes within QUIC's RTT for the link (`link.rtt.reported`) plus 50 ms of that refresh.

r[media.video.key-limit]
A key frame asked for after a loss, a decoder error or a stall goes at most once per 2 s; inside
that time the request gets a refresh when a long-term reference is confirmed, and otherwise waits
for the end of that time, unless a receiver report saying the stream is clean comes first. Every key
frame counts towards this limit and the one of `media.video.key-request`, whatever asked for it.

r[media.video.refresh-fails]
A refresh after which no receiver report shows a newer clean run within 1 s failed. After 3 failed
refreshes one after another, the next request after a loss, a decoder error or a stall gets a key
frame.

r[media.video.confirmed]
The camera machine remembers the last 64 long-term references it sent since the last key frame. A
receiver report's clean run confirms those of its frame numbers inside it, each once, and the
encoder may then code a refresh from them. A key frame forgets every reference before it.

## Camera streams: receiving

> r[media.video.reassembly]
> The receiving machine puts a picture together from its fragments in any order; a picture is whole
> once it holds every fragment its count names. A second copy of a fragment, a fragment whose count
> differs from the one first seen for its picture, and a fragment of a picture already decoded or
> given up are ignored. A picture's timestamp and flags are those of its first fragment seen.
>
> - fuzz: media-frame

r[media.video.in-flight]
The receiving machine keeps at most 160 pictures being put together once the stream has started, and
64 before, forgetting the oldest for a new one.

> r[media.video.nack]
> On a fragment whose packet number is more than one above the highest yet, the receiving machine at
> once asks the camera machine to send every missing packet again, as many to a request as fit. It
> asks again for each still missing 15 ms after the first request, and then once more after the retry
> interval, max(2 × QUIC's RTT for the link (`link.rtt.reported`), 15 ms): 3 requests in all. The
> first goes whatever the due time; each later one only while the oldest picture the gap may hold is
> not yet due, by its own due time even once it is lost or forgotten, and once one is skipped that
> packet is asked for no more. The pictures a gap may hold run from that oldest one, the picture of
> the last fragment before the gap or the next one when that fragment ended its picture, to the
> picture of the fragment after the gap, or the one before it when that fragment begins its picture;
> they are the oldest one alone when that range is empty. A gap of more than 64 packets is asked for
> not at all. What a gap does to its pictures is `media.video.lost`.
>
> - parity: Python took the due time of a gap's oldest picture that it had already lost or forgotten (`media.video.in-flight`) as an unseen picture's (`media.video.unseen-due`), from the next picture it still held a fragment of (a picture's own due time is one a test can decide from the spec; Python's estimate hung on which pictures it still held and on the frame period at the retry)

r[media.video.lost]
Loss evidence goes on up to 256 of the pictures a gap may hold (`media.video.nack`), counting from
the oldest: at once for a gap of more than 64 packets, and otherwise when a packet of the gap is
still missing one retry interval after its third request. No picture is lost while the stream waits
for a key frame: from its start, and again from each time it starts again, after 3 decoder errors
(`media.video.decoder-error`), a refused size (`media.video.size-refused`), or a new decoder that
asks for a key frame or a lost GPU (`media.video.decoder-reopen`), until a key frame starts it
(`media.video.first-key`). The pictures before that key frame are discarded with their evidence,
and a key frame it waits for that is not whole and carries loss evidence, or is due with a later
packet arrived, is asked for again, not lost. From the key frame that last started the stream on, a
picture that is not whole is lost when it carries loss evidence; when it is due and a later packet
has arrived; or when no fragment of it came, a later picture's has, and its due time
(`media.video.unseen-due`) has passed. A whole picture is never lost, and a picture merely late,
with nothing later arrived, is not lost. A picture is lost only in its turn, once every picture
before it since that key frame is decoded or lost: until then evidence on it waits, and so does the
request for a key frame its loss makes (`media.video.asks`). A picture that is whole by its turn is
decoded (`media.video.decoded-in-order`).

r[media.video.unseen-due]
A picture of which no fragment came falls due when the next picture of which a fragment came does,
less the frame period times the difference of their frame numbers. The frame period starts at one
over the camera quality's frame rate (`camera.quality.size`). On the first fragment of a picture
whose previous frame number has a fragment, it moves a fifth of the way towards the time between
their timestamps, when that is 10 ms to 200 ms.

r[media.video.clean]
A camera stream is broken from its start until a key frame or a refresh is whole and decodes: from
there, each picture is clean while every picture before it decoded. A lost picture or a decoder
error breaks it again.

r[media.video.first-key]
The receiving machine decodes nothing of a camera stream before a whole key frame newer than its
request to start, or to start again (`media.video.lost`), and discards every picture before it. Once
a fragment of a key frame newer than that request has come, when every such key frame is not whole
and carries loss evidence, or is due with a later packet arrived, it asks for a key frame again, and
counts none of them lost.

r[media.video.decoded-in-order]
From the key frame that last started the stream (`media.video.first-key`) on, every whole picture
is decoded in frame number order; a picture of a broken stream is decoded but never shown. A clean
picture that is not a key frame and whose encoder output is older than the playout delay plus the
lip sync delay plus 150 ms is decoded and not shown.

r[media.video.asks]
The receiving machine asks for a key frame to start at the stream's start, and every 100 ms until
the first fragment of a key frame newer than the newest picture it had seen when it began asking,
and likewise after a size was refused (`media.video.size-refused`). When the stream breaks after it
started, it asks at once for a key frame after a loss (after a stall, when the loss comes within 1 s
of 500 ms or more with no packet), or after a decoder error, and again every 300 ms while the stream
stays broken and no key frame or refresh it got waits to be decoded. A request while another is
still pending asks for nothing more. It asks nothing once the stream has failed. Each request names
the last picture of its clean run.

r[media.video.report]
The receiving machine sends the camera machine a receiver report for each camera stream it takes,
whatever else it sends: at once when a key frame or a refresh starts a new clean run, and otherwise
when one falls due. The first falls due 50 ms after the stream's start, and each next one 50 ms
after the one before fell due, or 50 ms after the one before was sent when that one was sent at once
or more than 50 ms after it fell due. A report carries the newest timestamp seen, the smallest
margin before a picture's due time over the period (none, or 0 when one was late), the highest
packet number seen, the pictures lost and the pictures shown late so far, the first and last frame
numbers of the newest clean run, the kbit/s received over the period, and whether the stream is
broken.

## Camera streams: showing

> r[media.video.offset]
> The receiving machine reads the sending machine's clock from each time map, as the sending
> machine's clock when the time map was sent, plus half QUIC's RTT for the link
> (`link.rtt.reported`), less the time it arrived here. On each time map, it takes, of the last 10
> to arrive, the reading of the one whose arrival time here less its send time on the sending
> machine's clock is least, and every due time uses the reading taken on the time map that arrived
> last.
>
> - parity: Python measured the offset from its own pings and pongs every 1 s, taking the one of the last 10 whose ping came back fastest (#14: no pings of media's own; the time map says when it was sent, and QUIC's RTT for the link takes the place of the pings' time to the peer and back)

r[media.video.playout]
A picture falls due at its encoder output time, read on the receiving machine's clock through the
time map, plus the playout delay and the lip sync delay. The playout delay is the 99th percentile,
over the last 10 s, of how long whole pictures took from encoder output to whole, plus 8 ms, kept
between 50 ms and 150 ms: 70 ms until 20 such samples exist. Key frames and pictures sent behind a
key frame give no sample. A higher playout delay is taken at once, and a lower one approached at 30
ms per second at most.

r[media.video.before-map]
Until the stream's first time map, a picture is shown as soon as it decodes, and falls due, for
telling a loss, 70 ms after its own first fragment arrived.

r[media.video.present]
The virtual camera shows the newest decoded picture that has fallen due, and drops older ones; it
never shows a picture older than the one it shows. A picture decoded after it fell due is shown as
soon as it decodes, and a key frame as soon as it decodes. Of more than 12 decoded pictures waiting,
the oldest is dropped.

r[media.video.decoder-error]
A picture that fails to decode is dropped with every picture after it that waits to be shown, and
the stream breaks. After 3 decoder errors within 10 s, the receiving machine asks for a key frame to
start again.

> r[media.video.decoder-reopen]
> On a PC, after 3 decoder errors within 10 s the decoder is opened again, in hardware unless it was
> opened again in hardware less than 30 s before, and in software then. When the GPU is lost, the
> software decoder takes over at once, and the hardware decoder is tried again after 30 s and takes
> over at the next key frame, with none asked for. A new decoder asks for a key frame once the
> stream has started, unless one waits for it.

r[media.video.low-latency-decoder]
A PC's decoder runs in its low-latency mode, so it keeps no pictures waiting.

r[media.video.size-refused]
A decoded picture of an odd width or height, or outside 64 to 1920 by 64 to 1080 pixels, is not
shown, and the receiving machine asks for a key frame for its size. When the same size is refused 3
times within 10 s, it asks no more, and the stream fails because the picture cannot be shown
(`camera.stream.receiver-ends`).

## The time map and lip sync

> r[media.time-map.carries]
> A time map carries, for at most 32 streams, each stream's id, a timestamp of it and the sending
> machine's clock when that timestamp was captured, and the sending machine's clock when the time
> map was sent. For a camera stream it is the newest picture sent; for an audio stream, the newest
> frame coded, captured when its first sample was.
>
> - parity: Python's time map did not say when it was sent (#14: no pings of media's own; `media.video.offset`)
> - fuzz: media-frame

r[media.time-map.when]
A machine sends each machine it sends a camera stream or a timed audio stream to a time map every 1
s, at once after any of those streams sends its first media packet, and again 20 ms later. It sends
none while it has nothing to put in it.

> r[media.time-map.audio]
> An audio stream is timed, and its time map entries sent, only when both its sending machine and
> its receiving machine have a camera (`camera.state.kinds`).
>
> - parity: Python timed an audio stream only towards a peer whose audio hello carried the word `video1` and when its own media helper had it (#14: capabilities become facts in each machine's own record)

r[media.time-map.no-loss]
A time map uses no packet number of any stream, and its loss is never counted as a stream's loss.

r[media.time-map.last-arrived]
The receiving machine keeps, of each stream, the time map entry that arrived last, even when it is
older than the one it replaces, and reads any timestamp of the stream on the sending machine's clock
from there.

r[media.lip-sync.when]
Lip sync applies only on a machine that takes a camera stream and a microphone stream from the same
machine, the microphone stream into its virtual microphone, both with time maps. A sound stream, a
microphone stream from another machine, or one played on an output gives none.

r[media.lip-sync.delay]
The lip sync delay is how much later the microphone is heard than the picture shows, as an
exponential average with a time constant of 2 s: the first difference measured since the start or a
break, then each new one moving it by 1 − e^(−t / 2 s) of the way, t being the time since the one
before. It is kept between 0 and 150 ms: audio is never delayed, and a picture later than the sound
is only measured. It applies once both streams have run for 2 s without a break, a break starts that
count again, and it moves at most 10 ms per second, also towards 0.

r[media.lip-sync.heard]
How long after its capture the microphone is heard is the time it is played into the virtual
microphone, less its capture time read on the receiving machine's clock, plus the virtual
microphone's path to the app: on a PC, 38 ms when Beamer plays into "CABLE Input" exclusively
(`audio.low-latency.pc`), 67 ms when shared at a 128-sample period, and 110 ms when shared at the
default period. A play time older than 500 ms gives none.

r[media.lip-sync.seen]
How long after its capture a picture shows is the encoder's time over it, plus the playout delay,
plus 2 ms for the virtual camera to take it, while the stream shows a live picture.

r[media.lip-sync.together]
With the microphone heard 0 to 150 ms later than the picture shows, the two machines' clocks 3
hours apart, an offset read 20 ms wrong and clocks drifting by 25 ppm, a picture and the sound
captured with it reach the app within 2 ms of each other, from 30 s after both streams start and
over 5 min.
