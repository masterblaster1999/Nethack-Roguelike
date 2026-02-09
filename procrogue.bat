@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "ROOT=%~dp0"
pushd "%ROOT%" >nul

if "%PROCROGUE_STOP_STALE_BUILD_TOOLS%"=="" set "PROCROGUE_STOP_STALE_BUILD_TOOLS=1"

set "CMD=%~1"
if "%CMD%"=="" set "CMD=play"

if /I "%CMD%"=="h" set "CMD=help"
if /I "%CMD%"=="?" set "CMD=help"
if /I "%CMD%"=="cfg" set "CMD=configure"
if /I "%CMD%"=="config" set "CMD=configure"
if /I "%CMD%"=="b" set "CMD=build"
if /I "%CMD%"=="r" set "CMD=run"
if /I "%CMD%"=="t" set "CMD=test"
if /I "%CMD%"=="diag" set "CMD=doctor"

if /I "%CMD%"=="help" goto :help
if /I "%CMD%"=="configure" goto :configure
if /I "%CMD%"=="build" goto :build
if /I "%CMD%"=="play" goto :play
if /I "%CMD%"=="run" goto :run
if /I "%CMD%"=="test" goto :test
if /I "%CMD%"=="clean" goto :clean
if /I "%CMD%"=="rebuild" goto :rebuild
if /I "%CMD%"=="doctor" goto :doctor
if /I "%CMD%"=="presets" goto :presets

echo [procrogue] Unknown command: %~1
goto :help_fail

:configure
set "NEED_NATIVE_COMPILER=0"
call :require_tool cmake
if errorlevel 1 goto :fail
call :maybe_stop_stale_build_tools

set "PRESET=%~2"
if "%PRESET%"=="" set "PRESET=msvc"
if /I "%PRESET%"=="debug" set "PRESET=ninja-debug"
if /I "%PRESET%"=="release" set "PRESET=ninja-release"
if /I "%PRESET%"=="test" set "PRESET=tests"
if /I "%PRESET%"=="msvc-md" set "PRESET=msvc"
if /I "%PRESET%"=="msvc-mt" set "PRESET=msvc-static"
if /I "%PRESET%"=="static" set "PRESET=msvc-static"
if /I "%PRESET%"=="vcpkg" set "PRESET=msvc-vcpkg"
if /I "%PRESET%"=="vcpkg-static" set "PRESET=msvc-static-vcpkg"
if /I "%PRESET%"=="ninja-debug" set "NEED_NATIVE_COMPILER=1"
if /I "%PRESET%"=="ninja-release" set "NEED_NATIVE_COMPILER=1"
if /I "%PRESET%"=="ninja-release-tests" set "NEED_NATIVE_COMPILER=1"
if /I "%PRESET%"=="tests" set "NEED_NATIVE_COMPILER=1"

if "%NEED_NATIVE_COMPILER%"=="1" (
    call :bootstrap_toolchain required
    if errorlevel 1 goto :fail
) else (
    call :bootstrap_toolchain optional
)

echo [procrogue] cmake --preset %PRESET%
cmake --preset "%PRESET%"
if errorlevel 1 goto :fail

call :check_config_output "%PRESET%"
if errorlevel 1 goto :fail
goto :done

:build
set "NEED_NATIVE_COMPILER=0"
call :require_tool cmake
if errorlevel 1 goto :fail
call :maybe_stop_stale_build_tools

