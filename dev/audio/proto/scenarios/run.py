"""Run one audio scenario between the Mac (this machine) and the PC (`ssh lebig`), then fetch the
PC's recordings into the same run directory and analyse it (analyze.py).

    uv run --python 3.13 --with pynacl --with numpy run.py SCENARIO [--name NAME] [--duration S]

Scenarios (see README.md): s1 s1j s1ac s2 s2noise s3 s4 s5 s5loop s5loopoff.
The Mac only ever initiates (peer.py send); the PC answers and punches first (no firewall rule).
"""
from __future__ import annotations

import argparse
import json
import os
import shlex
import subprocess
import sys
import time
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from sig import RATE, Injector  # noqa: E402

TR = os.path.normpath(os.path.join(HERE, "..", "transport"))
BA = os.path.normpath(os.path.join(HERE, "..", "mac", ".build", "beamer-audio"))
PY = sys.executable
MAC_IP, PC_IP, LPORT = "192.168.1.138", "192.168.1.134", 40123

PCD = r"C:\Users\Alex\audio-proto\scenarios"
PCT = r"C:\Users\Alex\audio-proto\transport"
PCPY = r"C:\Users\Alex\audio-proto\venv\Scripts\python.exe"
PCBA = PCD + r"\beamer-audio.exe"
PCTAP = PCD + r"\tap.py"
G733 = "{0.0.0.00000000}.{29dbf8ab-8ba2-461a-921d-46410ced382c}"
WAVE = "{0.0.1.00000000}.{184e5798-6f57-401d-95eb-23205bf52581}"
CABLE_IN = "{0.0.0.00000000}.{53ee99d1-271d-45c8-b798-d304f02687ba}"
CABLE_OUT = "{0.0.1.00000000}.{ddc7e301-3260-4ed3-8389-0938254820e0}"

MAC_PLAY = f"{BA} play --channels 2 --buffer-frames 128 --prefill-ms 15"
PC_PLAY = f"{PCBA} play --channels 2 --live-drop-ms 60 --queue-ms 20 --device "
# speakers-facing signals are kept quiet: pilot -40 dBFS, chirp peak -26 dBFS
QUIET = "--pilot-amp 0.01 --marker-amp 0.05"
# mixed into a live mic stream (never played at unity): loud enough that room noise picked up by
# the mic (Wave:3 bursts up to -26 dBFS, -30 dBFS in the pilot's lock-in band) cannot fake a glitch.
# Chirps at -20 dBFS, not -10: a -10 dBFS chirp makes the Opus VOIP encoder switch modes, and a
# decoder that ever missed a frame (a PLC, or joining a frame late) then renders the pilot with
# 6 dB dips after many chirps: 10-16 false glitches/min with no transport fault at all (offline,
# transport README "Mac->PC jitter"). With -20 dBFS chirps that floor is 0 and every marker is found.
MIXED = "--pilot-amp 0.2 --marker-amp 0.1"


