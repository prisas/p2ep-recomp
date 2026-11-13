# Options and controls

```
p2ep <disc-image> [options]
```

## Controls

| Console button | Keyboard | Gamepad |
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
| Fast-forward (hold) | Tab | Right trigger |
| Toggle fullscreen | F11 or Alt+Enter | — |

The first gamepad that is connected is used; it can be plugged in while the game runs.

## General options

| Option | Meaning |
|---|---|
| `--user-dir <dir>` | Where saves and settings are kept. Default: `userdata` next to where the program is started. |
| `--scale <n>` | Internal resolution multiplier from 1 to 10. 1 is the console's 480x272; 4 is 1920x1088; 8 is 3840x2176. |
| `--unthrottled` | Do not pace the game to real time and do not wait for the display. |

Saves are stored under `<user dir>\ms0\PSP\SAVEDATA`, one folder per save, as plain files.

## Options for testing and debugging

| Option | Meaning |
|---|---|
| `--headless` | Run without a window or audio device. |
| `--frames <n>` | Quit after `n` displayed frames. |
| `--dump-frames <dir>` | Write displayed frames to `dir` as PNG files. |
| `--dump-every <n>` | With `--dump-frames`: write every `n`th frame (default 60). |
| `--dump-textures <dir>` | Write every texture the game uses as a PNG file (colour on the left, alpha on the right). |
| `--dump-audio <file>` | Record the game's audio output to a WAV file. |
| `--input <script>` | Scripted controller input: `frame:buttons` pairs separated by commas, buttons as the console's hexadecimal bit mask. From each listed frame on, the pad holds that state. |
| `--no-gpu` | Do not create a renderer; only useful for testing system calls. |
| `--log <channels>` | Enable trace channels, comma-separated: `hle` (every system call), `io`, `thread`, `mem`, `display`, `ge`, `audio`, `media`, or `all`. |
| `--log-file <path>` | Write the log to a file. Trace output then goes only to the file. |
| `--verbose` | Include debug-level messages. |

Button bits for `--input`: Select `0001`, Start `0008`, Up `0010`, Right `0020`, Down `0040`,
Left `0080`, L `0100`, R `0200`, Triangle `1000`, Circle `2000`, Cross `4000`, Square `8000`.
For example `--input 600:2000,605:0` presses Circle for five frames starting at frame 600.

A headless run is deterministic: the same build, image and input script produce the same frames.
