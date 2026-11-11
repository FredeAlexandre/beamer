# H.264 file -> helper decode (feeder h264) -> ring -> source -> camprobe, at 720p and 1080p, HW and SW decode.
foreach ($c in @(@("720", "1280x720", "--hw --staging"), @("720", "1280x720", ""), @("1080", "1920x1080", "--hw --staging"), @("1080", "1920x1080", ""))) {
  $tag = "h264-$($c[0])-" + $(if ($c[2]) { "hw" } else { "sw" })
  "== $tag"
  powershell -NoProfile -ExecutionPolicy Bypass -File C:\Users\Alex\camera-c2\pipe.ps1 -Tag $tag -Size $c[1] -FeederArgs "h264 C:\Users\Alex\camera-c2\media\p$($c[0]).h264 --size $($c[1]) $($c[2])" |
    Select-String '"ok":true|cpu|stats|decoder'
}
