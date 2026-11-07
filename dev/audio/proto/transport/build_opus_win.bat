@echo off
rem Build opus.dll (BSD-3-Clause, official xiph source) with MSVC + the CMake bundled in VS Build Tools.
rem Usage (on the PC): build_opus_win.bat C:\Users\Alex\audio-proto\opus-build
rem Expects opus-1.6.1\ extracted in that folder (tarball sha256 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1).
setlocal
set ROOT=%~1
if "%ROOT%"=="" set ROOT=%CD%
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
cd /d "%ROOT%" || exit /b 1
"%CMAKE%" -S opus-1.6.1 -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF || exit /b 1
"%CMAKE%" --build build || exit /b 1
dir build\*.dll
