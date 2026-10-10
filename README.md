# CGDoom for the fx-CG100 / Graph Math+

DOOM for the Casio fx-CG100 and Graph Math+, running through the Math+ Mod (MPM). It is a port of CGDoom,
the fx-CG50 DOOM engine, to the [gint](https://git.planet-casio.com/Lephenixnoir/gint) kernel, with a
launcher, fx-CG100 keymaps, overclocking and SH4 assembly renderers.

> **Status: not yet run on a real calculator.** It was built and tested on a PC (a host build, an SH-4
> simulator and unit tests), so the first runs on hardware may fail. If it does not work, see
> [If something goes wrong](#if-something-goes-wrong).

## Install

You need an fx-CG100 or Graph Math+ with OS **02.00.0202** or **02.00.2202** (Settings → About), its USB
cable, and a Windows PC once, to install MPM.

**1. Install MPM (once).** MPM is the community mod that lets the calculator run add-ins. Its installer is
Windows-only. A Windows VM may work from a Mac, but nobody has confirmed it on Apple Silicon.

1. Download and unzip
   [`mpm-installer-1.0bw.exe`](https://www.planet-casio.com/storage/forums/mpm-installer-1.0bw-199064.zip)
   (64-bit Windows 8 or later).
2. Connect the calculator and choose **OS Update** on its USB popup.
3. In `cmd`, run `mpm-installer-1.0bw.exe`.
4. When the calculator shows "Press OK to continue", press **OK** within about 10 seconds.
5. Follow the steps on the calculator (roughly TOOLS → SETTINGS → RESTART), and press the RESTART button
   on the back with a pen. A final "USB device is no longer available" message on the PC is harmless.

**2. Copy the files.** Download the zip from [Releases](https://github.com/cheparukhin/cgdoom-cg100/releases)
and unzip it. Connect the calculator in **USB flash drive** mode, and copy everything in the zip's
`calculator/` folder to the **root** of the storage memory (not into `@MainMem`):

| File | What it is |
|---|---|
| `CGDoom.g3a` | The game. |
| `DOOM1.WAD` | The shareware DOOM data, packed for the calculator. |
| `mpm.bin` | MPM's add-in menu, version 1.1. |

**3. Play.** Eject the calculator. On the HOME screen press **TOOLS**, select **CGDoom** and press **EXE**.

Good to know:

- **Do not install Casio OS updates.** An update removes MPM, and MPM does not support the newer OS
  versions yet.
- On OS 02.10, 02.20 or 02.00.xx1x, MPM can only be installed after a downgrade, and only on units with
  the old display. [Check your calculator](docs/USER-GUIDE.md#check-your-calculator) explains how to tell.
- If an `mpm.bin` is already on the calculator (for example from KhiCAS or gpSP), keep a copy. Those
  add-ins ship their own build, and whether they run under 1.1 has not been checked.

## Playing

Default keys on the fx-CG100 / Graph Math+:

| Action | Key |
|---|---|
| Move forward / back, turn | Arrows |
| Strafe | PrevTab / NextTab |
| Shoot | SHIFT |
| Open doors, use switches | ALPHA |
| Run (toggle) | VARIABLE |
| Weapons | 1–7 |
| Map | CATALOG |
| Doom menu (save, load, quit) | BACK |

- Saves are written when you quit, so leave through the Doom menu (*Quit Game*). After a game, CGDoom
  returns to MPM.
- **Emergency quit: SHIFT + 7 + 3 + AC/ON.** Your saves are kept.
- If the game is slow, try *Detail: Low* or a smaller *View size* in the launcher's *Options*.

The launcher's *Controls...* screen rebinds keys. The [user guide](docs/USER-GUIDE.md) covers every
option, all keymaps (including the fx-CG50's), saves and demos.

## If something goes wrong

- **Black screen or reset at launch:** launch CGDoom again while holding **DEL** (safe start).
- **"Unsupported OS":** CGDoom only runs on the OS versions MPM supports.
- **The game froze:** hold **SHIFT + 7 + 3 + AC/ON**.

To report a problem, open an [issue](https://github.com/cheparukhin/cgdoom-cg100/issues) with your OS
version, what happened (with the exact text of any error), and photos of the five **Diagnostics** pages
from the launcher. More fixes are in the [troubleshooting table](docs/USER-GUIDE.md#troubleshooting).

## Other setups

- **Without the release zip:** `python3 tools/wadtool.py fetch-wad DOOM1.WAD` downloads the shareware WAD
  and packs it, and `tools/build-mpm.sh` builds `mpm.bin` 1.1. Only shareware-sized WADs fit in the
  calculator's 4.5 MB of storage (see [WAD files](docs/USER-GUIDE.md#wad-files)).
- **fx-CG50 / Graph 90+E:** the same `CGDoom.g3a` starts from the main MENU, without MPM. It is untested
  there, and `doom1.wad` needs *Use experimental RAM* (OS 3.60 or older). On the fx-CG50,
  [Lephenixnoir's CGDoom](https://git.planet-casio.com/Lephenixnoir/CGDoom) is the proven build.

## Building from source

```sh
tools/build-toolchain.sh               # SH toolchain, fxSDK, gint (dev), fxlibc -> toolchain/
export PATH=$PWD/toolchain/bin:$PATH
fxsdk build-cg                         # -> CGDoom.g3a
tools/build-mpm.sh                     # -> build-mpm/mpm.bin (1.1)
make test-host                         # golden-frame tests (needs Freedoom 0.13.0 in test-wads/)
```

[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) has the prerequisites, the pinned upstream revisions, the
tests, the architecture and the hardware test plan.

## Credits and license

CGDoom is free software under the GNU GPL, version 2 ([`LICENSE`](LICENSE)). It descends from id Software's
DOOM, nDoom (Mrakoplaz, Critor), CGDoom (MPoupe) and CGDoom for the fx-CG50 (Lephenixnoir), and runs on
gint and the fxSDK (Lephenixnoir and contributors) and MPM (Lephenixnoir, CalcLoverHK and others).
The release zip's `DOOM1.WAD` (© id Software) and `mpm.bin` are not covered by the GPL.
[`CREDITS.md`](CREDITS.md) has the details. DOOM is a trademark of id Software.
