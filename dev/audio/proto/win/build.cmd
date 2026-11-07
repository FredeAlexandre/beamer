@echo off
rem Builds build\beamer-audio.exe with the newest installed MSVC. Static CRT: no runtime to ship.
setlocal
set "HERE=%~dp0"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (echo MSVC not found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%HERE%build" mkdir "%HERE%build"
pushd "%HERE%build"
cl /nologo /std:c++20 /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE ..\beamer-audio.cpp /Fe:beamer-audio.exe /link /SUBSYSTEM:CONSOLE
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
