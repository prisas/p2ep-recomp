# Architecture

This project runs *Persona 2: Eternal Punishment* (PSP) as a native Windows program. The game's
machine code is translated to C++ ahead of time and compiled with the rest of the program; a
runtime stands in for the console's system software. Nothing is interpreted while the game runs.

```
disc image ──► p2recomp ──► generated C++ ──┐
                                            ├──► p2ep.exe ◄── disc image (data files at run time)
runtime (kernel, graphics, audio, media) ───┘
```

The generated C++ is produced on your machine from your own disc image at build time. It is never
part of this repository.

## Repository layout

| Path | Contents |
|---|---|
| `lib/common` | Logging, hashing, file helpers |
| `lib/disc` | Disc image access (ISO and CHD) and the ISO9660 file system |
| `lib/psp` | Executable loader and relocator, `PARAM.SFO`, function-name (NID) tables |
| `lib/allegrex` | Instruction decoder and disassembler for the CPU, FPU and VFPU |
| `tools/recomp` | `p2recomp`: analysis and C++ generation |
| `tools/icon` | Build-time conversion of an image to the program's Windows icon |
| `tools/texup` | `p2texup`: upscales dumped textures into a texture pack (FFmpeg xBR) |
| `runtime/cpu` | Guest CPU state, instruction semantics, services called by generated code |
| `runtime/mem` | The guest address space |
| `runtime/kernel` | Threads, scheduler, virtual clock, memory partitions, interrupts |
| `runtime/hle` | System-call implementations, one file per console library |
| `runtime/ge` | Display-list processor: vertex decoding, transform and lighting, textures |
| `runtime/gfx/vk` | Vulkan renderer and shaders |
| `runtime/gfx` | Texture dumping and replacement (texture packs) |
| `runtime/audio`, `runtime/media` | Audio device output; ATRAC and H.264 decoding |
| `runtime/platform` | Window, input, settings, frame pacing, dumps |
| `runtime/ui` | Launcher, and the overlay: settings menu, system dialogs, performance display |
| `data/psp_api` | Names of the console's system functions, grouped by library |
| `data/code_blobs.txt` | Files the game loads at fixed addresses and executes |
| `tests` | Unit tests and an end-to-end recompiler test |

## The recompiler

`p2recomp gen` turns the executable into C++ in four steps.

**Load.** The executable is read straight from the disc image, checked against the list of
supported builds by SHA-256, and relocated to the address the console would load it at. Fixing the
load address makes every code and data address a compile-time constant.

**Discover.** Function entry points come from direct calls and from the executable's relocation
table, which names every place where a code address is stored in data or formed by an instruction
pair. This finds jump tables, callbacks and virtual-method tables without guessing. Two additions
cover code that lives outside the executable's code sections:

- direct jumps from regular code into data sections are followed and the routines they reach are
  translated;
- files listed in `data/code_blobs.txt` are laid over the memory image at their load address
  first, so routines inside them are found the same way;
- such routines are often reached only through addresses that carry no relocation, so a stack-frame
  prologue that directly follows a return, or a return that directly precedes a known routine, is
  taken as the boundary of another routine.

**Analyse.** Each function is the code reachable from its entry without leaving the address range
up to the next function. Blocks are split at branch targets; `switch` statements are recognised
from their relocated tables and become C++ `switch` statements. A code address that is taken as a
pointer but lies inside a function becomes a *secondary entry* of that function rather than
splitting it.

**Emit.** One C++ function per guest function:

```cpp
void F_0880B248(Cpu* restrict c, u8* restrict m);
```

`c` is the guest CPU state and `m` the base of guest memory. Guest registers are fields of `c`;
within a function the compiler is free to keep them in host registers because `c` and `m` are
declared not to alias. Details:

- Delay slots are lowered explicitly: the branch condition is latched, the slot executes, then
  control transfers. "Likely" branches execute the slot only when taken.
- A direct call is a direct C++ call. An indirect call or jump indexes a table with one slot per
  instruction word of the executable, so dispatch is a bounds check and a load.
- A routine can end by jumping into the middle of its caller instead of returning (the patch code
  added to the executable does this). The rest of the caller then runs inside the call, including
  its return; the caller sees a foreign return address and a stack pointer above its own when the
  call comes back, and returns straight away too.
- Calls into system libraries are bound by name at generation time and become direct calls to the
  runtime's implementation. A missing implementation is a link error.
- Every block charges its instruction count to a per-thread counter; loop heads and function
  entries test it. That gives the scheduler regular, cheap preemption points and a deterministic
  clock.
- An instruction the emitter cannot translate fails generation. A jump at run time to an address
  that is not a known entry stops with a message naming the address; listing it in
  `data/extra_entries.txt` adds it on the next build.

The output is split into a fixed number of source files of similar size so they compile in
parallel.

## The runtime

