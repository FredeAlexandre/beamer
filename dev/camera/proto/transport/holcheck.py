"""TCP runs: at each big delay event, what did the UDP pings (same moment, same Wi-Fi) see?"""
import sys
import numpy as np
for name in sys.argv[1:]:
    t = np.load(f"out/{name}.trace.npz"); m = np.load(f"out/{name}.mac.npz")
    p = m["pings"]; rtt = (p[:, 3] - p[:, 0]) - (p[:, 2] - p[:, 1])
    a_s, a_d = t["a_send"], t["a_delay"]
    big = np.where(a_d > 0.100)[0]
    evs = []
    for i in big:
        if not evs or a_s[i] - evs[-1][1] > 1.0:
            evs.append([a_s[i], a_s[i], a_d[i]])
        else:
            evs[-1][1] = a_s[i]; evs[-1][2] = max(evs[-1][2], a_d[i])
    print(f"{name}: {len(evs)} events with audio delay > 100 ms in {(a_s[-1]-a_s[0])/60:.1f} min")
    for s, e, w in evs:
        w_ = (p[:, 0] > s - 1) & (p[:, 0] < e + 1)
        print(f"   t={s - a_s[0]:7.1f}s len={1e3*(e-s):6.0f}ms worst={1e3*w:6.0f}ms   UDP ping RTT max in +-1 s: {1e3*rtt[w_].max():6.1f} ms (n={w_.sum()})")
