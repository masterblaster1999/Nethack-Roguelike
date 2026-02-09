[CmdletBinding()]
param(
    [string]$Preset = "tests",
    [string]$BuildPreset,
    [switch]$ConfigureOnly,
    [switch]$SkipBuild,
    [switch]$SkipTest,
    [switch]$QuietEnv,
    [switch]$ForceEnv,
    [switch]$StopStaleBuildTools,
    [switch]$AllowConcurrentBuildTools,
    [int]$ConfigureTimeoutSec = 180,
    [int]$BuildTimeoutSec = 1200,
    [int]$TestTimeoutSec = 600
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

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

function Require-Tool {
    param(
        [string]$Name
    )

    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "[procrogue] Required tool '$Name' was not found in PATH."
    }
}

function Format-Command {
    param(
        [string]$Exe,
        [string[]]$Arguments
    )

    $parts = @($Exe) + $Arguments
    return ($parts -join " ")
}

function Invoke-Step {
    param(
        [string]$Exe,
        [string[]]$Arguments,
        [int]$TimeoutSec
    )

    if (-not $Arguments -or $Arguments.Count -eq 0) {
        throw "[procrogue] Internal error: no arguments provided for '$Exe'."
    }

    if ($TimeoutSec -le 0) {
        throw "[procrogue] Timeout must be > 0 seconds for '$Exe'."
    }

    $cmdLine = Format-Command -Exe $Exe -Arguments $Arguments
    Write-Host "[procrogue] $cmdLine"

    $proc = Start-Process -FilePath $Exe -ArgumentList $Arguments -NoNewWindow -PassThru

    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        try {
            Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
        } catch {
        }
        $lingering = Get-BuildToolProcesses
        if ($lingering) {
            foreach ($p in $lingering) {
                Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
            }
        }
        throw "[procrogue] Timed out after ${TimeoutSec}s: $cmdLine"
    }

    $exitCode = $proc.ExitCode
    if ($exitCode -ne 0) {
        throw "[procrogue] Command failed with exit code ${exitCode}: $cmdLine"
    }
}

function Get-BuildToolProcesses {
    return Get-Process -ErrorAction SilentlyContinue | Where-Object { $BuildToolNames -contains $_.ProcessName }
}

function Stop-BuildTools {
    $tools = Get-BuildToolProcesses
    if ($tools) {
        foreach ($p in $tools) {
            Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
        }
    }
}

try {
    $presetNormalized = $Preset.Trim()
    if (-not $presetNormalized) {
        throw "[procrogue] -Preset cannot be empty."
    }

    if (-not $BuildPreset) {
        $BuildPreset = $presetNormalized
    }

    if ($ConfigureOnly) {
        $SkipBuild = $true
        $SkipTest = $true
    }

    foreach ($pair in @(
        @{ Name = "ConfigureTimeoutSec"; Value = $ConfigureTimeoutSec },
        @{ Name = "BuildTimeoutSec"; Value = $BuildTimeoutSec },
        @{ Name = "TestTimeoutSec"; Value = $TestTimeoutSec }
    )) {
        if ([int]$pair.Value -le 0) {
            throw "[procrogue] -$($pair.Name) must be greater than 0."
        }
    }

    Require-Tool -Name "cmake"
    if (-not $SkipTest) {
        Require-Tool -Name "ctest"
    }

    $runningBuildTools = Get-BuildToolProcesses
    if ($runningBuildTools -and -not $AllowConcurrentBuildTools) {
        $ids = ($runningBuildTools | Select-Object -ExpandProperty Id) -join ", "
        $names = ($runningBuildTools | Select-Object -ExpandProperty ProcessName | Sort-Object -Unique) -join ", "
        Write-Host "[procrogue] Found running build processes ($names | PIDs: $ids). Stopping them to avoid lockups."
        Stop-BuildTools
        Start-Sleep -Milliseconds 300
    }

    $envParams = @{
        RepairPreset = $presetNormalized
        StopStaleBuildTools = $StopStaleBuildTools
        Quiet = $QuietEnv
    }
    if ($ForceEnv) {
        $envParams["Force"] = $true
    }

    & "$PSScriptRoot\procrogue-env.ps1" @envParams

    $start = Get-Date

    Invoke-Step -Exe "cmake" -Arguments @("--preset", $presetNormalized) -TimeoutSec $ConfigureTimeoutSec

    if (-not $SkipBuild) {
        Invoke-Step -Exe "cmake" -Arguments @("--build", "--preset", $BuildPreset) -TimeoutSec $BuildTimeoutSec
    }

    if (-not $SkipTest) {
        Invoke-Step -Exe "ctest" -Arguments @("--preset", $presetNormalized, "--output-on-failure") -TimeoutSec $TestTimeoutSec
    }

    $elapsed = (Get-Date) - $start
    Write-Host ("[procrogue] Completed in {0:n1}s." -f $elapsed.TotalSeconds)
    exit 0
} catch {
    Stop-BuildTools
    Write-Error $_.Exception.Message
    exit 1
}