def scenarios(D):
    mac_rec_tap = f"{PY} {HERE}/tap.py rec --out @RUN@/sink --channels 2 --seconds {D + 6} -- {BA} capture-system --channels 2"
    pc_rec_g733 = f"{PCPY} {PCTAP} rec --out @PCRUN@\\sink --channels 2 --seconds {D + 6} -- {PCBA} capture-system --device {G733} --channels 2"
    pc_inj_b = (f"{PCPY} {PCTAP} gen --out @PCRUN@\\inj --sig B --channels 2 --delay 5 --seconds {D - 6} {QUIET} "
                f"| {PCBA} play --device {G733} --channels 2")
    mac_afplay = "@AFPLAY@"
    # The PC's Wave:3 hears the MacBook's speakers at about -11 dB (measured: a -20 dBFS chirp
    # arrives at -31 dBFS), so playing the PC mic on the speakers at unity howls. The received mic
    # is recorded as it leaves the jitter buffer (file sink) and played 40 dB down.
    MAC_SINK_ATTEN = f"cmd:{PY} {HERE}/tap.py sink --out @RUN@/jb --channels 1 --gain-db -40 -- {BA} play --channels 1 --buffer-frames 128 --prefill-ms 15"
    return {
        "s1": dict(  # Mac mic (+ injected A at the mic's output) -> PC CABLE Input; detect on CABLE Output
            desc="Mac mic -> PC virtual mic (VB-CABLE)",
            mac_send=["--channels", "1", "--source",
                      f"cmd:{PY} {HERE}/tap.py src --out @RUN@/src --channels 1 --mix A --start 4 {MIXED} -- {BA} capture-mic --channels 1 --buffer-frames 128",
                      "--sink", "none"],
            pc_serve=f"--channels 1 --sink-channels 2 --source none --sink cmd:{PC_PLAY}{CABLE_IN}",
            pc_helpers=[f"{PCPY} {PCTAP} rec --out @PCRUN@\\sink --channels 1 --seconds {D + 6} -- {PCBA} capture-mic --device {CABLE_OUT} --channels 1"],
            pairs=[("mac/src", "pc/sink", "A")]),
        "s1j": dict(  # = s1, plus the PC records the jitter buffer's output on its way to `play`
            desc="Mac mic -> PC virtual mic (VB-CABLE), JB output recorded",
            mac_send=["--channels", "1", "--source",
                      f"cmd:{PY} {HERE}/tap.py src --out @RUN@/src --channels 1 --mix A --start 4 {MIXED} -- {BA} capture-mic --channels 1 --buffer-frames 128",
                      "--sink", "none"],
            pc_serve=f"--channels 1 --sink-channels 2 --source none --sink cmd:{PCPY} {PCTAP} sink --out @PCRUN@\\jb --channels 2 -- {PC_PLAY}{CABLE_IN}",
            pc_helpers=[f"{PCPY} {PCTAP} rec --out @PCRUN@\\sink --channels 1 --seconds {D + 6} -- {PCBA} capture-mic --device {CABLE_OUT} --channels 1"],
            pairs=[("mac/src", "pc/jb", "A"), ("mac/src", "pc/sink", "A")]),
        "s1ac": dict(  # acoustic: quiet chirps from the Mac speakers -> Mac mic -> PC CABLE
            desc="Mac speakers -> air -> Mac mic -> PC virtual mic (real-mic proof)",
            mac_send=["--channels", "1", "--source",
                      f"cmd:{PY} {HERE}/tap.py src --out @RUN@/src --channels 1 -- {BA} capture-mic --channels 1 --buffer-frames 128",
                      "--sink", "none"],
            mac_helpers=[f"{PY} {HERE}/tap.py gen --out @RUN@/inj --sig A --channels 2 --delay 4 --seconds {D - 6} "
                         f"--pilot-amp 0 --marker-amp 0.1 | {BA} play --channels 2"],
            pc_serve=f"--channels 1 --sink-channels 2 --source none --sink cmd:{PC_PLAY}{CABLE_IN}",
            pc_helpers=[f"{PCPY} {PCTAP} rec --out @PCRUN@\\sink --channels 1 --seconds {D + 6} -- {PCBA} capture-mic --device {CABLE_OUT} --channels 1"],
            pairs=[("mac/inj", "mac/src", "A"), ("mac/src", "pc/sink", "A"), ("mac/inj", "pc/sink", "A")]),
        "s2": dict(  # PC Wave:3 (+ injected B) -> Mac speakers; detect with a Mac process tap
            desc="PC mic (Wave:3) -> Mac speakers",
            mac_send=["--channels", "1", "--sink-channels", "1", "--source", "none", "--sink", MAC_SINK_ATTEN],
            mac_helpers=[mac_rec_tap],
            pc_serve=(f"--channels 1 --sink none --source cmd:{PCPY} {PCTAP} src --out @PCRUN@\\src --channels 1 --mix B "
                      f"--start 4 {MIXED} -- {PCBA} capture-mic --device {WAVE} --channels 1"),
            pairs=[("pc/src", "mac/jb", "B"), ("pc/src", "mac/sink", "B")]),
        "s2noise": dict(  # the Wave:3 alone (nobody speaks): its noise must arrive, continuously
            desc="PC mic (Wave:3) noise only -> Mac speakers",
            mac_send=["--channels", "1", "--sink-channels", "1", "--source", "none", "--sink", MAC_SINK_ATTEN],
            mac_helpers=[mac_rec_tap],
            pc_serve=(f"--channels 1 --sink none --source cmd:{PCPY} {PCTAP} src --out @PCRUN@\\src --channels 1 "
                      f"-- {PCBA} capture-mic --device {WAVE} --channels 1"),
            pairs=[]),
        "s3": dict(  # Mac system sound (afplay, A) muted locally -> PC G733; detect by G733 loopback
            desc="Mac system sound -> PC headset, muted on the Mac",
            mac_send=["--channels", "2", "--no-fec", "--source",
                      f"cmd:{PY} {HERE}/tap.py src --out @RUN@/src --channels 2 -- {BA} capture-system --mute-local --channels 2",
                      "--sink", "none"],
            # afplay outlives the capture by ~3 s: an unmuted tail the mic must hear (mute control)
            mac_helpers=[mac_afplay,
                         f"{PY} {HERE}/tap.py rec --out @RUN@/mic --channels 1 --seconds {D + 6} -- {BA} capture-mic --channels 1"],
            afplay=dict(sig="A", delay=4, seconds=D - 1, pilot=0.02, marker=0.1),
            pc_serve=f"--channels 2 --no-fec --source none --sink cmd:{PC_PLAY}{G733}",
            pc_helpers=[pc_rec_g733],
            pairs=[("mac/src", "pc/sink", "A")]),
        "s4": dict(  # PC system sound (B into the G733) muted locally -> Mac speakers; detect with Mac tap
            desc="PC system sound -> Mac speakers, muted on the PC",
            mac_send=["--channels", "2", "--no-fec", "--source", "none", "--sink", f"cmd:{MAC_PLAY}"],
            mac_helpers=[mac_rec_tap],
            pc_serve=(f"--channels 2 --no-fec --sink none --source cmd:{PCPY} {PCTAP} src --out @PCRUN@\\src --channels 2 "
                      f"-- {PCBA} capture-system --device {G733} --mute-local --channels 2"),
            pc_helpers=[pc_inj_b],
            pairs=[("pc/inj", "pc/src", "B"), ("pc/src", "mac/sink", "B")]),
        "s5": dict(  # 3 + 4 at once, each capture excluding its own player
            desc="Both directions at once, players excluded from capture",
            mac_send=["--channels", "2", "--no-fec", "--source",
                      f"cmd:{PY} {HERE}/tap.py src --out @RUN@/src --channels 2 --exclude-sibling '/beamer-audio play' "
                      f"-- {BA} capture-system --mute-local --channels 2",
                      "--sink", f"cmd:{MAC_PLAY}"],
            mac_helpers=[mac_afplay, mac_rec_tap],
            afplay=dict(sig="A", delay=4, seconds=D - 8, pilot=0.02, marker=0.1),
            pc_serve=(f"--channels 2 --no-fec --sink cmd:{PC_PLAY}{G733} --source cmd:{PCPY} {PCTAP} src --out @PCRUN@\\src "
                      f"--channels 2 --exclude-root-python -- {PCBA} capture-system --device {G733} --mute-local --channels 2"),
            pc_helpers=[pc_inj_b, pc_rec_g733],
            pairs=[("mac/src", "pc/sink", "A"), ("pc/src", "mac/sink", "B")],
            loopcheck=True),
    }


