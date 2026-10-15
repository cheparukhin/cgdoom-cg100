# CGDoom user guide

The [README](../README.md) covers installing CGDoom from the release zip. This guide is the full
reference: compatibility, which `mpm.bin`, WAD files, the launcher, all keymaps, saves, diagnostics and
troubleshooting.

- [Compatibility](#compatibility)
- [Which mpm.bin](#which-mpmbin)
- [WAD files](#wad-files)
- [The launcher](#the-launcher)
- [Controls](#controls)
- [Performance tips](#performance-tips)
- [CGDoom HD](#cgdoom-hd)
- [Saves and files](#saves-and-files)
- [Diagnostics and reporting a problem](#diagnostics-and-reporting-a-problem)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)

## Compatibility

### Check your calculator

CGDoom runs on the fx-CG100 / Graph Math+ with MPM. Install MPM with
[casio-mpm-220](https://github.com/cheparukhin/casio-mpm-220): it updates a calculator on an OS older than 2.20 to OS
02.20 with MPM, and currently needs a Mac.

Inside CGDoom, the Diagnostics show the OS as `02.20.0000`. That is the OS's internal string, so it
differs from HOME → System → Version (02.20.0202), which is normal. On an unsupported OS or another
model, CGDoom shows "Unsupported calculator" and quits without touching anything.

### fx-CG50 / Graph 90+E

Not supported. The `.g3a` starts from the fx-CG50's main MENU, shows "Unsupported calculator" and quits.
On the fx-CG50, use [Lephenixnoir's CGDoom](https://git.planet-casio.com/Lephenixnoir/CGDoom).

## Which mpm.bin

MPM has two parts: an **OS patch**, and **`mpm.bin`**, the add-in menu at the root of the storage memory.
Use the `mpm.bin` from [casio-mpm-220's release](https://github.com/cheparukhin/casio-mpm-220/releases/latest). It is built from MPM 1.1,
which hands add-ins about 5.5 MiB of RAM in one block (a figure from reading the loader's source, not a
measurement), enough to copy a packed `doom1.wad` to RAM with about 300 KiB to spare by calculation.

- Other `mpm.bin` builds don't work: casio-mpm-220's OS patch refuses them.
- Diagnostics → Memory shows how much RAM is free.

## WAD files

CGDoom needs a DOOM data file (a WAD). The release zip includes the shareware `DOOM1.WAD`, already
packed. To make it yourself, `tools/wadtool.py` (Python 3, no dependencies) downloads the shareware
`doom1.wad` v1.9 and packs it in one step:

```sh
python3 tools/wadtool.py fetch-wad DOOM1.WAD
```

It takes an unmodified copy from Debian's `doom-wad-shareware` package, checks its SHA-256, and writes the
packed `DOOM1.WAD`. Add `--save-original doom1.wad` to keep the original too. If you already have
`doom1.wad` (version 1.9, 4,196,020 bytes), pack it directly:

```sh
python3 tools/wadtool.py pack doom1.wad DOOM1.WAD --drop-ends
```

**What packing does.** CGDoom has no sound, so the tool removes the sound effects, music and the ENDOOM
text screen, and aligns every lump to 4 bytes. Everything else is unchanged. For the 1.9 shareware WAD
this gives 3,389,236 bytes, saving 19%. The tool prints whether the result fits its storage budget of
4,050,000 bytes.

- The WAD's own demos are kept: the title screen plays them. Add `--drop-demos` to remove them too, which
  saves about 44 KB more; the title screen then shows only its pictures.
- `python3 tools/wadtool.py info <wad>` shows what a WAD contains.
- `python3 tools/wadtool.py verify doom1.wad DOOM1.WAD` checks that every lump that was kept is unchanged.

**What fits.** The fx-CG100 has only about **4.5 MB** of storage memory, and the add-in and `mpm.bin` also
need room. That is why only shareware-sized IWADs fit:

- **Fits:** shareware `doom1.wad`, once packed.
- **Does not fit:** Ultimate Doom, Doom II, Final Doom, and Freedoom (about 28 MB per phase).
- CGDoom's split Ultimate Doom WADs (`doomu1.wad`–`doomu4.wad`) are recognized by the launcher. As
  distributed they are 5.2–6.6 MB each, and nobody has checked whether a packed one fits.

## The launcher

The launcher lists every `*.wad` file in the storage root (up to 16), then *Controls...*, *Diagnostics...*
and *Quit*.

**Navigation:** **Up/Down** select, **OK** or **EXE** select, **BACK** or **HOME** returns. On the main
screen, **BACK** or **HOME** quits to MPM.

| Item | What it does |
|---|---|
| *Play Doom (shareware)* (one row per WAD) | Starts the game with that WAD. The row text depends on the file name (`doom1.wad`, `doomu.wad`, `doom2.wad` and others); unknown names read "Play other WAD". |
| Controls... | Keymap presets and rebinding (see [Controls](#controls)). |
| Diagnostics... | Five pages of hardware information (see [Diagnostics](#diagnostics-and-reporting-a-problem)). |
| Quit | Returns to MPM. |

The game starts like DOOM on a PC: the title screen plays the WAD's demos, and any key opens the Doom
menu. Choose *New Game* there for the episode and skill, or *Load Game*.

Everything else is fixed:

- the CPU runs at 236 MHz (gint's F4 preset; the memory bus stays at its normal speed);
- the frame goes to the screen by DMA while the next one is drawn;
- automatic frameskip: the game always runs at 35 tics per second, and when a frame takes too long, up to
  3 frames in a row are not drawn;
- the WAD (without its sounds) is copied to RAM when there is enough memory left for the game; by
  calculation, the packed shareware WAD fits. Otherwise it is read in place from flash, which is slower.

*View size* and *Detail* are set in the game (see [Performance tips](#performance-tips)). CGDoom saves them,
with the keymap, in `CGDOOM.CFG` when it exits.

The game engine cannot be restarted. **When a game ends, CGDoom returns to MPM**, not to the launcher.
Launch it again to play again.

## Controls

The Controls screen (**Controls...**) has these actions:

- *Load preset*: pick a preset with Left/Right, then press OK/EXE to apply it.
- Rebind an action: select it, press OK/EXE, then press the new key. BACK or HOME cancels.
- **DEL** unbinds an action.
- *Discard changes* restores the keymap you had when you opened the screen.

Each key does one action. If you bind a key that was already in use, its old action is unbound and the
screen says so. **AC/ON** cannot be bound, and neither can the Doom menu key.

### Presets

The key names are the ones the Controls screen shows. On the fx-CG100, *X,O,t* is the X,θ,t key.

| Action | **Thumbs (CG100)** (default) | **Strafe arrows (CG100)** |
|---|---|---|
| Look left / Look right (turn) | Left / Right | PrevTab / NextTab |
| Move forward / Move backward | Up / Down | Up / Down |
| Strafe left / Strafe right | PrevTab / NextTab | Left / Right |
| Shoot | SHIFT | SHIFT |
| Interact (open doors, switches) | ALPHA | ALPHA |
| Toggle run | VARIABLE | VARIABLE |
| Weapons 1–7 (fist, pistol, shotgun, chaingun, rockets, plasma, BFG) | 1–7 | 1–7 |
| Map | CATALOG | CATALOG |
| Pause | PageUp | PageUp |
| Smaller view / Larger view | - / + | - / + |
| Cheat | , | , |
| Noclip | X,O,t | X,O,t |
| Brightness | Frac | Frac |
| FPS counter | PageDown | PageDown |
| Quicksave / Quickload (CGDoom HD only) | TOOLS / SETTINGS | TOOLS / SETTINGS |

**In the Doom menu:**

- **BACK** opens and closes the Doom menu. This key is fixed.
- **EXE** selects. **OK** also selects, as long as no action is bound to it; the presets leave it free.
- The move keys navigate. With *Strafe arrows*, Left/Right on sliders are the turn keys PrevTab/NextTab.
- Prompts say "press exe to confirm". EXE or the Interact key answers yes, and BACK answers no.

### Special keys

- **Cheat** turns god mode on or off, and gives full armor, all weapons, full ammo and all keys.
  **Noclip** lets you walk through walls.
- **Brightness** cycles through the five gamma levels.
- **FPS counter** shows or hides the frame rate.

### Typing a save name

Empty save slots are already filled with the map name (for example `E1M3`), so you can press **EXE** to
save without typing. If you want to type a name:

- **Letters:** press the key with that letter's alpha legend, without ALPHA. For example A is X,θ,t, B is
  Frac, M is 7 and U is 1. These are the same physical keys as on the fx-CG50.
  - On the fx-CG100, D, E and G–L follow the fx-CG50 layout. Casio's manual does not list them, so they
    are not confirmed.
- **Digits:** hold **ALPHA** and press the digit.
- **Space:** the **.** key.
- **DEL** erases a character.

### Leaving

- To quit normally, open the Doom menu and choose *Quit Game*, then press EXE.
- **Emergency quit: SHIFT + 7 + 3 + AC/ON.**
  - This works anywhere, including while a level loads. CGDoom leaves through its normal exit, so your
    saves and the settings are kept.
  - If CGDoom is frozen and does not react within 2 seconds while you hold the keys, it aborts without
    writing files.
- **Idle exit.** The add-in runs the calculator itself, so the OS cannot turn the calculator off. CGDoom
  therefore returns to MPM after a period with no key pressed, and MPM's own auto power-off then works.
  - The launcher and the title demos wait 10 minutes; a game you are playing, even paused, waits
    30 minutes.
  - A 60-second warning comes first. Press any key to stay.

## Performance tips

On the fx-CG100, the game plays normally with full detail and the status bar. The frame rate has not been
measured yet: press **PageDown** in the game to show it, or run the timedemo (Diagnostics → Benchmarks).

If it is too slow somewhere, two settings in the game make it faster:

- **Detail:** open the Doom menu, then *Options* → *Graphic Detail* (Low draws at half the horizontal
  resolution).
- **View size:** **-** makes the view smaller (with a black border), **+** makes it larger. The largest size
  hides the status bar.

In an SH-4 simulator, which counts instructions, they save this much work per frame:

| Setting | Work saved per frame |
|---|---|
| Detail: Low | 34–39% |
| View size 9 | 13–16% |
| View size 7 | 32–38% |

CGDoom keeps both settings for the next launch.

## CGDoom HD

`CGDoomHD.g3a` is a second build of the game, **CGDoom HD** in the TOOLS menu, with the improvements
below. **It has not run on a calculator yet.** Try it next to `CGDoom.g3a`: copy both to the storage
root. With the packed shareware WAD they take about 4.1 MB, which may not leave room for anything else
(see [WAD files](#wad-files)); keep only one of them if storage is short.

**What is different:**

- **The whole screen.** The game draws the calculator's full 396×224 pixels instead of 320×200 with black
  borders. The view is wider, so you see more to the sides. The status bar, menus and pictures keep
  their size, centred.
- **True colour.** Light and shadow are computed in full colour. Dark areas no longer band and change
  colour (dark reds turning brown), as they do with DOOM's 256-colour palette.
- **DOOM's proportions.** DOOM was drawn for 4:3 monitors, where its pixels were taller than wide. The
  view is stretched by 6/5 vertically, so rooms, monsters and weapons have the shapes they were drawn
  with.
- **Smoother lighting.** The light fades with distance in finer steps.
- **See-through effects.** Fireballs, plasma, BFG blasts, bullet puffs, smoke and teleport fog are
  translucent: an explosion in your face no longer hides the monster behind it. Fire, plasma and the gun
  flash glow.
- **Brightmaps.** Switch lights, exit signs, barrels and armour bonuses glow in dark rooms. (Most of the
  shareware WAD's brightmapped walls are in well-lit rooms, so the effect is rare there.)
- **Coloured numbers** on the status bar: health is red below 25%, gold below 50%, green up to 100% and
  blue above; armour is green or blue by armour type; ammo is red when low and gold below half; all
  three are gray in god mode.
- **Crosshair:** a small yellow cross at the centre of the view.
- **Full-screen HUD.** At the largest view size, without the status bar, ammo and health are shown at the
  bottom left of the view, armour and keys at the bottom right, with a shadow that keeps them readable on
  any wall.
- **Automap.** The Map key opens the map as before. A second press puts the map over the view, turned so
  that forward is up, while you keep playing; a third press closes it. The map shows the kills, items
  and secrets found, and the time spent in the level.
- **Quicksave and quickload:** **TOOLS** saves to your quicksave slot and **SETTINGS** loads it, in both
  presets (rebind them in *Controls...*). The first quicksave asks for a slot; after that, both ask
  before they save or load (EXE confirms). Loading a game from the Doom menu also makes its slot the
  quicksave slot.

**Shared with CGDoom.** Both add-ins read and write the same `CGDOOM.CFG` and the same saves, so a game
saved in one loads in the other. CGDoom does not know the two quick keys: when it writes `CGDOOM.CFG`,
CGDoom HD goes back to their default keys.

**Speed.** CGDoom HD draws 39% more pixels, at two bytes each instead of one, so it is slower than
CGDoom. Compare the two with the timedemo (Diagnostics → Benchmarks, key **1**). *Detail* and *View size*
make it faster as they do in CGDoom. It needs about 80 KB more RAM, and by calculation the packed
shareware WAD still fits in RAM.

## Saves and files

CGDoom only creates files with short 8.3 names, which the calculator requires, in the storage root.
The `<P>` prefix comes from the WAD's name. It is `DOOM1` for `doom1.wad` / `DOOM1.WAD`. Long names are
shortened with a hash, for example `FRE0P` for `freedoom1.wad`.

| File | What it is |
|---|---|
| `CGDOOM.CFG` | Settings and keymap (text). |
| `<P>_0.DSG` … `<P>_5.DSG` | The six save slots of the Doom menu, for example `DOOM1_0.DSG`. |

- **Files are written when you leave.** Writing to storage moves data in flash, and that would break
  CGDoom's direct view of the WAD in flash. So saves are kept in RAM until you quit.
  - When you save, the game says "Save file will be created on exit".
  - Loading works in the same session.
  - When you quit, CGDoom shows "Writing N files... Do not turn off the calculator".
- If a file cannot be written, for example because storage is full, the exit screen says **NOT SAVED** and
  names the file. Free some storage space before you play again.
  - If the file was replacing an older one, for example when you save over a slot, CGDoom normally puts
    the older one back. The screen then says that the previous version was kept.
- To delete files, use the Memory app or USB.

## Diagnostics and reporting a problem

**Diagnostics...** in the launcher has five pages. Change pages with Left/Right.

1. **System:**
   - model and OS string;
   - gint version, LCD controller id, and the stack pointer at entry;
   - MPM load info, the physical addresses of the code and static RAM, and the image size;
   - the clock registers and the CPU/bus/peripheral frequencies;
   - UTLB state: pages, miss handler, misses, repairs, pinning and URB;
   - world switches, and the tic timer. "SAFE START" appears here when DEL was held.
2. **Memory:** the RAM arenas (start, end, free, largest block) and whether the OS heap was removed. Game
   zones only exist once a game has started.
3. **Storage:**
   - Up/Down picks a WAD, and OK/EXE maps it read-only (nothing is written). The page then shows how the
     OS laid it out in flash: blocks, direct pointers, contiguous runs and the physical range.
   - It also shows which UTLB slots the OS changed, and URB before and after.
4. **Benchmarks:**
   - OK/EXE runs the tests at the OS clock and at the game's F4 clock.
   - Results: 64 KiB read from flash and from RAM, and the time to push a frame to the screen (320x200, or
     396x224 in CGDoom HD).
   - **1** runs the timedemo with the WAD picked on the Storage page: its demos (DEMO1 to DEMO3 for the
     shareware WAD, a few minutes) play back to back as fast as the calculator can draw them, every tic
     drawn. A screen then shows each demo's frames, time and frame rate, and CGDoom returns to MPM.
5. **Log:** the last 12 internal log lines.

**Safe start.** If CGDoom crashes or resets while it starts, launch it again while you hold **DEL**. This
starts the add-in without its memory-protection setup (UTLB pinning), without overclocking and without its
private stack, for that session only. A message confirms it. If CGDoom only works this way, please report
it with the Diagnostics pages.

**What to report.** Please open an issue on GitHub, and include:

- the calculator model and OS version (from HOME → System → Version);
- the `mpm.bin` build you use;
- the WAD (name and size) and how you prepared it;
- what you did and what happened. Copy the exact text of any error screen. A fatal error screen shows the
  engine's message, and for common errors a hint line under it.
- photos of the five Diagnostics pages, including Storage after a probe and Benchmarks after a run;
- whether a safe start (holding DEL) changes anything.

## Troubleshooting

| Problem | What to do |
|---|---|
| Black screen or reset when you launch CGDoom | Launch it again while you hold **DEL** (safe start), then report it. Check that your OS and MPM are supported (see [Compatibility](#compatibility)). |
| "No *.wad file in the storage root." | Copy the WAD over USB to the top folder of the storage memory, not to a subfolder or `@MainMem`. The name must end in `.wad`. |
| "Unsupported calculator" | CGDoom only runs on the fx-CG100 / Graph Math+ with MPM from casio-mpm-220 (see [Compatibility](#compatibility)). |
| "Not enough memory ... Install the mpm.bin for your OS" | Your `mpm.bin` leaves less than 3 MiB (an older build). Install casio-mpm-220's `mpm.bin` (see [Which mpm.bin](#which-mpmbin)). |
| "Cannot open DOOM1.WAD." with a reason | *Too small*, *truncated* or *could not be opened*: the copy is incomplete, so copy it again. *Not a WAD file*: the file is not a WAD. *Name too long or has characters CGDoom cannot use*: rename it, for example to `doom1.wad`. |
| "Z_Malloc ... failure" (out of memory) | Check that you use casio-mpm-220's `mpm.bin`, or use a smaller WAD. |
| "W_GetNumForName: ... not found!" | The WAD is incomplete or not one CGDoom can play. Copy it again. |
| "NOT SAVED" when leaving | Storage is full. Delete files to make room. The new version of the listed file is lost. The screen says whether its previous version was kept. |
| The game is slow | See [Performance tips](#performance-tips). |
| The game froze | Hold **SHIFT + 7 + 3 + AC/ON**. |
| The calculator clock is late after playing | gint gives the OS back its clock at every file access and at exit. If the clock still drifts, please report it with your clock setting. |

## Known limitations

- **Partly tested on hardware.** CGDoom plays normally on an fx-CG100 with the new display and OS 02.20.
  Saves, the title-screen demos and the old display have not been tried yet, and
  [CGDoom HD](#cgdoom-hd) has not run on a calculator at all.
  The parts that depend most on the hardware are memory protection (UTLB), the flash layout, display
  timing and the overclock.
- **No sound or music.**
- **Single player only.** Network and co-op play are not supported.
- **One WAD at a time.** Add-on WADs (PWADs) cannot be loaded on top of an IWAD.
- **Some WAD demos cannot play.** The engine follows vanilla's game logic and random numbers, and the
  demos DEMO1-3 of the shareware DOOM v1.9 `doom1.wad` play on the title screen exactly like id's
  linuxdoom-1.10, tic by tic, and end on the same tic. Single-player Freedoom demos also play in sync.
  Co-op demos (for example Freedoom's DEMO2-4) do not play correctly, because only player 0 is
  supported, and *New Game* is refused while one plays; no WAD with such demos fits the fx-CG100's
  storage. Multiplayer, vanilla's undefined-memory behaviour and the lost soul floor-bounce variant of
  the registered Doom v1.9 executables are not reproduced.
- **No demo recording or replay.** The title screen plays the WAD's own demos only.
- **Storage.** About 4.5 MB of storage means only shareware-sized WADs fit on the fx-CG100.
- **One game per launch.** After a game, CGDoom returns to MPM.
- **fx-CG100 only, with OS 02.20 and casio-mpm-220's MPM.** Other models, OS versions and `mpm.bin`
  builds are not supported.
