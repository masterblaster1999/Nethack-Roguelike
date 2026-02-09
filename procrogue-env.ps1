param(
    [switch]$Force,
    [switch]$Quiet,
    [string]$RepairPreset,
    [switch]$StopStaleBuildTools
)

$ErrorActionPreference = "Stop"

$KnownPresetDirs = @{
    "tests" = "build/tests"
    "ninja-debug" = "build/ninja-debug"
    "ninja-release" = "build/ninja-release"
    "ninja-release-tests" = "build/ninja-release-tests"
}

$BuildToolNames = @(
    "cmake",
    "ninja",
    "MSBuild",
    "cl",
    "link",
    "mspdbsrv",
    "ProcRogue",
    "ProcRogueHeadless",
    "procrogue_tests"
)

function Get-BuildToolProcesses {
    return Get-Process -ErrorAction SilentlyContinue | Where-Object { $BuildToolNames -contains $_.ProcessName }
}

function Stop-BuildTools {
    $tools = Get-BuildToolProcesses
    if ($tools) {
        foreach ($proc in $tools) {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

function Test-NativeCompiler {
    foreach ($name in @("cl", "clang-cl", "clang++", "g++")) {
        if (Get-Command $name -ErrorAction SilentlyContinue) {
            return $true
        }
    }
    return $false
}

function Get-PrimaryCompilerPath {
    foreach ($name in @("cl", "clang-cl", "clang++", "g++")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) {
            return $cmd.Source
        }
    }
    return $null
}

function Find-VcVars64 {
    if ($env:PROCROGUE_VCVARS_PATH -and (Test-Path -LiteralPath $env:PROCROGUE_VCVARS_PATH)) {
        return (Resolve-Path -LiteralPath $env:PROCROGUE_VCVARS_PATH).Path
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $queries = @(
            @(
                "-latest",
                "-products", "*",
                "-version", "[17.0,18.0)",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-find", "VC\Auxiliary\Build\vcvars64.bat"
            ),
            @(
                "-latest",
                "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-find", "VC\Auxiliary\Build\vcvars64.bat"
            )
        )

        foreach ($query in $queries) {
            $candidate = & $vswhere @query 2>$null | Select-Object -First 1
            if ($candidate -and (Test-Path -LiteralPath $candidate)) {
                return $candidate
            }
        }
    }

    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
    $versions = @("2022", "2019", "2017")
    $editions = @("Community", "Professional", "Enterprise", "BuildTools")

    foreach ($root in $roots) {
        foreach ($version in $versions) {
            foreach ($edition in $editions) {
                $candidate = Join-Path $root "Microsoft Visual Studio\$version\$edition\VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    return $null
}

function Repair-PresetCacheIfNeeded {
    param(
        [string]$PresetName
    )

    if (-not $PresetName) {
        return
    }

    if (-not $KnownPresetDirs.ContainsKey($PresetName)) {
        throw "[procrogue] Unknown preset '$PresetName' for -RepairPreset."
    }

    $buildDir = Join-Path $PSScriptRoot $KnownPresetDirs[$PresetName]
    if (-not (Test-Path -LiteralPath $buildDir)) {
        return
    }

    $cachePath = Join-Path $buildDir "CMakeCache.txt"
    $cmakeFilesPath = Join-Path $buildDir "CMakeFiles"

    function Remove-PathWithRetry {
        param(
            [string]$PathToRemove
        )

        if (-not (Test-Path -LiteralPath $PathToRemove)) {
            return
        }

        $attempts = 6
        for ($i = 1; $i -le $attempts; $i++) {
            try {
                Remove-Item -Recurse -Force -LiteralPath $PathToRemove -ErrorAction Stop
                return
            } catch {
                if ($i -eq $attempts) {
                    $holders = Get-BuildToolProcesses
                    if ($holders) {
                        $holderText = $holders | ForEach-Object { "$($_.ProcessName):$($_.Id)" }
                        throw "[procrogue] Failed to clean '$PathToRemove' (likely locked by running build tools: $($holderText -join ', ')). Stop those processes and retry."
                    }
                    throw "[procrogue] Failed to clean '$PathToRemove'. Close tools using the build directory and retry."
                }
                Start-Sleep -Milliseconds 350
            }
        }
    }

    if (-not (Test-Path -LiteralPath $cachePath)) {
        if (Test-Path -LiteralPath $cmakeFilesPath) {
            Remove-PathWithRetry -PathToRemove $cmakeFilesPath
            if (-not $Quiet) {
                Write-Host "[procrogue] Removed stale $($KnownPresetDirs[$PresetName])/CMakeFiles (cache missing)."
            }
        }
        return
    }

    $cacheText = Get-Content -LiteralPath $cachePath -Raw
    $needsRepair = $false

    if ($cacheText -match 'CMAKE_C_COMPILER:FILEPATH=.*NOTFOUND') { $needsRepair = $true }
    if ($cacheText -match 'CMAKE_CXX_COMPILER:FILEPATH=.*NOTFOUND') { $needsRepair = $true }

    if ($PresetName.StartsWith("ninja-")) {
        if ($cacheText -notmatch 'CMAKE_GENERATOR:INTERNAL=Ninja') {
            $needsRepair = $true
        }
    }

    if (-not $needsRepair) {
        return
    }

    $cleanupPaths = @(
        $cachePath,
        $cmakeFilesPath,
        (Join-Path $buildDir "build.ninja"),
        (Join-Path $buildDir ".ninja_deps"),
        (Join-Path $buildDir ".ninja_log"),
        (Join-Path $buildDir "cmake_install.cmake"),
        (Join-Path $buildDir "CTestTestfile.cmake")
    )

    foreach ($path in $cleanupPaths) {
        Remove-PathWithRetry -PathToRemove $path
    }

    if (-not $Quiet) {
        Write-Host "[procrogue] Repaired stale cache for preset '$PresetName' in $($KnownPresetDirs[$PresetName])."
    }
}

if ($RepairPreset) {
    if ($StopStaleBuildTools) {
        $stale = Get-BuildToolProcesses
        if ($stale) {
            Stop-BuildTools
            Start-Sleep -Milliseconds 250
            if (-not $Quiet) {
                Write-Host "[procrogue] Stopped stale build processes before cache repair."
            }
        }
    }
    Repair-PresetCacheIfNeeded -PresetName $RepairPreset
}

if ((Test-NativeCompiler) -and -not $Force) {
    if (-not $Quiet) {
        $compilerPath = Get-PrimaryCompilerPath
        Write-Host "[procrogue] Native compiler already on PATH: $compilerPath"
    }
    return
}

$vcvarsPath = Find-VcVars64
if (-not $vcvarsPath) {
    throw "[procrogue] Could not locate vcvars64.bat. Install VS Build Tools (C++ workload) or set PROCROGUE_VCVARS_PATH."
}

$envDump = & cmd.exe /d /s /c "`"$vcvarsPath`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw "[procrogue] Failed to initialize vcvars64.bat at '$vcvarsPath'."
}

foreach ($line in $envDump) {
    if ($line -match "^([^=]+)=(.*)$") {
        $name = $matches[1]
        $value = $matches[2]
        if ($name.StartsWith("=")) {
            continue
        }
        [System.Environment]::SetEnvironmentVariable($name, $value, "Process")
    }
}

if (-not (Test-NativeCompiler)) {
    throw "[procrogue] vcvars64 loaded but no native compiler is visible on PATH."
}

if (-not $Quiet) {
    $compilerPath = Get-PrimaryCompilerPath
    Write-Host "[procrogue] Toolchain initialized from: $vcvarsPath"
    Write-Host "[procrogue] Compiler on PATH: $compilerPath"
    Write-Host "[procrogue] Ready for Ninja presets:"
    Write-Host "  cmake --preset tests"
    Write-Host "  cmake --build --preset tests"
    Write-Host "  ctest --preset tests --output-on-failure"
}
