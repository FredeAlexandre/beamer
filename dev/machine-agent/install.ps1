# Builds the PC machine agent, installs it outside Beamer's install, and registers the scheduled
# task that starts it in the signed-in desktop session (#48, ADR 0005). It never ships: nothing
# that builds or installs Beamer reads this.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File dev\machine-agent\install.ps1
#   Start-ScheduledTask -TaskName BeamerDev-MachineAgent      # start it (the harness does this)
#
# SSH lands in session 0, which has no desktop, so the agent is never started over SSH with
# Start-Process: the task, an Interactive one for the signed-in user, starts it in their session.
# The task has no trigger; it runs only when started, and the agent quits after 15 idle minutes.
# Prints the install directory.
$ErrorActionPreference = 'Stop'

$Here = $PSScriptRoot
$Dir = Join-Path $env:LOCALAPPDATA 'BeamerDev\machine-agent'
$Task = 'BeamerDev-MachineAgent'
$Port = 24870

# Not redirected: Windows PowerShell turns a redirected native stderr line into an error, and
# $ErrorActionPreference would stop on cargo's first warning.
cargo build --release --quiet --manifest-path "$Here\Cargo.toml"
if ($LASTEXITCODE -ne 0) { throw "cargo build failed" }

# A running agent holds its .exe open: ask it to quit, then stop its task if it has not.
try {
    $client = [Net.Sockets.TcpClient]::new('127.0.0.1', $Port)
    $writer = [IO.StreamWriter]::new($client.GetStream())
    $writer.Write("{`"op`":`"quit`"}`n")
    $writer.Flush()
    [void][IO.StreamReader]::new($client.GetStream()).ReadLine()
    $client.Close()
} catch { }
$deadline = (Get-Date).AddSeconds(5)
while ((Get-Process machine-agent-windowless -ErrorAction SilentlyContinue) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
if (Get-ScheduledTask -TaskName $Task -ErrorAction SilentlyContinue) { Stop-ScheduledTask -TaskName $Task }

New-Item -ItemType Directory -Force -Path $Dir | Out-Null
foreach ($exe in 'machine-agent.exe', 'machine-agent-windowless.exe') {
    Copy-Item -Force "$Here\target\release\$exe" "$Dir\$exe"
}

$action = New-ScheduledTaskAction -Execute "$Dir\machine-agent-windowless.exe" `
    -Argument "serve --log `"$Dir\machine-agent.log`"" -WorkingDirectory $Dir
$principal = New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Limited
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Hours 12)
Register-ScheduledTask -TaskName $Task -Action $action -Principal $principal -Settings $settings -Force `
    -Description 'Beamer dev machine agent (dev/machine-agent): posts real input for the machine test tier. Not part of Beamer.' | Out-Null
$Dir
