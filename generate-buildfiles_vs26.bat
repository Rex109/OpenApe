@echo off
setlocal
pushd "%~dp0"

if not exist "tools\premake5.exe" (
    echo tools\premake5.exe not found.
    popd
    exit /b 1
)

git submodule update --init --recursive

tools\premake5.exe vs2026
set RESULT=%ERRORLEVEL%

popd
if not "%RESULT%"=="0" (
    echo Premake failed with error %RESULT%.
    pause
    exit /b %RESULT%
)

echo.
echo Generated build\openape.slnx - open it in Visual Studio 2026 and build x64 or Win32.
pause
endlocal
