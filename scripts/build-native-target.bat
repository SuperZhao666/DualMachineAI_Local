@echo off
setlocal EnableExtensions
chcp 65001 >nul
for %%I in ("%~dp0..") do set "LOCAL_ROOT=%%~fI"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT exit /b 1
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -no_logo -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1
set "VSLANG=1033"
if defined ANDROID_HOME set "PATH=%ANDROID_HOME%\cmake\3.22.1\bin;%PATH%"
cmake -S "%LOCAL_ROOT%\dual_machine_runtime" -B "%LOCAL_ROOT%\build\host" -G Ninja -DCMAKE_BUILD_TYPE=Release -DVFDUAL_ENABLE_PRIVATE_HOST_SYMBOLS=OFF
if errorlevel 1 exit /b 1
python "%LOCAL_ROOT%\scripts\repair-native-dependencies.py"
if errorlevel 1 exit /b 1
cmake --build "%LOCAL_ROOT%\build\host" --parallel 4 --target %*
if errorlevel 1 exit /b 1
python "%LOCAL_ROOT%\scripts\repair-native-dependencies.py" --build-dir "%LOCAL_ROOT%\build\host" --verify
if errorlevel 1 exit /b 1
