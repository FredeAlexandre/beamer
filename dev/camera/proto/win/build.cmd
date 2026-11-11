@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds the C2 prototypes into build\ with the newest MSVC (x64, static CRT), same toolchain as native\audio\build-win.cmd.
setlocal
set "HERE=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo MSVC not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%HERE%build" mkdir "%HERE%build"
pushd "%HERE%build"
set "CF=/nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE /wd4100 /wd4324 /D_CRT_SECURE_NO_WARNINGS"
cl %CF% /LD ..\source.cpp /Fe:BeamerCamC2.dll /link /DEF:..\source.def mfplat.lib mfuuid.lib mf.lib ole32.lib advapi32.lib || goto :fail
cl %CF% ..\vcamctl.cpp /Fe:vcamctl.exe /link mfplat.lib mfuuid.lib mfsensorgroup.lib ole32.lib advapi32.lib || goto :fail
cl %CF% ..\camprobe.cpp /Fe:camprobe.exe /link mfplat.lib mfuuid.lib mf.lib mfreadwrite.lib strmiids.lib ole32.lib oleaut32.lib advapi32.lib || goto :fail
cl %CF% ..\feeder.cpp /Fe:feeder.exe /link mfplat.lib mfuuid.lib mf.lib d3d11.lib winmm.lib wmcodecdspuuid.lib ole32.lib advapi32.lib || goto :fail
popd
exit /b 0
:fail
popd
exit /b 1
