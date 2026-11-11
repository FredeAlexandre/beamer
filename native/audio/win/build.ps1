# SPDX-License-Identifier: GPL-3.0-only
# Called by native\audio\build-win.cmd inside a vcvars64 environment. See that file for what it builds.
param([switch]$clean, [switch]$checkOnly)
foreach ($a in $args) { if ($a -eq '--clean') { $clean = $true } elseif ($a -eq '--check-only') { $checkOnly = $true } }
$ErrorActionPreference = 'Stop'

$audio = Split-Path -Parent $PSScriptRoot          # native\audio
$win = $PSScriptRoot
$engine = Join-Path $audio 'engine'
$third = Join-Path $audio 'third_party'
$build = Join-Path $audio 'build'
$outDir = Join-Path $build 'win'
$objDir = Join-Path $build 'obj\win-x64'
if ($clean -and (Test-Path $objDir)) { Remove-Item -Recurse -Force $objDir }
New-Item -ItemType Directory -Force $outDir, $objDir | Out-Null
# Build output is never committed (the repo-level ignore entry is WP2's; this keeps the tree clean meanwhile).
Set-Content -Path (Join-Path $build '.gitignore') -Value '*' -Encoding ascii

$cxxFlags = @('/nologo', '/c', '/std:c++20', '/O2', '/MT', '/W4', '/utf-8', '/EHsc', '/DUNICODE', '/D_UNICODE', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX', '/D_CRT_SECURE_NO_WARNINGS')
$cFlags = @('/nologo', '/c', '/O2', '/MT', '/W3', '/utf-8', '/D_CRT_SECURE_NO_WARNINGS')
$libs = @('ole32.lib', 'avrt.lib', 'propsys.lib', 'cfgmgr32.lib', 'shell32.lib', 'advapi32.lib', 'mmdevapi.lib', 'ws2_32.lib', 'user32.lib')

# Newest header that a C++ object depends on (backend + engine headers): an object older than it is rebuilt.
function NewestHeader([string[]]$dirs) {
    $t = [datetime]::MinValue
    foreach ($d in $dirs) {
        if (Test-Path $d) {
            Get-ChildItem -Path $d -Recurse -Include *.h, *.hpp, *.inc -File | ForEach-Object { if ($_.LastWriteTime -gt $t) { $t = $_.LastWriteTime } }
        }
    }
    return $t
}

# Compiles $src to an object under $objDir\$tag; returns the object path.
function Compile([string]$src, [string]$tag, [string[]]$flags, [datetime]$hdrTime) {
    $rel = (Resolve-Path -LiteralPath $src).Path.Substring($audio.Length).TrimStart('\') -replace '[\\/:]', '_'
    $obj = Join-Path $objDir ($tag + '_' + [IO.Path]::ChangeExtension($rel, '.obj'))
    $srcTime = (Get-Item -LiteralPath $src).LastWriteTime
    if ((Test-Path $obj) -and (Get-Item $obj).LastWriteTime -ge $srcTime -and (Get-Item $obj).LastWriteTime -ge $hdrTime) { return $obj }
    & cl.exe @flags "/Fo$obj" $src | Where-Object { $_ -and $_ -notmatch '^\S+\.(c|cpp)$' } | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $src" }
    return $obj
}

function Link([string]$exe, [string[]]$objs, [string[]]$extra = @()) {
    & link.exe /nologo /SUBSYSTEM:CONSOLE /MACHINE:X64 "/OUT:$exe" @objs @libs @extra | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "link failed: $exe" }
    # Static x64 exe: no MSVC runtime DLLs (vcruntime/msvcp/ucrt) may appear among its imports.
    $deps = & dumpbin.exe /nologo /dependents $exe | Where-Object { $_ -match '\.dll' } | ForEach-Object { $_.Trim() }
    $bad = $deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt|concrt)' }
    if ($bad) { throw "$exe imports the MSVC runtime: $($bad -join ', ')" }
    $machine = & dumpbin.exe /nologo /headers $exe | Select-String 'machine \(x64\)'
    if (-not $machine) { throw "$exe is not x64" }
    Write-Host ("built {0} ({1:N0} bytes; imports {2})" -f $exe, (Get-Item $exe).Length, ($deps -join ' '))
}