def loop_variant(base, exclude):
    """Single bursts, no pilots: A from the Mac at ~5 s, B from the PC at ~10 s."""
    s = json.loads(json.dumps(base))
    s["afplay"] = dict(sig="A", delay=4, seconds=3, pilot=0.0, marker=0.1, count=1, start=1.0)
    s["pc_helpers"] = [h for h in s["pc_helpers"] if "gen" not in h]
    s["pc_helpers"].append(f"{PCPY} {PCTAP} gen --out @PCRUN@\\inj --sig B --channels 2 --delay 14 --seconds 3 "
                           f"--start 1 --count 1 --pilot-amp 0 --marker-amp 0.1 | {PCBA} play --device {G733} --channels 2")
    if not exclude:
        s["mac_send"] = [a.replace("--exclude-sibling '/beamer-audio play' ", "") for a in s["mac_send"]]
        s["pc_serve"] = s["pc_serve"].replace("--exclude-root-python ", "")
    s["desc"] = "Loop test, single bursts, exclusion " + ("on" if exclude else "OFF (control)")
    s["pairs"] = []
    s["looptest"] = True
    return s


def write_wav(path, spec):
    inj = Injector(spec["sig"], spec["pilot"], spec["marker"], start=spec.get("start", 2.0), count=spec.get("count"))
    n = int(spec["seconds"] * RATE)
    x, starts = inj.render(0, n)
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.repeat((np.clip(x, -1, 1) * 32767).astype("<i2")[:, None], 2, axis=1).tobytes())
    return starts


