# CW5 acceptance drivers (PC)

The PowerShell drivers behind `dev/camera/reports/cw5.md`, as run from the Mac over `ssh lebig` (elevated,
session 0). They assume the work-folder layout used there: `C:\Users\Alex\camera-cw5\{src,bin,run,scripts,web}`
(`src` = this `vcam-win` folder, `bin` = `build.ps1 -OutDir` output, `scripts` = these files, `web` =
`dev/camera/proto/win/web`), the DLL installed with `tools\devinstall.ps1` and the camera created with
`feeder vcam create`. `common.ps1` runs tools in the signed-in user's session through `tools\insession.ps1`.

Run one with `powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-cw5\scripts\<name>.ps1`.

| Driver | Checks |
|---|---|
| `t_sizes.ps1` | 60 s at each offered size, gaps, feeder -> app latency, Frame Server CPU at 1080p |
| `t_thresh.ps1` | built-in image after the beat stops; waiting -> slot 3 |
| `t_security.ps1`, `t_squat2.ps1` | DACLs, squatted `.Frame` (closed and open DACL), inert user-process load, validation leaves no log |
| `t_storm.ps1` | 20 quick opens/closes: state_seq, repeated stops, Frame Server alive |
| `t_consumers.ps1` (+ `cons_dshow.ps1`, `cons_browser.ps1`) | DirectShow via ffmpeg, headless Chrome and Edge |
| `t_fuzz.ps1` | `shmfuzz` 10 min at 1 kHz from the user's session while `camprobe` reads |
| `t_update.ps1`, `t_idleexit.ps1` | re-point the CLSID to a new version folder; which DLL Frame Server runs, idle and after a restart; when Frame Server exits |
| `t_cleanup.ps1` | remove the camera, class, keys, folders, tasks; verify |
