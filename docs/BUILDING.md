# Building

## Requirements

- Windows 10 version 1803 or later, 64-bit.
- A graphics card and driver with Vulkan 1.3.
- **Visual Studio 2022** with the *Desktop development with C++* workload and the
  *C++ Clang tools for Windows* component. This provides everything the build uses: CMake, Ninja,
  clang-cl, the Windows SDK and vcpkg. Nothing else has to be installed.
- **Your own disc image of the game**, as an ISO or CHD file. The build supports the
  English-patched build of the Japanese release and identifies it by the checksum of its
  executable:

  ```
  SHA-256  b66bec74b78cc3e9017147135af2165e90a11e63db6767e8f28148eede03efd9
  ```

  `p2recomp info <image>` prints the checksum of an image's executable.

The game's code and data are never part of this repository. The build translates the executable
found in your image into C++ inside the build folder, and the finished program reads the same
image for its data.

## Build

From a PowerShell prompt in the repository root:

```powershell
.\scripts\build.ps1 -Image "D:\path\to\your-image.chd"
```

The script enters the Visual Studio developer environment, downloads and builds the third-party
libraries on first use (this takes a while once), recompiles the game executable and builds
`build\release\bin\p2ep.exe`.

| Option | Meaning |
|---|---|
| `-Image <path>` | Your disc image. Without it only the tools and tests are built. |
| `-Preset release` | Optimised build (default). |
| `-Preset release-lto` | Optimised build with link-time optimisation. Slower to build, slightly faster to run. |
| `-Preset dev` | Optimised with debug information and extra run-time checks (guest call stacks on errors). |
| `-Test` | Run the unit tests after building. |
| `-Fresh` | Discard the CMake cache first. |
| `-Target <name>` | Build a single target, for example `p2recomp`. |

**Artwork.** The project's own artwork is in the `assets` folder at the top of the repository and
is embedded in `p2ep.exe` by the build. Each image may be a PNG or a JPEG, and either can be
replaced by passing another image to CMake:

| File | Use | Instead, pass |
|---|---|---|
| `assets/icon.jpg` | The program's icon, which Explorer, the taskbar and the window's title bar show. The image is centre-cropped to a square. | `-DP2EP_ICON=<image>` |
| `assets/background.jpg` | The picture behind the launcher. It fills the window with its shape kept (anything cut off comes from the left and evenly from top and bottom), under a dark veil that keeps the text readable. | `-DP2EP_BACKGROUND=<image>` |

Without an icon the program has none of its own; without a background the launcher has a plain
dark one.

If the repository lives in a folder whose path contains a space, the third-party libraries are
built under `%LOCALAPPDATA%\p2ep-build` instead of the build folder, because some of them cannot
be built in such a path.

## Run

Start `build\release\bin\p2ep.exe`; the launcher opens, where you choose the disc image and press
*Play*. To skip the launcher, give the image on the command line:

```powershell
.\build\release\bin\p2ep.exe "D:\path\to\your-image.chd"
```

See [OPTIONS.md](OPTIONS.md) for the launcher, command-line options and controls.

## Third-party libraries

Fetched and built automatically, at versions pinned by `vcpkg-configuration.json`,
`lib/CMakeLists.txt` and `runtime/CMakeLists.txt`:

| Library | Use | Licence |
|---|---|---|
| SDL 3 | Window, input, audio device | zlib |
| FFmpeg (libavcodec, libswscale, libswresample) | H.264 and ATRAC decoding | LGPL 2.1 or later |
| Vulkan Headers, volk, Vulkan Memory Allocator | Rendering | Apache 2.0 / MIT |
| glslang | Shader compilation at build time | BSD-3-Clause and others |
| Dear ImGui | Settings menu and system dialogs | MIT |
| libchdr | CHD image support | BSD-3-Clause |
| stb | PNG reading and writing | MIT / public domain |

FFmpeg and SDL are linked dynamically; their DLLs are placed next to the executable.

## Package

```powershell
.\scripts\package.ps1
```

copies the program, the DLLs it needs and the third-party licence texts into `dist\p2ep`, a folder
that runs on its own on any machine that meets the run-time requirements above (Visual Studio is
only needed to build; the Microsoft Visual C++ runtime it also needs is present on almost every
Windows installation, and Microsoft's free "Visual C++ Redistributable" provides it otherwise). It contains no game data: the disc image is still given on the command
line, and the program refuses to run with an image whose executable is not the one it was built
from. Because the program contains the translation of that executable, the package is for your
own use, not for passing on.

## Troubleshooting

**"this executable is not a supported build"** during the recompile step: the image's executable
does not match the checksum above. `--allow-unknown` exists on `p2recomp gen` for experiments, but
the result is untested.

**"indirect jump to …, which is not a known entry point"** at run time: the game reached code the
recompiler did not find. Add the address printed in the message to `data/extra_entries.txt` (one
hexadecimal address per line) and rebuild.
