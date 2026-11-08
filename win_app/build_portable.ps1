param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'

function Invoke-Native {
    param([Parameter(Mandatory = $true)][scriptblock]$Command)
    $ErrorActionPreference = 'Continue'
    & $Command
}

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$ProjectDir = Split-Path -Parent $ScriptDir
$DistDir = Join-Path $ScriptDir 'dist\portable'
$WorkDir = Join-Path $ScriptDir 'build\portable'
$Payload = Join-Path $DistDir 'Beamer-portable'
$Version = (Get-Content (Join-Path $ProjectDir 'VERSION') -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+(?:-[A-Za-z0-9.]+)?$') { throw 'VERSION is not a supported Beamer version' }
if (-not $OutputDirectory) { $OutputDirectory = $DistDir }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$Archive = Join-Path $OutputDirectory "Beamer-Portable-$Version-windows-x64.zip"

Push-Location $ScriptDir
try {
    $Python = Join-Path $ScriptDir '.venv\Scripts\python.exe'
    if (-not (Test-Path $Python)) {
        $BootstrapPython = Get-Command python -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
        if (-not $BootstrapPython) { throw 'Python was not found on PATH' }
        Invoke-Native { & $BootstrapPython -m venv .venv }
        if ($LASTEXITCODE -ne 0) { throw 'Virtual environment creation failed' }
    }
    $Toolchain = @'
import platform, struct, sys
print('Portable build: Python', platform.python_version(), platform.machine())
sys.exit(0 if sys.platform == 'win32' and struct.calcsize('P') == 8 and sys.version_info.releaselevel == 'final' else 'A stable 64-bit Windows Python is required')
'@
    Invoke-Native { & $Python -c $Toolchain }
    if ($LASTEXITCODE -ne 0) { throw 'Unsupported portable build toolchain' }
    Invoke-Native { & $Python -m pip install -r requirements-win.txt }
    if ($LASTEXITCODE -ne 0) { throw 'Dependency installation failed' }
    Invoke-Native { & $Python -m PyInstaller --version }
    if ($LASTEXITCODE -ne 0) { throw 'PyInstaller is unavailable' }
    Invoke-Native { & $Python -m PyInstaller --clean --noconfirm --distpath $DistDir --workpath $WorkDir build_portable.spec }
    if ($LASTEXITCODE -ne 0) { throw 'Portable PyInstaller build failed' }
    $Executable = Join-Path $Payload 'Beamer.exe'
    if (-not (Test-Path $Executable)) { throw 'Portable build did not create Beamer.exe' }

    Copy-Item (Join-Path $ProjectDir 'LICENSE') (Join-Path $Payload 'LICENSE') -Force
    [IO.File]::WriteAllText((Join-Path $Payload 'Beamer.portable'), '')
    $Notices = @'
import importlib.metadata as metadata
from pathlib import Path
import shutil
import sys

payload = Path(sys.argv[1])
licences = payload / 'licences'
licences.mkdir()
python_licence = Path(sys.base_prefix) / 'LICENSE.txt'
if not python_licence.is_file():
    raise SystemExit('Python LICENSE.txt is missing')
shutil.copyfile(python_licence, licences / 'Python-LICENSE.txt')
notices = ['Beamer portable: GPL-3.0-only; see LICENSE.',
           'Third-party components retain their own licences, reproduced in licences/.',
           'Qt libraries are dynamically linked and may be replaced in _internal/.',
           'Python ' + sys.version.split()[0] + ': see licences/Python-LICENSE.txt.']
packages = ('PySide6-Essentials', 'shiboken6', 'PyNaCl', 'cffi', 'pycparser', 'pywin32', 'segno', 'PyInstaller')
for name in packages:
    package = metadata.distribution(name)
    files = []
    for relative in package.files or ():
        if any(part.lower() in {'licenses', 'licences'} for part in relative.parts) or relative.name.lower().startswith(('license', 'licence', 'copying', 'notice')):
            source = Path(package.locate_file(relative))
            if source.is_file():
                target = licences / name / relative
                if '..' in relative.parts or relative.is_absolute():
                    raise SystemExit('Unsafe licence path in ' + name)
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, target)
                files.append(target.relative_to(payload).as_posix())
    if not files:
        raise SystemExit('No distributed licence text found for ' + name)
    licence = package.metadata.get('License-Expression') or package.metadata.get('License') or 'See reproduced licence text.'
    notices.append('\n' + name + ' ' + package.version + '\n' + licence + '\n' + '\n'.join(sorted(files)))
(payload / 'THIRD-PARTY-NOTICES.txt').write_text('\n'.join(notices) + '\n', encoding='utf-8')
'@
    Invoke-Native { & $Python -c $Notices $Payload }
    if ($LASTEXITCODE -ne 0) { throw 'Portable licence collection failed' }

    # A new COLLECT payload contains no settings, pairings, logs, caches or host identity.
    $Unexpected = Get-ChildItem $Payload -Force | Where-Object Name -notin @('Beamer.exe', 'beamer-audio.exe', '_internal', 'Beamer.portable', 'LICENSE', 'licences', 'THIRD-PARTY-NOTICES.txt')
    if ($Unexpected) { throw 'Unexpected files in portable payload' }
    if ((Get-AuthenticodeSignature $Executable).Status -ne 'NotSigned') { throw 'The portable candidate must be unsigned' }
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    Compress-Archive -Path $Payload -DestinationPath $Archive -Force
    Write-Output "Built unsigned portable candidate: $Archive"
    Write-Output "SHA256: $((Get-FileHash $Archive -Algorithm SHA256).Hash.ToLowerInvariant())"
}
finally {
    Pop-Location
}
