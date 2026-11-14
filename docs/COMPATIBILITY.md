# Compatibility and status

What has been checked, how, and what is known to be missing. "Checked" means it was run and the
resulting frames, sound or files were inspected; nothing here is assumed.

Test configuration: Windows 11, AMD Ryzen 7 7800X3D, NVIDIA GeForce RTX 4080 SUPER.

## Verified

| Area | Result |
|---|---|
| Start-up | Autosave notice, legal notice, publisher and middleware logos |
| Opening movie | Video and music play; skipping with Start works |
| Title screen | Menu, New Game, Load Game |
| New game | Disclaimer, both story movies (with subtitles), the scenes that follow |
| 3D scenes | Rooms, characters, lighting and fades are drawn completely, and identically at 1x and 2x |
| Dialogue | Text boxes, portraits, name plates, text advance |
| Free movement | Walking, room exits, talking to characters, story-gated exits, a second party member joining |
| In-game menus | Command menu and System menu |
| Music | Streamed music through track changes and fade-outs (checked from recordings and from the game's behaviour, not by ear) |
| Saving | Slot list, new slot, overwrite with confirmation; the game's system data |
| Loading | Slot list from the title screen; the game resumes where it was saved |
| Settings | The menu opens over the paused game in the window; a settings file is read back and applied |
| Window | Paced to 59.94 Hz; the picture is drawn at twice the console's resolution in the default window |

| Beyond the first area | The city map, the school grounds, classrooms, corridors and the teachers' lounge, with conversations and dialogue choices (reached by the unattended runs below) |

Long unattended runs with randomised input completed without an error, a stall or an unknown code
address. The longest covered 1.6 million frames, about seven and a half hours of game time and
525 million draw calls, and wandered from the first area across the city into the school.

Not checked by a person at the keyboard: how the game feels to play, sound quality by ear,
gamepads, resizing the window and full screen, changing settings from the menu with the mouse, and
rebinding keys. These work in code and were exercised only as far as scripted runs allow.

## Not verified yet

These parts of the game have not been reached in testing. They use the same machinery as the parts
above, but until someone has played them they should be treated as untested:

- battles, including spell effects and Persona summoning;
- shops, the Velvet Room and other facilities;
- the story beyond the first visit to the school;
- the Gallery and Config entries of the title menu;
- the delete-data list (the game did not offer it in the parts played).

If the game stops with *"indirect jump to …, which is not a known entry point"*, it reached a
routine the recompiler did not find; [BUILDING.md](BUILDING.md) explains the one-line fix.

## Known limitations

| Area | Limitation |
|---|---|
| Stencil buffer | The stencil value is kept in the alpha channel. Replacing or zeroing it is exact; incrementing, decrementing and inverting are not carried out. |
| Render-target feedback | A draw that samples the target it is drawing to sees the target's previous contents. |
| Curved surfaces | Bézier and spline patches are not drawn (the game is not known to use them). |
| Sound synthesizer | The reverb the game applies to sound effects is an algorithmic approximation of each of the console's nine effect types, not a sample-exact copy; it can be switched off in the settings. Envelope curves are close approximations of the console's. |
| Saves | Saves are plain files. Saves made on a console (which encrypts them) cannot be imported. |
| Disc image | Only the one supported build of the game is accepted. |

## Performance

Measured unattended and unthrottled, on the test configuration, over the first 22,000 frames of a
new game (movies, 3D scenes and dialogue; about six minutes of game time):

| | Host time | Share |
|---|---|---|
| Translated game code and system calls | 4.0 s | 41% |
| Display-list processing and draw submission | 3.0 s | 31% |
| Presentation and waiting for the GPU | 2.8 s | 28% |
| **Total** | **9.8 s for 367 s of game time (about 37x real time)** | |

At normal speed the game therefore needs about 3% of one processor core. The translated game code
alone runs at roughly 90 times the console's speed.

The log of any unattended run ends with the same breakdown, so the effect of a change can be
measured (see [OPTIONS.md](OPTIONS.md)).
