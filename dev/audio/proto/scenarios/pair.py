"""`peer.py send` (the Mac side of the transport, unchanged) that also saves its ping exchange.

    pair.py PINGS.json send --to HOST:PORT ... (all peer.py send options)

peer.py's client pings the PC every --ping-s and keeps (t_mac, rtt, offset) with
offset = PC monotonic - Mac monotonic from the NTP formula; this writes them out so the
scenarios can put Mac and PC timestamps on one timeline (sig.clock_fit).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "transport"))
import peer  # noqa: E402

out = sys.argv.pop(1)


def send_and_save(a):
    key = peer.load_key(a.key_file)
    host, port = a.to.rsplit(":", 1)
    res, sess = peer.client_run(peer.UdpClient(host, int(port), a.local_port), key, a)
    with open(out, "w") as f:
        json.dump({"pings": sess.pings, "connected": res is not None}, f)
    if res is None:
        sys.exit(2)


peer.cmd_send = send_and_save
peer.main()