set "BUILD_PRESET=%~2"
if "%BUILD_PRESET%"=="" set "BUILD_PRESET=build-msvc-release"
if /I "%BUILD_PRESET%"=="debug" set "BUILD_PRESET=build-ninja-debug"
if /I "%BUILD_PRESET%"=="release" set "BUILD_PRESET=build-ninja-release"
if /I "%BUILD_PRESET%"=="msvc" set "BUILD_PRESET=build-msvc-release"
if /I "%BUILD_PRESET%"=="msvc-static" set "BUILD_PRESET=build-msvc-static-release"
if /I "%BUILD_PRESET%"=="build-msvc-static" set "BUILD_PRESET=build-msvc-static-release"
if /I "%BUILD_PRESET%"=="static" set "BUILD_PRESET=build-msvc-static-release"
if /I "%BUILD_PRESET%"=="vcpkg" set "BUILD_PRESET=build-msvc-vcpkg-release"
if /I "%BUILD_PRESET%"=="vcpkg-static" set "BUILD_PRESET=build-msvc-static-vcpkg-release"
if /I "%BUILD_PRESET%"=="msvc-vcpkg" set "BUILD_PRESET=build-msvc-vcpkg-release"
if /I "%BUILD_PRESET%"=="msvc-static-vcpkg" set "BUILD_PRESET=build-msvc-static-vcpkg-release"
if /I "%BUILD_PRESET%"=="build-msvc-vcpkg" set "BUILD_PRESET=build-msvc-vcpkg-release"
if /I "%BUILD_PRESET%"=="build-msvc-static-vcpkg" set "BUILD_PRESET=build-msvc-static-vcpkg-release"
if /I "%BUILD_PRESET%"=="test" set "BUILD_PRESET=tests"
if /I "%BUILD_PRESET%"=="build-ninja-debug" set "NEED_NATIVE_COMPILER=1"
if /I "%BUILD_PRESET%"=="build-ninja-release" set "NEED_NATIVE_COMPILER=1"
if /I "%BUILD_PRESET%"=="tests" set "NEED_NATIVE_COMPILER=1"

if "%NEED_NATIVE_COMPILER%"=="1" (
    call :bootstrap_toolchain required
    if errorlevel 1 goto :fail
) else (
    call :bootstrap_toolchain optional
)

echo [procrogue] cmake --build --preset %BUILD_PRESET%
cmake --build --preset "%BUILD_PRESET%"
if errorlevel 1 goto :fail
goto :done

:play
set "NEED_NATIVE_COMPILER=0"
call :require_tool cmake
if errorlevel 1 goto :fail
call :maybe_stop_stale_build_tools

set "MODE=%~2"
set "CFG_PRESET=msvc"
set "BLD_PRESET=build-msvc-release"
set "RUN_CFG=Release"

if /I "%MODE%"=="debug" (
    set "CFG_PRESET=ninja-debug"
    set "BLD_PRESET=build-ninja-debug"
    set "RUN_CFG=Debug"
    set "NEED_NATIVE_COMPILER=1"
) else if /I "%MODE%"=="release" (
    set "RUN_CFG=Release"
) else if /I "%MODE%"=="static" (
    set "CFG_PRESET=msvc-static"
    set "BLD_PRESET=build-msvc-static-release"
    set "RUN_CFG=Release"
) else if /I "%MODE%"=="vcpkg" (
    set "CFG_PRESET=msvc-vcpkg"
    set "BLD_PRESET=build-msvc-vcpkg-release"
    set "RUN_CFG=Release"
) else if /I "%MODE%"=="vcpkg-static" (
    set "CFG_PRESET=msvc-static-vcpkg"
    set "BLD_PRESET=build-msvc-static-vcpkg-release"
    set "RUN_CFG=Release"
) else if not "%MODE%"=="" (
    echo [procrogue] ERROR: unknown play mode "%MODE%". Use release, debug, static, vcpkg, or vcpkg-static.
    goto :fail
)

if "%NEED_NATIVE_COMPILER%"=="1" (
    call :bootstrap_toolchain required
    if errorlevel 1 goto :fail
) else (
    call :bootstrap_toolchain optional
)

echo [procrogue] cmake --preset %CFG_PRESET%
cmake --preset "%CFG_PRESET%"
if errorlevel 1 goto :fail

call :check_config_output "%CFG_PRESET%"
if errorlevel 1 goto :fail

echo [procrogue] cmake --build --preset %BLD_PRESET%
cmake --build --preset "%BLD_PRESET%"
if errorlevel 1 goto :fail

call :resolve_game_exe "%RUN_CFG%"
if not defined GAME_EXE (
    echo [procrogue] ERROR: ProcRogue.exe not found after build.
    goto :fail
)

echo [procrogue] Running %GAME_EXE%
"%GAME_EXE%"
if errorlevel 1 goto :fail
goto :done

:run
set "RUN_CFG=%~2"
if "%RUN_CFG%"=="" set "RUN_CFG=Release"
if /I "%RUN_CFG%"=="release" set "RUN_CFG=Release"
if /I "%RUN_CFG%"=="debug" set "RUN_CFG=Debug"

call :resolve_game_exe "%RUN_CFG%"
if not defined GAME_EXE (
    echo [procrogue] ERROR: ProcRogue.exe not found for %RUN_CFG%.
    echo [procrogue] Build first with: procrogue.bat play
    goto :fail
)

