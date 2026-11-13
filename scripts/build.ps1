<#
.SYNOPSIS
    Configures and builds p2ep.
.PARAMETER Image
    Path to your disc image of the game. Required for the game itself; without it only the tools and tests build.
.PARAMETER Preset
    release (default), release-lto or dev.
.PARAMETER Target
    Optional single build target.
.PARAMETER Test
    Run the unit tests after building.
.PARAMETER Fresh
    Discard the existing CMake cache first.
#>
[CmdletBinding()]
param(
    [string]$Image = "",
    [ValidateSet("release", "release-lto", "dev")]
    [string]$Preset = "release",
    [string]$Target = "",
    [switch]$Test,
    [switch]$Fresh
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "devshell.ps1")
Enter-DevShell

$configure = @("--preset", $Preset)
if ($Fresh) { $configure += "--fresh" }

# Some third-party packages cannot be built under a path that contains spaces. When this checkout
# lives in one, their build and install trees go to a space-free folder in the local app data.
$packages = Join-Path $root "build"
if ($root -match " ") { $packages = Join-Path $env:LOCALAPPDATA "p2ep-build" }
$packages = $packages -replace "\\", "/"
$configure += "-DVCPKG_INSTALLED_DIR=$packages/vcpkg_installed"
$configure += "-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=$packages/vcpkg_buildtrees;--x-packages-root=$packages/vcpkg_packages"
if ($Image) {
    $resolved = (Resolve-Path -LiteralPath $Image).Path -replace "\\", "/"
    $configure += "-DP2EP_IMAGE=$resolved"
}

Push-Location $root
try {
    cmake @configure
    if ($LASTEXITCODE) { throw "CMake configure failed." }

    $build = @("--build", "--preset", $Preset)
    if ($Target) { $build += @("--target", $Target) }
    cmake @build
    if ($LASTEXITCODE) { throw "Build failed." }

    if ($Test) {
        ctest --preset $Preset
        if ($LASTEXITCODE) { throw "Tests failed." }
    }
}
finally {
    Pop-Location
}
