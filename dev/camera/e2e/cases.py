# SPDX-License-Identifier: GPL-3.0-only
"""The §10.5 cases: each one is a list of sessions (timeline of dev commands + the PC app's camera reads) and
an analysis that turns the collected run into pass/fail checks with the §10.5 marks (C4's value beside).

A session spec is data, so `run.py --dry-run` prints the exact timeline without touching either machine:
  {"sub": name, "desc", "prepare": {...camlib.prepare kwargs...} | None, "duration": s,
   "actions": [(at, label, ("mac"|"pc", command))], "items": [pcagent items], "kills": [{"at"}],
   "cable": bool, "resources_every": s, "pc_status_every": s}
Ids are filled from the status files (`ids.mac`, `ids.pc`, `ids.mac_mic`); placeholders in a dry run.
"""
from __future__ import annotations

from collections import Counter

import analysis as A
from analysis import check

SIZE = {"720p": "1280x720", "1080p": "1920x1080"}
MIC_A = {"test_source": {"kind": "mic", "signal": "A:0.2"}}


class Ids:
    def __init__(self, mac="<mac-id>", pc="<pc-id>", mac_mic=None):
        self.mac, self.pc = mac, pc
        self.mac_mic = mac_mic or {"key": "<BuiltInMicrophoneDevice>", "name": "MacBook Air Microphone"}


def read(at, seconds, tag, size="1280x720"):
    return {"kind": "read", "at": at, "seconds": seconds, "size": size, "tag": tag}


def mac(cmd):
    return ("mac", cmd)


def pcc(cmd):
    return ("pc", cmd)


def helper_dev(**kw):
    return {"helper_dev": kw}


# ================================================================ session builders

def s_cycles(sub, n, on, off, test_source="synth", quality="720p", lead=5.0):
    items = [read(lead + k * (on + off), on, f"c{k:02d}", SIZE[quality]) for k in range(n)]
    return {"sub": sub, "desc": f"{n} opens of Beamer Camera ({on} s on, {off} s off), {quality}, source {test_source or 'camera'}",
            "prepare": {"quality": quality, "test_source": test_source}, "duration": lead + n * (on + off) + 3,
            "actions": [], "items": items, "kills": [], "cable": False, "resources_every": 0, "pc_status_every": 0}


def s_read(sub, desc, seconds, quality="720p", actions=(), lead=5.0, extra_items=(), test_source="synth",
           cable=False, resources_every=0, pc_status_every=0, prepare=None, kills=()):
    items = [read(lead, seconds, "r", SIZE[quality])] + list(extra_items)
    end = max([lead + seconds] + [i["at"] + i.get("seconds", 0) for i in extra_items]) + 5
    return {"sub": sub, "desc": desc, "prepare": prepare if prepare is not None else {"quality": quality, "test_source": test_source},
            "duration": end, "actions": list(actions), "items": items, "kills": list(kills), "cable": cable,
            "resources_every": resources_every, "pc_status_every": pc_status_every}


# ================================================================ analyses (shared)

def send_streams_in(run, t0, t1):
    ss = A.video_send_streams(run.mac_events)
    return {sid: s for sid, s in ss.items() if s.get("on") is not None and t0 <= s["on"] <= t1}


def invariant_checks(run, reads):
    """§6.5 on every run: Mac camera never on without a PC app holding Beamer Camera (+ allowed holds)."""
    demand = A.vcam_in_use_intervals(run.pc_events, run.t_end)
    for r in reads:
        if r and r["t_open"] and r["t_closed"]:
            demand.append((r["t_open"], r["t_closed"]))
    bad = A.camera_on_invariant(A.video_send_streams(run.mac_events), demand, run.t_end)
    return [check("camera-on invariant (§6.5): Mac camera never on without a receiver", bad or "no violation",
                  not bad, "on <= 3.5 s (never ran) / 5.5 s (ran) outside PC demand")]


def mac_send(run):
    return A.video_stats(run.mac_events, "send")


def pc_recv(run):
    return A.video_stats(run.pc_events, "recv")


def idr_per_stream(run, streams):
    last = A.per_stream_last(mac_send(run))
    return {sid: (last.get(sid) or {}).get("idr") for sid in streams}


def analyze_cycles(run, spec, p50_mark, p95_mark, c4, warm_after_first=True, real=False):
    rows, reads = [], []
    on = spec["items"][0]["seconds"] if spec["items"] else 10
    streams_all = A.video_send_streams(run.mac_events)
    last_send = A.per_stream_last(mac_send(run))
    recv = pc_recv(run)
    for k, item in enumerate(spec["items"]):
        r = run.read(item["tag"])
        reads.append(r)
        if not r or r["t_open"] is None:
            rows.append({"cycle": k, "error": "no read data"})
            continue
        s = A.start_of_read(r["frames"], r["t_open"])
        mine = {sid: st for sid, st in streams_all.items()
                if st.get("on") is not None and r["t_open"] - 1.0 <= st["on"] <= r["t_open"] + on}
        summ = r["summary"] or {}
        # t_open = camprobe's t_begin (before MFEnumDeviceSources), like C4's getUserMedia call; enum/activate beside
        row = dict(cycle=k, first_ms=s["first_ms"], first_frame=s["first_frame"], black_before=s["black_before"],
                   placeholder_before=s["placeholder_before"], streams=len(mine),
                   enum_ms=summ.get("enum_ms"), activate_ms=summ.get("activate_ms"))
        if mine:
            sid, st = sorted(mine.items(), key=lambda x: x[1]["on"])[0]
            row.update(stream=sid, encoder=st.get("encoder"), encoder_ms=st.get("encoder_ms"),
                       idr=(last_send.get(sid) or {}).get("idr"))
            if real and st.get("running") is not None:
                # real camera: no code strip; first picture ~ first AU sent + the PC's playout target + ~10 ms
                d = [x[2].get("target_ms") for x in recv if x[1] == sid and x[2].get("target_ms")]
                row["first_ms_est"] = round((st["running"] - r["t_open"]) * 1e3 + (d[0] if d else 50) + 10, 1)
            if r["t_closed"] and st.get("camera_stopped"):
                row["stop_s"] = round(st["camera_stopped"] - r["t_closed"], 3)
            if st.get("end") and st.get("camera_stopped"):
                row["enc_stop_proxy_ms"] = round(abs(st["camera_stopped"] - st["end"]) * 1e3, 1)
        if s["first_ms"] is not None:
            integ = A.integrity(A.Frames([x for x in r["frames"].rows if x[0] >= r["t_open"]], ""))
            row["corrupt"] = A.corrupt_count(integ)
        rows.append(row)
    key = "first_ms_est" if real else "first_ms"
    firsts = [x.get(key) for x in rows if x.get(key) is not None]
    st = A.pct(firsts, (50, 95))
    n = len(spec["items"])
    checks = [
        check("every open got the Mac picture", f"{len(firsts)}/{n}", len(firsts) == n, f"{n}/{n}"),
        check(f"first picture p50 (ms){' (estimate)' if real else ''}", st.get("p50"), st.get("p50") is not None and st["p50"] <= p50_mark,
              f"<= {p50_mark}", c4),
        check(f"first picture p95 (ms){' (estimate)' if real else ''}", st.get("p95"), st.get("p95") is not None and st["p95"] <= p95_mark,
              f"<= {p95_mark}"),
        check("exactly 1 IDR per start (Mac stats)", [x.get("idr") for x in rows],
              all(x.get("idr") == 1 for x in rows if "stream" in x) and all("stream" in x for x in rows), "1 each"),
        check("one Mac stream per open", [x.get("streams") for x in rows], all(x.get("streams") == 1 for x in rows), "1 each"),
    ]
    if not real:
        checks += [
            check("placeholder before the first picture, never black", [(x.get("placeholder_before"), x.get("black_before")) for x in rows],
                  all((x.get("placeholder_before") or 0) > 0 and x.get("black_before") == 0 for x in rows), "placeholder > 0, black 0"),
            check("0 corrupt pictures (code strip) while on", sum(x.get("corrupt") or 0 for x in rows),
                  all((x.get("corrupt") or 0) == 0 for x in rows), "0"),
        ]
    if warm_after_first:
        enc = [x.get("encoder") for x in rows[1:]]
        checks.append(check("encoder standby/warm in every start after the first", Counter(enc),
                            all(e in ("standby", "warm") for e in enc), "standby or warm"))
    checks += stop_checks(run, rows)
    checks += invariant_checks(run, reads)
    info = {"first_ms": st, "rows": rows}
    return checks, info


