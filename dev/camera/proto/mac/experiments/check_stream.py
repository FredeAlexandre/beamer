#!/usr/bin/env python3
"""Check a beamer-camera Annex-B recording with an independent decoder (ffmpeg/libavcodec).

    check_stream.py STREAM.h264 TRACE.jsonl [--drop 90,91]

- Splits the stream into access units with the trace sizes (the file is the frames in trace order).
- Decodes it with ffmpeg (Y plane only) and reads the embedded code strip of every decoded frame
  (sync 0xA5, frame number, host time / 100 us, checksum; block = width / 80, see src/synth.mm).
- With --drop: decodes the stream again without those access units and compares every decoded frame
  with the full decode of the same (embedded) frame number: PSNR-Y per frame shows the damage and
  whether a later LTR refresh / keyframe heals the picture in this decoder.
"""
import json, math, subprocess, sys


def load(stream, trace):
    data = open(stream, "rb").read()
    tr = [json.loads(l) for l in open(trace)]
    aus, off = [], 0
    for r in tr:
        if r.get("dropped"):
            continue
        aus.append((r["f"], data[off:off + r["size"]], r))
        off += r["size"]
    assert off == len(data), f"trace sizes cover {off} of {len(data)} bytes"
    return aus


def decode(blob, w, h):
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "h264", "-i", "-", "-fps_mode", "passthrough",
                        "-f", "rawvideo", "-pix_fmt", "gray", "-"], input=blob, capture_output=True)
    n = w * h
    frames = [p.stdout[i:i + n] for i in range(0, len(p.stdout) - n + 1, n)]
    return frames, p.stderr.decode(errors="replace").strip()


def read_code(y, w):
    b = w // 80
    bits = [1 if y[(b // 2) * w + i * b + b // 2] > 128 else 0 for i in range(80)]
    by = [int("".join(map(str, bits[i * 8:i * 8 + 8])), 2) for i in range(10)]
    x = 0
    for v in by[1:9]:
        x ^= v
    if by[0] != 0xA5 or x != by[9]:
        return None
    return int.from_bytes(bytes(by[1:5]), "big"), int.from_bytes(bytes(by[5:9]), "big")


def psnr(a, b):
    se = sum((x - y) ** 2 for x, y in zip(a[::7], b[::7]))  # every 7th pixel: fast, enough for a damage signal
    mse = se / len(a[::7])
    return 99.0 if mse == 0 else 10 * math.log10(255 * 255 / mse)


def main():
    stream, trace = sys.argv[1], sys.argv[2]
    drop = []
    if "--drop" in sys.argv:
        drop = [int(x) for x in sys.argv[sys.argv.index("--drop") + 1].split(",")]
    aus = load(stream, trace)
    # dimensions from ffprobe
    pr = json.loads(subprocess.run(["ffprobe", "-v", "error", "-f", "h264", "-show_streams", "-of", "json", stream],
                                   capture_output=True, text=True).stdout)["streams"][0]
    w, h = pr["width"], pr["height"]
    full, err = decode(b"".join(a[1] for a in aus), w, h)
    codes = [read_code(y, w) for y in full]
    ok = [c for c in codes if c]
    seq_ok = [c[0] for c in ok] == [a[0] for a in aus]
    out = {"stream": stream, "profile": pr.get("profile"), "level": pr.get("level"), "size": f"{w}x{h}", "access_units": len(aus),
           "decoded": len(full), "codes_read": len(ok), "frame_numbers_match_trace": seq_ok, "ffmpeg_errors": err[:300]}
    if ok:
        # embedded time vs trace cap_ns (both host clock / 100 us): must match exactly
        tr = {a[0]: a[2] for a in aus}
        out["embedded_ts_matches_trace"] = all(c[1] == (tr[c[0]]["cap_ns"] // 100000) % (1 << 32) for c in ok)
    if drop:
        ref = {c[0]: y for c, y in zip(codes, full) if c}
        lossy, err2 = decode(b"".join(a[1] for a in aus if a[0] not in drop), w, h)
        # decoded lossy frames are in order of the kept access units
        kept = [a[0] for a in aus if a[0] not in drop]
        rows = []
        for f, y in zip(kept, lossy):
            rows.append((f, round(psnr(y, ref[f]), 1) if f in ref else None))
        first_drop = min(drop)
        out["lossy_decoded"] = len(lossy)
        out["lossy_psnr_vs_full_around_drop"] = [r for r in rows if first_drop - 2 <= r[0] <= first_drop + 12]
        bad = [r[0] for r in rows if r[1] is not None and r[1] < 60]
        out["lossy_frames_differing"] = f"{len(bad)} frames, {bad[0] if bad else '-'}..{bad[-1] if bad else '-'}"
        out["lossy_ffmpeg_errors"] = err2[:300]
    print(json.dumps(out))


if __name__ == "__main__":
    main()
