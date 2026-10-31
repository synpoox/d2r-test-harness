# Configures and builds the test-harness plugin (Visual Studio generator, x64, Release).
#   powershell -ExecutionPolicy Bypass -File build.ps1 [-Sdk <PluginSDK checkout>] [-Build <dir>]
# Output: <Build>\Release\test-harness.dll -> copy it to <game>\d2rloader\plugins\.
param(
    [string]$Sdk = "",                             # PluginSDK directory (default the third_party/PluginSDK submodule, SDK 0.3.0)
    [string]$Build = ""                            # build directory (default <this folder>\build)
)
$ErrorActionPreference = 'Stop'
$Source = $PSScriptRoot
if (-not $Build) { $Build = Join-Path $Source "build" }
if (-not $Sdk) { $Sdk = Join-Path $Source "third_party\PluginSDK" }
cmake -S $Source -B $Build -A x64 "-DD2RL_SDK_DIR=$((Resolve-Path $Sdk).Path)"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $Build --config Release
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
$dll = Join-Path $Build "Release\test-harness.dll"
if (-not (Test-Path $dll)) { throw "build finished but $dll is missing" }
Write-Output "built: $dll"

