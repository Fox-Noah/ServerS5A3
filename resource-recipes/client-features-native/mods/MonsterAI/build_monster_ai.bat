@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 (
    echo [ERROR] vcvars32.bat not found - check VS BuildTools path
    exit /b 1
)
cd /d "%~dp0"

set "TMPOBJ=%TEMP%\monster_ai_build"
if not exist "%TMPOBJ%" mkdir "%TMPOBJ%"

cl /nologo /std:c++17 /utf-8 /O2 /MT /EHsc /Brepro /LD ^
   monster_ai_mod.cpp ..\..\native\monster_tuning_v208.cpp ^
   /I..\..\native /Fo"%TMPOBJ%\\" /Fd"%TMPOBJ%\\" ^
   /link /Brepro /OUT:MonsterAI.dll /IMPLIB:"%TMPOBJ%\MonsterAI.lib"
if errorlevel 1 exit /b %errorlevel%

echo Built: %~dp0MonsterAI.dll
