@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars32.bat not found - check VS BuildTools path
    exit /b 1
)
cd /d "%~dp0"

set "TMPOBJ=%TEMP%\rs_loader_build"
if not exist "%TMPOBJ%" mkdir "%TMPOBJ%"

cl /nologo /DUNICODE /D_UNICODE /std:c++17 /utf-8 /O2 /MT /EHsc /Brepro /LD rs_loader.cpp ^
   /Fo"%TMPOBJ%\\" /Fd"%TMPOBJ%\\" ^
   /link /Brepro /DEF:rs_loader.def /OUT:RS.dll /IMPLIB:"%TMPOBJ%\rs.lib"
if errorlevel 1 exit /b %errorlevel%

echo Built: %~dp0RS.dll
