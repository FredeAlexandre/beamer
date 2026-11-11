# Beamer Camera on Windows (C2 prototype, Phase 1)

A Windows 11 virtual camera made with `MFCreateVirtualCamera` and a custom Media Foundation media source,
fed by a user-session process through shared memory. Results and the Phase 2 recommendation are in
`dev/camera/reports/c2.md`.

| File | What |
|---|---|
| `common.h` | CLSID, kernel object names + DACL, shared-memory layout (3-slot NV12 ring with seqlocks), frame stamp (counter + wall-clock time in pixel blocks), test pattern, NV12 scale / YUY2 conversion |
| `source.cpp`, `source.def` | `BeamerCamC2.dll`: activate -> media source -> one shared stream (NV12 1280x720, 1920x1080, 640x360 @ 30). Runs in the Frame Server (`svchost -k Camera`, LocalService). Creates the ring, publishes Start/Stop and the app's media type, signals the helper; delivers the newest helper frame on each pending `RequestSample`, else a stamped test pattern / placeholder. Logs to `C:\ProgramData\BeamerCamC2\source.log`. Optional `config.txt` with `formats=nv12,yuy2` adds YUY2 types. |
| `vcamctl.cpp` | Registrar: `regcom hklm|hkcu <dll>`, `unregcom`, `create session|system user|all [--hold S]`, `remove ...`, `supported` |
| `camprobe.cpp` | Consumer: `list` (MF + DirectShow enumeration), `read` (IMFSourceReader, per-frame stamp check, latency, gaps, `--rgb`, `--no-set`, `--per-second`), `yuvcheck` (raw NV12 on stdin, e.g. from `ffmpeg -f dshow`) |
| `feeder.cpp` | Helper stand-in: `pattern` / `h264 FILE [--hw --staging]` produce on demand into the ring; `decbench` (MF H.264 decoder latency, SW / D3D11, low-latency on/off); `genyuv` (stamped raw NV12 to encode); `cpu PID S` |
| `web/gum.html` | `getUserMedia` consumer for headless Chrome/Edge, decodes the stamp per frame (`requestVideoFrameCallback`) |
| `scripts/*.ps1` | PC test drivers (`in-session.ps1` runs a command in Alex's session through a temporary scheduled task), `setup.ps1` / `cleanup.ps1` |

## Build and run (PC)

```bat
build.cmd            :: -> build\BeamerCamC2.dll, vcamctl.exe, camprobe.exe, feeder.exe (MSVC x64, static CRT)
```

The tests ran from `C:\Users\Alex\camera-c2\` (`src\` = this folder, `bin\` = the build output, scripts at the top).
`setup.ps1` (elevated) installs the camera; `cleanup.ps1` (elevated) removes everything. The DLL must live where
LocalService can read it (not under `C:\Users`), and its CLSID must be in HKLM.
