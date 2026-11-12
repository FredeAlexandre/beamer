# Camera E2E harness (PHASE2 §10.5)

Two-machine end-to-end runs of the camera, driven from the Mac: the Mac's synthetic source (C1 code strip:
frame number + Mac capture time in every picture) goes through the deployed Beamer on both machines into
"Beamer Camera" on lebig, where an app (`camprobe`, an IMFSourceReader like Chromium/Teams/Camera) reads every
frame in Alex's session. No camera and no camera permission are needed for the unattended cases.

Owner: CW11. Reuses `dev/audio/e2e/beamer.py` (dev command files) and `clock.py` (ping clock link) unchanged,
and C4's analysis method (`proto/e2e/analyze*.py`: per-frame join, freezes, Chrome clock correction), re-checked
on C4's recordings (`run.py replay-c4`).

## Run it (operator, after `dev/deploy.sh`)

```sh
cd ~/Repositories/github.com/FredeAlexandre/beamer
dev/deploy.sh                                  # same commit on both, camera included
e2e() { uv run --python 3.13 --with numpy dev/camera/e2e/run.py "$@"; }   # bash and zsh
e2e preflight                                 # ~10 s; STOPs with the reason if anything is missing
e2e smoke                                     # ~3 min: 3 opens + 60 s latency, proves the harness end to end
e2e all                                       # ~1 h 35, unattended, every synthetic case
e2e soak                                      # ~34 min (or `all --with-soak`)
e2e table                                     # newest result of each case -> runs/SUMMARY.md
git add dev/camera/e2e/runs/*/result.json dev/camera/e2e/runs/SUMMARY.md   # results are committed, raw data not
```

Preconditions the pre-flight checks (it stops instead of failing cases):

- lebig answers over ssh; its clone contains the Mac's last product commit (`core`, `mac_app`, `win_app`,
  `native`, `locales`); both installed apps are newer than that commit; the PC's installed helper is byte-identical
  to its clone's build. **Deploy missing -> "run dev/deploy.sh".**
- Beamer runs on both in dev mode (`audio-dev.enabled` in `~/Library/Application Support/Beamer` and
  `%LOCALAPPDATA%\Beamer`, status file fresh), with the camera part, Mac helper features `video1`,
  `camera_capture`, `camera_synth` (the helper runs with `--dev`), PC `video1`, `virtual_camera`.
- Each side holds the other's `camera_devices` (camera link up); Beamer Camera installed and present on the PC,
  no app holding it. The deploy only copies the DLL beside the helper; installing the camera is the page's
  "Install Beamer Camera…" (UAC, H6) or, over ssh (elevated), the helper verbs of `native/audio/vcam-win/README.md`
  ("Dev install": `vcam-install --owner-sid ... --dll-sha256 ...`).
- **H8**: someone is signed in on lebig (the app reads run in that session). If not: "Alex signs in at the PC".
- The deploy lock is free. The harness takes `~/Developer/.beamer-camera-lab.lock` for the whole invocation.
  Do not run the audio E2E (`dev/audio/e2e`) at the same time: both change routes on the same apps.
- Reported, not blocking: the Mac's camera permission (acceptance wants it still `undetermined`), routes not idle.

