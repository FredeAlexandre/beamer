"""Passive baseline sampler, Mac side. Writes JSON lines to OUT.

- clock: PC wall clock vs Mac wall clock over one persistent ssh session, every 1 s
  (offset = pc - midpoint(mac send, mac receive); error <= rtt/2).
- mac: cumulative CPU seconds and RSS of Beamer processes every 5 s.
- pc: cumulative CPU seconds, working set and private bytes of Beamer processes every 5 s.
"""
import json, os, re, subprocess, sys, threading, time

OUT = sys.argv[1]
SECONDS = float(sys.argv[2])
lock = threading.Lock()
out = open(OUT, "a", buffering=1)
stop = time.time() + SECONDS


def emit(rec):
    with lock:
        out.write(json.dumps(rec) + "\n")


PC_PY = r"C:\Users\Alex\Developer\beamer\win_app\.venv\Scripts\python.exe"
SERVE = "import sys,time\nfor l in sys.stdin:\n    sys.stdout.write(l.strip()+' '+repr(time.time())+'\\n'); sys.stdout.flush()\n"


def clock_link():
    # base64 avoids quoting the server through PowerShell. Reconnects when a reply is late.
    import base64, select
    b = base64.b64encode(SERVE.encode()).decode()
    cmd = f"& '{PC_PY}' -u -c \"import base64;exec(base64.b64decode('{b}'))\""
    n = 0
    while time.time() < stop:
        p = subprocess.Popen(["ssh", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5", "lebig", cmd],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, bufsize=0)
        emit({"k": "clock_connect", "t": time.time()})
        buf = b""
        try:
            while time.time() < stop:
                n += 1
                t0 = time.time()
                p.stdin.write(f"{n}\n".encode())
                deadline = t0 + 5
                while b"\n" not in buf:
                    left = deadline - time.time()
                    if left <= 0 or not select.select([p.stdout], [], [], left)[0]:
                        raise TimeoutError
                    chunk = p.stdout.read1(4096) if hasattr(p.stdout, "read1") else os.read(p.stdout.fileno(), 4096)
                    if not chunk:
                        raise EOFError
                    buf += chunk
                t1 = time.time()
                lines_, buf = buf.split(b"\n"), b""
                for raw in lines_:
                    line = raw.decode().split()
                    if len(line) == 2 and line[0] == str(n):
                        pc = float(line[1])
                        emit({"k": "clock", "t0": t0, "t1": t1, "pc": pc, "off": pc - (t0 + t1) / 2, "rtt": t1 - t0})
                time.sleep(1.0)
        except (TimeoutError, EOFError, OSError, ValueError):
            emit({"k": "clock_lost", "t": time.time()})
        p.kill()
        time.sleep(2)


def cpu_secs(s):
    # ps TIME: [[dd-]hh:]mm:ss.cc
    parts = s.replace("-", ":").split(":")
    total = 0.0
    for x in parts:
        total = total * 60 + float(x)
    return total


def mac():
    while time.time() < stop:
        t = time.time()
        r = subprocess.run(["ps", "-axo", "pid=,time=,rss=,comm="], capture_output=True, text=True).stdout
        procs = []
        for line in r.splitlines():
            m = re.match(r"\s*(\d+)\s+(\S+)\s+(\d+)\s+(.*)", line)
            if m and re.search(r"Beamer\.app|BeamerAudio|VTEncoderXPCService|coreaudiod", m.group(4)):
                procs.append({"pid": int(m.group(1)), "cpu_s": cpu_secs(m.group(2)), "rss_kb": int(m.group(3)),
                              "name": m.group(4).rsplit("/", 1)[-1]})
        emit({"k": "mac", "t": t, "procs": procs})
        time.sleep(5.0)


PS = r"""
while ($true) {
  $t = [double]([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()) / 1000
  $p = Get-Process | Where-Object { $_.ProcessName -match '^(Beamer|beamer-audio|audiodg)$' } |
    ForEach-Object { @{pid=$_.Id; name=$_.ProcessName; cpu_s=$_.TotalProcessorTime.TotalSeconds; ws=$_.WorkingSet64; priv=$_.PrivateMemorySize64} }
  Write-Output (ConvertTo-Json -Compress -Depth 3 @{t=$t; procs=@($p)})
  Start-Sleep -Seconds 5
}
"""


def pc():
    import base64, select
    enc = base64.b64encode(PS.encode("utf-16-le")).decode()
    while time.time() < stop:
        p = subprocess.Popen(["ssh", "-o", "ConnectTimeout=10", "-o", "ServerAliveInterval=5", "lebig",
                              f"powershell -NoProfile -EncodedCommand {enc}"], stdout=subprocess.PIPE, text=True)
        while time.time() < stop:
            if not select.select([p.stdout], [], [], 20)[0]:
                break
            line = p.stdout.readline()
            if not line:
                break
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            rec["k"] = "pc"
            emit(rec)
        emit({"k": "pc_lost", "t": time.time()})
        p.kill()
        time.sleep(2)


threads = [threading.Thread(target=f, daemon=True) for f in (clock_link, mac, pc)]
for th in threads:
    th.start()
for th in threads:
    th.join()
