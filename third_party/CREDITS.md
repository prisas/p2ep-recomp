# Third-party extras in the package

`scripts\package.ps1` downloads these from their authors' pages and puts them next to the program. None of them is
part of this repository.

## Real-ESRGAN

The second texture upscaling method. The upscaler's own build of realesrgan-ncnn-vulkan (2022-04-24, from the
Real-ESRGAN v0.2.5.0 release) goes into `realesrgan\` with its models, unchanged.

- Real-ESRGAN, by Xintao Wang: BSD 3-Clause.
- Real-ESRGAN-ncnn-vulkan, by Xintao Wang, based on realsr-ncnn-vulkan by nihui: MIT.
- ncnn, by Tencent: BSD 3-Clause.

`vcomp140.dll`, Microsoft's OpenMP runtime, is the redistributable library the program needs. The licence texts are
in `third_party\realesrgan\` and are copied into the package's `licences\` folder.

## Community texture packs

Texture packs made by players for the PSP game, used unchanged. They go into `texturepacks\` and are drawn before
anything else whenever replacement textures are on. They are made from the game's artwork and belong to their
authors. This program does not redistribute them; the package script fetches them for the person who builds it.

- **Eternal Punishment HD Textures 1.03**, by Rarno. <https://gamebanana.com/mods/355619>
  - Licence: CC BY-NC-ND 4.0.
  - The separate "red eyed shadows" add-on is not used.
- **UI Upscale**, by Ryuubu. Shared by the author on MEGA; no licence stated.

Both packs' thin font sheet is left out (`exclude` in each pack's `pack.ini`): Rarno's Font7, Ryuubu's ThinText. It
lacks the comma and the accented letters the English patch adds, so text drawn with it would lose them.

The packs hash only the first half of each texture (`reduceHash`), so the patch's change to the font does not change
the font's name, and the packs' version would otherwise be used.
