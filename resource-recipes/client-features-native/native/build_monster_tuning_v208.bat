@echo off
setlocal
if "%~2"=="" (
  echo Usage: %~nx0 original-DNF.exe new-output-directory
  exit /b 2
)
set "input=%~f1"
set "output=%~f2"
if exist "%output%" (
  echo Output directory already exists.
  exit /b 2
)

if not defined VCToolsInstallDir (
  if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" (
    call "%ProgramFiles%\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
  ) else (
    call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
  )
  if errorlevel 1 exit /b 1
)
mkdir "%output%"
if errorlevel 1 exit /b 1

cl /nologo /c /O2 /Oi /Zl /GS- /GR- /EHs-c- /arch:IA32 /std:c++17 /Brepro /DMONSTER_TUNING_NO_CRT "/Fo%output%\receiver.obj" "%~dp0monster_tuning_v208.cpp"
if errorlevel 1 exit /b 1
link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X86 /BASE:0x06F90000 /FIXED /DYNAMICBASE:NO /MERGE:.rdata=.text /Brepro "/OUT:%output%\receiver.dll" "%output%\receiver.obj"
if errorlevel 1 exit /b 1
python -B "%~dp0build_monster_tuning_v208.py" --base "%input%" --payload "%output%\receiver.dll" --output "%output%\DNF-receiver.exe"
if errorlevel 1 exit /b 1
python -B "%~dp0test_monster_tuning_receiver_v208.py" --candidate "%output%\DNF-receiver.exe" --payload "%output%\receiver.dll"
exit /b %errorlevel%
