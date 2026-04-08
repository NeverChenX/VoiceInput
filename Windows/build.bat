@echo off
setlocal

:: ─── Find g++ (WinLibs installed via winget, or any g++ in PATH) ─────────
set GPP=
for /f "delims=" %%i in ('where g++ 2^>nul') do if "!GPP!"=="" set GPP=%%i

if "%GPP%"=="" (
    set WINGET_GPP=%LOCALAPPDATA%\Microsoft\WinGet\Packages\BrechtSanders.WinLibs.POSIX.UCRT_Microsoft.Winget.Source_8wekyb3d8bbwe\mingw64\bin\g++.exe
    if exist "!WINGET_GPP!" set GPP=!WINGET_GPP!
)

if "%GPP%"=="" (
    echo ERROR: g++ not found.
    echo Install via: winget install BrechtSanders.WinLibs.POSIX.UCRT
    exit /b 1
)

echo Using compiler: %GPP%
echo Building voice_input.exe ...

"%GPP%" -std=c++17 -O2 -o voice_input.exe voice_input.cpp ^
    -lwinmm -lwinhttp -luser32 -lgdi32 -lole32 ^
    -static-libgcc -static-libstdc++ -static -lpthread ^
    -mwindows

if %ERRORLEVEL% neq 0 (
    echo.
    echo Build FAILED.
    exit /b %ERRORLEVEL%
)

echo.
echo Build OK: voice_input.exe  (standalone, no extra DLLs needed)
echo Run:  voice_input.exe
echo Exit: press ESC at any time.
endlocal
