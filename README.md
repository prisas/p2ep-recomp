# Persona 2: Eternal Punishment — static recompilation for Windows

A native Windows build of the PSP game *Persona 2: Eternal Punishment*. The game's machine code is
translated to C++ ahead of time and compiled into an ordinary 64-bit program; a purpose-built
runtime replaces the console's system software. There is no interpreter or JIT compiler in the
loop.

**No game code or data is included.** You need your own disc image of the game. The build
translates the executable in your image on your machine, and the finished program reads the same
image for its data.

## Supported game build

The English-patched build of the Japanese release (`NPJH-50581`), identified by the SHA-256 of its
executable:

```
b66bec74b78cc3e9017147135af2165e90a11e63db6767e8f28148eede03efd9
```

Disc images in ISO and CHD format are accepted.

## Features

- Ahead-of-time translation of the whole executable, including the routines the translation patch
  adds outside the regular code sections.
- Vulkan renderer that draws at up to ten times the original 480x272; by default it matches the
  window, so the picture is sharp at any size.
- Deterministic emulated clock paced to the console's 59.94 Hz, with hold-to-fast-forward at a
  chosen or unlimited speed.
- Instant disc access: data is read straight from the image with no simulated drive delays.
- An in-game settings menu (Esc) for video, audio and controls; settings persist.
- An optional quick start that skips the start-up notices, logos and opening movie and boots
  straight to the main menu.
- Texture packs: the game's textures can be dumped, upscaled with your own tools and loaded back
  in place of the originals ([docs/TEXTURE_PACKS.md](docs/TEXTURE_PACKS.md)). Packs are made from
  the game's artwork, so none are included here.
- Save, load and message screens of its own in place of the console's, with save pictures and
  descriptions. Saves are plain files in a normal folder.
- Keyboard (rebindable) and gamepad input, hot-pluggable.
- Fast: the opening minutes of the game run at about 43 times real time on a current desktop
  processor when left unthrottled, so normal play uses a few percent of one core.

## Quick start

1. Install Visual Studio 2022 with the *Desktop development with C++* workload and the
   *C++ Clang tools for Windows* component.
2. Build, pointing the script at your disc image:

   ```powershell
   .\scripts\build.ps1 -Image "D:\path\to\your-image.chd"
   ```

3. Run:

   ```powershell
   .\build\release\bin\p2ep.exe "D:\path\to\your-image.chd"
   ```

Details are in [docs/BUILDING.md](docs/BUILDING.md); controls and command-line options are in
[docs/OPTIONS.md](docs/OPTIONS.md); how it all works is in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Status

The game starts, plays its movies, runs the opening of the story into the first dungeon, fights
battles (including spells, talking to demons, level-ups and game over), and saves and loads. See
[docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) for exactly what has been verified, what has not,
and the known limitations.

## Legal

This project contains no code or assets from the game and is not affiliated with or endorsed by
its publisher. You are responsible for using a disc image you are entitled to use. Third-party
libraries are used under their own licences, listed in [docs/BUILDING.md](docs/BUILDING.md).

This repository is published without a licence: you are welcome to read the code, but no
permission is granted to copy, modify or redistribute it.
