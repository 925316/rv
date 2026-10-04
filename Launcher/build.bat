@echo off
setlocal

rem RideFlightLauncher - one-command build:
rem   1. build RideFlight.dll (delegates to ..\FlightDll\build.bat)
rem   2. embed the DLL bytes as build\dllbytes.h (binary-to-C-array via PowerShell)
rem   3. compile the launcher as a standalone x64 console exe with /MT
rem Output: Launcher\build\RideFlightLauncher.exe

set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSVARS%" (
    echo [build] vcvars64.bat not found: %VSVARS%
    exit /b 1
)

rem --- 1. the DLL itself ----------------------------------------------------------
call "%~dp0..\FlightDll\build.bat"
if errorlevel 1 exit /b 1

call "%VSVARS%" >nul

set "BUILD=%~dp0build"
if not exist "%BUILD%" mkdir "%BUILD%"

rem --- 2. embed DLL bytes ---------------------------------------------------------
rem kDllBytes[] + kDllSize, one byte per item; large but compiles fine with /O2.
rem $ErrorActionPreference=Stop + exit code so a PS failure fails this script
rem instead of feeding a stale/empty header to cl.
powershell -NoProfile -Command ^
    "$ErrorActionPreference='Stop';" ^
    "$b=[IO.File]::ReadAllBytes('%~dp0..\FlightDll\build\RideFlight.dll');" ^
    "$sb=[Text.StringBuilder]::new();" ^
    "for($i=0;$i -lt $b.Length;$i++){" ^
      "[void]$sb.Append(('0x{0:X2},' -f $b[$i]));" ^
      "if(($i %% 16) -eq 15){[void]$sb.Append([char]10)}}" ^
    "[IO.File]::WriteAllText('%BUILD%\dllbytes.h', " ^
      "'static const unsigned char kDllBytes[] = {' + [char]10 + $sb.ToString() + [char]10 + '};' + [char]10 + " ^
      "'static const unsigned int kDllSize = ' + $b.Length + ';' + [char]10)"
if errorlevel 1 (
    echo [build] embedding the DLL failed
    exit /b 1
)

rem --- 3. the launcher exe --------------------------------------------------------
cl /nologo /std:c++latest /EHsc /O2 /W3 /utf-8 /MT /DUNICODE /D_UNICODE ^
    /I"%BUILD%" ^
    "%~dp0main.cpp" ^
    /link /INCREMENTAL:NO /OUT:"%BUILD%\RideFlightLauncher.exe" ^
    user32.lib advapi32.lib

if errorlevel 1 (
    echo [build] FAILED
    exit /b 1
)

rem --- 4. UPX pack (optional shell) -----------------------------------------------
rem Shrinks the exe to ~1/3; skipped when UPX is not installed.
rem Variable must NOT be named UPX: UPX reads an env var called UPX as its own
rem option string and dies on a path value. No parentheses inside if-block echos
rem - batch treats them as block delimiters. UPX lives in the repo root.
set "UPX_EXE=%~dp0..\upx.exe"
if exist "%UPX_EXE%" (
    "%UPX_EXE%" -9 --quiet "%BUILD%\RideFlightLauncher.exe"
    if errorlevel 1 echo [build] UPX packing failed - exe remains usable, unpacked
) else (
    echo [build] UPX not found, skipping pack: %UPX_EXE%
)

for %%F in ("%BUILD%\RideFlightLauncher.exe") do echo [build] OK: %%F ^(~%%~zF bytes^)
