<#
.SYNOPSIS
    Collects a built p2ep and everything it needs to run into one folder.
.PARAMETER Preset
    The build to package: release (default) or release-lto.
.PARAMETER Output
    Destination folder. Default: dist\p2ep in the repository.
.NOTES
    The program in the package contains the translation of one particular game executable. It is
    meant for the person who built it; it contains no game data and still needs the disc image.
#>
[CmdletBinding()]
param(
    [ValidateSet("release", "release-lto")]
    [string]$Preset = "release",
    [string]$Output = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Preset\bin"
$exe = Join-Path $bin "p2ep.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "There is no $exe. Build first: .\scripts\build.ps1 -Image <your image> -Preset $Preset"
}
if (-not $Output) { $Output = Join-Path $root "dist\p2ep" }

# Replace everything but the user folder: saves and settings live next to the program.
if (Test-Path -LiteralPath $Output) {
    Get-ChildItem -LiteralPath $Output -Force | Where-Object { $_.Name -ne "userdata" } | Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $Output | Out-Null
}
Copy-Item -LiteralPath $exe -Destination $Output
# The texture upscaler, which the settings menu runs (it builds with the game).
$texup = Join-Path $bin "p2texup.exe"
if (Test-Path -LiteralPath $texup) { Copy-Item -LiteralPath $texup -Destination $Output }
else { Write-Warning "No $texup: the settings' Upscale button will be unavailable." }
Get-ChildItem -LiteralPath $bin -Filter *.dll | Copy-Item -Destination $Output

# Third-party licence texts travel with the binaries, as their licences require.
$licences = Join-Path $Output "licences"
New-Item -ItemType Directory -Path $licences | Out-Null
$installed = Join-Path $root "build\vcpkg_installed"
if ($root -match " ") { $installed = Join-Path $env:LOCALAPPDATA "p2ep-build\vcpkg_installed" }
foreach ($port in "sdl3", "ffmpeg", "volk", "vulkan-headers", "vulkan-memory-allocator", "stb") {
    $text = Join-Path $installed "x64-windows\share\$port\copyright"
    if (Test-Path -LiteralPath $text) { Copy-Item -LiteralPath $text -Destination (Join-Path $licences "$port.txt") }
    else { Write-Warning "No licence text found for $port." }
}
foreach ($fetched in "imgui", "libchdr") {
    $text = Get-ChildItem -LiteralPath (Join-Path $root "build\$Preset\_deps\$fetched-src") -Filter "LICENSE*" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($text) { Copy-Item -LiteralPath $text.FullName -Destination (Join-Path $licences "$fetched.txt") }
    else { Write-Warning "No licence text found for $fetched." }
}

foreach ($doc in "README.md", "docs\OPTIONS.md", "docs\COMPATIBILITY.md") {
    $path = Join-Path $root $doc
    if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $Output }
}

$size = (Get-ChildItem -LiteralPath $Output -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("Packaged {0} ({1:N1} MB). Start {0}\p2ep.exe and choose your disc image in the launcher." -f $Output, ($size / 1MB))
