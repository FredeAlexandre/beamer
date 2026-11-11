# SPDX-License-Identifier: GPL-3.0-only
param([string]$Size = '1280x720')
$ff = 'C:\Users\Alex\AppData\Local\Microsoft\WinGet\Links\ffmpeg.exe'
$list = cmd /c "`"$ff`" -hide_banner -list_devices true -f dshow -i dummy 2>&1"
$i = [array]::FindIndex([string[]]$list, [Predicate[string]] { param($l) $l -match 'Beamer Camera' })
$alt = ([regex]::Match(($list[$i + 1]), 'Alternative name "(.*)"')).Groups[1].Value
"dshow device: $($list[$i].Trim())"
cmd /c "`"$ff`" -hide_banner -loglevel error -f dshow -video_size $Size -framerate 30 -pixel_format nv12 -rtbufsize 10M -fflags nobuffer -analyzeduration 0 -probesize 32 -i video=`"$alt`" -frames:v 300 -fps_mode passthrough -f rawvideo -pix_fmt nv12 - | C:\Users\Alex\camera-cw5\bin\camprobe.exe yuvcheck - $Size dshow$Size"
