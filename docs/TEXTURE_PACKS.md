# Texture packs

The program can write out the textures the game draws with, and draw with replacements instead:
higher-resolution versions made with an upscaler or by hand. A set of replacements is a
*texture pack*.

Texture packs are made from the game's artwork. They are not part of this repository and must
never be added to it; keep them in your user folder, and share them only where you have the right
to.

## Folders

Your own files live in the user folder (`userdata` by default, see [OPTIONS.md](OPTIONS.md)):

| Folder | Contents |
|---|---|
| `textures\dump\` | Textures written by the program, one PNG per texture. |
| `textures\replace\` | Your texture pack: PNG files the program draws with instead. Subfolders are allowed, so several packs can sit side by side, and linked folders are followed. |
| `textures\replace\upscaled\xbr\`, `...\realesrgan\` | What the built-in upscaler made, one set per method (below). |

Next to the program:

| Folder | Contents |
|---|---|
| `texturepacks\` | Community texture packs (below), one per subfolder. |
| `realesrgan\` | The Real-ESRGAN upscaler, if installed. |

The settings menu (Esc, Video tab) has a button that opens the `textures` folder.

## Which file is used

With *Use replacement textures* on (the default), a texture is drawn with the first of these that
has a file for it:

1. a community pack, if *Use community texture packs* is on (the default);
2. a file of your own anywhere in `textures\replace\` outside `upscaled\`;
3. the upscaled set of the method chosen in *Upscaler*;
4. the game's own texture.

## Making a pack

1. **Dump.** Turn on *Dump textures* in the settings menu (Video tab), or start the program with
   `--dump-textures`, and play through the parts of the game you want to cover. Every texture the
   game draws with is written once, as `textures\dump\<id>.png`; textures already in the folder are
   not written again, so dumping can continue over several sessions. Movie pictures and other
   textures that change every frame are left out.
2. **Upscale.** Choose the *Upscaler* and press *Upscale dumped textures* in the same section. The
   built-in upscaler (below) makes 4x versions of the dumps in the method's folder in the
   background, and the game uses them as soon as it is done; press it again after dumping more. Or
   process the files with a tool of your choice: keep each file's name, and keep its shape (any
   resolution works, but a texture 64 wide and 256 high should become, say, 256 by 1024). The alpha
   channel (transparency) is used, so keep it too.
3. **Install.** Files from another tool go in `textures\replace\` (or a subfolder of it). Only the
   textures you want to change need a file; everything else keeps its original look.
4. **Play.** After adding or changing files while the game runs, press *Reload* next to *Use
   replacement textures*.

## The built-in upscaler

`p2texup.exe`, next to `p2ep.exe`, is what the *Upscale dumped textures* button runs; it can also be
started on its own (double-click, or from a terminal). It offers two methods, chosen with the
*Upscaler* setting (or `--method`). Each keeps its own set, so switching between them needs no new
upscaling.

**xBR** (the default) uses FFmpeg's xBR filter, made for pixel art: edges stay sharp and get smoothed
along their direction instead of turning into blocks or blur. It is fast and needs nothing else. It
suits the game's drawn sprites, menus and text best; large painted pictures come out a little
painterly.

**Real-ESRGAN** runs a neural network made for anime-style art on the graphics card (through Vulkan).
It gives cleaner lines and more detail on portraits, maps and other painted art. It is slower, takes
the graphics card while it works (the game may slow down meanwhile), and uses about 3.5 GB for every
texture of a full play-through. It is a separate program: `scripts\package.ps1` puts it in
`realesrgan\` next to the program. `realesrgan-x4plus-anime` is the default model;
`realesr-animevideov3` is faster.

For each dumped texture, either method:

1. gives fully transparent pixels the colour of what is visible around them, so that edges do not
   pick up the junk colour hidden there (dark fringes around sprites);
2. scales the colour: with xBR and light contrast-adaptive sharpening, or with Real-ESRGAN (then
   resized with Lanczos filtering when the model's scale is larger than the one asked for);
3. scales the transparency separately with xBR, so outlines stay clean;
4. writes the result to `textures\replace\upscaled\<method>\<id>.png`.

Textures done in an earlier run are skipped, and so is any texture that has a replacement of your
own elsewhere in `textures\replace\`: a hand-made file always wins over the upscaler's. Each set's
`upscaler.ini` records the settings that made it; when they change, the whole set is done again.
Files directly in `upscaled\` (from before there were two methods) are moved into `xbr\`.

Some graphics cards and drivers make this build of Real-ESRGAN give black pictures for a few
textures. Those are tried again on the computer's other graphics processors, if it has any, and
whatever is still black is done with xBR instead, so the set is complete.

| Option | Meaning |
|---|---|
| `--user-dir <dir>` | The game's user folder. Default: `userdata` next to the program. |
| `--method <m>` | `xbr` (default) or `realesrgan`. |
| `--scale <n>` | 2, 3 or 4 times the original size. Default 4. |
| `--sharpen <s>` | xBR: sharpening from 0 (off) to 1. Default 0.25. |
| `--model <name>` | Real-ESRGAN: the model. Default `realesrgan-x4plus-anime`. |
| `--realesrgan <exe>` | Real-ESRGAN: the program. Default `realesrgan\realesrgan-ncnn-vulkan.exe` next to `p2texup.exe`. |
| `--force` | Redo textures that are done already. |
| `--jobs <n>` | Textures worked on at once. Default: one fewer than the processor's threads. |

## Community texture packs

Players have made texture packs for the PSP game in the format of the PSP emulator PPSSPP: hand
redrawn and AI-upscaled portraits, menus, fonts, maps and title screens. The program reads such packs
as they are. Each goes in its own subfolder of `texturepacks\` next to the program, with its
`textures.ini` and pictures unchanged, and is drawn before anything else (see *Which file is used*).
`scripts\package.ps1` downloads two of them from their authors' pages:

- **Eternal Punishment HD Textures 1.03**, by Rarno (<https://gamebanana.com/mods/355619>,
  CC BY-NC-ND 4.0). Its separate "red eyed shadows" add-on is not used.
- **UI Upscale**, by Ryuubu (shared by the author on MEGA, no licence stated).

When both have the same texture, the larger picture wins; at the same size, Ryuubu's.

A `pack.ini` next to each pack's folder gives the name, author and licence the settings show (and,
for people reading it, where the pack comes from), its place in that tie-break (`order`, lower
wins), and textures to leave out (`exclude`, a comma list of the pack's 24-digit names). Both packs' thin font sheet is left out this way: it lacks the
comma and the accented letters that the English patch adds to that font.

How a texture is matched: PPSSPP names a texture by its address and size, a hash of its palette, and
a hash of its data in memory, as set in the pack's `textures.ini` (`hash`, `ignoreAddress`,
`reduceHash`, `[hashranges]`, `[reducehashranges]`). The program computes the same names. Packs
that use the `quick` hash are not supported.

See [third_party/CREDITS.md](../third_party/CREDITS.md). The packs belong to their authors; they are
not part of this repository or of anything made from it.

## How textures are matched

A texture's name is a 16-digit hexadecimal hash of its decoded pixels and its size. It does not
depend on where the game keeps the texture in memory, on the machine, or on the order textures
are met, so a pack made on one computer works on any other. A texture whose pixels change (a
palette swap, an animation frame) is a different texture with a different name.

The hash is fixed: future versions of the program will keep producing the same names, so packs
stay valid. (Community packs use their own names, as described above.)

## Notes

- Replacements are loaded in the background. The first time one is needed the original shows for
  a moment, then the replacement takes over. Loaded replacements are kept in memory up to a budget
  and loaded again when needed.
- Replaced textures are always drawn with smooth filtering, since they are meant to be shown at
  higher rendering resolutions.
- Replacements may be up to 8192 by 8192 pixels.
- Textures the game draws into itself at run time (render targets, such as screen effects) cannot
  be replaced.