def pc_files(name, sc, D):
    """PowerShell driver + one .cmd per process (cmd.exe does the pipes and redirections, so no
    argument goes through PowerShell 5.1's quoting)."""
    pcrun = PCD + "\\runs\\" + name
    sub = lambda s: s.replace("@PCRUN@", pcrun)  # noqa: E731
    files = {}
    ps = [
        "$ErrorActionPreference = 'Continue'",
        f"cmd /c {pcrun}\\endpoints.cmd before",
        "$h = @()",
        # endpoint state half way through (shows --mute-local in effect)
        f"$h += Start-Process cmd -ArgumentList '/c', 'ping -n {int(D / 2)} 127.0.0.1 >nul & {pcrun}\\endpoints.cmd during' -PassThru -WindowStyle Hidden",
    ]
    files["endpoints.cmd"] = f"@echo off\r\n{PCBA} list --json > {pcrun}\\endpoints_%1.json\r\n"
    for i, hcmd in enumerate(sc.get("pc_helpers", [])):
        files[f"helper{i}.cmd"] = f"@echo off\r\n{sub(hcmd)} 2> {pcrun}\\helper{i}.err\r\n"
        ps.append(f"$h += Start-Process cmd -ArgumentList '/c', '{pcrun}\\helper{i}.cmd' -PassThru -WindowStyle Hidden")
    args = sub(sc["pc_serve"])
    # peer.py splits a cmd: value itself, so each --source/--sink cmd: value must be one argument
    for opt in ("--source", "--sink"):
        tok = opt + " cmd:"
        if tok in args:
            k = args.index(tok)
            rest = args[k + len(tok):]
            cuts = [rest.find(x) for x in (" --source ", " --sink ") if rest.find(x) >= 0]
            cut = min(cuts) if cuts else len(rest)
            args = args[:k] + f'{opt} "cmd:{rest[:cut]}"' + rest[cut:]
    files["serve.cmd"] = (f"@echo off\r\n{PCPY} {PCT}\\peer.py serve --key-file {PCT}\\test.key --duration {D + 5} --quiet "
                          f"--punch {MAC_IP}:{LPORT} --stats {pcrun}\\serve.jsonl {args} 2> {pcrun}\\serve.err\r\n")
    ps += [
        f"cmd /c {pcrun}\\serve.cmd",
        "foreach ($p in $h) { if (-not $p.WaitForExit(20000)) { Stop-Process -Id $p.Id -Force -EA SilentlyContinue } }",
        "Start-Sleep 2",
        f"cmd /c {pcrun}\\endpoints.cmd after",
        "Get-Process beamer-audio -EA SilentlyContinue | Where-Object { $_.Path -like '*audio-proto\\scenarios*' } | ForEach-Object { 'leftover beamer-audio pid ' + $_.Id }",
        "'pc script done'",
    ]
    files["run.ps1"] = "\r\n".join(ps) + "\r\n"
    return pcrun, files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenario")
    ap.add_argument("--name", default=None)
    ap.add_argument("--duration", type=float, default=75)
    ap.add_argument("--no-analyze", action="store_true")
    ap.add_argument("--mac-extra", default="", help="extra peer.py send options (Mac)")
    ap.add_argument("--pc-extra", default="", help="extra peer.py serve options (PC)")
    a = ap.parse_args()
    D = a.duration
    sc_all = scenarios(D)
    if a.scenario == "s5loop":
        sc = loop_variant(sc_all["s5"], True)
    elif a.scenario == "s5loopoff":
        sc = loop_variant(sc_all["s5"], False)
    else:
        sc = sc_all[a.scenario]
    name = a.name or f"{a.scenario}_{time.strftime('%H%M%S')}"
    run = os.path.join(HERE, "runs", name)
    os.makedirs(run, exist_ok=True)
    sub = lambda s: s.replace("@RUN@", run)  # noqa: E731
    meta = {"scenario": a.scenario, "name": name, "desc": sc["desc"], "duration": D, "pairs": sc.get("pairs", []),
            "loopcheck": sc.get("loopcheck", False), "looptest": sc.get("looptest", False),
            "started": time.strftime("%Y-%m-%d %H:%M:%S")}
    # PC side first: it listens and punches towards the Mac's fixed port
    sc = dict(sc)
    sc["pc_serve"] = f"--pkt-log @PCRUN@\\pkt {a.pc_extra} " + sc["pc_serve"]
    meta["mac_extra"], meta["pc_extra"] = a.mac_extra, a.pc_extra
    pcrun, files = pc_files(name, sc, D)
    os.makedirs(os.path.join(run, "pc_scripts"), exist_ok=True)
    for fn, body in files.items():
        open(os.path.join(run, "pc_scripts", fn), "w", newline="").write(body)
    subprocess.run(["ssh", "lebig", f"New-Item -ItemType Directory -Force {pcrun} | Out-Null"], check=True)
    subprocess.run(["scp", "-q"] + [os.path.join(run, "pc_scripts", fn) for fn in files]
                   + [f"lebig:{pcrun}/".replace("\\", "/")], check=True)
    pc = subprocess.Popen(["ssh", "lebig", f"powershell -NoProfile -ExecutionPolicy Bypass -File {pcrun}\\run.ps1"],
                          stdout=open(os.path.join(run, "pc_ssh.log"), "w"), stderr=subprocess.STDOUT)
    t_pc = time.monotonic()
    helpers = []
    if sc.get("afplay"):
        wav = os.path.join(run, "inject.wav")
        starts = write_wav(wav, sc["afplay"])
        meta["afplay"] = dict(sc["afplay"], markers=starts)
    time.sleep(3.0)
    for i, h in enumerate(sc.get("mac_helpers", [])):
        if h == "@AFPLAY@":
            sp = sc["afplay"]
            h = f"sleep {sp['delay']}; exec afplay {os.path.join(run, 'inject.wav')}"
        helpers.append(subprocess.Popen(sub(h), shell=True, stderr=open(os.path.join(run, f"helper{i}.err"), "w"),
                                        stdout=subprocess.DEVNULL))
    send = [PY, os.path.join(HERE, "pair.py"), os.path.join(run, "pings.json"), "send",
            "--to", f"{PC_IP}:24823", "--key-file", os.path.join(TR, "out", "test.key"), "--duration", str(D),
            "--quiet", "--stats", os.path.join(run, "send.jsonl"), "--local-port", str(LPORT), "--ping-s", "0.25"]
    send += ["--pkt-log", os.path.join(run, "pkt")] + shlex.split(a.mac_extra)
    send += [sub(x) for x in sc["mac_send"]]
    meta["mac_send"] = send
    meta["t_send_start"] = time.monotonic()
    rc = subprocess.run(send, stdout=open(os.path.join(run, "send.summary.json"), "w"),
                        stderr=open(os.path.join(run, "send.err"), "w")).returncode
    meta["send_rc"] = rc
    for h in helpers:
        try:
            h.wait(timeout=15)
        except subprocess.TimeoutExpired:
            h.terminate()
    pc.wait(timeout=120)
    meta["pc_elapsed"] = round(time.monotonic() - t_pc, 1)
    os.makedirs(os.path.join(run, "pc"), exist_ok=True)
    subprocess.run(["scp", "-q", "-r", f"lebig:{PCD}/runs/{name}/*".replace("\\", "/"), os.path.join(run, "pc") + "/"])
    json.dump(meta, open(os.path.join(run, "meta.json"), "w"), indent=1)
    print(f"run dir {run} send_rc={rc}")
    print(open(os.path.join(run, "pc_ssh.log")).read()[-1500:])
    if not a.no_analyze:
        subprocess.run([PY, os.path.join(HERE, "analyze.py"), run])


if __name__ == "__main__":
    main()
