<#
.SYNOPSIS
    Collects a built p2ep and everything it needs to run into one folder.
.PARAMETER Preset
    The build to package: release (default) or release-lto.
.PARAMETER Output
    Destination folder. Default: dist\p2ep in the repository.
.PARAMETER NoRealEsrgan
    Leave out Real-ESRGAN (the second texture upscaling method, about 45 MB, downloaded once).
.PARAMETER NoCommunityPacks
    Leave out the community texture packs (about 1.1 GB, downloaded once).
.NOTES
    The program in the package contains the translation of one particular game executable. It is
    meant for the person who built it; it contains no game data and still needs the disc image.
    Real-ESRGAN and the community texture packs are downloaded from their authors' pages into
    build\downloads (see scripts\downloads.ps1 and third_party\CREDITS.md); a failed download only
    leaves that part out.
#>
[CmdletBinding()]
param(
    [ValidateSet("release", "release-lto")]
    [string]$Preset = "release",
    [string]$Output = "",
    [switch]$NoRealEsrgan,
    [switch]$NoCommunityPacks
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\$Preset\bin"
$exe = Join-Path $bin "p2ep.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "There is no $exe. Build first: .\scripts\build.ps1 -Image <your image> -Preset $Preset"
}
if (-not $Output) { $Output = Join-Path $root "dist\p2ep" }

. (Join-Path $PSScriptRoot "downloads.ps1")
$downloads = Join-Path $root "build\downloads"

# The extras are large: a folder installed from the same pinned download is kept as it is (its
# .version file names the download's SHA-256).
function Test-P2Installed([string]$Folder, [string]$Name) {
    $stamp = Join-Path $Folder ".version"
    (Test-Path -LiteralPath $stamp) -and ((Get-Content -LiteralPath $stamp -Raw).Trim() -eq $script:P2Extras[$Name].Sha256)
}
$keepRealEsrgan = -not $NoRealEsrgan -and (Test-P2Installed (Join-Path $Output "realesrgan") "realesrgan")
$keepPacks = -not $NoCommunityPacks -and (Test-P2Installed (Join-Path $Output "texturepacks\ryuubu") "ryuubu") -and
    (Test-P2Installed (Join-Path $Output "texturepacks\rarno") "rarno")

# Replace everything but the user folder (saves and settings live next to the program) and the extras kept.
if (Test-Path -LiteralPath $Output) {
    Get-ChildItem -LiteralPath $Output -Force |
        Where-Object { $_.Name -ne "userdata" -and -not ($_.Name -eq "realesrgan" -and $keepRealEsrgan) -and -not ($_.Name -eq "texturepacks" -and $keepPacks) } |
        Remove-Item -Recurse -Force
} else {
    New-Item -ItemType Directory -Path $Output | Out-Null
}
Copy-Item -LiteralPath $exe -Destination $Output
# The texture upscaler, which the settings menu runs (it builds with the game).
$texup = Join-Path $bin "p2texup.exe"
if (Test-Path -LiteralPath $texup) { Copy-Item -LiteralPath $texup -Destination $Output }
else { Write-Warning "No ${texup}: the settings' Upscale button will be unavailable." }
Get-ChildItem -LiteralPath $bin -Filter *.dll | Copy-Item -Destination $Output

# Third-party licence texts travel with the binaries, as their licences require.
$licences = Join-Path $Output "licences"
New-Item -ItemType Directory -Path $licences | Out-Null
$installed = Join-Path $root "build\vcpkg_installed"
if ($root -match " ") { $installed = Join-Path $env:LOCALAPPDATA "p2ep-build\vcpkg_installed" }
foreach ($port in "sdl3", "ffmpeg", "volk", "vulkan-headers", "vulkan-memory-allocator", "stb", "xxhash") {
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

# Real-ESRGAN: the program, its OpenMP runtime and its models (not the demo pictures or the debug
# runtime that come in the same zip).
if (-not $NoRealEsrgan -and -not $keepRealEsrgan) {
    $zip = Get-P2Extra "realesrgan" $downloads
    if ($zip) {
        $staging = Join-Path $downloads "realesrgan-unpacked"
        Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Path $staging | Out-Null
        & tar -xf $zip -C $staging
        $target = Join-Path $Output "realesrgan"
        New-Item -ItemType Directory -Force -Path (Join-Path $target "models") | Out-Null
        Copy-Item -LiteralPath (Join-Path $staging "realesrgan-ncnn-vulkan.exe"), (Join-Path $staging "vcomp140.dll") -Destination $target
        Copy-Item -Path (Join-Path $staging "models\*") -Destination (Join-Path $target "models")
        Set-Content -LiteralPath (Join-Path $target ".version") -Value $script:P2Extras.realesrgan.Sha256 -NoNewline
        Remove-Item -LiteralPath $staging -Recurse -Force
    } else {
        Write-Warning "Real-ESRGAN left out: the settings will offer xBR only."
    }
}
if (-not $NoRealEsrgan -and (Test-Path -LiteralPath (Join-Path $Output "realesrgan"))) {
    Get-ChildItem -LiteralPath (Join-Path $root "third_party\realesrgan") -Filter *.txt | Copy-Item -Destination $licences
}

# Community texture packs, unchanged, each with our pack.ini (credits, tie order, files left out).
$packInfo = [ordered]@{
    ryuubu = "name = UI Upscale`nauthor = Ryuubu`nlicence = no licence stated`nlink = https://mega.nz/file/ndgwUYjS`norder = 1`n" +
             "# The thin font sheet lacks the English patch's comma and accented letters (its changed half is not hashed).`n" +
             "exclude = 00000000e56222f48c209cf5, 00000000e56222f4e5e68b11`n"
    rarno  = "name = Eternal Punishment HD Textures 1.03`nauthor = Rarno`nlicence = CC BY-NC-ND 4.0`nlink = https://gamebanana.com/mods/355619`norder = 2`n" +
             "# The thin font sheet lacks the English patch's comma and accented letters (its changed half is not hashed).`n" +
             "exclude = 00000000e56222f48c209cf5, 00000000e56222f4e5e68b11`n"
}
if (-not $NoCommunityPacks -and -not $keepPacks) {
    foreach ($name in $packInfo.Keys) {
        $archive = Get-P2Extra $name $downloads
        if (-not $archive) { Write-Warning "Community pack $name left out."; continue }
        $target = Join-Path $Output "texturepacks\$name"
        Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force -Path $target | Out-Null
        Write-Host "Unpacking $($script:P2Extras[$name].Title)..."
        & tar -xf $archive -C $target
        if ($LASTEXITCODE -ne 0) { Write-Warning "Could not unpack $archive."; Remove-Item -LiteralPath $target -Recurse -Force; continue }
        Set-Content -LiteralPath (Join-Path $target ".version") -Value $script:P2Extras[$name].Sha256 -NoNewline
    }
}
if (Test-Path -LiteralPath (Join-Path $Output "texturepacks")) {
    foreach ($name in $packInfo.Keys) {
        $target = Join-Path $Output "texturepacks\$name"
        if (Test-Path -LiteralPath $target) { [IO.File]::WriteAllText((Join-Path $target "pack.ini"), $packInfo[$name]) }
    }
    Copy-Item -LiteralPath (Join-Path $root "third_party\CREDITS.md") -Destination (Join-Path $Output "texturepacks\CREDITS.md")
}

$size = (Get-ChildItem -LiteralPath $Output -Recurse -File | Measure-Object -Property Length -Sum).Sum
Write-Host ("Packaged {0} ({1:N1} MB). Start {0}\p2ep.exe and choose your disc image in the launcher." -f $Output, ($size / 1MB))
