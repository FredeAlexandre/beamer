@echo off
rem SPDX-License-Identifier: GPL-3.0-only
rem Builds the engine tests and the null-backend helper on Windows with MSVC (x64, static CRT).
rem   native\audio\engine\build-tests.cmd [run]
rem Outputs native\audio\build\tests\engine_tests.exe and beamer-audio-null.exe. Finds MSVC through
rem vswhere when cl.exe is not already on PATH. libopus is compiled once from its own .mk lists
rem (objects cached in native\audio\build\obj\win\opus); engine and tests are rebuilt every time.
setlocal EnableDelayedExpansion
set "HERE=%~dp0"
set "HERE=%HERE:~0,-1%"
for %%I in ("%HERE%\..") do set "ROOT=%%~fI"
set "OPUS=%ROOT%\third_party\opus-1.6.1"
set "OUT=%ROOT%\build\tests"
set "OBJ=%ROOT%\build\obj\win"

where cl >nul 2>nul
if errorlevel 1 (
  set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "!VSWHERE!" (echo error: vswhere.exe not found & exit /b 1)
  for /f "usebackq tokens=* delims=" %%P in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%P"
  if not defined VSPATH (echo error: no MSVC x64 toolset found & exit /b 1)
  call "!VSPATH!\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%\opus" mkdir "%OBJ%\opus"
if not exist "%OBJ%\engine" mkdir "%OBJ%\engine"

rem ---- libopus (float, no intrinsics/RTCD), once
if not exist "%OBJ%\opus\.done" (
  set "RSP=%OBJ%\opus\sources.rsp"
  type nul > "!RSP!"
  rem each "VAR = \" block of the .mk files, until a line without a trailing "\"
  for %%V in (celt_sources.mk:CELT_SOURCES silk_sources.mk:SILK_SOURCES silk_sources.mk:SILK_SOURCES_FLOAT opus_sources.mk:OPUS_SOURCES opus_sources.mk:OPUS_SOURCES_FLOAT) do (
    for /f "tokens=1,2 delims=:" %%A in ("%%V") do (
      set "inside="
      for /f "usebackq tokens=* delims=" %%L in ("%OPUS%\%%A") do (
        set "line=%%L"
        if defined inside (
          set "p=!line:\=!"
          for /f "tokens=1" %%T in ("!p!") do echo "%OPUS%\%%T">> "!RSP!"
          if not "!line:~-1!"=="\" set "inside="
        ) else (
          if "!line!"=="%%B = \" set "inside=1"
        )
      )
    )
  )
  echo compiling libopus
  cl /nologo /c /MP /O2 /MT /W0 /utf-8 /DOPUS_BUILD /DUSE_ALLOCA /DHAVE_LRINTF /D_CRT_SECURE_NO_WARNINGS ^
     /DPACKAGE_VERSION=\"1.6.1\" /I"%OPUS%\include" /I"%OPUS%\celt" /I"%OPUS%\silk" /I"%OPUS%\silk\float" ^
     /I"%OPUS%\src" /Fo"%OBJ%\opus\\" @"!RSP!" || exit /b 1
  echo ok > "%OBJ%\opus\.done"
)

rem ---- engine + tests
set "SRCS="
set "CSRCS="
for /f "usebackq eol=# tokens=* delims=" %%S in ("%HERE%\sources.txt") do (
  if /i "%%~xS"==".c" (set "CSRCS=!CSRCS! "%HERE%\%%S"") else (set "SRCS=!SRCS! "%HERE%\%%S"")
)
set "SPEEX=%ROOT%\third_party\speexdsp-1.2.1"
set "INC=/I"%HERE%" /I"%OPUS%\include" /I"%ROOT%\third_party\monocypher-4.0.2\src" /I"%ROOT%\third_party\nlohmann-json-3.11.3\single_include" /I"%SPEEX%\include""
echo compiling engine
rem C sources: Monocypher and the speexdsp echo canceller (float, kiss FFT, no config.h; defines harmless for the rest)
cl /nologo /c /O2 /MT /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /DFLOATING_POINT /DUSE_KISS_FFT /DEXPORT= /wd4244 /wd4267 /wd4305 ^
   /I"%SPEEX%\include" /Fo"%OBJ%\engine\\" %CSRCS% || exit /b 1
cl /nologo /c /MP /std:c++20 /O2 /MT /W4 /utf-8 /EHsc /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
   /wd4100 /wd4244 /wd4267 %INC% /Fo"%OBJ%\engine\\" %SRCS% "%ROOT%\tests\engine_tests.cpp" "%ROOT%\tests\video_tests.cpp" "%ROOT%\tests\null_main.cpp" || exit /b 1

set "ENGOBJ="
for /f "usebackq eol=# tokens=* delims=" %%S in ("%HERE%\sources.txt") do set "ENGOBJ=!ENGOBJ! "%OBJ%\engine\%%~nS.obj""
set "LIBS=ws2_32.lib ole32.lib avrt.lib propsys.lib bcrypt.lib winmm.lib"
link /nologo /OUT:"%OUT%\engine_tests.exe" "%OBJ%\engine\engine_tests.obj" "%OBJ%\engine\video_tests.obj" %ENGOBJ% "%OBJ%\opus\*.obj" %LIBS% || exit /b 1
link /nologo /OUT:"%OUT%\beamer-audio-null.exe" "%OBJ%\engine\null_main.obj" %ENGOBJ% "%OBJ%\opus\*.obj" %LIBS% || exit /b 1
echo built %OUT%\engine_tests.exe and %OUT%\beamer-audio-null.exe

if /i "%~1"=="run" (
  "%OUT%\engine_tests.exe" --vectors "%ROOT%\tests\vectors"
  exit /b !errorlevel!
)
exit /b 0
