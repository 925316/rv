@echo off
setlocal

rem RideFlight.dll - x64, /std:c++latest, game code + the four translation
rem units Dumper-7's UsingTheSDK.md requires.
rem Output: FlightDll\build\RideFlight.dll

set "VSVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VSVARS%" (
    echo [build] vcvars64.bat not found: %VSVARS%
    exit /b 1
)
call "%VSVARS%" >nul

set "BUILD=%~dp0build"
if not exist "%BUILD%" mkdir "%BUILD%"
cd /d "%BUILD%"

cl /nologo /LD /std:c++latest /EHsc /O2 /W3 /utf-8 /wd4369 /wd4309 ^
    "%~dp0Main.cpp" ^
    /link /INCREMENTAL:NO /OUT:"%BUILD%\RideFlight.dll" /PDB:"%BUILD%\RideFlight.pdb"
    
if errorlevel 1 (
    echo [build] FAILED
    exit /b 1
)
echo [build] OK: %BUILD%\RideFlight.dll
