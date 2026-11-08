$ErrorActionPreference = 'Stop'

# Over SSH there is nobody to press Enter and no desktop to open a window on: Read-Host fails on
# the null stdin, and a Start-Process lands Beamer in session 0, where it is an invisible,
# tray-less receiver that also holds the SSH pipe open so the install never returns.
$Interactive = [Environment]::UserInteractive -and -not $env:SSH_CLIENT

trap {
    Write-Host $_ -ForegroundColor Red
    if ($Interactive) { Read-Host 'Press Enter to close' }
    break
}

$PowerShellPath = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$MaintenanceLauncher = Join-Path $PSScriptRoot 'Invoke-InstallMaintenance.ps1'
& $PowerShellPath -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $MaintenanceLauncher -CheckIdentityOnly
if ($LASTEXITCODE -ne 0) { throw 'The installer must run for the signed-in user.' }

$InstallDir = Join-Path $env:LOCALAPPDATA 'Beamer'
$ExeSource = Join-Path $PSScriptRoot 'dist\Beamer.exe'
$ExeDestination = Join-Path $InstallDir 'Beamer.exe'
$HelperSource = Join-Path (Split-Path -Parent $PSScriptRoot) 'native\audio\build\win\beamer-audio.exe'

# Copies with retries: an exe can stay locked for a moment after its process has gone, so the copy
# can fail with "being used by another process" straight after Wait-Process returns.
function Copy-WithRetry {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$Destination)
    for ($attempt = 1; ; $attempt++) {
        try {
            Copy-Item -Path $Source -Destination $Destination -Force -ErrorAction Stop
            break
        } catch [System.IO.IOException] {
            if ($attempt -ge 10) { throw }
            Start-Sleep -Milliseconds 500
        }
    }
}

# Audio (PHASE2 10.2): the helper beside Beamer.exe, where core\audio\helper.py looks for it. A
# running copy from this folder is stopped first (it also exits by itself within a second of
# Beamer). Then `beamer-audio.exe restore` replays its mute journal once, so an endpoint a crashed
# helper left muted is restored now rather than at Beamer's next start. A build without the helper
# removes the installed one, so the folder always matches the commit.
function Install-BeamerAudioHelper {
    param([Parameter(Mandatory = $true)][string]$Source, [Parameter(Mandatory = $true)][string]$InstallDir,
          [int]$RestoreTimeoutMs = 15000)
    $Destination = Join-Path $InstallDir 'beamer-audio.exe'
    $Running = @(Get-Process 'beamer-audio' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $Destination })
    $Running | Stop-Process -Force
    $Running | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    if (-not (Test-Path $Source)) {
        if (Test-Path $Destination) {
            Remove-Item -Path $Destination -Force
            Write-Host "This build has no audio helper: removed $Destination"
        }
        return
    }
    Copy-WithRetry -Source $Source -Destination $Destination
    # Bounded: a helper that hangs must not hang the install. Failing here is only a warning, since
    # every helper start replays the journal again before it reports ready.
    try {
        $Restore = Start-Process -FilePath $Destination -ArgumentList 'restore' -WorkingDirectory $InstallDir `
            -NoNewWindow -PassThru -RedirectStandardOutput (Join-Path $env:TEMP 'beamer-audio-restore.out') `
            -RedirectStandardError (Join-Path $env:TEMP 'beamer-audio-restore.err')
        $null = $Restore.Handle  # keeps the exit code readable after the process has gone
        if (-not $Restore.WaitForExit($RestoreTimeoutMs)) {
            $Restore | Stop-Process -Force -ErrorAction SilentlyContinue
            $Problem = "did not finish within $($RestoreTimeoutMs / 1000) s"
        } elseif ($Restore.ExitCode -ne 0) {
            $Problem = "exited with $($Restore.ExitCode)"
        } else {
            $Problem = $null
        }
    } catch {
        $Problem = "could not run: $_"
    }
    if ($Problem) {
        Write-Warning "Installed $Destination, but beamer-audio.exe restore $Problem; its next start replays the mute journal"
    } else {
        Write-Host "Installed $Destination (mute journal restored)"
    }
}

& "$PSScriptRoot\build_win_app.ps1"
if (-not (Test-Path $ExeSource)) { throw 'The Windows build did not produce Beamer.exe' }

New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
$InstalledProcesses = @(Get-Process 'Beamer' -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $ExeDestination })
$InstalledProcesses | Stop-Process -Force
$InstalledProcesses | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
Copy-WithRetry -Source $ExeSource -Destination $ExeDestination
Install-BeamerAudioHelper -Source $HelperSource -InstallDir $InstallDir

$Shell = New-Object -ComObject WScript.Shell
$StartMenuPath = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Beamer.lnk'
$StartMenuShortcut = $Shell.CreateShortcut($StartMenuPath)
$StartMenuShortcut.TargetPath = $ExeDestination
$StartMenuShortcut.WorkingDirectory = $InstallDir
$StartMenuShortcut.Description = 'Beamer Windows Receiver'
$StartMenuShortcut.Save()

# No installer writes a desktop shortcut; delete any this or an older install left.
Remove-Item -Path (Join-Path ([Environment]::GetFolderPath('Desktop')) 'Beamer.lnk') -Force -ErrorAction SilentlyContinue

# Remove pre-task startup entries so there is only one starter.
$StartupPath = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Startup\Beamer.lnk'
Remove-Item -Path $StartupPath -Force -ErrorAction SilentlyContinue

# Defensively remove a legacy Run-key entry too, in case an older install used one instead.
Remove-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'Beamer' -ErrorAction SilentlyContinue

& $PowerShellPath -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $MaintenanceLauncher -ExePath $ExeDestination
if ($LASTEXITCODE -ne 0) { throw 'Beamer was installed, but administrator maintenance did not complete.' }

if ($Interactive) {
    Write-Host "Beamer installed at $ExeDestination" -ForegroundColor Green
    Read-Host 'Press Enter to close'
} else {
    Write-Host "Beamer installed at $ExeDestination and not started; start it at the PC." -ForegroundColor Green
}