echo [procrogue] Running %GAME_EXE%
"%GAME_EXE%"
if errorlevel 1 goto :fail
goto :done

:test
call :require_tool cmake
if errorlevel 1 goto :fail
call :require_tool ctest
if errorlevel 1 goto :fail
call :maybe_stop_stale_build_tools
call :bootstrap_toolchain required
if errorlevel 1 goto :fail

set "TEST_PRESET=%~2"
if "%TEST_PRESET%"=="" set "TEST_PRESET=tests"
if /I "%TEST_PRESET%"=="test" set "TEST_PRESET=tests"

echo [procrogue] cmake --preset %TEST_PRESET%
cmake --preset "%TEST_PRESET%"
if errorlevel 1 goto :fail

call :check_config_output "%TEST_PRESET%"
if errorlevel 1 goto :fail

echo [procrogue] cmake --build --preset %TEST_PRESET%
cmake --build --preset "%TEST_PRESET%"
if errorlevel 1 goto :fail

echo [procrogue] ctest --preset %TEST_PRESET% --output-on-failure
ctest --preset "%TEST_PRESET%" --output-on-failure
if errorlevel 1 goto :fail
goto :done

:clean
call :maybe_stop_stale_build_tools
if not exist "%CD%\build" (
    echo [procrogue] No build folder found.
    goto :done
)

echo [procrogue] Removing %CD%\build
rmdir /s /q "%CD%\build"
if exist "%CD%\build" (
    echo [procrogue] ERROR: build folder still present ^(likely locked by another process^).
    echo [procrogue] HINT: close running cmake/ninja processes, then retry clean.
    goto :fail
)
goto :done

:rebuild
set "MODE=%~2"
if /I "%MODE%"=="debug" (
    call "%~f0" clean
    if errorlevel 1 goto :fail
    call "%~f0" configure ninja-debug
    if errorlevel 1 goto :fail
    call "%~f0" build build-ninja-debug
    if errorlevel 1 goto :fail
    goto :done
)

if /I "%MODE%"=="static" (
    call "%~f0" clean
    if errorlevel 1 goto :fail
    call "%~f0" configure msvc-static
    if errorlevel 1 goto :fail
    call "%~f0" build build-msvc-static-release
    if errorlevel 1 goto :fail
    goto :done
)

if /I "%MODE%"=="vcpkg" (
    call "%~f0" clean
    if errorlevel 1 goto :fail
    call "%~f0" configure msvc-vcpkg
    if errorlevel 1 goto :fail
    call "%~f0" build build-msvc-vcpkg-release
    if errorlevel 1 goto :fail
    goto :done
)

if /I "%MODE%"=="vcpkg-static" (
    call "%~f0" clean
    if errorlevel 1 goto :fail
    call "%~f0" configure msvc-static-vcpkg
    if errorlevel 1 goto :fail
    call "%~f0" build build-msvc-static-vcpkg-release
    if errorlevel 1 goto :fail
    goto :done
)

if /I "%MODE%"=="release" set "MODE="
if not "%MODE%"=="" (
    echo [procrogue] ERROR: unknown rebuild mode "%MODE%". Use release, debug, static, vcpkg, or vcpkg-static.
    goto :fail
)

call "%~f0" clean
if errorlevel 1 goto :fail
call "%~f0" configure msvc
if errorlevel 1 goto :fail
call "%~f0" build build-msvc-release
if errorlevel 1 goto :fail
goto :done

:doctor
echo [procrogue] Script: %~f0
echo [procrogue] Repo:   %CD%
echo.
call :bootstrap_toolchain optional
call :report_tool cmake
call :report_tool ctest
call :report_tool ninja
call :report_tool cl
call :report_tool clang-cl
call :report_tool clang++
call :report_tool g++
echo.
if defined PROCROGUE_TOOLCHAIN_READY (
    echo [procrogue] TOOLCHAIN: initialized - !PROCROGUE_TOOLCHAIN_LABEL!
) else (
    call :find_vcvars
    if errorlevel 1 (
        echo [procrogue] TOOLCHAIN: no vcvars64.bat discovered.
    ) else (
        echo [procrogue] TOOLCHAIN: vcvars64.bat available at "!VCVARS_PATH!"
    )
)
echo.
where cmake >nul 2>nul
if not errorlevel 1 cmake --version
goto :done

