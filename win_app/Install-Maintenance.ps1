[CmdletBinding()]
param(
    [string]$ExePath,
    [string]$UserSid,
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

function Resolve-BeamerUserSid([string]$Identity) {
    if ($Identity -match '^S-1-') {
        return ([Security.Principal.SecurityIdentifier]$Identity).Value
    }
    return ([Security.Principal.NTAccount]$Identity).Translate([Security.Principal.SecurityIdentifier]).Value
}

function Get-BeamerAdministratorPreference([string]$UserSid) {
    $key = [Microsoft.Win32.Registry]::Users.OpenSubKey("$UserSid\Software\KalkmanCode\Beamer")
    if ($null -eq $key) { return $false }
    try {
        if ($key.GetValueKind('RunAsAdministrator') -ne [Microsoft.Win32.RegistryValueKind]::DWord) { return $false }
        return $key.GetValue('RunAsAdministrator', 0) -eq 1
    } catch [System.IO.IOException] {
        return $false
    } finally {
        $key.Dispose()
    }
}

function Update-BeamerAutostart([string]$ExePath, [string]$UserSid, [switch]$Remove) {
    $expectedPath = [IO.Path]::GetFullPath($ExePath)
    $runLevel = if (-not $Remove -and (Get-BeamerAdministratorPreference -UserSid $UserSid)) { 'HighestAvailable' } else { 'LeastPrivilege' }
    foreach ($task in @(Get-ScheduledTask -TaskPath '\' -ErrorAction Stop)) {
        if ($task.TaskName -ne 'Beamer' -and $task.TaskName -notlike 'Beamer-*') { continue }
        [xml]$xml = Export-ScheduledTask -TaskName $task.TaskName -TaskPath '\' -ErrorAction Stop
        $ns = New-Object Xml.XmlNamespaceManager($xml.NameTable)
        $ns.AddNamespace('t', 'http://schemas.microsoft.com/windows/2004/02/mit/task')
        $principals = $xml.SelectNodes('/t:Task/t:Principals/t:Principal', $ns)
        $actions = $xml.SelectNodes('/t:Task/t:Actions/*', $ns)
        if ($principals.Count -ne 1 -or $actions.Count -ne 1 -or $actions[0].LocalName -ne 'Exec') { continue }
        $principal = $principals[0]
        if ($principal.LogonType -ne 'InteractiveToken') { continue }
        try {
            if ((Resolve-BeamerUserSid $principal.UserId) -ne $UserSid) { continue }
            $command = [string]$actions[0].Command
            if (-not [IO.Path]::IsPathRooted($command)) { continue }
            if (-not [string]::Equals([IO.Path]::GetFullPath($command), $expectedPath, [StringComparison]::OrdinalIgnoreCase)) { continue }
        } catch { continue }
        if ($Remove) {
            Unregister-ScheduledTask -TaskName $task.TaskName -TaskPath '\' -Confirm:$false -ErrorAction Stop
            continue
        }
        $runLevelNode = $principal.SelectSingleNode('t:RunLevel', $ns)
        $currentRunLevel = if ($null -eq $runLevelNode) { 'LeastPrivilege' } else { $runLevelNode.InnerText }
        if ($currentRunLevel -eq $runLevel) { continue }
        # Keep the existing task definition: disabled startup, arguments and logon triggers are user choices.
        if ($null -eq $runLevelNode) {
            $runLevelNode = $xml.CreateElement('RunLevel', $principal.NamespaceURI)
            [void]$principal.AppendChild($runLevelNode)
        }
        $runLevelNode.InnerText = $runLevel
        Register-ScheduledTask -TaskName $task.TaskName -TaskPath '\' -Xml $xml.OuterXml -Force -ErrorAction Stop | Out-Null
    }
}

function Remove-BeamerFirewall([string]$ExePath) {
    foreach ($name in @('Beamer Receiver (TCP-In)', 'Beamer Pairing (UDP-In)', 'Beamer Pairing (TCP-In)')) {
        foreach ($rule in @(Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue)) {
            $filter = $rule | Get-NetFirewallApplicationFilter -ErrorAction Stop
            if ([string]::Equals($filter.Program, $ExePath, [StringComparison]::OrdinalIgnoreCase)) {
                $rule | Remove-NetFirewallRule -ErrorAction Stop
            }
        }
    }
}

function Repair-BeamerFirewall([string]$ExePath) {
    Remove-BeamerFirewall -ExePath $ExePath
    $rules = @(
        @{ Name = 'Beamer Receiver (TCP-In)'; Protocol = 'TCP'; Port = 24820 },
        @{ Name = 'Beamer Pairing (UDP-In)'; Protocol = 'UDP'; Port = 24821 },
        @{ Name = 'Beamer Pairing (TCP-In)'; Protocol = 'TCP'; Port = 24821 }
    )
    foreach ($rule in $rules) {
        New-NetFirewallRule -DisplayName $rule.Name -Direction Inbound -Protocol $rule.Protocol -LocalPort $rule.Port -Action Allow -Profile Private -Program $ExePath -ErrorAction Stop | Out-Null
    }
}

function Repair-BeamerGameInput {
    if (Get-Service -Name 'GameInputSvc' -ErrorAction SilentlyContinue) {
        try {
            Restart-Service -Name 'GameInputSvc' -ErrorAction Stop
        } catch {
            # The receiver can still run; its remote controller reports when a local click is needed.
            Write-Warning 'GameInput could not be restarted. A local click may be needed to release remote controller focus.'
        }
    }
}

function Update-BeamerGameInputRecovery([switch]$Remove) {
    $taskName = 'Beamer-GameInputRecovery'
    if ($Remove) {
        if (Get-ScheduledTask -TaskName $taskName -TaskPath '\' -ErrorAction SilentlyContinue) {
            Unregister-ScheduledTask -TaskName $taskName -TaskPath '\' -Confirm:$false -ErrorAction Stop
        }
        return
    }

    $scheduler = New-Object -ComObject 'Schedule.Service'
    $scheduler.Connect()
    $folder = $scheduler.GetFolder('\')
    $definition = $scheduler.NewTask(0)
    $definition.RegistrationInfo.Description = 'Restarts GameInput when its service window blocks remote pointer control.'
    $definition.Principal.UserId = 'S-1-5-18'
    $definition.Principal.LogonType = 5
    $definition.Principal.RunLevel = 1
    $definition.Settings.Enabled = $true
    $definition.Settings.AllowDemandStart = $true
    $definition.Settings.DisallowStartIfOnBatteries = $false
    $definition.Settings.StopIfGoingOnBatteries = $false
    $definition.Settings.ExecutionTimeLimit = 'PT30S'
    $definition.Settings.MultipleInstances = 2
    $windows = [Environment]::GetFolderPath([Environment+SpecialFolder]::Windows)
    $action = $definition.Actions.Create(0)
    $action.Path = Join-Path $windows 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $action.Arguments = '-NoLogo -NoProfile -NonInteractive -Command "Restart-Service -Name ''GameInputSvc'' -ErrorAction Stop"'
    $action.WorkingDirectory = Join-Path $windows 'System32'
    # Set the protected ACL at registration: no interval with an elevated, user-writable task.
    # Owner is Administrators, not SYSTEM: a UAC-elevated token cannot assign SYSTEM as owner
    # (0xD000005A, STATUS_INVALID_OWNER). An SSH admin token can, so SSH tests never caught it.
    $sddl = 'O:BAG:SYD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;IU)'
    $folder.RegisterTaskDefinition($taskName, $definition, (0x6 -bor 0x10), 'S-1-5-18', $null, 5, $sddl) | Out-Null
}

# Beamer Camera (camera PHASE2 §9.2). `beamer-audio.exe vcam-install` (elevated) puts the source DLL in
# %ProgramFiles%\Beamer\Camera\<version>\, its log folder in %ProgramData%\Beamer\Camera, the COM class in
# HKLM and HKLM\SOFTWARE\Beamer\Camera. Only an elevated process can delete those, so every elevated
# maintenance run clears what is stale and the uninstall run removes the lot. The uninstaller has already
# run `beamer-audio.exe vcam-remove` (unelevated) to Remove() the camera itself.
$BeamerCameraClsid = '{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}'

function Get-BeamerCameraPaths {
    return @{
        ClsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$BeamerCameraClsid"
        CameraKey = 'HKLM:\SOFTWARE\Beamer\Camera'
        ProgramFolder = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)) 'Beamer\Camera'
        DataFolder = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)) 'Beamer\Camera'
    }
}

