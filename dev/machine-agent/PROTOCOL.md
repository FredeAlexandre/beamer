# The machine-agent protocol, version 1

How the `machine` tier (`cargo xtask machine`, #104) talks to the machine agents on the Mac
(#47) and on `lebig` (#48). An agent posts real OS input and reads back the OS state a test needs
to see it land. It never talks to Beamer: the harness observes Beamer only through its logs
(ADR 0005).

Both agents speak exactly this. A change a harness would notice raises the version that `hello`
reports.

## Transport

- TCP on **127.0.0.1 only**, port **24870**. An agent never listens on another interface, so
  synthetic input is never offered to the network.
- The harness reaches the PC's agent through an SSH tunnel from the Mac, which the tailnet and
  `lebig`'s key-only SSH already guard:
  `ssh -N -L 24871:127.0.0.1:24870 lebig`, then the PC's agent is `127.0.0.1:24871` on the Mac.
- One JSON object per line, UTF-8, ending in `\n`. Each request gets exactly one reply line, in
  order. Several connections may be open at once; their requests take turns.
- An agent quits after 15 minutes without a request (open but silent connections don't count),
  so none is left behind posting input.

## Requests and replies

```json
{"id": 7, "op": "move", "by": [-40, 0]}
{"id": 7, "ok": true, "mono_ns": 428024183112541, "x": 0.0, "y": 658.0}
```

A request is an object with an `op` and an optional `id`. The `id` may be any JSON value and comes
back as sent (`null` when the line had none, or was not JSON).

Every reply has `id` and `ok`. A reply with `"ok": true` also has `mono_ns` and the op's own
fields. A reply with `"ok": false` has `error`, one of the codes below, and `message`, for a person.

| `error` | When |
|---|---|
| `bad-request` | The line is not a JSON object, the `op` is unknown, or a field is missing or of the wrong kind |
| `unknown-key` | A `key` that is not in the key table below |
| `not-trusted` | The OS does not let the agent post input (on the Mac: no Accessibility grant). Nothing was posted |
| `failed` | The OS refused the event |

A refused request posts nothing.

## Ops

| `op` | Request fields | Reply fields | Posts input |
|---|---|---|---|
| `hello` | | `protocol` (1), `agent` (`"mac"` or `"pc"`), `trusted` (bool), `displays` | no |
| `clock` | | `unix_ns`, the OS wall clock | no |
| `pointer` | | `x`, `y`: where the pointer is | no |
| `move` | `to: [x, y]`, or `by: [dx, dy]` | `x`, `y`: where the move puts the pointer | yes |
| `button` | `button`: `left`, `right`, `middle`; `action`: `down`, `up`, `click` | | yes |
| `key` | `key`: a name from the key table; `action`: `down`, `up`, `press` | | yes |
| `key-state` | `key` | `down` (bool): whether the OS sees the key held | no |
| `scroll` | `dx`, `dy`: whole lines, either may be left out, not both 0 | | yes |
| `quit` | | | no; the agent stops after replying |

- **`hello`**: `trusted: false` means every op that posts input will fail with `not-trusted`. A
  harness checks it first. `displays` is a list of `{x, y, width, height, main}`, in the
  coordinates below; `main` marks the display whose top-left corner is the origin.
- **`move`** with `to` moves the pointer there. With `by`, it moves from where the pointer is. If
  no display holds the target, the pointer is held at the edge of the display it is on, and the
  event still carries the whole `[dx, dy]` as its motion, as a mouse pushed against an edge does.
  This is how a test makes push pressure. While a button is held, a move is a drag.
- **`click`** and **`press`** are a `down` then an `up`, posted back to back.
- **Modifiers** are keys: a `down` of `ShiftLeft` holds it, and every event that follows carries
  it until its `up`. Keys and buttons stay held until released; an agent never releases them
  by itself.
- **`scroll`**: a positive `dy` is the wheel turned away from the user, a positive `dx` is the
  wheel tilted right, before any natural-scrolling setting.

### Reads lag posts

An input reply means the event was posted, not that the OS has applied it. On the Mac the pointer
and key state show a posted event 0.1 to 20 ms later (measured 2026-11-16). A test that reads back
what it posted polls until the read shows it.

## Coordinates

The OS's own global pointer coordinates, as numbers (fractions allowed):

- **Mac**: Quartz global display coordinates, in points. The origin is the top-left corner of the
  main display, `y` grows downwards, and other displays may sit at negative coordinates.
- **PC**: virtual-screen coordinates, in physical pixels (the agent is per-monitor DPI aware). The
  origin is the top-left corner of the primary display, `y` grows downwards.

## Time

`mono_ns` is the OS monotonic clock, in nanoseconds since an OS-chosen point:

- **Mac**: `CLOCK_UPTIME_RAW` (`mach_absolute_time`, the clock Rust's `Instant` reads), which
  stops while the Mac sleeps.
- **PC**: `QueryPerformanceCounter` (the clock Rust's `Instant` reads).

For an op that posts input, `mono_ns` is read just before the first event is posted. For any
other op, it is read while the reply is made.

The two machines' clocks are never compared directly: the PC's drifts against the Mac's by
hundreds of milliseconds. The harness ties them together with out-and-back pairs: it notes its
own clock before sending `clock` and after the reply, and takes the agent's `mono_ns` as falling
in between.

## Keys

Keys are named by physical position, with the W3C UI Events
[`code`](https://www.w3.org/TR/uievents-code/) names, so a request presses the same key on the
Mac's layout and on the PC's French AZERTY one. `KeyQ` is the key right of Tab, which types `a` on
AZERTY.

| Group | Names |
|---|---|
| Letters | `KeyA` … `KeyZ` |
| Digits | `Digit0` … `Digit9` |
| Function | `F1` … `F20` |
| Punctuation | `Minus`, `Equal`, `BracketLeft`, `BracketRight`, `Backslash`, `Semicolon`, `Quote`, `Backquote`, `Comma`, `Period`, `Slash`, `IntlBackslash` |
| Editing | `Enter`, `Escape`, `Backspace`, `Delete`, `Tab`, `Space`, `CapsLock` |
| Modifiers | `ShiftLeft`, `ShiftRight`, `ControlLeft`, `ControlRight`, `AltLeft`, `AltRight`, `MetaLeft`, `MetaRight` |
| Navigation | `ArrowLeft`, `ArrowRight`, `ArrowUp`, `ArrowDown`, `Home`, `End`, `PageUp`, `PageDown` |

`AltLeft` is ⌥ Option on the Mac and `MetaLeft` is ⌘ Command; on the PC they are Alt and the
Windows key. [`src/keys.rs`](src/keys.rs) maps each name to the native code on each OS.