# ---------------------------------------------------------------- backend + harness
$backendSrc = @('backend_win.cpp', 'devices_win.cpp', 'loopback_win.cpp', 'mute_journal_win.cpp', 'sessions_win.cpp') | ForEach-Object { Join-Path $win $_ }
$incEngine = @("/I$audio")
$hdr = NewestHeader @($win, $engine)
$backendObjs = @()
foreach ($s in $backendSrc) { $backendObjs += Compile $s 'win' ($cxxFlags + $incEngine) $hdr }
$checkObj = Compile (Join-Path $win 'backend_check.cpp') 'win' ($cxxFlags + $incEngine) $hdr
Link (Join-Path $outDir 'backend_check.exe') ($backendObjs + @($checkObj))

# ---------------------------------------------------------------- video sink (camera PHASE2 §4.6, CW4) + its harness
# Media Foundation is delay-loaded: on Windows N without the Media Feature Pack mfplat.dll is absent, and the helper
# must still start (audio); the video backend checks for it before any MF call. MFCreateVirtualCamera is resolved at
# run time (mfsensorgroup.dll exports it only on Windows 11).
$videoSrc = @('video_backend_win.cpp', 'videodec_win.cpp', 'vcam_win.cpp', 'placeholder_win.cpp', 'app_usage_win.cpp', 'vcam_install_win.cpp') | ForEach-Object { Join-Path $win $_ }
$videoLibs = @('mfplat.lib', 'mf.lib', 'mfuuid.lib', 'mfreadwrite.lib', 'wmcodecdspuuid.lib', 'd3d11.lib', 'dxgi.lib', 'd2d1.lib', 'dwrite.lib', 'windowscodecs.lib', 'shlwapi.lib', 'version.lib', 'delayimp.lib', '/DELAYLOAD:mfplat.dll')
$vhdr = NewestHeader @($win, $engine, (Join-Path $audio 'vcam-win'))
$videoObjs = @()
foreach ($s in $videoSrc) { $videoObjs += Compile $s 'win' ($cxxFlags + $incEngine) $vhdr }
$vcheckObj = Compile (Join-Path $win 'video_check.cpp') 'win' ($cxxFlags + $incEngine) $vhdr
Link (Join-Path $outDir 'video_check.exe') ($backendObjs + $videoObjs + @($vcheckObj)) $videoLibs
if ($checkOnly) { exit 0 }

# ---------------------------------------------------------------- engine + third party -> beamer-audio.exe
$sourcesTxt = Join-Path $engine 'sources.txt'
if (-not (Test-Path $sourcesTxt)) {
    Write-Host 'build-win: native\audio\engine\sources.txt not found (engine not merged yet): beamer-audio.exe not linked.'
    exit 0
}

# Engine sources: one path per line (# comments), relative to engine\ or to native\audio\.
$engineSrc = @()
foreach ($line in Get-Content $sourcesTxt) {
    $l = ($line -replace '#.*$', '').Trim()
    if (-not $l) { continue }
    $p = Join-Path $engine $l
    if (-not (Test-Path $p)) { $p = Join-Path $audio $l }
    if (-not (Test-Path $p)) { throw "sources.txt lists a missing file: $l" }
    $engineSrc += (Resolve-Path $p).Path
}
$listed = ($engineSrc -join ';').ToLower()

# Include dirs: engine, every third_party\<lib>\include, plus the library-internal dirs below.
$inc = @("/I$audio", "/I$engine")
if (Test-Path $third) { Get-ChildItem $third -Directory | ForEach-Object {
    foreach ($sub in 'include', 'single_include', 'src') { $d = Join-Path $_.FullName $sub; if (Test-Path $d) { $inc += "/I$d" } } } }

