@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..") do set "LOCAL_ROOT=%%~fI"
pushd "%LOCAL_ROOT%\android_inference_benchmark"
call gradlew.bat :app:assembleRelease --console=plain %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
