# CGDoom for the fx-CG100 / Graph Math+

DOOM for the Casio fx-CG100 and Graph Math+, running through the Math+ Mod (MPM). It is a port of CGDoom,
the fx-CG50 DOOM engine, to the [gint](https://git.planet-casio.com/Lephenixnoir/gint) kernel, with a
launcher, fx-CG100 keymaps, overclocking and SH4 assembly renderers.

> **Status: not yet run on a real calculator.** It was built and tested on a PC (a host build, an SH-4
> simulator and unit tests), so the first runs on hardware may fail. If it does not work, see
> [If something goes wrong](#if-something-goes-wrong).

## Install

You need an fx-CG100 or Graph Math+ with OS **02.00.0202**, **02.00.2202** or **02.20.0202** (HOME →
System → Version), with MPM installed for that OS.

**1. Install MPM.** MPM (the Math+ Mod) is the community mod that lets the calculator run add-ins: an OS
patch, plus `mpm.bin`, the add-in menu at the root of the storage memory. It is distributed separately,
by [casio-mpm-220](https://github.com/cheparukhin/casio-mpm-220), which has the steps for each OS:

- **OS 02.00.0202 / 02.00.2202:** MPM's Windows installer and `mpm.bin` 1.1. CGDoom needs 1.1; older
  builds leave it too little RAM.
- **OS 02.20.0202:** an experimental OS patch and its matching `mpm.bin`. **This patch has never been
  installed on a calculator.**
- Other versions (02.10, 02.00.xx1x) cannot run MPM yet. casio-mpm-220 explains how to check your
  calculator and what its options are.

**2. Copy the files.** Download the zip from [Releases](https://github.com/cheparukhin/cgdoom-cg100/releases)
and unzip it. Connect the calculator in **USB flash drive** mode, and copy both files in the zip's
`calculator/` folder to the **root** of the storage memory (not into `@MainMem`):

| File | What it is |
|---|---|
| `CGDoom.g3a` | The game, for both OS 02.00 and OS 02.20. |
| `DOOM1.WAD` | The shareware DOOM data, packed for the calculator. |

**3. Play.** Eject the calculator. On the HOME screen press **TOOLS**, select **CGDoom** and press **EXE**.

**Do not install Casio OS updates** on a calculator where MPM works: an update removes MPM.

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
option, the keymaps, saves and demos.

## If something goes wrong

- **Black screen or reset at launch:** launch CGDoom again while holding **DEL** (safe start).
- **"Unsupported calculator":** CGDoom only runs on the fx-CG100 / Graph Math+, with OS 02.00 or 02.20
  under MPM.
- **The game froze:** hold **SHIFT + 7 + 3 + AC/ON**.

To report a problem, open an [issue](https://github.com/cheparukhin/cgdoom-cg100/issues) with your OS
version, what happened (with the exact text of any error), and photos of the five **Diagnostics** pages
from the launcher. More fixes are in the [troubleshooting table](docs/USER-GUIDE.md#troubleshooting).

## Other setups

- **Without the release zip:** `python3 tools/wadtool.py fetch-wad DOOM1.WAD` downloads the shareware WAD
  and packs it. Only shareware-sized WADs fit in the calculator's 4.5 MB of storage (see [WAD files](docs/USER-GUIDE.md#wad-files)).
- **fx-CG50 / Graph 90+E:** not supported (CGDoom says so and quits). Use
  [Lephenixnoir's CGDoom](https://git.planet-casio.com/Lephenixnoir/CGDoom) there.

## Building from source

```sh
tools/build-toolchain.sh               # SH toolchain, fxSDK, patched gint, fxlibc -> toolchain/
export PATH=$PWD/toolchain/bin:$PATH
fxsdk build-cg                         # -> one CGDoom.g3a for OS 2.00 and 2.20
make test-host                         # golden-frame tests (needs Freedoom 0.13.0 in test-wads/)
```

[`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) has the prerequisites, the pinned upstream revisions, the
tests, the architecture and the hardware test plan.

For an existing SDK, run `tools/build-toolchain.sh gint` once to install the
required runtime patch, then rebuild normally. No OS-specific build flag is needed.

## Credits and license

CGDoom is free software under the GNU GPL, version 2 ([`LICENSE`](LICENSE)). It descends from id Software's
DOOM, nDoom (Mrakoplaz, Critor), CGDoom (MPoupe) and CGDoom for the fx-CG50 (Lephenixnoir), and runs on
gint and the fxSDK (Lephenixnoir and contributors) and MPM (Lephenixnoir, CalcLoverHK and others).
The release zip's `DOOM1.WAD` (© id Software) is not covered by the GPL.
[`CREDITS.md`](CREDITS.md) has the details. DOOM is a trademark of id Software.
