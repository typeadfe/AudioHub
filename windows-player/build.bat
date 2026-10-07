@echo off
rem ============================================================
rem  AudioHub Windows Player - build script
rem  Uses the MinGW-w64 already present on this machine.
rem  NOTE: keep this file ASCII-only. cmd.exe reads .bat in the
rem  OEM codepage (GBK here), so non-ASCII comments break parsing.
rem ============================================================
setlocal enabledelayedexpansion

set "MINGW=C:\easy\mingw64\bin"
set "SRCDIR=%~dp0src"
set "OUTDIR=%~dp0build"
set "EXENAME=ahub-player.exe"
if not "%~1"=="" set "EXENAME=%~1"

if not exist "%MINGW%\g++.exe" (
    echo [ERROR] MinGW not found: %MINGW%\g++.exe
    exit /b 1
)
if not exist "%OUTDIR%" mkdir "%OUTDIR%"

set "SOURCES="
for %%F in ("%SRCDIR%\*.cpp") do set "SOURCES=!SOURCES! "%%F""

echo Compiler : %MINGW%\g++.exe
echo Output   : %OUTDIR%\%EXENAME%
echo ------------------------------------------------------------

rem Compile the resource script: application manifest + version info.
rem Without this the exe has no identity at all, which looks exactly like
rem an unsigned malware dropper to antivirus heuristics.
"%MINGW%\windres.exe" --codepage=65001 -I "%SRCDIR%" -i "%SRCDIR%\resource.rc" -o "%OUTDIR%\resource.o"
if errorlevel 1 (
    echo.
    echo [FAILED] resource compilation
    exit /b 1
)

"%MINGW%\g++.exe" ^
    -std=c++17 -O2 -Wall -Wextra ^
    -mwindows -municode ^
    -o "%OUTDIR%\%EXENAME%" ^
    %SOURCES% "%OUTDIR%\resource.o" ^
    -lole32 -luuid -lavrt -lksuser -lwinmm -lws2_32 -liphlpapi ^
    -lgdiplus -ldwmapi -lcomctl32 -luxtheme -lshell32 -ladvapi32 ^
    -static-libgcc -static-libstdc++ -static

rem -mwindows : GUI subsystem, so no console window appears.
rem -municode : Unicode entry point (wWinMain).
rem -lgdiplus : self-drawn Fluent UI. -ldwmapi : Win11 rounded corners.
rem Pass --console to get the old text diagnostics (attaches to parent console).

if errorlevel 1 (
    echo.
    echo [FAILED] compilation errors
    exit /b 1
)

echo.
echo [OK] %OUTDIR%\%EXENAME%
endlocal