:presets
call :require_tool cmake
if errorlevel 1 goto :fail
cmake --list-presets
if errorlevel 1 goto :fail
goto :done

:help
echo Usage: procrogue.bat [command] [arg]
echo.
echo Commands:
echo   play [release^|debug^|static^|vcpkg^|vcpkg-static] Configure + build + run. Default command.
echo   configure [preset]      Configure CMake preset.
echo   build [build-preset]    Build CMake preset.
echo   run [release^|debug]    Run existing ProcRogue.exe.
echo   test [test-preset]      Configure + build + run tests.
echo   rebuild [release^|debug^|static^|vcpkg^|vcpkg-static] Clean + configure + build.
echo   clean                   Delete build directory.
echo   doctor                  Show toolchain diagnostics.
echo   presets                 List CMake presets.
echo   help                    Show this help.
echo.
echo Examples:
echo   procrogue.bat
echo   procrogue.bat configure tests
echo   procrogue.bat build tests
echo   procrogue.bat test
echo   procrogue.bat play debug
echo   procrogue.bat play static
echo   procrogue.bat play vcpkg
echo   procrogue.bat play vcpkg-static
echo.
echo Environment:
echo   PROCROGUE_SKIP_TOOLCHAIN=1   Skip automatic vcvars64 initialization.
echo   PROCROGUE_VCVARS_PATH=...    Force a specific vcvars64.bat path.
echo   PROCROGUE_STOP_STALE_BUILD_TOOLS=1  Kill stale build/game processes before configure/build/test/clean ^(default: 1^).
goto :done

:help_fail
echo [procrogue] Use "procrogue.bat help" for usage.
goto :fail

:require_tool
where %~1 >nul 2>nul
if errorlevel 1 (
    echo [procrogue] ERROR: required tool "%~1" not found in PATH.
    exit /b 1
)
exit /b 0

:report_tool
where %~1 >nul 2>nul
if errorlevel 1 (
    echo [procrogue] MISSING: %~1
    exit /b 0
)
for /f "delims=" %%P in ('where %~1 2^>nul') do (
    echo [procrogue] FOUND: %~1 = %%P
    goto :report_tool_done
)
:report_tool_done
exit /b 0

:bootstrap_toolchain
set "TOOLCHAIN_MODE=%~1"
if "%TOOLCHAIN_MODE%"=="" set "TOOLCHAIN_MODE=optional"

if defined PROCROGUE_TOOLCHAIN_READY exit /b 0
if /I "%PROCROGUE_SKIP_TOOLCHAIN%"=="1" exit /b 0

call :require_native_compiler silent
if not errorlevel 1 (
    set "PROCROGUE_TOOLCHAIN_READY=1"
    set "PROCROGUE_TOOLCHAIN_LABEL=already in PATH"
    exit /b 0
)

call :find_vcvars
if errorlevel 1 (
    if /I "%TOOLCHAIN_MODE%"=="required" (
        echo [procrogue] ERROR: no C/C++ compiler in PATH and vcvars64.bat was not found.
        echo [procrogue] HINT: install Visual Studio Build Tools with C++ workload, then rerun.
        echo [procrogue] HINT: or set PROCROGUE_VCVARS_PATH to a valid vcvars64.bat file.
        exit /b 1
    )
    exit /b 0
)

echo [procrogue] Initializing MSVC toolchain...
call "%VCVARS_PATH%" >nul
if errorlevel 1 (
    if /I "%TOOLCHAIN_MODE%"=="required" (
        echo [procrogue] ERROR: vcvars64 initialization failed.
        echo [procrogue] PATH: "%VCVARS_PATH%"
        exit /b 1
    )
    exit /b 0
)

call :require_native_compiler silent
if errorlevel 1 (
    if /I "%TOOLCHAIN_MODE%"=="required" (
        echo [procrogue] ERROR: toolchain initialized but no compiler is visible in PATH.
        exit /b 1
    )
    exit /b 0
)

set "PROCROGUE_TOOLCHAIN_READY=1"
set "PROCROGUE_TOOLCHAIN_LABEL=%VCVARS_PATH%"
exit /b 0

