# Options and controls

```
p2ep <disc-image> [options]
```

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
tabs are General, Video, Audio, Controls and Help.

| Setting | Meaning |
|---|---|
| Skip the intro at start-up | Off by default. When on, the game starts at the main menu: the start-up notices, the logos and the opening movie are skipped (the window stays black for about a second meanwhile, then the title fades in with its music). Takes effect the next time the game starts. Left idle on the main menu, the game still replays its intro as it does on the console. |
| Rendering resolution | The resolution the game is drawn at: a multiple of the console's 480x272, from 1x to 10x, or *Match the window* (the default), which picks the smallest multiple that fills the window. |
| Picture size | *Fit the window* keeps the picture's shape and makes it as large as fits; *Whole multiples only* additionally limits it to exact multiples of 480x272; *Stretch to fill* ignores the shape. |
| Smooth scaling | Filter the picture when it is scaled to the window. Off gives hard pixel edges. |
| Full screen | Borderless full screen on the current display. |
| Vertical sync | Present in step with the display. Off removes the display's latency at the cost of tearing. |
| Show performance | A small display of frame rate, speed, frame time and rendering resolution. |
| Use replacement textures | On by default. Draw with the files of a texture pack in `textures\replace` wherever there is one; *Reload* reads the folder again. See [TEXTURE_PACKS.md](TEXTURE_PACKS.md). |
| Dump textures | Off by default. Write each texture the game draws with to `textures\dump`, once, as PNG: the starting point for a texture pack. |
| Volume | Output volume. |
| Reverb on sound effects | The echo the game puts on its sound effects in some places. The program's reverb approximates the console's; switch it off for a dry sound. |
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
| `--user-dir <dir>` | Where saves, settings and caches are kept. Default: `userdata` next to where the program is started. |
| `--scale <n>` | Rendering resolution multiplier from 1 to 10 for this run, overriding the saved setting. |
| `--fullscreen` | Start in full screen. |
| `--skip-intro` | Start at the main menu this time, whatever the setting says. |
| `--unthrottled` | Do not pace the game to real time and do not wait for the display. |

The user folder holds:

| Path | Contents |
|---|---|
| `ms0\PSP\SAVEDATA\` | Saves, one folder per slot, as plain files with a description and picture. |
| `settings.ini` | Settings. |
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
