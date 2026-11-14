# Options and controls

```
p2ep [disc-image] [options]
```

## Launcher

Started without a disc image (for example by double-clicking `p2ep.exe`), the program opens the
launcher in its window:

- **Disc image.** *Browse...* opens a file dialog for the game's `.chd` or `.iso` file; a file
  dropped onto the window works too. The image is checked at once: *Ready to play*, or why it
  cannot be played (not a disc image, not a PSP game, or a different game or version from the
  one the build was made from). The last image that passed is remembered.
- **Settings.** The same tabs as the in-game menu (see below); changes apply and are saved at once.
- **Play** starts the game in the same window (Enter or the gamepad's confirm button presses it
  when it has the focus); **Quit** closes the program.

With *Show the launcher at start* switched off (General tab), the game starts straight away with
the remembered image. Holding Shift while starting the program shows the launcher anyway, and so
does an image that has gone missing or no longer passes the check. Giving an image on the command
line always skips the launcher.

The program is a Windows program, so no console window opens with it. Started from a terminal, it
writes its messages there; otherwise errors are shown in a message box.

## Controls

| Console button | Keyboard (default) | Gamepad |
|---|---|---|
| D-pad | Arrow keys | D-pad |
| Cross | Z | South button (A on Xbox pads) |
| Circle | X | East button (B) |
| Square | A | West button (X) |
| Triangle | S | North button (Y) |
| L / R | Q / W | Shoulder buttons |
| Start | Enter | Start |
| Select | Backspace | Back / View |
| Analog stick | — | Left stick |

| Function | Keyboard | Gamepad |
|---|---|---|
| Settings menu (pauses the game) | Esc or F1 | Guide button |
| Fast-forward (hold) | Tab | Right trigger |
| Full screen | F11 or Alt+Enter | — |

The keyboard keys can be changed in the settings menu. The first gamepad that is connected is
used; it can be plugged in while the game runs. In this build of the game Cross confirms and
Circle cancels.

## Settings menu

Esc opens the menu over the paused game. It works with the mouse, the keyboard or a gamepad. Its
tabs are General, Video, Audio, Controls and Help; the launcher shows the same ones.

| Setting | Meaning |
|---|---|
| Show the launcher at start | On by default. Off: starting the program without a disc image plays the image last chosen in the launcher straight away (hold Shift while starting to see the launcher anyway). |
| Skip the intro at start-up | Off by default. When on, the game starts at the main menu: the start-up notices, the logos and the opening movie are skipped (the window stays black for about a second meanwhile, then the title fades in with its music). Takes effect the next time the game starts. Left idle on the main menu, the game still replays its intro as it does on the console. |
| Rendering resolution | The resolution the game is drawn at: a multiple of the console's 480x272, from 1x to 10x, or *Match the window* (the default), which picks the smallest multiple that fills the window. |
| Picture size | *Fit the window* keeps the picture's shape and makes it as large as fits; *Whole multiples only* additionally limits it to exact multiples of 480x272; *Stretch to fill* ignores the shape. |
| Smooth scaling | Filter the picture when it is scaled to the window. Off gives hard pixel edges. |
| Window size | The size of the window's picture area in pixels when the game is not in full screen: a multiple of the console's 480x272 (1x, 2x, 3x...) or a common size from 1280x720 to 3840x2160. Only sizes that fit the current display are listed. The window is resized and centred at once; chosen in full screen, the size is used on leaving it. Resizing the window by dragging its border also sets this (shown as *custom*), and the size is remembered for the next start. A saved size larger than the display is shrunk to fit, keeping its shape. Default 960x544 (2x). |
| Full screen | Borderless full screen on the current display. |
| Vertical sync | Present in step with the display. Off removes the display's latency at the cost of tearing. |
| Show performance | A small display of frame rate, speed, frame time and rendering resolution. |
| Use replacement textures | On by default. Draw with the files of a texture pack in `textures\replace` wherever there is one; *Reload* reads the folder again. See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| Dump textures | Off by default. Write each texture the game draws with to `textures\dump`, once, as PNG: the starting point for a texture pack. |
| Upscale dumped textures | Runs the built-in upscaler (`p2texup.exe`) in the background: 4x versions of the dumps go to `textures\replace\upscaled`, and the game uses them when it is done. Textures done before, and textures with a replacement of your own, are skipped. See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| Volume | Output volume. |
| Reverb on sound effects | Off by default. When on, the echo the game puts on its sound effects in some places is added, with the program's approximation of the console's reverb. |
| Fast-forward speed | How fast the game runs while fast-forward is held: unlimited, or 2x to 8x. |
| Keys | The keyboard key for each console button. |

