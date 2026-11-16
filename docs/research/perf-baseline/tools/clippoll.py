"""Record every clipboard change with the wall-clock time, polling every 1 ms.

Never opens the clipboard (so it cannot contend with Beamer): it reads the change counter only,
NSPasteboard.changeCount on macOS, GetClipboardSequenceNumber on Windows (must run in the
interactive session, not session 0).

usage: clippoll.py <out.jsonl> <seconds>
"""
import json, sys, time

out = open(sys.argv[1], "a", buffering=1)
stop = time.time() + float(sys.argv[2])

if sys.platform == "darwin":
    from AppKit import NSPasteboard
    pb = NSPasteboard.generalPasteboard()
    read = pb.changeCount
else:
    import ctypes
    ctypes.windll.winmm.timeBeginPeriod(1)
    read = ctypes.windll.user32.GetClipboardSequenceNumber

last = read()
out.write(json.dumps({"k": "start", "t": time.time(), "seq": last}) + "\n")
while time.time() < stop:
    now = read()
    if now != last:
        out.write(json.dumps({"k": "change", "t": time.time(), "seq": now}) + "\n")
        last = now
    time.sleep(0.001)
out.write(json.dumps({"k": "stop", "t": time.time(), "seq": last}) + "\n")
