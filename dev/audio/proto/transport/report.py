"""Turn out/<name>.summary.json files into Markdown table rows (used to fill README.md).

    python3 report.py NAME [NAME ...]
"""
import json
import sys


def load(name):
    t = open(f"out/{name}.summary.json").read()
    return json.loads(t[t.index("{"):])


def row(name):
    d = load(name)
    c = d["local"].get("click_latency_ms", {})
    la, ra = d["local_agg"], d["remote_agg"]
    r = d.get("rtt_ms", {})
    tone = d["local"].get("tone", {})
    return {
        "run": name,
        "rtt_p50": r.get("p50"), "rtt_p95": r.get("p95"), "rtt_max": r.get("max"),
        "loop_p50": c.get("p50"), "loop_p95": c.get("p95"), "loop_max": c.get("max"),
        "jb_pc": ra.get("level_ms_mean"), "jb_mac": la.get("level_ms_mean"),
        "jit_pc": ra.get("jitter_ms_mean"), "jit_mac": la.get("jitter_ms_mean"),
        "tx_kbps": la.get("tx_kbps"), "cpu_mac": la.get("cpu_pct_mean"), "cpu_pc": ra.get("cpu_pct_mean"),
        "enc_us_mac": la.get("enc_us_mean"), "dec_us_mac": la.get("dec_us_mean"),
        "und_pc": ra.get("underrun"), "und_mac": la.get("underrun"),
        "lost_mac": la.get("lost"), "fec_mac": la.get("fec"), "plc_mac": la.get("plc"),
        "drop_mac": la.get("drop"), "flush_mac": la.get("flushed"),
        "glitch_win": tone.get("glitch_windows"), "glitch_ev": tone.get("glitch_events"),
        "tone_windows": tone.get("windows"), "resid_db": tone.get("residual_db_median"),
        "lbrr_mac": la.get("lbrr"), "rx_mac": la.get("rx"), "inj_drop_pc": ra.get("inj_drop"),
        "drift_env_mac": d["local"].get("drift_env_ppm"), "ppm_i_mac": d["local"].get("ppm_i"),
        "drift_env_pc": (d.get("remote") or {}).get("drift_env_ppm"), "ppm_i_pc": (d.get("remote") or {}).get("ppm_i"),
        "clock_slope": d.get("clock_offset_slope_ppm"),
    }


if __name__ == "__main__" and sys.argv[1:2] != ["--drift"]:
    for n in sys.argv[1:]:
        try:
            print(json.dumps(row(n)))
        except Exception as e:  # missing run
            print(json.dumps({"run": n, "error": str(e)}))


def drift(name):
    """Per-minute buffer level and resampling ratio for both sides of a long run."""
    rows = [json.loads(l) for l in open(f"out/{name}.jsonl")]
    out = {}
    for side in ("client", "server"):
        S = [r for r in rows if r.get("side") == side and "rx" in r]
        mins = []
        for i in range(0, len(S), 60):
            ch = S[i:i + 60]
            lv = [c["rx"]["level_ms"] for c in ch]
            mins.append({"min": i // 60, "level_mean": round(sum(lv) / len(lv), 1),
                         "level_min": min(c["rx"]["level_min_ms"] for c in ch),
                         "level_max": max(c["rx"]["level_max_ms"] for c in ch),
                         "ppm_mean": round(sum(c["rx"]["ppm"] for c in ch) / len(ch), 1),
                         "ppm_i_end": ch[-1]["rx"]["ppm_i"],
                         "target_mean": round(sum(c["rx"]["target_ms"] for c in ch) / len(ch), 1),
                         **{k: sum(c["rx"].get(k, 0) for c in ch) for k in ("underrun", "drop", "flushed", "lost")}})
        out[side] = mins
    return out


if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "--drift":
    for side, mins in drift(sys.argv[2]).items():
        print(side)
        for m in mins:
            print(" ", m)