def stop_checks(run, rows):
    stops = [x["stop_s"] for x in rows if "stop_s" in x]
    enc = [x["enc_stop_proxy_ms"] for x in rows if "enc_stop_proxy_ms" in x]
    warm = [x for x in run.mac_status if x.get("camera", {}).get("warm")]
    return [
        check("Mac camera stopped after the app closed (s, max)", max(stops) if stops else None,
              bool(stops) and max(stops) <= 4.0 and len(stops) == len(rows), "<= 4.0 every close"),
        check("helper stop -> encoder stopped (ms, max; proxy: stream end vs camera stopped)", max(enc) if enc else None,
              (max(enc) <= 100) if enc else None, "<= 100", "C4: max 51 ms; CW3: 10-14 ms"),
        check("standby encoder planned after the stops", f"{len(warm)}/{len(run.mac_status)} status samples",
              bool(warm), "warm in the Mac plan"),
    ]


# ================================================================ cases

def cam_start(ids, o):
    n = o.get("cycles") or 20
    return [s_cycles("start", n, 10, 10)]


def an_cam_start(runs, specs, o):
    return analyze_cycles(runs[0], specs[0], 450, 600, "C4: 260 / 308 ms at 4x")


def cam_start_slow(ids, o):
    n = o.get("cycles") or 20
    return [s_cycles("start_slow", n, 10, 10, test_source="synth;start_ms=800")]


def an_cam_start_slow(runs, specs, o):
    return analyze_cycles(runs[0], specs[0], 1250, 1400, "R2 #1: 800 ms camera start")


def cam_stop(ids, o):
    return [s_cycles("stop", o.get("cycles") or 5, 8, 12)]


def an_cam_stop(runs, specs, o):
    checks, info = analyze_cycles(runs[0], specs[0], 450, 600, "C4: 260 / 308 ms")
    keep = [c for c in checks if c["name"].startswith(("Mac camera stopped", "helper stop", "standby", "camera-on",
                                                       "encoder standby"))]
    return keep, info


def cam_start_real(ids, o):
    n = o.get("cycles") or 10
    return [s_cycles("start_real", n, 10, 10, test_source=None)]


def an_cam_start_real(runs, specs, o):
    checks, info = analyze_cycles(runs[0], specs[0], 1000, 1500, "assumed cold start 0.2-0.8 s (risk 1)", real=True)
    cpu = A.helper_cpu(runs[0].mac_events)
    info["mac_helper_cpu_pct"] = A.pct([c for _, c in cpu], (50, 95))
    return checks, info


def cam_latency(ids, o):
    secs = o.get("seconds") or 600
    out = []
    for q in o.get("qualities") or ("720p", "1080p"):
        extra = []
        if q == "1080p" and not o.get("no_lastrows"):
            # within the 3 s demand hold, so the same live stream is grabbed (the last frames are checked)
            extra = [{"kind": "ffmpeg", "at": 5 + secs + 1, "seconds": 3, "size": "1920x1080", "tag": "lastrows"}]
        out.append(s_read(f"latency_{q}", f"{secs} s continuous read at {q}, no mic stream (lip-sync hold 0), floor 50",
                          secs, quality=q, extra_items=extra, resources_every=30))
    return out


