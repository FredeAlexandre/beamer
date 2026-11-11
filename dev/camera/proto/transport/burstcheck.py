"""Does a keyframe burst hurt? Audio delay and video loss near keyframe bursts vs elsewhere."""
import sys
import numpy as np
for name in sys.argv[1:]:
    t = np.load(f"out/{name}.trace.npz")
    fk = t["f_t"][t["f_key"] == 1]
    a_s, a_d = t["a_send"], t["a_delay"]
    # audio packets sent in [key frame time, +40 ms) vs the rest
    idx = np.searchsorted(fk, a_s) - 1
    near = (idx >= 0) & (a_s - fk[np.clip(idx, 0, None)] < 0.040)
    def q(x): x = x[~np.isnan(x)]; return " ".join(f"{np.percentile(x, p)*1e3:6.2f}" for p in (50, 95, 99)) + f" (n={len(x)})"
    print(name, "audio p50/p95/p99 ms  near key bursts:", q(a_d[near]), "| elsewhere:", q(a_d[~near]))
    lost = np.isnan(t["v_delay"])
    print("   lost video packets:", int(lost.sum()), "of which in keyframes:", int((lost & (t["v_key"] == 1)).sum()),
          "frag idx:", t["v_frag"][lost].astype(int).tolist(), "t_s:", np.round(t["v_send"][lost] - t["v_send"][0], 1).tolist())
    vd = t["v_delay"]; vk = t["v_key"] == 1
    print("   video pkt delay key/P p99:", round(np.nanpercentile(vd[vk], 99)*1e3, 1), round(np.nanpercentile(vd[~vk], 99)*1e3, 1))
