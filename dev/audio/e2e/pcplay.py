# SPDX-License-Identifier: GPL-3.0-only
"""Run a `tap.py gen | proto-audio play` pipe on the PC in the signed-in user's session, hidden.

    pythonw pcplay.py SPEC.json      SPEC = {"gen": [argv...], "play": [argv...], "err": path, "pids": path}

Started by pcside.py through a one-shot scheduled task (`schtasks /IT`): Windows process loopback only
captures processes of the capturing process's own session, so a player started over ssh (session 0)
is invisible to Beamer's helper (session 1); an app Alex runs is in session 1, and so is this pipe.
"""
import json
import subprocess
import sys

NO_WINDOW = 0x08000000


def main():
    spec = json.load(open(sys.argv[1], encoding="utf-8"))
    err = open(spec["err"], "ab")
    gen = subprocess.Popen(spec["gen"], stdout=subprocess.PIPE, stderr=err, creationflags=NO_WINDOW)
    play = subprocess.Popen(spec["play"], stdin=gen.stdout, stderr=err, creationflags=NO_WINDOW)
    gen.stdout.close()
    with open(spec["pids"], "w") as f:
        json.dump({"gen": gen.pid, "play": play.pid}, f)
    gen.wait()
    play.wait()


if __name__ == "__main__":
    main()