def lastrows_check(run):
    """1080p: the last rows of the picture and its colours (UV plane offset) look like the rows above them
    (a coded-size bug shows a green/black/garbage band at the bottom). Needs ffmpeg on the PC; else None."""
    p = run.dir / "pc" / "lastrows.nv12"
    if not p.exists() or p.stat().st_size < 1920 * 1080 * 3 // 2:
        return None, "no ffmpeg grab (ffmpeg missing on the PC, or DirectShow name differs)"
    import numpy as np
    w, h = 1920, 1080
    fs = w * h * 3 // 2
    data = np.frombuffer(p.read_bytes()[: fs * (p.stat().st_size // fs)], np.uint8).reshape(-1, fs)
    worst = 0.0
    for f in data[-30:]:
        y = f[: w * h].reshape(h, w).astype(float)
        uv = f[w * h:].reshape(h // 2, w).astype(float)
        body = np.abs(np.diff(y[h - 40: h - 8], axis=0)).mean()
        edge = np.abs(y[h - 8:] - y[h - 9]).mean()
        uv_edge = np.abs(uv[-4:] - uv[-5]).mean()
        uv_body = np.abs(np.diff(uv[-24:-4], axis=0)).mean()
        worst = max(worst, edge / max(body, 1.0), uv_edge / max(uv_body, 1.0))
    return bool(worst < 4.0), f"bottom rows vs rows above: worst ratio {worst:.2f} over the last {min(len(data), 30)} frames"


def an_cam_latency(runs, specs, o):
    checks, info = [], {}
    for run, spec in zip(runs, specs):
        q = spec["sub"].split("_")[1]
        r = run.read("r")
        if not r:
            checks.append(check(f"{q}: read data", None, False, "camprobe dump"))
            continue
        lat = A.latency_ms(r["frames"], skip_s=3.0)
        st = A.pct([v for _, v in lat])
        integ = A.integrity(r["frames"])
        recv = pc_recv(run)
        t0, t1 = r["t_open"], r["t_closed"] or run.t_end
        minutes = max(1e-9, (t1 - t0) / 60)
        late = A.counter_delta(recv, "late", t0, t1)
        sync = [x[2].get("sync_ms") for x in recv if x[2].get("sync_ms") is not None]
        streams = send_streams_in(run, t0 - 1, t1)
        idrs = idr_per_stream(run, streams)
        c4 = {"720p": "C4: 57.5 / 60.3 / 65", "1080p": "C4: 56.8 / 63.3 / 107"}[q]
        checks += [
            check(f"{q}: capture -> app p50 (ms)", st.get("p50"), st.get("p50") is not None and st["p50"] <= 70, "<= 70", c4),
            check(f"{q}: capture -> app p95 (ms)", st.get("p95"), st.get("p95") is not None and st["p95"] <= 90, "<= 90"),
            check(f"{q}: capture -> app p99 (ms)", st.get("p99"), st.get("p99") is not None and st["p99"] <= 140, "<= 140"),
            check(f"{q}: late frames per minute (PC stats)", round(late / minutes, 2), late / minutes <= 20, "<= 20", "C4: 7.6-11"),
            check(f"{q}: 0 corrupt pictures (code strip)", integ, A.corrupt_count(integ) == 0 and integ["black"] == 0, "0"),
            check(f"{q}: no lip-sync hold (no mic stream)", max(sync) if sync else None, (max(sync) == 0) if sync else None, "sync_ms 0"),
            check(f"{q}: one stream, 1 IDR", idrs, len(idrs) == 1 and all(v == 1 for v in idrs.values()), "1"),
        ]
        if q == "1080p":
            typ = (r["summary"] or {}).get("type", "")
            wh = {(x[2].get("w"), x[2].get("h")) for x in recv}
            failed = [e for e in run.pc_events if e.get("ev") == "stream_state" and e.get("error") == "decoder"]
            pli = A.counter_delta(recv, "pli_tx", t0, t1)
            ok_rows, note = lastrows_check(run)
            checks += [
                check("1080p: frames read as 1920x1080 (app media type)", typ, "1920x1080" in typ, "NV12 1920x1080"),
                check("1080p: PC decoder output 1920x1080 (stats w/h)", sorted(map(str, wh)), wh == {(1920, 1080)}, "1920x1080"),
                check("1080p: 0 size PLIs (1 IDR, no decoder failure)", {"pli_tx": pli, "decoder_failed": len(failed)},
                      not failed and all(v == 1 for v in idrs.values()), "0 (IDR stays 1)"),
                check("1080p: picture's last rows intact (ffmpeg grab)", note, ok_rows, "no band at the bottom"),
            ]
        checks += invariant_checks(run, [r])
        info[q] = {"latency_ms": st, "integrity": integ, "late_total": late, "minutes": round(minutes, 2),
                   "frozen_episodes_100ms": len(A.frozen_episodes(r["frames"])),
                   "pc_d_ms": A.pct([x[2].get("target_ms") for x in recv], (50, 99)),
                   "dec_ms": A.pct([x[2].get("dec_ms") for x in recv], (50, 99)),
                   "decoders": dict(Counter(x[2].get("decoder") for x in recv))}
    return checks, info


DROP_PK = (1, 5, 30, 80)


def cam_loss(ids, o):
    subs = o.get("parts") or ("a", "b", "c")
    out = []
    if "a" in subs:
        reps = o.get("reps") or 5
        acts, t = [], 10.0
        for k in range(reps):
            for n in DROP_PK:
                acts.append((t, f"drop_pk {n} #{k}", mac(helper_dev(video_drop_pk=n))))
                t += 8.0
        out.append(s_read("loss_a", f"video_drop_pk {DROP_PK} x{reps}, 8 s apart, 720p", t + 5, actions=acts))
    if "b" in subs:
        secs = o.get("seconds") or 600
        acts = [(10.0, "loss 0.5% bursty", mac(helper_dev(video_loss={"pct": 0.5, "burst": 6}))),
                (10.0 + secs, "loss 1% iid", mac(helper_dev(video_loss={"pct": 1.0, "burst": 0}))),
                (10.0 + 2 * secs, "loss off", mac(helper_dev(video_loss={"pct": 0, "burst": 0})))]
        out.append(s_read("loss_b", f"video_loss 0.5 % bursty (mean 6) {secs} s, then 1 % iid {secs} s", 2 * secs + 15, actions=acts))
    if "c" in subs:
        n = o.get("drops") or 10
        acts = [(15.0 + 30 * k, f"drop_au #{k}", mac(helper_dev(video_drop_au=1))) for k in range(n)]
        out.append(s_read("loss_c", f"video_drop_au 1 every 30 s x{n}", 15 + 30 * n + 5, actions=acts))
    return out


def an_cam_loss(runs, specs, o):
    checks, info = [], {}
    for run, spec in zip(runs, specs):
        r = run.read("r")
        if not r:
            checks.append(check(f"{spec['sub']}: read data", None, False, "camprobe dump"))
            continue
        send = mac_send(run)
        t0, t1 = r["t_open"], r["t_closed"] or run.t_end
        integ = A.integrity(r["frames"])
        streams = send_streams_in(run, t0 - 1, t1)
        idrs = idr_per_stream(run, streams)
        common = [check(f"{spec['sub']}: IDR count = 1 (the start)", idrs, len(idrs) == 1 and all(v == 1 for v in idrs.values()), "1"),
                  check(f"{spec['sub']}: 0 corrupt pictures (code strip)", integ, A.corrupt_count(integ) == 0, "0")]
        if spec["sub"] == "loss_a":
            by = {}
            for n_ in run.actions("drop_pk"):
                n = int(n_["label"].split()[1])
                a, b = n_["t"], n_["t"] + 6.0
                worst, total = A.frozen_in(r["frames"], a, b)
                by.setdefault(n, []).append({"frozen_ms": total, "worst_gap_frozen_ms": worst,
                                             "pli": A.counter_delta(send, "pli_rx", a, b),
                                             "refresh": A.counter_delta(send, "refresh", a, b),
                                             "nack": A.counter_delta(send, "nack_rx", a, b)})
            for n, rows in sorted(by.items()):
                fz = A.pct([x["frozen_ms"] for x in rows], (50,))
                pli = sum(x["pli"] for x in rows)
                if n in (1, 5):
                    checks.append(check(f"drop {n} pk: NACK repair, no PLI, frozen max (ms)", {"frozen": fz, "pli": pli},
                                        fz.get("max", 1e9) <= 60 and pli == 0, "<= 60, 0 PLI", "C4: 0 p50, 52 max"))
                elif n == 30:
                    checks.append(check("drop 30 pk: frozen p50 / max (ms)", fz, fz.get("p50", 1e9) <= 100 and fz.get("max", 1e9) <= 200,
                                        "<= 100 / <= 200", "C4: 63 / 137"))
                elif n == 80:
                    ref = sum(x["refresh"] for x in rows)
                    checks.append(check("drop 80 pk: PLI -> refresh, frozen p50 / max (ms)", {"frozen": fz, "pli": pli, "refresh": ref},
                                        fz.get("p50", 1e9) <= 400 and fz.get("max", 1e9) <= 500 and pli >= len(rows) and ref >= len(rows),
                                        "<= 400 / <= 500, a refresh each", "C4: 300 / 336"))
            info["loss_a"] = by
        elif spec["sub"] == "loss_b":
            acts = run.actions("loss")
            ph = [(acts[0]["t"], acts[1]["t"], "0.5% bursty", 300), (acts[1]["t"], acts[2]["t"] if len(acts) > 2 else t1, "1% iid", 200)]
            recv = pc_recv(run)
            res = {}
            for a, b, label, mark in ph:
                eps = A.frozen_episodes(r["frames"], 50.0, a + 1.5, b)
                minutes = max(1e-9, (b - a - 1.5) / 60)
                fz = sum(x[2] for x in eps) / minutes
                pcfz = sum(x[2].get("frozen_ms", 0) for x in recv if a + 1.5 <= x[0] <= b) / minutes
                res[label] = {"frozen_ms_per_min": round(fz, 1), "pc_stats_frozen_ms_per_min": round(pcfz, 1),
                              "refresh": A.counter_delta(send, "refresh", a, b), "pli": A.counter_delta(send, "pli_rx", a, b)}
                checks.append(check(f"loss {label}: frozen ms per minute", res[label], fz <= mark, f"<= {mark}",
                                    "CW2: 82.5 ms/min at 0.5 % bursty (null codec)"))
            info["loss_b"] = res
        elif spec["sub"] == "loss_c":
            last = A.per_stream_last(send)
            dropped = sum((v.get("dev_drop_au") or 0) for v in last.values())
            ref = A.counter_delta(send, "refresh", t0, t1)
            per = [A.frozen_in(r["frames"], n_["t"], n_["t"] + 5) for n_ in run.actions("drop_au")]
            checks.append(check("drop_au: each dropped AU recovered by a refresh", {"dropped": dropped, "refresh": ref},
                                dropped >= len(run.actions("drop_au")) and ref >= dropped, "refresh >= dropped AUs"))
            info["loss_c"] = {"dropped": dropped, "refresh": ref, "frozen_per_drop": per}
        checks += common + invariant_checks(run, [r])
    return checks, info


def cam_stall(ids, o):
    n = o.get("drops") or 5
    acts = [(60.0 + 120 * k, f"drop_ms 1500 #{k}", mac(helper_dev(video_drop_ms=1500))) for k in range(n)]
    return [s_read("stall", f"video_drop_ms 1500 every 2 min x{n}, 720p", 60 + 120 * (n - 1) + 50, actions=acts)]


def an_cam_stall(runs, specs, o):
    run = runs[0]
    r = run.read("r")
    if not r:
        return [check("read data", None, False, "camprobe dump")], {}
    send, recv = mac_send(run), pc_recv(run)
    rows = []
    for n_ in run.actions("drop_ms"):
        a, b = n_["t"], n_["t"] + 6.0
        g = [x for x in A.gaps(r["frames"]) if a <= x[0] + x[1] / 1e3 <= b]
        worst = max(g, key=lambda x: x[1]) if g else None
        rows.append({"gap_ms": round(worst[1], 1) if worst else None,
                     "after_drop_ms": round(worst[1] - A.FRAME_MS - 1500, 1) if worst else None,
                     "refresh": A.counter_delta(send, "refresh", a, b), "idr": A.counter_delta(send, "idr", a, b)})
    after = [x["after_drop_ms"] for x in rows if x["after_drop_ms"] is not None]
    integ = A.integrity(r["frames"])
    shown = Counter(x[2].get("shown") for x in recv if r["t_open"] + 2 <= x[0] <= (r["t_closed"] or run.t_end))
    streams = send_streams_in(run, r["t_open"] - 1, r["t_closed"] or run.t_end)
    idrs = idr_per_stream(run, streams)
    checks = [
        check("clean picture after each 1.5 s drop (ms after the drop, max)", after,
              len(after) == len(rows) and bool(after) and max(after) <= 120, "<= 120", "C4: drop + 66 p50, + 103 max"),
        check("recovery by refresh, no IDR", [(x["refresh"], x["idr"]) for x in rows],
              all(x["refresh"] >= 1 and x["idr"] == 0 for x in rows) and all(v == 1 for v in idrs.values()), "refresh, IDR stays 1"),
        check("no placeholder at all (no `reconnecting`, < 2 s)", {"non_live_frames": integ.get("non_live"), "pc_shown": dict(shown)},
              integ.get("non_live", 0) == 0 and set(shown) <= {"live"}, "0 placeholder frames"),
        check("0 corrupt pictures (code strip)", integ, A.corrupt_count(integ) == 0, "0"),
    ] + invariant_checks(run, [r])
    return checks, {"drops": rows}


def cam_fallback(ids, o):
    out = []
    parts = o.get("parts") or ("a", "b")
    mic_on = [MIC_A, {"set_mic": {"machine": ids.mac, "device": ids.mac_mic}}]
    if "a" in parts:
        acts = [(1.0, "mic on (test A)", mac(mic_on)),
                (20.0, "udp off (audio only)", pcc(helper_dev(drop_udp=True))),
                (50.0, "udp on (audio only)", pcc(helper_dev(drop_udp=False))),
                (75.0, "video loss 1%", mac(helper_dev(video_loss={"pct": 1.0, "burst": 0}))),
                (95.0, "udp off (camera)", pcc(helper_dev(drop_udp=True))),
                (125.0, "udp on (camera)", pcc(helper_dev(drop_udp=False))),
                (130.0, "video loss off", mac(helper_dev(video_loss={"pct": 0, "burst": 0}))),
                (216.0, "mic off", mac([{"set_mic": {"machine": None, "device": None}},
                                        {"test_source": {"kind": "mic", "signal": None}}]))]
        out.append(s_read("fallback_a", "mic (test A) to the PC; UDP blocked 30 s audio-only, then 30 s with the camera "
                          "and 1 % video loss on the link", 150, lead=65.0, actions=acts, cable=True, resources_every=0))
    if "b" in parts:
        acts = [(1.0, "udp off", pcc(helper_dev(drop_udp=True))),
                (55.0, "udp on", pcc(helper_dev(drop_udp=False)))]
        out.append(s_read("fallback_b", "UDP blocked first, then Beamer Camera opened (stream starts in link state, "
                          "provisional offset), UDP back at 55 s", 70, lead=15.0, actions=acts))
    return out


def glitch_rate(stats_rows, a, b):
    rows = [x for x in stats_rows if a <= x[0] <= b]
    bad = sum(1 for _, _, s in rows if (s.get("plc") or 0) > 0 or (s.get("underrun") or 0) > 0 or (s.get("dec_err") or 0) > 0)
    minutes = len(rows) / 60.0
    return (round(bad / minutes, 2) if minutes else None), len(rows)


def an_cam_fallback(runs, specs, o):
    checks, info = [], {}
    for run, spec in zip(runs, specs):
        r = run.read("r")
        if not r:
            checks.append(check(f"{spec['sub']}: read data", None, False, "camprobe dump"))
            continue
        send, recv = mac_send(run), pc_recv(run)
        notes = {n["label"]: n for n in run.notes if n.get("event") == "action"}
        if spec["sub"] == "fallback_a":
            a_off, a_on = notes["udp off (audio only)"], notes["udp on (audio only)"]
            c_off, c_on = notes["udp off (camera)"], notes["udp on (camera)"]
            w_audio = (a_off.get("t_done", a_off["t"]) + 3.0, a_on["t"])
            w_cam = (c_off.get("t_done", c_off["t"]) + 3.0, c_on["t"])
            link = [x for x in send if w_cam[0] <= x[0] <= w_cam[1]]
            on_link = [x for x in link if x[2].get("path") == "link"]
            kbps = max([x[2].get("kbps", 0) for x in on_link], default=None)
            pli = A.counter_delta(send, "pli_rx", *w_cam)
            ref = A.counter_delta(send, "refresh", *w_cam)
            idr = A.counter_delta(send, "idr", r["t_open"] + 3, run.t_end)
            pc_audio = A.audio_stats(run.pc_events, "recv")
            g_audio, n_a = glitch_rate(pc_audio, *w_audio)
            g_cam, n_c = glitch_rate(pc_audio, *w_cam)
            t_unblock = c_on.get("t_done", c_on["t"])
            # the flow's own state change (helper `flow_state` up) when there is one, else the first video stats on UDP
            up = next((e["t"] for e in run.mac_events if e.get("ev") == "flow_state" and e.get("state") == "up"
                       and e["t"] > t_unblock), None)
            back = up or next((x[0] for x in send if x[0] > t_unblock and x[2].get("path") == "udp"), None)
            resume = round(back - t_unblock, 2) if back else None
            checks += [
                check("fallback (a): video on the link (kbit/s, max)", kbps, kbps is not None and len(on_link) > 0 and kbps <= 1100,
                      "<= 1100 (1 Mbit/s target)", f"{len(on_link)}/{len(link)} Mac stats on the link"),
                check("fallback (a): PLIs reach the Mac over the link, recovery by refresh", {"pli": pli, "refresh": ref, "idr_after_start": idr},
                      pli > 0 and ref > 0 and idr == 0, "PLI > 0, refresh > 0, no IDR after the start"),
                check("fallback (a): mic glitch s/min with camera vs audio-only fallback", {"camera": g_cam, "audio_only": g_audio, "n": [n_c, n_a]},
                      None if g_cam is None or g_audio is None else g_cam <= g_audio + 1, "<= audio-only + 1"),
                check("fallback (a): video back on UDP after the unblock (s, from the PC command)", resume,
                      None if resume is None else resume <= 2.5 + 1.0, "<= 2.5 (+1 s dev poll)"),
            ]
            info["fallback_a"] = {"windows": {"audio_only": w_audio, "camera": w_cam}, "kbps_link": kbps, "pli": pli,
                                  "refresh": ref, "resume_s": resume, "glitch_per_min": [g_cam, g_audio]}
        else:
            s = A.start_of_read(r["frames"], r["t_open"])
            on = notes["udp on"]
            t_link_end = on.get("t_done", on["t"])
            lat = [(t, v) for t, v in A.latency_ms(r["frames"], skip_s=0.0) if t < t_link_end]
            vals = [v for _, v in lat]
            streams = send_streams_in(run, r["t_open"] - 1, r["t_closed"] or run.t_end)
            idrs = idr_per_stream(run, streams)
            drops = [x.get("camera", {}).get("link_out_dropped") for x in run.mac_status]
            drops = [d for d in drops if d is not None]
            d_link = [x[2].get("target_ms") for x in recv if x[0] < t_link_end and x[2].get("target_ms") is not None]
            sw = A.frozen_in(r["frames"], t_link_end, t_link_end + 6)
            path_after = [x[2].get("path") for x in recv if x[0] > t_link_end + 4]
            checks += [
                check("fallback (b): first picture after the open, stream starting in link state (ms)", s["first_ms"],
                      s["first_ms"] is not None and s["first_ms"] <= 2000, "<= 2000"),
                check("fallback (b): 1 IDR", idrs, len(idrs) == 1 and all(v == 1 for v in idrs.values()), "1"),
                check("fallback (b): no `video_link dropped` caused by the start (paced)", (drops[0], drops[-1]) if drops else None,
                      (drops[-1] == drops[0]) if drops else None, "unchanged"),
                check("fallback (b): PC target_ms within [50, 150] on the link", A.pct(d_link, (50,)),
                      bool(d_link) and 50 <= min(d_link) and max(d_link) <= 150, "[50, 150]"),
                check("fallback (b): present times sane on the link (capture -> app, ms)", A.pct(vals, (50, 99)),
                      bool(vals) and min(vals) >= 0 and max(vals) <= 400, "0..400 (no picture held for seconds or due in the future)",
                      "R2 #5: within 25 ms of completion + D"),
                check("fallback (b): clean switch back to UDP", {"frozen_ms": sw, "path_after": dict(Counter(path_after))},
                      sw[1] <= 500 and path_after and all(p == "udp" for p in path_after), "frozen <= 500 ms, path udp"),
            ]
            info["fallback_b"] = {"start": s, "latency_link_ms": A.pct(vals), "d_link": A.pct(d_link, (50, 99))}
        checks += invariant_checks(run, [r])
    return checks, info


def cam_noreceiver(ids, o):
    hold = o.get("seconds") or 60
    acts, items, t = [], [], 2.0
    for mode in ("not_source", "helper", "silent"):
        acts.append((t, f"refuse {mode}", pcc({"camera_refuse": mode})))
        items.append(read(t + 8, hold, f"n_{mode}"))
        t += 8 + hold + 12
    acts.append((t, "refuse off", pcc({"camera_refuse": None})))
    items.append(read(t + 8, 15, "n_after"))
    return [{"sub": "noreceiver", "desc": f"PC camera_refuse not_source / helper / silent, {hold} s open each, then off",
             "prepare": {"quality": "720p", "test_source": "synth"}, "duration": t + 8 + 15 + 5, "actions": acts,
             "items": items, "kills": [], "cable": False, "resources_every": 0, "pc_status_every": 4}]


def an_cam_noreceiver(runs, specs, o):
    run = runs[0]
    checks, info, reads = [], {}, []
    for mode in ("not_source", "helper", "silent"):
        r = run.read(f"n_{mode}")
        reads.append(r)
        if not r:
            checks.append(check(f"{mode}: read data", None, False, "camprobe dump"))
            continue
        t0, t1 = r["t_open"], r["t_closed"] or (r["t_open"] + 60)
        took = [x for x in run.pc_status if x.get("t", 0) < t0 and x.get("camera", {}).get("refuse") == mode]
        ss = sorted(send_streams_in(run, t0 - 0.5, t1).values(), key=lambda s: s["on"])
        ons = [round((s.get("off") or t1) - s["on"], 2) for s in ss]
        starts = [round(s["on"] - t0, 1) for s in ss]
        errs = [s.get("error") for s in ss]
        info[mode] = {"sends": len(ss), "on_s": ons, "start_after_open_s": starts, "errors": errs, "refuse_seen_before_open": bool(took)}
        checks.append(check(f"{mode}: PC refusal in force before the open", bool(took), bool(took), "PC status refuse"))
        if mode in ("not_source", "helper"):
            checks.append(check(f"{mode}: Mac camera stopped at once, no re-open in {round(t1 - t0)} s", {"sends": len(ss), "on_s": ons},
                                len(ss) == 1 and ons[0] <= 0.8, "1 send, on <= 0.5 s after the refusal (+ ~0.3 s round trip)"))
        else:
            checks.append(check("silent: each send stopped by no_receiver", {"on_s": ons, "errors": errs},
                                bool(ss) and all(x <= 3.6 for x in ons) and all(e == "no_receiver" for e in errs), "on <= 3.5 s, no_receiver"))
            checks.append(check("silent: re-opens only on the 10/30/60 s backoff", {"sends": len(ss), "start_after_open_s": starts},
                                1 <= len(ss) <= 3 and all(b - a >= 9.0 for a, b in zip(starts, starts[1:])), "<= 3 sends in 60 s, >= 10 s apart"))
    r = run.read("n_after")
    reads.append(r)
    s = A.start_of_read(r["frames"], r["t_open"]) if r else {"first_ms": None}
    checks.append(check("after camera_refuse null: streaming within the cam_start budget (ms)", s["first_ms"],
                        s["first_ms"] is not None and s["first_ms"] <= 600, "<= 600"))
    checks += invariant_checks(run, reads)
    return checks, info


def cam_killswitch(ids, o):
    acts = [(1.0, "sharing off", mac({"camera_sharing": False})),
            (4.0, "pc route -> mac camera", pcc({"set_camera": {"machine": ids.mac, "device": None}})),
            (42.0, "sharing on", mac({"camera_sharing": True}))]
    return [s_read("killswitch", "Mac sharing off, the PC routes the Mac camera and opens Beamer Camera; sharing on at 42 s",
                   55, lead=14.0, actions=acts, pc_status_every=3,
                   prepare={"quality": "720p", "test_source": "synth", "route": False})]


def an_cam_killswitch(runs, specs, o):
    run = runs[0]
    r = run.read("r")
    if not r:
        return [check("read data", None, False, "camprobe dump")], {}
    t_on = run.at("sharing on")
    t_open = r["t_open"]
    before = send_streams_in(run, run.t0, t_on)
    cam_ev = [e for e in run.mac_events if e.get("ev") == "camera" and run.t0 <= e["t"] < t_on]
    pcst = [x for x in run.pc_status if t_open + 2 <= x.get("t", 0) < t_on]
    says = ["sharing_off" in str(x.get("camera", {}).get("snapshot", {}).get("status", {})) or
            "sharing_off" in str(x.get("camera", {}).get("snapshot", {}).get("sinks", {})) for x in pcst]
    fr_before = Counter(x[1] for x in r["frames"].rows if x[0] < t_on)
    after = send_streams_in(run, t_on, run.t_end)
    first = r["frames"].first_c1(after=t_on)
    st = sorted(after.values(), key=lambda s: s["on"])[0] if after else None
    checks = [
        check("sharing off: the Mac never starts capture", {"streams": len(before), "camera_events": len(cam_ev)},
              not before and not cam_ev, "0 streams, 0 camera events"),
        check("sharing off: PC status sharing_off", f"{sum(says)}/{len(says)} samples", bool(says) and all(says), "every sample"),
        check("sharing off: the PC app gets the placeholder, no Mac picture", dict(fr_before),
              fr_before.get("c1", 0) == 0 and fr_before.get("uniform", 0) == 0 and sum(fr_before.values()) > 0, "placeholder frames only"),
        check("sharing on: first Mac picture after the Mac camera started (ms)",
              round((first[0] - st["on"]) * 1e3, 1) if first and st else None,
              bool(first and st) and (first[0] - st["on"]) * 1e3 <= 600, "<= 600 (cam_start budget)",
              f"from the dev command: {round((first[0] - t_on) * 1e3) if first else None} ms (incl. the 1 s dev poll)"),
    ] + invariant_checks(run, [r])
    return checks, {"frames_before": dict(fr_before)}


def cam_helper_kill(ids, o):
    return [s_read("helper_kill", "PC helper killed at 25 s while streaming (watchdog restart)", 60, lead=5.0,
                   kills=[{"at": 25.0}])]


def an_cam_helper_kill(runs, specs, o):
    run = runs[0]
    r = run.read("r")
    kills = run.kills()
    if not r or not kills:
        return [check("read data / kill", None, False, "camprobe dump + kill record")], {}
    tk = kills[0]["t"]
    rows = r["frames"].rows
    last_live = next((x for x in reversed(rows) if x[0] < tk and x[1] == "c1"), None)
    t_builtin = A.first_class_after(r["frames"], tk, "builtin")
    held = [x for x in rows if tk <= x[0] < (t_builtin or tk)]
    held_ok = all(x[1] == "c1" and last_live and x[2] <= last_live[2] for x in held)
    ready = next((e["t"] for e in run.pc_events if e.get("ev") == "ready" and e["t"] > tk), None)
    back = next((x for x in rows if x[0] > (t_builtin or tk) and x[1] == "c1"), None)
    checks = [
        check("last picture held, then the DLL's built-in placeholder (ms after the kill)",
              round((t_builtin - tk) * 1e3, 1) if t_builtin else None,
              bool(t_builtin) and (t_builtin - tk) * 1e3 <= 600 and held_ok, "<= 600 (500 ms threshold), held picture before",
              "CW5: 507-509 ms"),
        check("live again after the watchdog restart (s after the new helper's ready)",
              round(back[0] - ready, 2) if back and ready else None,
              bool(back and ready) and back[0] - ready <= 5.0, "<= 5.0",
              f"{round(back[0] - tk, 2) if back else None} s after the kill"),
    ] + invariant_checks(run, [r])
    return checks, {"kill": kills[0], "held_frames": len(held)}


def cam_soak(ids, o):
    secs = o.get("seconds") or 1800
    lead = o.get("audio_only") or 185.0
    acts = [(1.0, "mic on (test A)", mac([MIC_A, {"set_mic": {"machine": ids.mac, "device": ids.mac_mic}}])),
            (lead + secs + 2, "mic off", mac([{"set_mic": {"machine": None, "device": None}},
                                                {"test_source": {"kind": "mic", "signal": None}}]))]
    return [s_read("soak", f"mic (test A) to the PC throughout; {lead:.0f} s audio only, then the camera {secs} s at 720p",
                   secs, lead=lead, actions=acts, cable=True, resources_every=30)]


def an_cam_soak(runs, specs, o):
    run = runs[0]
    r = run.read("r")
    if not r:
        return [check("read data", None, False, "camprobe dump")], {}
    t0, t1 = r["t_open"], r["t_closed"] or run.t_end
    recv = pc_recv(run)
    sync = sorted((x[0], x[2].get("sync_ms") or 0.0) for x in recv)

    def sync_at(t):
        prev = [v for tt, v in sync if tt <= t]
        return prev[-1] if prev else 0.0
    lat = A.latency_ms(r["frames"], skip_s=3.0, exclude=sync_at)
    wins = {}
    for t, v in lat:
        wins.setdefault(int((t - t0) // 300), []).append(v)
    p50s = [round(float(A.np.percentile(v, 50)), 2) for k, v in sorted(wins.items()) if len(v) > 3000]
    spikes = run.clock.rtt_spikes(0.15)
    eps = A.frozen_episodes(r["frames"], 500.0)
    eps_out = [e for e in eps if not any(abs(s - e[0]) < 3.0 for s in spikes)]
    minutes = (t1 - t0) / 60
    integ = A.integrity(r["frames"])
    # resources over the camera phase (after a minute of warm-up)
    mres = [x for x in run.mac_resources() if x["t"] >= t0 + 60]
    pres = [x for x in run.pc_resources() if x["t"] >= t0 + 60]
    mh, vt = A.cpu_mem(mres, "helper", 0), A.cpu_mem(mres, "VTEncoderXPCService", 0)
    ph, fs = A.cpu_mem(pres, "helper", 0), A.cpu_mem(pres, "frameserver", 0)
    pb, mb = A.cpu_mem(pres, "Beamer", 0), A.cpu_mem(mres, "Beamer", 0)
    pc_audio = A.audio_stats(run.pc_events, "recv")
    loss_a = [s.get("loss_pct") for t, _, s in pc_audio if run.t0 + 20 <= t < t0 and s.get("loss_pct") is not None]
    loss_c = [s.get("loss_pct") for t, _, s in pc_audio if t0 + 5 <= t <= t1 and s.get("loss_pct") is not None]
    la, lc = (sum(loss_a) / len(loss_a) if loss_a else None), (sum(loss_c) / len(loss_c) if loss_c else None)
    holes = A.counter_delta(recv, "holes", t0, t1)
    lost = A.counter_delta(recv, "lost", t0, t1)
    # CW4 risk: the PC hardware decoder blocking 0.4-3.3 s. A freeze > 400 ms with no network cause.
    lost_series = [(x[0], x[2].get("lost", 0), x[2].get("holes", 0)) for x in recv]

    def net_cause(t):
        before = [v for v in lost_series if v[0] <= t - 2]
        after = [v for v in lost_series if v[0] >= t + 2]
        changed = before and after and (after[0][1] > before[-1][1] or after[0][2] > before[-1][2])
        return bool(changed) or any(abs(s - t) < 3 for s in spikes)
    stalls = [{"t_s": round(e[0] - t0, 1), "gap_ms": round(e[1])} for e in A.frozen_episodes(r["frames"], 400.0) if not net_cause(e[0])]
    dec = [x[2].get("dec_ms") for x in recv if x[2].get("dec_ms") is not None]
    decoders = Counter(x[2].get("decoder") for x in recv)
    closed = send_streams_in(run, t0 - 1, t1)
    stop_s = [round(s["camera_stopped"] - t1, 2) for s in closed.values() if s.get("camera_stopped")]
    checks = [
        check("frozen episodes > 500 ms per 10 min, outside Wi-Fi stalls", round(len(eps_out) / minutes * 10, 2),
              len(eps_out) / minutes * 10 <= 1.0, "<= 1", f"{len(eps)} in all, {len(spikes)} ping RTT spikes > 150 ms"),
        check("0 corrupt pictures (code strip)", integ, A.corrupt_count(integ) == 0, "0"),
        check("capture -> app p50 per 5 min within +-3 ms of the first (sync_ms excluded)", p50s,
              len(p50s) >= 2 and all(abs(v - p50s[0]) <= 3 for v in p50s), "+-3 ms", "C4: 57.0 +- 0.1, 10.5 ppm drift"),
        check("memory growth: Mac helper (MB)", mh and mh["mem_growth_mb"], bool(mh) and mh["mem_growth_mb"] < 5, "< 5"),
        check("memory growth: VTEncoderXPCService (MB)", vt and vt["mem_growth_mb"], (vt["mem_growth_mb"] < 5) if vt else None, "< 5",
              "C4: Mac sender +2.5 MB, unexplained"),
        check("memory growth: PC helper (MB, private)", ph and ph["mem_growth_mb"], bool(ph) and ph["mem_growth_mb"] < 5, "< 5"),
        check("CPU Mac helper (% of a core)", mh and mh["cpu_pct"], bool(mh) and mh["cpu_pct"] < 6, "< 6",
              "C4: 1.3 % + VT 1.3 %; CW3 real camera ~9 % (gestures?) is a real-camera item"),
        check("CPU PC helper (% of a core)", ph and ph["cpu_pct"], bool(ph) and ph["cpu_pct"] < 5, "< 5", "C4: 3.1 %"),
        check("CPU Frame Server (% of a core)", fs and fs["cpu_pct"], bool(fs) and fs["cpu_pct"] < 1.5, "< 1.5", "C4: 0.8 %"),
        check("mic loss_pct with the camera vs audio only (no phantom loss from the time map)",
              {"camera": None if lc is None else round(lc, 3), "audio_only": None if la is None else round(la, 3)},
              None if la is None or lc is None else lc <= la + 0.1, "not above audio only (+0.1 pt)"),
        check("Mac camera stopped after the soak's app closed (s)", stop_s, bool(stop_s) and max(stop_s) <= 4.0, "<= 4.0"),
    ] + invariant_checks(run, [r])
    info = {"latency_ms": A.pct([v for _, v in lat]), "p50_by_5min": p50s, "frozen_500": [(round(e[0] - t0, 1), round(e[1])) for e in eps],
            "holes": holes, "lost_frames": lost, "decoder_stall_suspects": stalls, "dec_ms": A.pct(dec, (50, 99)),
            "decoders": dict(decoders), "mac": {"helper": mh, "vt": vt, "beamer": mb}, "pc": {"helper": ph, "frameserver": fs, "beamer": pb},
            "helper_cpu_stats_mac": A.pct([c for _, c in A.helper_cpu(run.mac_events)], (50, 95)),
            "helper_cpu_stats_pc": A.pct([c for _, c in A.helper_cpu(run.pc_events)], (50, 95)),
            "note": "holes > 5 packets with their frame type: the product logs only counters (holes, lost); counts above"}
    return checks, info


def cam_latency_real(ids, o):
    secs = o.get("seconds") or 120
    return [{"sub": "latency_real", "desc": f"real camera; Alex opens the Windows Camera app on the PC with a phone stopwatch "
             f"in view of the Mac camera and photographs both; the harness records {secs} s of stats and CPU",
             "prepare": {"quality": "720p", "test_source": None}, "duration": secs, "actions": [], "items": [], "kills": [],
             "cable": False, "resources_every": 10, "pc_status_every": 0, "interactive": True}]


def an_cam_latency_real(runs, specs, o):
    run = runs[0]
    recv = pc_recv(run)
    mres = run.mac_resources()
    mh = A.cpu_mem(mres, "helper", 10)
    ss = A.video_send_streams(run.mac_events)
    checks = [check("glass -> app (phone stopwatch photo)", None, None, "~85-130 ms expected", "read the photos: PC screen - Mac stopwatch")]
    info = {"pc_d_ms": A.pct([x[2].get("target_ms") for x in recv], (50, 99)), "mac_helper": mh,
            "mac_helper_cpu_stats": A.pct([c for _, c in A.helper_cpu(run.mac_events)], (50, 95)),
            "camera": [{k: v for k, v in s.items() if k in ("encoder", "encoder_ms")} for s in ss.values()],
            "effects": [e.get("effects") for e in run.mac_events if e.get("ev") == "camera" and e.get("effects")][-1:],
            "cam_fps": A.pct([x[2].get("cam_fps") for x in mac_send(run)], (50,))}
    return checks, info


# ================================================================ registry

CASES = {
    # name: (builder, analysis, minutes (approx), needs)
    "cam_start": (cam_start, an_cam_start, 7.5, "synthetic"),
    "cam_stop": (cam_stop, an_cam_stop, 2.0, "synthetic"),
    "cam_start_slow": (cam_start_slow, an_cam_start_slow, 7.5, "synthetic"),
    "cam_latency": (cam_latency, an_cam_latency, 22.0, "synthetic (+ffmpeg on the PC for the 1080p last-rows grab)"),
    "cam_loss": (cam_loss, an_cam_loss, 30.0, "synthetic"),
    "cam_stall": (cam_stall, an_cam_stall, 10.5, "synthetic"),
    "cam_fallback": (cam_fallback, an_cam_fallback, 6.0, "synthetic + Mac mic test source + CABLE capture tool"),
    "cam_noreceiver": (cam_noreceiver, an_cam_noreceiver, 5.0, "synthetic"),
    "cam_killswitch": (cam_killswitch, an_cam_killswitch, 1.5, "synthetic"),
    "cam_helper_kill": (cam_helper_kill, an_cam_helper_kill, 1.5, "synthetic"),
    "cam_soak": (cam_soak, an_cam_soak, 34.0, "synthetic + Mac mic test source + CABLE capture tool"),
    "cam_start_real": (cam_start_real, an_cam_start_real, 4.0, "REAL camera: H1 (Beamer.app granted Camera)"),
    "cam_latency_real": (cam_latency_real, an_cam_latency_real, 3.0, "REAL camera: H1 + Alex with a phone stopwatch"),
}
# cam_oldpeer and cam_lipsync are run.py specials (no session / CW10's script)
REAL = ("cam_start_real", "cam_latency_real", "cam_lipsync_real")
ALL_ORDER = ("cam_start", "cam_stop", "cam_start_slow", "cam_latency", "cam_loss", "cam_stall", "cam_fallback",
             "cam_noreceiver", "cam_killswitch", "cam_helper_kill", "cam_oldpeer", "cam_lipsync")
