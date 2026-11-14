# Texture packs

The program can write out the textures the game draws with, and draw with replacements instead:
higher-resolution versions made with an upscaler or by hand. A set of replacements is a
*texture pack*.

Texture packs are made from the game's artwork. They are not part of this repository and must
never be added to it; keep them in your user folder, and share them only where you have the right
to.

## Folders

Everything lives in the user folder (`userdata` by default, see [OPTIONS.md](OPTIONS.md)):

| Folder | Contents |
|---|---|
| `textures\dump\` | Textures written by the program, one PNG per texture. |
| `textures\replace\` | Your texture pack: PNG files the program draws with instead. Subfolders are allowed, so several packs can sit side by side. |

The settings menu (Esc, Video tab) has a button that opens the `textures` folder.

## Making a pack

1. **Dump.** Turn on *Dump textures* in the settings menu (Video tab), or start the program with
   `--dump-textures`, and play through the parts of the game you want to cover. Every texture the
   game draws with is written once, as `textures\dump\<id>.png`; textures already in the folder are
   not written again, so dumping can continue over several sessions. Movie pictures and other
   textures that change every frame are left out.
2. **Upscale.** Press *Upscale dumped textures* in the same section. The built-in upscaler
   (below) makes 4x versions of the dumps in `textures\replace\upscaled\` in the background and
   the game uses them as soon as it is done; press it again after dumping more. Or process the
   files with a tool of your choice: keep each file's name, and keep its shape (any resolution
   works, but a texture 64 wide and 256 high should become, say, 256 by 1024). The alpha channel
   (transparency) is used, so keep it too.
3. **Install.** Files from another tool go in `textures\replace\` (or a subfolder of it). Only the
   textures you want to change need a file; everything else keeps its original look.
4. **Play.** *Use replacement textures* is on by default. After adding or changing files while the
   game runs, press *Reload* next to it.

## The built-in upscaler

`p2texup.exe`, next to `p2ep.exe`, is what the *Upscale dumped textures* button runs; it can also
be started on its own (double-click, or from a terminal). For each dumped texture it:

1. gives fully transparent pixels the colour of what is visible around them, so that edges do not
   pick up the junk colour hidden there (dark fringes around sprites);
2. scales the colour with FFmpeg's xBR filter, made for pixel art: edges stay sharp and get
   smoothed along their direction instead of turning into blocks or blur;
3. sharpens it lightly (contrast-adaptive sharpening);
4. scales the transparency the same way, separately, so outlines stay clean;
5. writes the result to `textures\replace\upscaled\<id>.png`.

Textures done in an earlier run are skipped, and so is any texture that has a replacement of your
own elsewhere in `textures\replace\`: a hand-made file always wins over the upscaler's.

| Option | Meaning |
|---|---|
| `--user-dir <dir>` | The game's user folder. Default: `userdata` next to the program. |
| `--scale <n>` | 2, 3 or 4 times the original size. Default 4. |
| `--sharpen <s>` | Sharpening from 0 (off) to 1. Default 0.25. |
| `--force` | Redo textures that are done already (for example with another scale). |
| `--jobs <n>` | Textures worked on at once. Default: one fewer than the processor's threads. |

xBR suits the game's drawn sprites, menus and text best; large painted pictures come out a little
painterly. Any texture can be replaced by a file of your own made another way.

## How textures are matched

A texture's name is a 16-digit hexadecimal hash of its decoded pixels and its size. It does not
depend on where the game keeps the texture in memory, on the machine, or on the order textures
are met, so a pack made on one computer works on any other. A texture whose pixels change (a
palette swap, an animation frame) is a different texture with a different name.

The hash is fixed: future versions of the program will keep producing the same names, so packs
stay valid.

## Notes

- Replacements are loaded in the background. The first time one is needed the original shows for
  a moment, then the replacement takes over. Loaded replacements are kept in memory up to a budget
  and loaded again when needed.
- Replaced textures are always drawn with smooth filtering, since they are meant to be shown at
  higher rendering resolutions.
- Replacements may be up to 8192 by 8192 pixels.
- Textures the game draws into itself at run time (render targets, such as screen effects) cannot
  be replaced.