# Deletes a folder or registry key, then its `Beamer` parent when that is left empty. Returns $false when
# it could not be deleted (a file in use: Frame Server holds the DLL it loaded), $true when it is gone.
function Remove-BeamerCameraItem([string]$Path) {
    if (Test-Path -LiteralPath $Path) {
        try { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop } catch { }
        if (Test-Path -LiteralPath $Path) { return $false }
    }
    $parent = Split-Path -Parent $Path
    if ((Split-Path -Leaf $parent) -eq 'Beamer' -and (Test-Path -LiteralPath $parent) -and
        -not (Get-ChildItem -LiteralPath $parent -Force -ErrorAction SilentlyContinue) -and
        -not (Get-Item -LiteralPath $parent -Force).Property) {
        Remove-Item -LiteralPath $parent -Force -ErrorAction SilentlyContinue
    }
    return $true
}

function Remove-BeamerCamera([hashtable]$Paths = (Get-BeamerCameraPaths)) {
    foreach ($key in @($Paths.ClsidKey, $Paths.CameraKey)) {
        if (-not (Remove-BeamerCameraItem $key)) { throw "Could not delete $key" }
    }
    $locked = @()
    foreach ($folder in @($Paths.ProgramFolder, $Paths.DataFolder)) {
        if (-not (Remove-BeamerCameraItem $folder)) { $locked += $folder }
    }
    if (-not $locked) { return }
    # Frame Server keeps the DLL (and its log) open while a Beamer Camera source object lives. Stopping
    # it pauses every camera for about a second; Windows starts it again at the next camera use.
    Stop-Service -Name 'FrameServer' -Force -ErrorAction Stop
    foreach ($folder in $locked) {
        for ($attempt = 1; -not (Remove-BeamerCameraItem $folder); $attempt++) {
            if ($attempt -ge 10) { throw "Could not delete $folder" }
            Start-Sleep -Milliseconds 500
        }
    }
}

