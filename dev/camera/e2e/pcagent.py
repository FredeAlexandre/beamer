# SPDX-License-Identifier: GPL-3.0-only
"""The "PC app" of the camera E2E runs: runs in Alex's signed-in session (started by pcside_cam.py through
a scheduled task) and opens "Beamer Camera" exactly as an app does (camprobe = IMFSourceReader, like
Chromium, Teams and the Camera app), reading the C1 code strip of every frame. Standard library only.

    pythonw pcagent.py PLAN.json

Items (times relative to the plan's start_mono, the PC's time.monotonic()):
  {"kind": "read", "at": s, "seconds": s, "size": "1280x720" | null, "tag": "r01"}
      camprobe read --strip c1 --events --dump DIR/tag.dump --seconds S [--size WxH] --tag tag
      stdout -> DIR/tag.out (JSON lines; the last one is the summary with t_begin/t_first/t_stop/t_closed,
      unix µs); the dump has "t_us class frame ts100us" per frame.
  {"kind": "ffmpeg", "at": s, "seconds": s, "size": "1920x1080", "tag": "lastrows"}
      grabs raw NV12 frames through DirectShow into DIR/tag.nv12 (1080p last-rows check), if ffmpeg exists.
Writes DIR/agent.jsonl ({"ev": "spawn"|"exit"|"error", "tag", "mono", "unix", ...}) and DIR/agent.done.
Reads never overlap here: Windows lets one app at a time open the camera by default (the harness plans them
apart); an item whose start time has passed while the previous one ran starts at once (noted `late_s`).
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time

NO_WINDOW = 0x08000000   # CREATE_NO_WINDOW: no console window pops up on Alex's desktop (as dev/audio/e2e/pcplay.py)


def main():
    plan = json.load(open(sys.argv[1], encoding="utf-8"))
    d = plan["dir"]
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, "agent.jsonl"), "a", buffering=1)

    def note(**kw):
        kw.setdefault("mono", time.monotonic())
        kw.setdefault("unix", time.time())
        log.write(json.dumps(kw) + "\n")

    note(ev="agent_start", pid=os.getpid(), session=os.environ.get("SESSIONNAME"), user=os.environ.get("USERNAME"))
    probe = plan["probe"]
    t0 = plan["start_mono"]
    try:
        for item in sorted(plan.get("items", []), key=lambda x: x["at"]):
            due = t0 + item["at"]
            while time.monotonic() < due:
                time.sleep(min(0.01, max(0.0, due - time.monotonic())))
            tag = item["tag"]
            late = time.monotonic() - due
            if item["kind"] == "read":
                args = [probe, "read", "--strip", "c1", "--events", "--dump", os.path.join(d, tag + ".dump"),
                        "--seconds", str(item["seconds"]), "--tag", tag]
                if item.get("size"):
                    args += ["--size", item["size"]]
            elif item["kind"] == "ffmpeg":
                ff = shutil.which("ffmpeg")
                if not ff:
                    note(ev="error", tag=tag, error="ffmpeg not found")
                    continue
                args = [ff, "-hide_banner", "-loglevel", "error", "-f", "dshow", "-video_size", item.get("size", "1920x1080"),
                        "-pixel_format", "nv12", "-i", "video=Beamer Camera", "-t", str(item.get("seconds", 2)),
                        "-f", "rawvideo", "-pix_fmt", "nv12", "-y", os.path.join(d, tag + ".nv12")]
            else:
                note(ev="error", tag=tag, error="unknown kind " + str(item["kind"]))
                continue
            with open(os.path.join(d, tag + ".out"), "w", encoding="utf-8") as out, \
                    open(os.path.join(d, tag + ".err"), "w", encoding="utf-8") as err:
                note(ev="spawn", tag=tag, kind=item["kind"], late_s=round(late, 3), args=args)
                rc = subprocess.run(args, stdout=out, stderr=err, timeout=item.get("seconds", 10) + 60,
                                    creationflags=NO_WINDOW).returncode
                note(ev="exit", tag=tag, rc=rc)
    except Exception as error:  # noqa: BLE001  (the run must still end with agent.done)
        note(ev="error", error=repr(error))
    finally:
        note(ev="agent_done")
        open(os.path.join(d, "agent.done"), "w").close()


if __name__ == "__main__":
    main()
