# Puts the Visual Studio 2022 build tools (CMake, Ninja, clang-cl, Windows SDK) on PATH for this session.
function Enter-DevShell {
    if ($env:P2EP_DEVSHELL -eq "1") { return }

    $installer = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer"
    $vswhere = Join-Path $installer "vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "Visual Studio 2022 was not found (vswhere.exe is missing). Install it with the 'Desktop development with C++' workload plus the 'C++ Clang tools for Windows' component."
    }

    $vs = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang `
        -property installationPath
    if (-not $vs) {
        throw "No Visual Studio installation with the MSVC toolset and 'C++ Clang tools for Windows' was found."
    }

    $env:PATH = "$installer;$env:PATH"
    & (Join-Path $vs "Common7\Tools\Launch-VsDevShell.ps1") -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

    foreach ($tool in "cmake", "ninja", "clang-cl") {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "'$tool' is not available in the Visual Studio developer shell. Add the matching component in the Visual Studio Installer."
        }
    }
    $env:P2EP_DEVSHELL = "1"
}