# Install and repair: version folders other than the registered one are left over from updates whose old
# DLL Frame Server still held. Never stops Frame Server: a folder still in use waits for the next run.
function Remove-BeamerCameraStaleVersions([hashtable]$Paths = (Get-BeamerCameraPaths)) {
    if (-not (Test-Path -LiteralPath $Paths.ProgramFolder) -or -not (Test-Path -LiteralPath $Paths.CameraKey)) { return }
    $current = (Get-ItemProperty -LiteralPath $Paths.CameraKey -ErrorAction SilentlyContinue).DllPath
    if (-not $current) { return }
    $currentFolder = [IO.Path]::GetFullPath((Split-Path -Parent $current))
    foreach ($folder in @(Get-ChildItem -LiteralPath $Paths.ProgramFolder -Directory -Force)) {
        if ([string]::Equals([IO.Path]::GetFullPath($folder.FullName), $currentFolder, [StringComparison]::OrdinalIgnoreCase)) { continue }
        try {
            Remove-Item -LiteralPath $folder.FullName -Recurse -Force -ErrorAction Stop
        } catch {
            Write-Warning "Beamer Camera: $($folder.FullName) is still in use; the next maintenance run deletes it."
        }
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
        $principal = New-Object Security.Principal.WindowsPrincipal($identity)
        if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
            throw 'Installer maintenance requires administrator permission.'
        }
        if (-not [IO.Path]::IsPathRooted($ExePath) -or [IO.Path]::GetFileName($ExePath) -ine 'Beamer.exe' -or -not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
            throw 'Installer maintenance requires the installed Beamer.exe path.'
        }
        $ExePath = [IO.Path]::GetFullPath($ExePath)
        $UserSid = ([Security.Principal.SecurityIdentifier]$UserSid).Value
        Update-BeamerAutostart -ExePath $ExePath -UserSid $UserSid -Remove:$Remove
        if ($Remove) {
            Update-BeamerGameInputRecovery -Remove
            Remove-BeamerFirewall -ExePath $ExePath
            Remove-BeamerCamera
        } else {
            Repair-BeamerFirewall -ExePath $ExePath
            Update-BeamerGameInputRecovery
            Repair-BeamerGameInput
            # Leftover camera versions are harmless (~200 KB each): never a reason to fail an install.
            try { Remove-BeamerCameraStaleVersions } catch { Write-Warning "Beamer Camera: stale versions not cleared: $_" }
        }
        exit 0
    } catch {
        Write-Error $_ -ErrorAction Continue
        exit 1
    }
}