$thirdObjs = @()
# libopus 1.6.1, float build, from its own *.mk lists (PHASE2 §2.3), unless sources.txt already lists it.
$opus = Get-ChildItem $third -Directory -Filter 'opus-*' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($opus -and -not $listed.Contains('\opus-')) {
    $o = $opus.FullName
    $opusInc = @("/I$o\include", "/I$o\celt", "/I$o\silk", "/I$o\silk\float", "/I$o")
    # PACKAGE_VERSION is a string literal in opus_get_version_string(); Windows PowerShell 5.1 passes the
    # backslashes through and cl's argument parser turns \" into a quote.
    $opusDef = @('/DOPUS_BUILD', '/DUSE_ALLOCA', '/DHAVE_LRINTF', '/DPACKAGE_VERSION=\"1.6.1\"', '/wd4244', '/wd4267', '/wd4996')
    $vars = @('CELT_SOURCES', 'SILK_SOURCES', 'SILK_SOURCES_FLOAT', 'OPUS_SOURCES', 'OPUS_SOURCES_FLOAT')
    $files = @()
    foreach ($mk in 'celt_sources.mk', 'silk_sources.mk', 'silk_sources_float.mk', 'opus_sources.mk', 'opus_sources_float.mk') {
        $mp = Join-Path $o $mk
        if (-not (Test-Path $mp)) { continue }
        $text = (Get-Content $mp -Raw) -replace '\\\r?\n', ' '
        foreach ($m in [regex]::Matches($text, '(?m)^\s*([A-Z_]+)\s*\+?=\s*(.*)$')) {
            if ($vars -contains $m.Groups[1].Value) { $files += ($m.Groups[2].Value -split '\s+' | Where-Object { $_ -match '\.c$' }) }
        }
    }
    if (-not $files) { throw "no opus sources found in $o\*.mk" }
    $opusHdr = NewestHeader @("$o\include", "$o\celt", "$o\silk", "$o\src")
    foreach ($f in ($files | Select-Object -Unique)) { $thirdObjs += Compile (Join-Path $o $f) 'opus' ($cFlags + $opusInc + $opusDef) $opusHdr }
    $inc += "/I$o\include"
}
# Monocypher (crypto primitives), unless listed.
$mono = Get-ChildItem $third -Directory -Filter 'monocypher-*' -ErrorAction SilentlyContinue | Select-Object -First 1
if ($mono -and -not $listed.Contains('\monocypher-')) {
    $mc = Join-Path $mono.FullName 'src\monocypher.c'
    if (Test-Path $mc) { $thirdObjs += Compile $mc 'mono' $cFlags (NewestHeader @($mono.FullName)) }
}
# speexdsp echo canceller (PHASE2 §7): its files are listed in sources.txt; float build, kiss FFT, no
# config.h (speexdsp_types.h has its own _WIN32 branch), its own include dir.
$speexDef = @('/DFLOATING_POINT', '/DUSE_KISS_FFT', '/DEXPORT=', '/wd4244', '/wd4305', '/wd4267', '/wd4100', '/wd4127', '/wd4456')

$engHdr = NewestHeader @($engine, $win, $third)
$engineObjs = @()
foreach ($s in $engineSrc) {
    if ($s -match '\\speexdsp-[^\\]+\\.*\.c$') { $engineObjs += Compile $s 'eng' ($cFlags + $inc + $speexDef) $engHdr }
    elseif ($s -match '\.c$') { $engineObjs += Compile $s 'eng' ($cFlags + $inc) $engHdr }
    else { $engineObjs += Compile $s 'eng' ($cxxFlags + $inc) $engHdr }
}
# run_helper(Backend&, VideoBackend*, ...) comes with CW2's engine: until it is there, main.cpp builds audio only.
# The define is part of the object's tag, so main.obj is rebuilt when it flips.
$helperVideo = Select-String -Path (Join-Path $engine 'helper.cpp') -Pattern 'VideoBackend' -Quiet
$mainTag = if ($helperVideo) { 'winv' } else { 'win' }
$mainDef = if ($helperVideo) { @('/DBEAMER_HELPER_VIDEO=1') } else { @('/DBEAMER_HELPER_VIDEO=0') }
$mainObj = Compile (Join-Path $win 'main.cpp') $mainTag ($cxxFlags + $inc + $mainDef) (NewestHeader @($engine, $win, $third, (Join-Path $audio 'vcam-win')))
Link (Join-Path $outDir 'beamer-audio.exe') ($backendObjs + $videoObjs + $engineObjs + $thirdObjs + @($mainObj)) $videoLibs

# Beamer Camera's source DLL and its tools (CW5's script) -> build\win\camera\BeamerCameraSource.dll (§2.5).
$vcamBuild = Join-Path $audio 'vcam-win\build.ps1'
if (Test-Path $vcamBuild) {
    & $vcamBuild
    if (-not $?) { throw "vcam-win\build.ps1 failed" }
} else {
    Write-Host 'build-win: native\audio\vcam-win\build.ps1 not found (CW5 not merged yet): no camera DLL.'
}
