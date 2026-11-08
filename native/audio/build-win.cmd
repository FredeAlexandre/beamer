@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds the Windows audio helper with the newest MSVC (x64, static CRT, no runtime to ship):
rem   native\audio\build\win\beamer-audio.exe   (needs the engine: native\audio\engine\sources.txt)
rem   native\audio\build\win\backend_check.exe  (WASAPI backend test harness, always)
rem Objects are cached under native\audio\build\obj\win-x64 and rebuilt only when a source or header is newer.
rem Options: --clean (drop the object cache first), --check-only (backend_check.exe only).
setlocal
set "HERE=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo build-win: vswhere not found, install Visual Studio Build Tools with the C++ workload & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo build-win: MSVC x64 tools not found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
powershell -NoProfile -ExecutionPolicy Bypass -File "%HERE%win\build.ps1" %*
exit /b %ERRORLEVEL%