Settings are saved to `settings.ini` in the user folder as soon as they change.

## System dialogs

Where the console would show its own screens, the program shows its own:

- **Save, load and delete lists** show each slot with the save's picture, description and time.
  Move with Up and Down, choose with Cross, go back with Circle, or use the mouse. Saving over an
  existing slot and deleting ask for confirmation.
- **Messages** are acknowledged with Cross or Circle; questions are answered with Left/Right and
  Cross.

## General options

| Option | Meaning |
|---|---|
| `--user-dir <dir>` | Where saves, settings and caches are kept. Default: the `userdata` folder next to `p2ep.exe`, however the program is started. |
| `--scale <n>` | Rendering resolution multiplier from 1 to 10 for this run, overriding the saved setting. |
| `--fullscreen` | Start in full screen. |
| `--window-size <W>x<H>` | Window size in pixels for this run, for example `1920x1088`, overriding the saved setting without changing it. At least 320x180. |
| `--skip-intro` | Start at the main menu this time, whatever the setting says. |
| `--unthrottled` | Do not pace the game to real time and do not wait for the display. |

The user folder holds:

| Path | Contents |
|---|---|
| `ms0\PSP\SAVEDATA\` | Saves, one folder per slot, as plain files with a description and picture. |
| `settings.ini` | Settings, and the disc image last chosen in the launcher. |
| `textures\dump\`, `textures\replace\` | Dumped textures and your texture pack (see [TEXTURE_PACKS.md](TEXTURE_PACKS.md)). |
| `cache\pipelines.bin` | The graphics driver's compiled render states, so later runs start without hitches. Safe to delete. |

## Options for testing and debugging

| Option | Meaning |
|---|---|
| `--headless` | Run without a window or audio device. System dialogs then answer themselves: loads take the first slot with data, saves the first slot, questions their default. |
| `--overlay` | With `--headless`: draw the overlay into a 960x544 picture and let scripted input answer the dialogs, as a player would. Frame dumps then show that picture. |
| `--show-menu <frame>[:<tab>]` | Open the settings menu at a frame (for screenshots). |
| `--frames <n>` | Quit after `n` displayed frames. |
| `--dump-frames <dir>` | Write displayed frames to `dir` as PNG files. |
| `--dump-every <n>` | With `--dump-frames`: write every `n`th frame (default 60). |
| `--dump-from <frame>` | With `--dump-frames`: start writing at this frame. |
| `--dump-textures` | Dump textures this time, whatever the setting says (see [TEXTURE_PACKS.md](TEXTURE_PACKS.md)). |
| `--dump-audio <file>` | Record the game's audio output to a WAV file. |
| `--input <script>` | Scripted controller input: `frame:buttons` pairs separated by commas, buttons as the console's hexadecimal bit mask. From each listed frame on, the pad holds that state. `--input @file` reads the script from a file. |
| `--no-gpu` | Do not create a renderer; only useful for testing system calls. |
| `--log <channels>` | Enable trace channels, comma-separated: `hle` (every system call), `io`, `thread`, `mem`, `display`, `ge`, `audio`, `media`, or `all`. |
| `--log-from <frame>` | Switch the `--log` channels on only once this many frames have been shown. |
| `--log-file <path>` | Write the log to a file. Trace output then goes only to the file. |
| `--trace-frame <frame>` | Log every draw of one frame with the state that shapes it. |
| `--watch <addr>:<bytes>@<frame>` | From a frame on, log every write to a range of game memory (hexadecimal address) with the thread that made it; the `dev` build adds the game's call chain. |
| `--dump-ram <file>@<frame>` | Write the game's main memory to a file at a frame. |

Button bits for `--input`: Select `0001`, Start `0008`, Up `0010`, Right `0020`, Down `0040`,
Left `0080`, L `0100`, R `0200`, Triangle `1000`, Circle `2000`, Cross `4000`, Square `8000`.
For example `--input 600:4000,605:0` presses Cross for five frames starting at frame 600.

A headless run is deterministic: the same build, image, user folder and input script produce the
same frames. When it ends, the log gives the time spent in the game's code, in graphics processing
and in presentation.
