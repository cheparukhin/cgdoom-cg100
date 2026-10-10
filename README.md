# CGDoom for the fx-CG100 / Graph Math+

DOOM for the Casio fx-CG100 and Graph Math+, as an add-in for the Math+ Mod (MPM). This is a port of
CGDoom, the fx-CG50 DOOM engine, onto the [gint](https://git.planet-casio.com/Lephenixnoir/gint) kernel.
It adds a launcher, keymaps for the fx-CG100 keyboard, overclocking and SH4 assembly renderers. The same
`CGDoom.g3a` also starts on the fx-CG50 / Graph 90+E, but support there is limited (see
[Requirements](#requirements)).

> **Status: this build has not been run on a real calculator yet.** Everything here was built and tested
> on a PC: a host build, an SH-4 instruction-set simulator, and unit tests of the calculator code. The
> first runs on hardware may fail. If CGDoom does not work for you, see [Reporting a problem](#diagnostics-and-reporting-a-problem).
> [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) has the plan for testing on hardware.

## Contents

- [Requirements](#requirements)
- [Installing MPM](#installing-mpm)
- [Getting a WAD](#getting-a-wad)
- [Installing](#installing)
- [The launcher](#the-launcher)
- [Controls](#controls)
- [Performance tips](#performance-tips)
- [Saves, demos and files](#saves-demos-and-files)
- [Diagnostics and reporting a problem](#diagnostics-and-reporting-a-problem)
- [Troubleshooting](#troubleshooting)
- [Known limitations](#known-limitations)
- [Building from source](#building-from-source)
- [Credits and license](#credits-and-license)

## Requirements

**Calculator:** an fx-CG100 or Graph Math+ that has MPM installed.

- **OS 02.00.0202 or 02.00.2202.** These are the versions the MPM installer accepts. To check yours, open
  Settings → About. MPM cannot be installed on other versions today: 01.00, 02.00.xx1x (for example
  02.00.0212), 02.10 and 02.20.
  - Inside CGDoom, the Advanced screen and the Diagnostics show the OS as `02.00.0000`. That is the
    internal string of both supported builds, so it is normal.
  - On OS strings `01.00.0000` and `02.10.0000`, CGDoom asks before it continues ("Experimental OS"):
    its file access there uses entry points nobody has tested. On any other unknown OS it shows
    "Unsupported OS" and quits.
- **MPM** (the OS patch, plus `mpm.bin` at the root of the storage memory). The MPM topic on Planète
  Casio explains how to install it, and which units the installer supports.

**Which `mpm.bin`.** Several builds of `mpm.bin` are in use, and they leave very different amounts of RAM
to add-ins. CGDoom detects the build and uses all the RAM it can find. The figures below come from reading
the loaders' source, not from measurements.

| `mpm.bin` build | Free RAM for CGDoom | Where the WAD is read from |
|---|---|---|
| **1.1 or later** (recommended) | about 5.5 MiB in one block | Copied to RAM: a packed `doom1.wad` should fit, with about 300 KiB to spare by calculation |
| Public Feb-2025 zip | 2 MiB, plus about 2.5 MiB that CGDoom finds itself | Read in place from flash (slower) |
| "dynload" build (shipped by gpSP and KhiCAS) | about 4.5 MiB, which CGDoom finds itself | Read in place from flash (slower) |
| builds before Feb-2025 | 2 MiB, plus about 2.5 MiB | Read in place from flash (slower) |

- With less than 1.5 MiB in total, CGDoom stops at startup with "Not enough memory" and asks for
  mpm.bin 1.1 or later.
- The Advanced screen shows which loader was detected and how much RAM is free. For example, it shows
  `mpm.bin >= 1.1, <N> KiB free`.
- MPM publishes no binary releases, so `mpm.bin` 1.1 has to be built from the MPM repository
  (`git.planet-casio.com/PlaneteCasio/mpm`). `tools/build-mpm.sh` does this; see
  [Installing MPM](#installing-mpm).

**fx-CG50 / Graph 90+E.** The g3a starts on the fx-CG50, with its own keymaps. This is a bonus target,
and it is untested. The game needs about 700 KiB of zone memory for `doom1.wad`. Without CGDoom's
"experimental RAM" the fx-CG50 leaves only about 150 KiB.

- **OS 3.60 or older:** turn on Advanced → *Use experimental RAM*.
- **OS 3.70 and later:** there is no experimental RAM, so `doom1.wad` cannot run.

Before a game starts, the launcher estimates the memory and warns you if it is too small. On the fx-CG50,
[Lephenixnoir's original CGDoom](https://git.planet-casio.com/Lephenixnoir/CGDoom) is the proven build.

## Installing MPM

MPM has two parts: an **OS patch**, installed once from a PC, and **`mpm.bin`**, the add-in menu, which is a
file at the root of the storage memory. The steps below come from the MPM topic on Planète Casio.

**1. Check that your calculator is supported.**

- Settings → About must show **02.00.0202** or **02.00.2202**.
- On 02.10, 02.20 or 02.00.xx1x, check the bootcode date first:
  1. With the calculator off, hold **HOME + ×10ˣ + AC** and press **ON**.
  2. Press **SETTINGS**, then **9** (nothing is erased), then **4 (VERSION)**.
  3. Read **DateA**. If it is `2017.0106.2008` (old display), you can downgrade to 02.00.0202 and then
     install MPM. If it is `2025.0304.1404` (new display), MPM cannot be installed on this unit yet. Do not
     downgrade: the screen stays black.
- **Do not install Casio OS updates on an MPM calculator.** Any OS update removes the patch, and the
  newer OS versions (2.10, 2.20) are not supported by the installer yet.

**2. Install the OS patch (Windows only).**

The installer is a closed-source Windows program. There is no macOS or Linux version. From a Mac, use a
Windows PC, or a Windows VM with the calculator's USB device attached to it. MPM's instructions allow a VM,
but nobody has confirmed it on Apple Silicon.

1. Download and unzip
   [`mpm-installer-1.0bw.exe`](https://www.planet-casio.com/storage/forums/mpm-installer-1.0bw-199064.zip).
   It needs 64-bit Windows 8 or later.
2. Connect the USB cable. On the calculator's popup, choose **OS Update**.
3. In `cmd`, run `mpm-installer-1.0bw.exe`.
4. When the PC shows "Communicating more...", the calculator shows "Press OK to continue". Press **OK**
   within about 10 seconds.
5. Follow the steps on the calculator (roughly TOOLS → SETTINGS → RESTART). Press the RESTART button on
   the back with a pen.
6. A final "USB device is no longer available" message on the PC is harmless.

**3. Get `mpm.bin` 1.1.** Build it with `tools/build-mpm.sh` (needs the toolchain, see
[Building from source](#building-from-source)). Copies from the forum or from other add-ins are usually
older builds. They work, but leave CGDoom less RAM. KhiCAS and gpSP ship their own "dynload" `mpm.bin`.
Whether they run under 1.1 has not been checked, so keep a copy of the `mpm.bin` you replace.

Copying `mpm.bin`, add-ins and WADs only needs USB mass storage, which works from any OS.

## Getting a WAD

CGDoom needs a DOOM data file (a WAD). **No WAD is included.** Use the shareware `doom1.wad` (version 1.9,
4,196,020 bytes).

**The easy way:** `tools/wadtool.py` (Python 3, no dependencies) can download it and pack it for the
calculator in one step:

```sh
python3 tools/wadtool.py fetch-wad DOOM1.WAD
```

It downloads an unmodified copy from Debian's `doom-wad-shareware` package (the WAD id Software made
freely distributable), checks its SHA-256, and writes a packed `DOOM1.WAD`, described below. Add
`--save-original doom1.wad` to keep the original too. If the download fails, get `doom1.wad` yourself
(it is also on the original shareware media) and pack it as below.

The fx-CG100 has only about **4.5 MB** of storage memory, and the add-in and `mpm.bin` also need room.
That is why only shareware-sized IWADs fit:

- **Fits:** shareware `doom1.wad`, once packed (below).
- **Does not fit:** Ultimate Doom, Doom II, Final Doom, and Freedoom (about 28 MB per phase).
- CGDoom's split Ultimate Doom WADs (`doomu1.wad`–`doomu4.wad`) are recognized by the launcher. As
  distributed they are 5.2–6.6 MB each, and nobody has checked whether a packed one fits.

**Pack the WAD on your PC first** (`fetch-wad` already did this). CGDoom has no sound, so
`tools/wadtool.py` removes the sound effects, music and the ENDOOM text screen. It also aligns every lump
to 4 bytes:

```sh
python3 tools/wadtool.py pack doom1.wad DOOM1.WAD --drop-ends
```

For the 1.9 shareware WAD this gives 3,389,236 bytes, saving 19%. The tool prints whether the result fits
its storage budget of 4,050,000 bytes.

- The WAD's own demos are kept. Add `--drop-demos` to remove them too, which saves about 44 KB more.
- `python3 tools/wadtool.py info <wad>` shows what a WAD contains.
- `python3 tools/wadtool.py verify doom1.wad DOOM1.WAD` checks that every lump that was kept is unchanged.

## Installing

1. Connect the calculator to your PC with USB and choose the USB flash drive (mass storage) mode.
2. Copy `CGDoom.g3a` and your packed `DOOM1.WAD` to the **root** of the storage memory (the top folder,
   not `@MainMem` and not a subfolder). `mpm.bin` must already be there (see
   [Installing MPM](#installing-mpm)).
3. Eject the calculator safely and unplug it.
4. On the HOME screen, press **TOOLS**. MPM's add-in grid opens. Move to **CGDoom** with the arrow keys
   and press **EXE**. (MPM's own menu starts an add-in on EXE; its source does not check for OK.)

On the fx-CG50, copy the same files to the storage root and start CGDoom from the main MENU.

**Safe start.** If CGDoom crashes or resets while it starts, launch it again while you hold **DEL**. This
starts the add-in without its memory-protection setup (UTLB pinning), without overclocking and without its
private stack, for that session only. A message confirms it. If CGDoom only works this way, please report
it with the Diagnostics pages.

## The launcher

The launcher lists every `*.wad` file in the storage root, with up to 16 WADs and 24 demos. Below the list
are the game options.

**Navigation (fx-CG100):** **Up/Down** select, **Left/Right** change a value, **OK** or **EXE** select,
**BACK** or **HOME** returns. On the main screen, **BACK** or **HOME** quits to MPM.

**Navigation (fx-CG50):** the arrows, **EXE**, and **EXIT** to return. **MENU** quits.

On the episode and map rows you can also type a digit.

### Main screen

| Item | What it does | Default |
|---|---|---|
| *Play Doom (shareware)* (one row per WAD) | Starts the game with that WAD. The row text depends on the file name (`doom1.wad`, `doomu.wad`, `doom2.wad` and others); unknown names read "Play other WAD". | — |
| Skill | Difficulty for a direct start. It is used only when *Title screen* is *Skip*; otherwise the Doom menu asks. | Hurt me plenty |
| Start at episode | 1–4. Changing it switches *Title screen* to *Skip* for this launch. | 1 |
| Start at map | 1–99. Changing it switches *Title screen* to *Skip* for this launch. If the WAD has no such map, you return to the launcher with a message. | 1 |
| Title screen | **Skip**: start the game directly. **No demos**: show the title screen, then use the Doom menu. **Full**: the title screen also plays the WAD's demos (the shareware `doom1.wad` demos play exactly like vanilla; some Freedoom demos cannot, see [Known limitations](#known-limitations)). | No demos |
| Record demo on slot | None or 0–9. Recording forces a direct start for this launch. If the slot already holds a demo for this WAD, CGDoom asks before it is overwritten. | None |
| Replay demo... | Only shown when `.lmp` files exist. Lists the demos with the WAD each one belongs to, and plays one. | — |
| Controls... | Keymap presets and rebinding (see [Controls](#controls)). | — |
| Options... | Speed and display options (below). | — |
| Advanced... | Memory and WAD access options (below). | — |
| Diagnostics... | Five pages of hardware information (see [Diagnostics](#diagnostics-and-reporting-a-problem)). | — |
| Quit | Returns to MPM. | — |

The game engine cannot be restarted. **When a game ends, CGDoom returns to MPM**, not to the launcher.
Launch it again to play again.

### Options

| Option | Values | Default |
|---|---|---|
| CPU clock | **Off (OS clock)**; **F4: 236 MHz CPU**, the bus is unchanged (gint's safe preset); **Ptune4 F5 (exp.)**, a 236 MHz CPU with the memory bus at 118 MHz (fx-CG100 only, asks for confirmation, see [Performance tips](#performance-tips)). | F4 |
| Display | **DMA (async)**: the frame is sent to the screen by DMA while the next one is drawn. **DMA strips (X/Y RAM)**: the frame goes out in 10-row strips through on-chip memory. **CPU direct**: the CPU writes each pixel. | DMA (async) |
| Frameskip | **Auto (max 3)**: the game always runs at 35 tics per second and skips drawing at most 3 frames in a row when it falls behind. **0–3**: a fixed number of frames skipped after every drawn frame. | Auto |
| FPS counter | Shows the frame rate in the game. | Off |
| View size | 3–9 (smaller views, with a black border), 10 (**Status bar**), 11 (**Full screen**, no status bar). | 10 |
| Detail | **High**, or **Low** (half horizontal resolution). | High |
| Flat floors | Draws floors and ceilings in one solid color each ("potato" mode). | Off |

View size and detail can also be changed in the game: there are keys for the view size, and *Detail* is in
the Doom menu. CGDoom saves those changes. Flat floors can only be changed here.

### Advanced

| Option | What it does | Default |
|---|---|---|
| Developer info | Shows statistics screens before and after the game: WAD mode, RAM image, heap, clock, zone peak, stack peak, UTLB repairs. | Off |
| WAD access | **Memory**: a RAM copy, or reading in place from flash. **BFile reads**: reads every lump through the OS, which is slow (for debugging). | Memory |
| Copy WAD to RAM if it fits | Copies the WAD (without its sounds) to RAM when there is enough memory left for the game. Otherwise lumps are read in place from flash. | On |
| Trust unaligned lumps | Uses lumps that are not 4-byte aligned in place instead of copying them. Packed WADs have none. | Off |
| Use experimental RAM | Only shown on the fx-CG50 with OS 3.60 or older. Uses the RAM above the 2 MB line. | Off |

The screen also shows the model, OS string, `mpm.bin` build and free RAM.

Settings are stored in `CGDOOM.CFG` and written when CGDoom exits.

## Controls

The Controls screen (**Controls...**) has these actions:

- *Load preset*: pick a preset with Left/Right, then press OK/EXE to apply it.
- Rebind an action: select it, press OK/EXE, then press the new key. BACK or HOME cancels (MENU on the fx-CG50).
- **DEL** unbinds an action.
- *Discard changes* restores the keymap you had when you opened the screen.

Each key does one action. If you bind a key that was already in use, its old action is unbound and the
screen says so. **AC/ON** cannot be bound, and neither can the Doom menu key. (Two fx-CG50 presets
put Pause on AC/ON; that binding is kept, but you cannot make it yourself.)

### fx-CG100 / Graph Math+

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
| Heap stats | sqrt | sqrt |
| FPS counter | PageDown | PageDown |
| Frameskip | TOOLS | TOOLS |
| Run profiler | ) | ) |

**In the Doom menu:**

- **BACK** opens and closes the Doom menu. This key is fixed.
- **EXE** selects. **OK** also selects, as long as no action is bound to it; the presets leave it free.
- The move keys navigate. With *Strafe arrows*, Left/Right on sliders are the turn keys PrevTab/NextTab.
- Prompts say "press exe to confirm". EXE or the Interact key answers yes, and BACK answers no.

### fx-CG50 / Graph 90+E

| Action | **Thumbs only** (default) | **Full hands** | **CGDoom 0.3** |
|---|---|---|---|
| Look left / Look right | Left / Right | F5 / F6 | Left / Right |
| Move forward / Move backward | Up / Down | OPTN / x^2 | Up / Down |
| Strafe left / Strafe right | X,O,T / log | ALPHA / ^ | 0 / . |
| Shoot | ALPHA | DEL | ALPHA |
| Interact | x^2 | x | EXE |
| Toggle run | tan | cos | (none) |
| Weapons 1–6 / 7 | F1–F6 / 7 | 1–6 / 7 | F1–F6 / SHIFT |
| Map | EXIT | tan | EXIT |
| Pause | AC/ON | AC/ON | OPTN |
| Smaller / Larger view | - / + | - / + | - / + |
| Cheat | , | , | ^ |
| Noclip | -> | -> | -> |
| Brightness | Frac | Frac | Frac |
| Heap stats | S<>D | S<>D | S<>D |
| FPS counter | ( | ( | ( |
| Frameskip | VARS | sin | VARS |
| Run profiler | ) | ) | ) |

On the fx-CG50, **MENU** opens the Doom menu and **EXE** selects.

### Special keys

- **Cheat** turns god mode on or off, and gives full armor, all weapons, full ammo and all keys.
  **Noclip** lets you walk through walls.
- **Brightness** cycles through the five gamma levels.
- **Heap stats** shows free/total zone memory.
- **Frameskip** cycles through 0 → 1 → 2 → 3 → auto.
- **Run profiler** measures 40 tics and then shows the time spent in allocation (DA), rendering (GR),
  display (DI), lump loading (LL) and unaligned-lump loading (ULL), in milliseconds.

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
    saves, a demo being recorded, and the settings are kept.
  - If CGDoom is frozen and does not react within 2 seconds while you hold the keys, it aborts without
    writing files.
- **Idle exit.** The add-in runs the calculator itself, so the OS cannot turn the calculator off. CGDoom
  therefore returns to MPM after a period with no key pressed, and MPM's own auto power-off then works.
  - The launcher and the title demos wait 10 minutes; a game you are playing, even paused, waits
    30 minutes.
  - A 60-second warning comes first. Press any key to stay.

## Performance tips

Real frame rates on hardware are not known yet: the figures below come from an SH-4 simulator, which counts
instructions. In the simulator, in instructions per frame, compared with the default settings:

| Setting | Work saved per frame |
|---|---|
| Detail: Low | 36–39% |
| View size 9 | 12–16% |
| View size 7 | 31–38% |
| Flat floors | 6.5–16% |
| Low detail + flat floors | 42–46% |

- **Keep CPU clock at F4** (the default). It is gint's own preset and other fx-CG100 add-ins ship with it.
- **Ptune4 F5 is experimental.** It also runs the memory bus at 118 MHz. gint does not offer it on the
  fx-CG100 because some units cannot handle the faster bus: **crashes or corrupted data are possible**.
  gpSP and KhiCAS use it on many units. Try it only if you accept that risk, and turn it off if anything
  odd happens. The fx-CG50 only gets Off and F4.
- **Leave Frameskip on Auto.** The game then keeps real-time speed and draws as often as it can.
- **Use mpm.bin 1.1** so that the WAD can be copied to RAM. Reading from flash is several times slower.
  The Developer info screen shows the mode that was used: *RAM image*, *flash block map* or *BFile reads*.
  (Diagnostics → storage only shows a WAD after a probe, which always maps it as a block map, so it does
  not tell you what a game used.)
- **Display mode.** Which of the three is fastest on real hardware is not known yet. *DMA (async)* should be,
  because the transfer overlaps the drawing of the next frame. Diagnostics → benchmarks measures all three
  on your calculator.

## Saves, demos and files

CGDoom only creates files with short 8.3 names, which the calculator requires, in the storage root.
The `<P>` prefix comes from the WAD's name. It is `DOOM1` for `doom1.wad` / `DOOM1.WAD`. Long names are
shortened with a hash, for example `FRE0P` for `freedoom1.wad`.

| File | What it is |
|---|---|
| `CGDOOM.CFG` | Settings and keymap (text). |
| `<P>_0.DSG` … `<P>_5.DSG` | The six save slots of the Doom menu, for example `DOOM1_0.DSG`. |
| `<P>D00.LMP` … `<P>D09.LMP` | Recorded demos, slots 0–9 of *Record demo on slot*, for example `DOOM1D03.LMP`. |

- **Files are written when you leave.** Writing to storage moves data in flash, and that would break
  CGDoom's direct view of the WAD in flash. So saves and demos are kept in RAM until you quit.
  - When you save, the game says "Save file will be created on exit".
  - Loading works in the same session.
  - When you quit, CGDoom shows "Writing N files... Do not turn off the calculator".
- If a file cannot be written, for example because storage is full, the exit screen says **NOT SAVED** and
  names the file. Free some storage space before you play again.
- A demo can be up to 128 KiB, which is about 15 minutes. Recording stops when the buffer is full, and the
  game goes on.
- To replay a demo, use *Replay demo...*. If CGDoom cannot tell which WAD a demo belongs to, it asks (with
  only one WAD installed it uses that one). When the demo ends, CGDoom returns to MPM.
- To delete files, use the Memory app or USB.

## Diagnostics and reporting a problem

**Diagnostics...** in the launcher has five pages. Change pages with Left/Right.

1. **System:**
   - model, OS string and OS access path;
   - gint version, LCD controller id, and the stack pointer at entry;
   - MPM load info, the physical addresses of the code and static RAM, the loader and the image size;
   - the clock registers and the CPU/bus/peripheral frequencies;
   - UTLB state: pages, miss handler, misses, repairs, pinning and URB;
   - world switches, and the tic timer. "SAFE START" appears here when DEL was held.
2. **Memory:** the RAM arenas (start, end, free, largest block), whether the OS heap was removed, and any
   private arenas. Game zones only exist once a game has started.
3. **Storage:**
   - Up/Down picks a WAD, and OK/EXE maps it read-only (nothing is written). The page then shows how the
     OS laid it out in flash: blocks, direct pointers, contiguous runs and the physical range.
   - It also shows which UTLB slots the OS changed, and URB before and after.
4. **Benchmarks:**
   - OK/EXE runs the tests at the OS clock and at the selected overclock.
   - Results: 64 KiB read from flash and from RAM, and the time to push a 320x200 frame by DMA, by DMA
     strips and by CPU.
5. **Log:** the last 12 internal log lines.

**What to report.** Please open an issue or post on the forum thread, and include:

- the calculator model and OS version (from Settings → About);
- the `mpm.bin` build you use (the Advanced screen shows CGDoom's guess);
- the WAD (name and size) and how you prepared it;
- what you did and what happened. Copy the exact text of any error screen. A fatal error screen shows the
  engine's message, and for common errors a hint line under it.
- photos of the five Diagnostics pages, including Storage after a probe and Benchmarks after a run;
- whether a safe start (holding DEL) changes anything;
- with Advanced → *Developer info* on, photos of the screens before and after the game.

## Troubleshooting

| Problem | What to do |
|---|---|
| Black screen or reset when you launch CGDoom | Launch it again while you hold **DEL** (safe start), then report it. Check that your OS and MPM are supported (see [Requirements](#requirements)). |
| "No *.wad file in the storage root." | Copy the WAD over USB to the top folder of the storage memory, not to a subfolder or `@MainMem`. The name must end in `.wad`. |
| "Unsupported OS" | CGDoom only runs on the OS versions that MPM supports (02.00.0202 / 02.00.2202), and on the fx-CG50. |
| "Not enough memory ... Install mpm.bin version 1.1 or later" | Your `mpm.bin` leaves less than 1.5 MiB. Install mpm.bin 1.1. |
| "Cannot open DOOM1.WAD." with a reason | *Too small*, *truncated* or *could not be opened*: the copy is incomplete, so copy it again. *Not a WAD file*: the file is not a WAD. *Name too long or has characters CGDoom cannot use*: rename it, for example to `doom1.wad`. |
| "DOOM1.WAD has no map E2M1" | The shareware WAD only has episode 1. Change *Start at episode/map*. |
| "Z_Malloc ... failure" (out of memory) | On the fx-CG100, untick Advanced → *Copy WAD to RAM if it fits* to leave more memory to the game, or use a smaller WAD. On the fx-CG50, turn on *Use experimental RAM* (OS 3.60 or older). After an earlier session, reset the calculator first. |
| "Not enough memory" / "Low memory" before a game (fx-CG50) | Follow what the dialog says: turn on experimental RAM, or reset the calculator. On OS 3.70+ the game cannot run. |
| "Demo is from a different game version" | Delete that `.LMP` file or record it again. |
| "W_GetNumForName: ... not found!" | The WAD is incomplete or not one CGDoom can play. Copy it again. |
| "NOT SAVED" when leaving | Storage is full. Delete files to make room. The listed file is lost. |
| The game is slow | See [Performance tips](#performance-tips). Check that the WAD is in RAM (Developer info). |
| The game froze | Hold **SHIFT + 7 + 3 + AC/ON**. |
| The calculator clock is late after playing | gint gives the OS back its clock at every file access and at exit. If the clock still drifts, please report it with your clock setting. |

## Known limitations

- **Untested on hardware.** Expect problems in the first hardware runs. The parts that depend most on the
  hardware are memory protection (UTLB), the flash layout, display timing and the overclock.
- **No sound or music.**
- **Single player only.** Network and co-op play are not supported.
- **Some WAD demos cannot play.** The engine follows vanilla's game logic and random numbers, and the
  demos DEMO1-3 of the shareware DOOM v1.9 `doom1.wad` play exactly like id's linuxdoom-1.10, tic by tic,
  and end on the same tic. Single-player Freedoom demos also play in sync. Co-op demos (for example
  Freedoom's DEMO2-4) cannot play, because only player 0 is supported. Multiplayer, vanilla's
  undefined-memory behaviour and the lost soul floor-bounce variant of the registered Doom v1.9 executables
  are not reproduced.
  - Demos recorded with an earlier CGDoom build no longer replay correctly, because the random-number use
    changed to match vanilla. Saved games still load. Demos recorded with this build replay exactly.
  - The title demos are still off by default.
- **Storage.** About 4.5 MB of storage means only shareware-sized WADs fit on the fx-CG100.
- **One game per launch.** After a game, CGDoom returns to MPM.
- **fx-CG50:** untested. It needs experimental RAM (OS 3.60 or older) for `doom1.wad`.
- **Ptune4 F5** can crash some units.

## Building from source

This needs cmake, libpng, libusb, ncurses and Python 3 with Pillow. `tools/build-toolchain.sh` was
written for macOS with Homebrew (its library paths are Homebrew's; on Linux it may need adjusting).
It builds sh-elf binutils 2.42, GCC 14.1, OpenLibm, fxlibc, gint (dev branch, which has the fx-CG100 support) and the fxSDK into
`toolchain/`, from clones in `ref/`:

- `ref/fxsdk`, `ref/gint`, `ref/OpenLibm`, `ref/sh-elf-gcc` from `git.planet-casio.com/Lephenixnoir/...`
- `ref/fxlibc` from `git.planet-casio.com/Vhex-Kernel-Core/fxlibc`

```sh
tools/build-toolchain.sh            # everything (or one step: fxsdk binutils gcc openlibm fxlibc gint)
tools/build-toolchain.sh sim        # optional: GDB's SH simulator, for the simulator tests
export PATH=$PWD/toolchain/bin:$PATH

fxsdk build-cg                      # -> CGDoom.g3a (and build-cg/cgdoom.bin)
make test-host                      # host build, golden-frame tests (needs the Freedoom WADs)
tools/build-mpm.sh                  # optional: mpm.bin 1.1 -> build-mpm/mpm.bin
```

The host tests use Freedoom 0.13.0 (`test-wads/fd/freedoom-0.13.0/freedoom1.wad` and `freedoom2.wad`,
BSD license). [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) has the full list of test targets,
the architecture and the hardware test plan.

**Not in this repository.** `ref/`, `toolchain/`, `test-wads/` and the build directories are
gitignored. To recreate them:

```sh
mkdir -p test-wads/fd && cd test-wads/fd
curl -fLO https://github.com/freedoom/freedoom/releases/download/v0.13.0/freedoom-0.13.0.zip
unzip freedoom-0.13.0.zip && cd ../..
```

Pinned upstream revisions (the release in this repository was built and tested against these):

| Path | Repository | Commit |
|---|---|---|
| `ref/fxsdk` | `git.planet-casio.com/Lephenixnoir/fxsdk` (dev) | `a6e46eb6df060dc4e6e931521205e516c92b6c7f` |
| `ref/gint` | `git.planet-casio.com/Lephenixnoir/gint` (dev) | `463f180ffb71ccdcef05c928ad19f2a480b14bc1` |
| `ref/fxlibc` | `git.planet-casio.com/Vhex-Kernel-Core/fxlibc` (dev) | `a6903066cf4777aa96556a6da9699d4704867185` |
| `ref/OpenLibm` | `git.planet-casio.com/Lephenixnoir/OpenLibm` | `9828d2e3f265a882af6a5bba8ffe6a00c3eeb8d7` |
| `ref/sh-elf-gcc` | `git.planet-casio.com/Lephenixnoir/sh-elf-gcc` | `fdbc1be7a84e88f45f9b768b7428ff33b77eb9bb` |
| `ref/sh-elf-binutils` | `git.planet-casio.com/Lephenixnoir/sh-elf-binutils` | `a963554d12c0236ee10c7545daa4c422bf107de6` |
| `ref/CGDoom` | `git.planet-casio.com/Lephenixnoir/CGDoom` (upstream engine) | `8516c16d9d3c65b45882433b74abdef171265c99` |
| `ref/mpm` | `git.planet-casio.com/PlaneteCasio/mpm` | `cbd1a414d96b55e14adc20a88e2140f2c8723b5a` |

`tools/build-toolchain.sh` checks out the `dev` branch tip of fxsdk, gint and fxlibc. To reproduce
this build exactly, check out the commits above after cloning.
`tools/demosync-oracle/build.sh` takes the `linuxdoom-1.10` source tarball as its argument (id
Software's DOOM source release).

**Development loop with MPM 1.1.** You can load a build without copying it to storage:

1. In the MPM menu, press **SETTINGS**. MPM waits for an add-in over USB.
2. Run this on the PC:

   ```sh
   fxlink -pw build-cg/cgdoom.bin
   ```

   MPM loads that header-less binary into RAM and starts it. The WAD must still be in storage.

## Credits and license

CGDoom is free software under the **GNU General Public License, version 2** (see [`LICENSE`](LICENSE)).
It is a derivative of the DOOM source code released by id Software, and of nDoom. See
[`CREDITS.md`](CREDITS.md) for details.

Lineage of the code:

1. id Software's DOOM.
2. **nDoom**: Mrakoplaz for the TI-Nspire, then Critor for the Nspire CX.
3. **CGDoom** for the fx-CG10/20, by MPoupe.
4. **CGDoom for the fx-CG50**, by Lephenixnoir, with Computer_Freak_2004 and earlier attempts by
   ComputerNerd.
5. This fx-CG100 port.

Other credits:

- **gint and the fxSDK:** Lephenixnoir and contributors (Planète Casio).
- **MPM**, the Math+ Mod: the MPM authors, including Lephenixnoir and CalcLoverHK (Planète Casio).
- **fxlibc:** Vhex-Kernel-Core.
- **gpSP-sh4-jit**, the fx-CG100 port by KaraRyougi: the patterns proven on hardware that this port reuses.
  These are the display strip presenter, the flash block-map handling, the Ptune4 F5 register set
  (CalcLoverHK's Ptune4), and the bounded key-release wait.
- **Chocolate Doom:** the corrected low-detail drawing.
- **GBADoom:** a column-drawer technique (`COLEXTRABITS`).
- DOOM is a trademark of id Software. No id Software data is included.
