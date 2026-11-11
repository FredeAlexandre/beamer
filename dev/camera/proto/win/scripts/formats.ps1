# What the camera offers: MF with RGB32 conversion by the source reader, and the DirectShow pin formats.
Set-Location C:\Users\Alex\camera-c2\bin
.\camprobe.exe read --rgb --size 1280x720 --seconds 3 --tag rgb | Select-String '"ok"'
$ff = "C:\Users\Alex\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-8.1.1-full_build\bin\ffmpeg.exe"
$list = cmd /c "`"$ff`" -hide_banner -list_devices true -f dshow -i dummy 2>&1"
$line = ($list | Select-String -Pattern 'Alternative name .*vcamdevapi' | Select -First 1).Line
$alt = ([regex]::Match($line, 'Alternative name "(.*)"')).Groups[1].Value
cmd /c "`"$ff`" -hide_banner -list_options true -f dshow -i video=`"$alt`" 2>&1" | Select-String 'pixel_format|vcodec' | % { ($_.Line -replace '^.*\] ', '').Trim() } | Sort-Object -Unique