:require_native_compiler
set "COMPILER_MODE=%~1"
where cl >nul 2>nul
if not errorlevel 1 exit /b 0
where clang-cl >nul 2>nul
if not errorlevel 1 exit /b 0
where clang++ >nul 2>nul
if not errorlevel 1 exit /b 0
where g++ >nul 2>nul
if not errorlevel 1 exit /b 0
if /I "%COMPILER_MODE%"=="silent" exit /b 1
echo [procrogue] ERROR: no native C/C++ compiler found (cl / clang-cl / clang++ / g++).
exit /b 1

:find_vcvars
set "VCVARS_PATH="

if defined PROCROGUE_VCVARS_PATH (
    if exist "%PROCROGUE_VCVARS_PATH%" (
        set "VCVARS_PATH=%PROCROGUE_VCVARS_PATH%"
        exit /b 0
    )
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%P in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat 2^>nul`) do (
        if exist "%%P" (
            set "VCVARS_PATH=%%P"
            exit /b 0
        )
    )
)

for %%V in (2022 2019 2017) do (
    for %%E in (Community Professional Enterprise BuildTools) do (
        if exist "%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS_PATH=%ProgramFiles%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
            exit /b 0
        )
        if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
            set "VCVARS_PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
            exit /b 0
        )
    )
)

exit /b 1

:check_config_output
set "PRESET_DIR="
if /I "%~1"=="msvc" set "PRESET_DIR=build\msvc"
if /I "%~1"=="msvc-vcpkg" set "PRESET_DIR=build\msvc-vcpkg"
if /I "%~1"=="msvc-static" set "PRESET_DIR=build\msvc-static"
if /I "%~1"=="msvc-static-vcpkg" set "PRESET_DIR=build\msvc-static-vcpkg"
if /I "%~1"=="ninja-debug" set "PRESET_DIR=build\ninja-debug"
if /I "%~1"=="ninja-release" set "PRESET_DIR=build\ninja-release"
if /I "%~1"=="ninja-release-tests" set "PRESET_DIR=build\ninja-release-tests"
if /I "%~1"=="tests" set "PRESET_DIR=build\tests"

if not "%PRESET_DIR%"=="" (
    if not exist "%CD%\%PRESET_DIR%\CMakeCache.txt" (
        echo [procrogue] ERROR: configure did not produce %PRESET_DIR%\CMakeCache.txt
        exit /b 1
    )
)
exit /b 0

:maybe_stop_stale_build_tools
if /I not "%PROCROGUE_STOP_STALE_BUILD_TOOLS%"=="1" exit /b 0
echo [procrogue] Stopping stale build processes...
for %%P in (
    cmake.exe
    ninja.exe
    MSBuild.exe
    cl.exe
    link.exe
    mspdbsrv.exe
    ProcRogue.exe
    ProcRogueHeadless.exe
    procrogue_tests.exe
) do (
    taskkill /f /t /im "%%P" >nul 2>nul
)
timeout /t 1 /nobreak >nul
exit /b 0

:resolve_game_exe
set "GAME_EXE="
set "CFG=%~1"
if /I "%CFG%"=="debug" set "CFG=Debug"
if /I "%CFG%"=="release" set "CFG=Release"
if "%CFG%"=="" set "CFG=Release"

if /I "%CFG%"=="Debug" (
    if exist "%CD%\build\ninja-debug\ProcRogue.exe" set "GAME_EXE=%CD%\build\ninja-debug\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc\Debug\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc\Debug\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-vcpkg\Debug\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-vcpkg\Debug\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-static\Debug\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-static\Debug\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-static-vcpkg\Debug\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-static-vcpkg\Debug\ProcRogue.exe"
)

if /I "%CFG%"=="Release" (
    if exist "%CD%\build\msvc\Release\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc\Release\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-vcpkg\Release\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-vcpkg\Release\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-static\Release\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-static\Release\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\msvc-static-vcpkg\Release\ProcRogue.exe" set "GAME_EXE=%CD%\build\msvc-static-vcpkg\Release\ProcRogue.exe"
    if not defined GAME_EXE if exist "%CD%\build\ninja-release\ProcRogue.exe" set "GAME_EXE=%CD%\build\ninja-release\ProcRogue.exe"
)
exit /b 0

:fail
set "RC=%ERRORLEVEL%"
if "%RC%"=="" set "RC=1"
if "%RC%"=="0" set "RC=1"
popd >nul
exit /b %RC%

:done
popd >nul
exit /b 0
