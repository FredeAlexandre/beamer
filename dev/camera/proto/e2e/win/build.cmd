@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds camrecv.exe (C4 PC receiver) with the newest MSVC (x64, static CRT). Expects camrecv.cpp, proto.h
rem and C2's common.h side by side in this folder (the PC work folder's src\), output in ..\bin.
setlocal
set "HERE=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo MSVC not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%HERE%..\bin" mkdir "%HERE%..\bin"
pushd "%HERE%..\bin"
cl /nologo /std:c++20 /EHsc /O2 /MT /W3 /utf-8 /wd4100 /wd4324 /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS ..\src\camrecv.cpp /Fe:camrecv.exe /link ws2_32.lib mfplat.lib mfuuid.lib mf.lib d3d11.lib winmm.lib wmcodecdspuuid.lib ole32.lib advapi32.lib psapi.lib || goto :fail
popd
exit /b 0
:fail
popd
exit /b 1
