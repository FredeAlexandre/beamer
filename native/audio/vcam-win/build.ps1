# SPDX-License-Identifier: GPL-3.0-only
# Builds the Beamer Camera source DLL and its test tools (PHASE2 §2.5) with the newest MSVC (x64, static CRT):
#   <OutDir>\camera\BeamerCameraSource.dll       the media source Frame Server loads (installed by vcam-install)
#   <OutDir>\camprobe.exe, feeder.exe, shmfuzz.exe
# OutDir defaults to native\audio\build\win. Called by native\audio\win\build.ps1 (inside vcvars64); run alone
# it finds MSVC itself:  powershell -ExecutionPolicy Bypass -File native\audio\vcam-win\build.ps1 [-OutDir D] [-DllOnly]
param([string]$OutDir = '', [switch]$DllOnly)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path (Split-Path -Parent $here) 'build\win' }
$camDir = Join-Path $OutDir 'camera'
$objDir = Join-Path $OutDir 'obj-vcam'
New-Item -ItemType Directory -Force $OutDir, $camDir, $objDir | Out-Null

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    # Not inside vcvars64: import its environment.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere not found: install Visual Studio Build Tools with the C++ workload' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'MSVC x64 tools not found' }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
}

$flags = @('/nologo', '/std:c++20', '/O2', '/MT', '/W4', '/utf-8', '/EHsc', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS',
           '/wd4324', "/I$here")   # C4324: alignas(64) padding in shm_protocol.h, intended

function Build([string]$out, [string[]]$srcs, [string[]]$extra, [string[]]$libs) {
    Push-Location $objDir
    try {
        & cl.exe @flags @extra @srcs "/Fe:$out" /link @libs | Where-Object { $_ -and $_ -notmatch '^\S+\.cpp$' -and $_ -notmatch 'Creating library|Cr.ation de la biblioth' } | ForEach-Object { Write-Host $_ }
        if ($LASTEXITCODE -ne 0) { throw "build failed: $out" }
    } finally { Pop-Location }
    $deps = & dumpbin.exe /nologo /dependents $out | Where-Object { $_ -match '^\s*\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
    $bad = $deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt|concrt)' }
    if ($bad) { throw "$out imports the MSVC runtime: $($bad -join ', ')" }
    Write-Host ("built {0} ({1:N0} bytes; imports {2})" -f $out, (Get-Item $out).Length, ($deps -join ' '))
}

Build (Join-Path $camDir 'BeamerCameraSource.dll') @((Join-Path $here 'source.cpp')) @('/LD') `
    @("/DEF:$(Join-Path $here 'source.def')", 'mfplat.lib', 'mfuuid.lib', 'ole32.lib', 'advapi32.lib', 'windowscodecs.lib')
if ($DllOnly) { return }
$tools = Join-Path $here 'tools'
$toolLibs = @('mfplat.lib', 'mfuuid.lib', 'mf.lib', 'mfreadwrite.lib', 'mfsensorgroup.lib', 'strmiids.lib', 'ole32.lib', 'oleaut32.lib',
              'advapi32.lib', 'windowscodecs.lib', 'winmm.lib')
foreach ($t in 'camprobe', 'feeder', 'shmfuzz') {
    Build (Join-Path $OutDir "$t.exe") @((Join-Path $tools "$t.cpp")) @() $toolLibs
}