`run.py setup` (run automatically before cases) fills `C:\Users\Alex\camera-cw11\` on the PC: `camprobe.exe`
copied from the deployed build (`native\audio\build\win\camprobe.exe`), a Python 3.13 venv (stdlib only),
`pcside_cam.py`, `pcagent.py`, `clock.py`, and a CABLE capture tool (`proto-audio.exe`, copied from the audio
harness folders) that keeps CABLE Output open so a Mac mic streams on demand.

## Cases

Synthetic, unattended (`all` runs them in this order):

| case | what | min | pass marks (C4 value) |
|---|---|---|---|
| `cam_start` | 20 opens, 10 s on / 10 s off, 720p | 7.2 | first Mac picture p50 <= 450, p95 <= 600 ms after the open (C4 260/308); placeholder before, never black; 1 IDR per start; `standby`/`warm` encoder after the first; stop <= 4 s; §6.5 invariant |
| `cam_stop` | 5 opens, 8 s / 12 s | 2.2 | Mac camera stopped <= 4.0 s after close; encoder stop <= 100 ms (proxy, see limits); standby kept |
| `cam_start_slow` | 20 opens, source `start_ms=800` | 7.2 | 1 IDR per start; first picture p50 <= 1250, p95 <= 1400 ms; placeholder before |
| `cam_latency` | 10 min at 720p, 10 min at 1080p, no mic | 21.2 | capture -> app p50 <= 70, p95 <= 90, p99 <= 140 ms (C4 57.5/60.3/65); late <= 20/min; 0 corrupt; 1080p: app reads 1920x1080, decoder 1920x1080, 1 IDR (0 size PLIs), last rows intact (ffmpeg grab) |
| `cam_loss` | (a) `video_drop_pk` 1/5/30/80 x5; (b) `video_loss` 0.5 % bursty 10 min + 1 % iid 10 min; (c) `video_drop_au 1` every 30 s x10 | 30.2 | (a) 1, 5: no PLI, frozen <= 60; 30: p50 <= 100, max <= 200; 80: refresh, p50 <= 400, max <= 500 (C4 0-52 / 63-137 / 300-336); (b) <= 300 / 200 ms frozen per min; (c) a refresh per dropped AU; IDR = 1; 0 corrupt |
| `cam_stall` | `video_drop_ms 1500` every 2 min x5 | 10.4 | clean picture <= 120 ms after each drop (C4 +66/+103); refresh, no IDR; no placeholder |
| `cam_fallback` | (a) Mac mic + camera, UDP blocked 30 s audio-only then 30 s with camera and 1 % video loss; (b) UDP blocked first, then open | 6.0 | (a) video <= 1.1 Mbit/s on the link, PLIs over the link, refresh not IDR, mic glitch-s/min <= audio-only + 1, back on UDP <= 2.5 s; (b) first picture <= 2 s, 1 IDR, no `video_link dropped`, D in [50,150], sane present times, clean switch back |
| `cam_noreceiver` | PC `camera_refuse` not_source / helper / silent, 60 s open each, then off | 4.9 | (a)(b) stopped at once, no re-open; (c) each send <= 3.5 s (`no_receiver`), <= 3 sends, backoff; then streaming within the start budget |
| `cam_killswitch` | Mac sharing off, PC routes the Mac camera and opens; sharing on at 42 s | 1.6 | Mac never starts capture; PC status `sharing_off`; placeholder only; first picture <= 600 ms after the camera starts |
| `cam_helper_kill` | PC helper killed while streaming | 1.6 | held picture, then the DLL's built-in image <= 600 ms; live again <= 5 s after the watchdog restart |
| `cam_oldpeer` | PC Beamer.log since start | 0.5 | asusrog link up, no `camera_*`/`time_map` lines, no warnings (needs asusrog online) |
| `cam_lipsync` | CW10's `cam_lipsync.py setup` + `run` (shared + low-latency, 300 s each); the runner lets go of the lab lock around it | ~12 | CW10's marks (-20..+40 ms) |
| `cam_soak` (`soak`) | 3 min mic only, then 30 min mic + camera at 720p | 33.6 | <= 1 freeze > 500 ms per 10 min outside Wi-Fi stalls; 0 corrupt; p50 per 5 min within ±3 ms; memory < 5 MB growth (Mac helper, VTEncoderXPCService, PC helper); CPU Mac helper < 6 %, PC helper < 5 %, Frame Server < 1.5 %; mic loss not above audio-only; also reports PC decoder stall suspects (CW4 risk) |

Real camera, **skipped by default**, need Alex (H1: Beamer.app granted Camera; `run.py real` checks it):

| case | what | min |
|---|---|---|
| `cam_start_real` | 10 opens with the real camera: first picture estimate (first AU sent + PC target + 10 ms; no code strip on a real picture) p50 <= 1.0 s, p95 <= 1.5 s, 1 IDR per start; also the Mac helper CPU with the real camera (CW3 saw ~9 %) | 4 |
| `cam_latency_real` | Alex opens the PC Camera app with a phone stopwatch in view of the Mac camera and photographs both screens; the harness records stats, CPU, effects for 120 s | 3 |
| `cam_lipsync_real` | CW10's clap with the real camera and mic, with Alex (the script has no unattended variant; `real` just reminds) | CW10 |

Useful options: `--dry-run` (print every timeline, touch nothing), `--cycles`, `--seconds`, `--qualities 720p`,
`--parts a,c`, `--reps`, `--drops`, `--keep-going`. `run.py list` shows the table above from the code;
`run.py analyze runs/<case>_<stamp>` re-runs an analysis on collected data; `run.py idle` puts both apps back.

## How it measures

- **Clocks.** Everything is converted to the Mac's monotonic clock (`mach_absolute_time`: the Mac app's `mono`
  stamps, the Mac helper's `now_ns`, hence the capture time in the code strip, 100 µs units, unwrapped).
  Mac <-> PC monotonic: `clock.py` pings over ssh, lowest-RTT ping per 10 s, linear fit (drift tracked).
  PC monotonic <-> PC wall clock (camprobe's `t`): pairs written every second by `pcside_cam.py`.
- **A session** = Mac actions (dev commands at planned times; PC commands go by scp + ssh, ~1 s, and every
  app reads its command file once a second, so a command takes effect up to ~1-2 s after its planned time:
  analyses use the noted write times and windows, never the planned time) + the PC plan run by `pcside_cam.py`
  (session 0: clock pairs, CPU/memory of Beamer, helper, Frame Server; helper kill; CABLE hold) and `pcagent.py`
  (Alex's session through a one-shot scheduled task: the camprobe reads, `--strip c1 --events --dump`).
- **Collected** into `runs/<case>_<stamp>/<sub>/`: both events files (`audio-dev-events.jsonl`, both rotations,
  cut to the run), camprobe dumps and summaries, status samples, pings, resources, notes; `result.json` per case.
- **Corrupt pictures** = code-strip checks after the first picture: an unreadable strip (not a placeholder
  window), a frame older than one already shown, capture time not increasing with the frame number, any
  `uniform` (black) frame.
- **Camera-on invariant (§6.5)** on every run: each Mac video stream's on-time outside PC demand (vcam
  `in_use` or the harness's reads) is <= 3.5 s (never ran) / 5.5 s (ran).

## Limits (read before trusting a number)

- A picture corrupt below the code strip with an intact strip is not detected (C4 had a PSNR oracle inside its
  receiver; the product has none). The engine's `lost`/`skipped`/`holes` counters are reported beside.
- "helper stop -> encoder stopped <= 100 ms" uses the gap between the helper's `stream_state stopped` and its
  `camera stopped` event (the `stop` command itself is not logged); CW3 measured the real thing (10-14 ms).
- `cam_soak` "holes > 5 packets with their frame type": the product only counts holes; counts are reported.
- Dev commands take effect after the app's 1 s poll, so per-injection windows start at the write and last 5-6 s.
- `cam_start_real` has no code strip: the first-picture time is an estimate from the Mac's first sent AU.
- `cam_oldpeer` checks the PC log only (asusrog's `n_rejected` needs a shell there).

## Files

| file | what |
|---|---|
| `run.py` | entry point (commands above), result files, table |
| `cases.py` | each §10.5 case: timeline (data) + analysis with the marks |
| `analysis.py` | pure analysis: clocks, frames, latency, freezes, integrity, events, invariant, resources, C4 loader |
| `camlib.py` | Mac/PC driving: dev commands, prepare/teardown, timed `Session`, collection, `Run` loader |
| `preflight.py` | the pre-flight above |
| `pcside_cam.py`, `pcagent.py` | the PC half (copied by `setup`) |
| `replay_c4.py` | dry run of the analysis on C4's recordings vs C4's published numbers |
| `test_analysis.py` | unit tests + synthetic run folders through every analysis (`run.py selftest`) |
| `cam_lipsync.py` | CW10's (not in this harness's ownership); `run.py cam_lipsync` and `all` run its `setup` + `run`, `table` reads its `runs/cam_lipsync/<name>/result.json` |
