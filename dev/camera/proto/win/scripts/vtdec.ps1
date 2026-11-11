# Decode latency of C1's VideoToolbox recordings (run in Alex's session through in-session.ps1).
Set-Location C:\Users\Alex\camera-c2\media\c1
$f = "C:\Users\Alex\camera-c2\bin\feeder.exe"
& $f decbench synth_720p30_2500k_30s.h264 --size 1280x720 --frames 300 --hw --staging
& $f decbench synth_720p30_2500k_30s.h264 --size 1280x720 --frames 300
& $f decbench synth_1080p30_5000k_8s.h264 --size 1920x1080 --frames 240 --hw --staging
& $f decbench synth_1080p30_5000k_8s.h264 --size 1920x1080 --frames 240