**Memory.** The whole 4 GiB guest address space is reserved. RAM, video memory and the scratchpad
are mapped at every mirror the hardware has, so a guest access is a single indexed load or store
with no masking. Anything else faults and is reported with the guest address.

**Threads and time.** Guest threads are Windows fibers on one host thread, scheduled by the
console's priority rules. Guest time is a count of executed instructions, advanced to the next
scheduled event when every thread is waiting. The 59.94 Hz vertical blank is the one point where
guest time is tied to real time; fast-forward simply skips that wait.

**System calls.** Each of the 212 functions the game imports has a native implementation in
`runtime/hle`. A call that blocks switches fibers; guest callbacks and interrupt handlers run as
nested guest calls on the calling fiber.

**Graphics.** The display-list processor executes the command lists the game submits. Vertices are
decoded, skinned, transformed and lit on the CPU and reach the renderer as ready-to-rasterise
primitives with the pixel-pipeline state they need. The Vulkan renderer keeps one image per guest
render target at a multiple of the original resolution. Points and lines, which the game uses for
text-box backgrounds, borders and menu rules, are drawn as quads exactly one console pixel wide,
so they cover the same pixels at every resolution instead of thinning out. The renderer uses a
single fragment shader for the
fixed-function pixel pipeline, so no shader is ever compiled while playing; the driver's compiled
render states are kept on disk between runs. The frame the game selects for display is scaled into
the window.

Textures are cached by address, layout and palette, and a cached texture is trusted only as long
as its pixels cannot have changed. The game may rewrite a texture between any two draws, so the
pixels are compared again (by a vectorised hash) whenever the game has touched the texture state
*and* its code has run since the last comparison. Display lists run to completion when they are
submitted, so no game code runs between the draws of one submission and they share a comparison.

**Texture packs.** When a texture is decoded, its ID (a hash of the decoded pixels and size, the
same on every machine; `runtime/gfx/texture_id.h`) is looked up in the replacement folder, and a
file found there is uploaded instead, at its own resolution. Files are read and dumps written on a
background thread, so neither stalls the game. Textures are dumped only once their pixels have
stayed the same for several frames, and the movie player's picture buffers are excluded, which
keeps animated surfaces out of the dump.

**Skipping the intro.** The game lets the player skip each start-up screen with Start and shows
its main menu by itself once the opening movie is gone. With the setting on, the program presses
Start on the player's behalf and runs at full speed, with the window blank and the sound muted,
until the game shuts the opening movie down. It stops early if a system dialog or the settings menu
needs the player, and gives up after two minutes of game time. No game code or data is altered.

**Overlay and system dialogs.** The settings menu and the screens the console's system software
would show (save-slot lists, messages) are drawn with Dear ImGui into the final picture, after the
game's frame has been scaled into it. A dialog keeps the game's dialog call in its *running* state
until the player answers, exactly as the console's does, and takes the pad away from the game
meanwhile. Unattended runs without the overlay answer dialogs with a default instead.

**Launcher.** Started without a disc image, the program opens the window and the renderer as usual
and shows the launcher in it before anything of the game is loaded: a full-window screen drawn by
the same overlay, over a blank picture, with the settings tabs of the in-game menu. A chosen image
is opened and its executable's SHA-256 compared with the one the build was generated from, so a
wrong file is reported before Play is offered. Play hands the image to the normal start-up and the
same window carries on as the game's. The program is a Windows (not console) program; started from
a terminal it attaches to it for its messages, and otherwise reports errors in message boxes.

**Audio.** The game's own sound engine drives a 32-voice software synthesizer and streams
ATRAC3plus music; both are mixed at 44.1 kHz in guest time and handed to the sound device, with a
small rate adjustment to keep the device's clock and the game's in step. The synthesizer's
reverb is an algorithmic one (damped comb filters into all-pass filters, plus a feedback delay for
the echo types) with its own settings for each of the console's effect types.

**Movies.** The program stream is demultiplexed as the game feeds it; H.264 video and ATRAC3plus
audio are decoded with FFmpeg and returned one picture and one audio frame at a time, the way the
game asks for them.

**Storage.** `disc0:` reads come directly from the disc image. The memory stick is a folder in the
user directory; saves are stored there as plain files next to a `PARAM.SFO`.

## Testing

- `p2_tests`: unit tests for the libraries, the instruction semantics and the VFPU.
- `p2_recomp_tests`: a synthetic guest program is assembled, recompiled at build time and executed;
  every routine is checked against reference results computed in C++.
- `p2ep --headless --frames N --dump-frames <dir>` runs the game without a window and writes
  frames as PNG files. Because guest time is deterministic, such runs are reproducible: the same
  input script gives byte-identical frames, which is how a change to the renderer or the
  recompiler is checked for regressions.
- For tracking down a wrong picture or value, the program can log every draw of one frame, report
  which game routine writes a given memory range, and dump the game's memory
  (see [OPTIONS.md](OPTIONS.md)).
