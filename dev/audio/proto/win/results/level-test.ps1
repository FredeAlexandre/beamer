# Gain of the render paths: tone through each path, measured raw on the loopback's FL channel.
# Run from dev\audio\proto\win.
$exe = "build\beamer-audio.exe"
$cases = @(
    @{ label = "direct_8ch_amp0.2";  args = "--amp 0.2" },
    @{ label = "direct_8ch_amp0.05"; args = "--amp 0.05" },
    @{ label = "direct_8ch_amp0.5";  args = "--amp 0.5" },
    @{ label = "wasapi_src_amp0.2";  args = "--amp 0.2 --force-src" },
    @{ label = "wasapi_src_amp0.05"; args = "--amp 0.05 --force-src" }
)
foreach ($c in $cases) {
    $p = Start-Process -FilePath $exe -ArgumentList "measure tone --seconds 4 $($c.args)" -PassThru -NoNewWindow
    Start-Sleep -Milliseconds 700
    & cmd /c "$exe measure level --seconds 2 --label $($c.label)"
    $p.WaitForExit()
}
