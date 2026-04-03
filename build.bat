@echo off
setlocal

echo === FO4RemixPlugin Build ===
echo.

:: Find cmake — prefer PATH, fall back to VS 2022 bundled location
where cmake >nul 2>&1
if %errorlevel% equ 0 (
    set "CMAKE=cmake"
) else (
    set "CMAKE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if not exist "%CMAKE%" (
        echo ERROR: cmake not found. Install CMake or Visual Studio 2022.
        exit /b 1
    )
)

"%CMAKE%" -B build -G "Visual Studio 17 2022" -A x64
if %errorlevel% neq 0 (
    echo.
    echo CMake configure FAILED
    exit /b 1
)

"%CMAKE%" --build build --config Release
if %errorlevel% neq 0 (
    echo.
    echo Build FAILED
    exit /b 1
)

echo.
echo Build succeeded: build\Release\FO4RemixPlugin.dll
