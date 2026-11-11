# DirectShow consumer: ffmpeg -f dshow reads Beamer Camera, pipes raw NV12 to camprobe yuvcheck (live stamp check).
param([string]$Size = "1280x720", [int]$Frames = 300, [string]$Fmt = "nv12", [string]$Tag = "dshow")
Set-Location C:\Users\Alex\camera-c2\bin
$ff = "C:\Users\Alex\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe"
$list = cmd /c "`"$ff`" -hide_banner -list_devices true -f dshow -i dummy 2>&1"
$i = [array]::FindIndex([string[]]$list, [Predicate[string]] { param($l) $l -match "Beamer Camera" })
$alt = ([regex]::Match(($list[$i + 1]), 'Alternative name "(.*)"')).Groups[1].Value
"device: $alt"
cmd /c "`"$ff`" -hide_banner -loglevel warning -f dshow -video_size $Size -framerate 30 -pixel_format $Fmt -rtbufsize 10M -fflags nobuffer -analyzeduration 0 -probesize 32 -i video=`"$alt`" -frames:v $Frames -fps_mode passthrough -f rawvideo -pix_fmt nv12 - | camprobe.exe yuvcheck - $Size $Tag"
