# Golden CRC changes

Every intentional change to `tests/golden/*.crc` is listed here, newest last: what changed, why, and how it was
checked. Goldens are regenerated with `make golden` (host, `tests/scenarios.txt`).

## 2026-10-09 — step 1: vanilla-exact FixedMul/FixedDiv (perf P1)

**Changed:** every golden except `title.crc` (the title screen draws no 3D view and runs no game logic).

| golden | frames (old → new) | first differing frame | why |
|---|---|---|---|
| demo1, e1m1, e1m1-exit, e1m2, e3m4, map07, map30-cast | unchanged | 0 | renderer math: wall/sprite/flat projection |
| demo2-1 | unchanged | 1 | renderer math |
| e1m3 | 982 → 978 | 2 | renderer and game math; the player now dies at tic 472 instead of 757, so the respawn and its wipe come earlier |
| e2m2 | unchanged | 3 | renderer math |
| map01 | unchanged | 9 | renderer math |
| e1m8-finale | unchanged | 16 | renderer math |
| title | unchanged | none | — |

**Why:** CGDoom's `FixedMul` was a bit-serial loop that rounded toward zero, where vanilla rounds toward minus infinity
(`((int64)a*b) >> 16`). It gave a different result for about 47% of opposite-sign pairs. CGDoom's `FixedDiv` never
computed the last quotient bit, and 34–37% of pairs differed. Both now follow linuxdoom-1.10 exactly (`src/engine/m_fixed.h`,
`m_fixed.c`). All rendering and game logic use them, so almost every frame changes. The new values are the vanilla ones.

**Checks:**
- `make test-fixed`: 51 M pairs on the host, edge values included, 0 mismatches against an int64 reference and against
  vanilla's `double` `FixedDiv2`.
- `make test-fixed-sim`: the same test in the SH simulator, where `FixedDiv` is the `div0u`/`div1` inline asm. 20 M
  pairs gave 0 mismatches. The default run uses 3 M pairs.
- An exhaustive host check over every `b` for `a` in {MININT, ±MAXINT, ±2^30, 12345678, −1} matched vanilla.
- `make test-sim-full` reproduces the new goldens in the simulator.
- `finecosine`/`SlopeDiv` were made inline in the same step. That is bit-exact by construction: the code is the same,
  only moved into `tables.h`.

## 2026-10-10 — step 2: new golden `e2m4-limits` (renderer limits, design B10)

**Changed:** no existing golden. Every step-2 change is meant to be bit-exact, and `make golden` rewrote all 14 existing
files byte-for-byte identical (checked with `diff -r` against a copy taken before). One golden was **added**:

| golden | frames | what it covers |
|---|---|---|
| `e2m4-limits` | 741 | Freedoom E2M4, `builtin:noclip`, 700 tics. Peaks of 251 visplanes, 547 drawsegs and 179 vissprites in one frame, against vanilla's static 128/256/128. 82 frames are over at least one vanilla limit. |

**Why a new golden:** the old engine cannot produce this run. Built from the pre-step-2 sources, it gives the same CRCs
for frames 0–389 and then:
- at frame 390 (tic 350, the first frame with more than 256 drawsegs: 306) it drops walls, so the CRC differs;
- in the next frame (135 visplanes) it stops with `R_FindPlane: no more visplanes`.

The new engine renders every wall and sprite from frame 390 on. Earlier frames are unchanged, which shows the growable
arrays and the visplane hashing reproduce vanilla wherever vanilla's limits hold.

**Variants that must reproduce goldens exactly** (added to `tests/scenarios.txt`):
- `e2m4-rsmall` (golden `e2m4-limits`) and `map01-rsmall` (golden `map01`) run with the host option `-rlimits small`.
  The renderer arrays start tiny: one visplane block of 64, 4 drawsegs, 4 vissprites, openings blocks of 3×320 shorts.
  So every frame goes through the growth paths: new visplane blocks, drawseg and vissprite doubling with pointer
  rebasing, and openings block changes.
- `e2m4-rsmall` also uses `-fragment 4096 -poison 0xa5`. Poison fills new zone blocks with garbage, which checks that
  the grown arrays are zero-filled as vanilla's static arrays were. `R_DrawPlanes` reads `bottom[]` of uncovered
  columns, so the zero fill matters.

## 2026-10-10 — step 3: ten new goldens for the optional modes (low detail, view size, potato floors, auto frameskip)

**Changed:** no existing golden. `make golden` rewrote all 14 earlier goldens byte-for-byte identical (checked with
`cmp` against a copy taken before). The default mode (high detail, view size 10, textured floors, fixed frameskip 0)
renders exactly as before. Step 3 adds optional modes; each new golden below covers one of them. These runs use the
same input scripts and demos as their default counterparts, and the game state at the end is identical (checked with
`-gamelog`): every mode changes only what is drawn and when.

| golden | frames | what it covers |
|---|---|---|
| `e1m1-low` | 941 | `-lowdetail`: `R_DrawColumnLow`/`R_DrawSpanLow` (Chocolate Doom semantics, `src/engine/sh4/test/ref_draw.c`), 160-column view. Comparable with `e1m1`. |
| `e3m4-low` | 641 | low detail, skill 5, more monsters and sprites |
| `demo1-low` | 2041 | low detail on the heaviest scenario (sprites, masked columns) |
| `map07-low` | 741 | low detail with `builtin:noclip`: view size 10→8→10, automap, menu, gamma, and `R_DrawFuzzColumnLow` |
| `e1m1-options` | 1241 | new input script `builtin:options`: menu Options > Detail switches to low detail and back (`M_ChangeDetail`); the view size keys go 10→7, then up to 11 (full screen, no status bar), then down to 9; automap and menu over a reduced view and over the full-screen view |
| `e1m1-view7` | 941 | `-viewsize 7` (224×112 view, black border) |
| `map01-flat` | 941 | `-flatfloors`: potato floors, solid-colour spans (`R_DrawSpanFlat`) |
| `e2m2-lowflat` | 641 | low detail + potato floors + view size 9, `builtin:noclip` (`R_DrawSpanFlatLow`, size changes) |
| `e1m1-auto` | 377 | `-autoframeskip 3 -rendercost 2.5`: automatic frameskip against the host's virtual clock, where each presented frame costs 2.5 tics |
| `demo2-1-auto` | 1381 | `-autoframeskip 2 -rendercost 1.6 -ticcost 0.2`, a demo with a level change and its wipe |

**New variants that must reproduce existing goldens** (`-slowwad`: `PL_WadIsSlow()` returns 1, so mappable lumps are
copied into the zone while it keeps its reserve, levels are precached, and purgeable lumps are dropped at level start):
- `e1m1-slow` (golden `e1m1`);
- `demo2-1-slow` (golden `demo2-1`, with a level change);
- `e1m1x-slowz` (golden `e1m1-exit`): a 2560 KiB zone with `-poison 0x33`, so the zone fills up, lumps get purged and
  copied again, and the in-place fallback is used.

**Default-mode changes that could have moved pixels but did not:**
- the vanilla drawer function pointers (`colfunc`/`spanfunc`/`transcolfunc`) replace CGDoom's direct calls;
- the reduced-view border is now redrawn after the menu, the automap, a help screen or a game-state change;
- `R_VideoErase` clears to black instead of copying from `screens[1]`.

The existing `builtin:noclip` scenarios already shrink the view to 8 and back. Their goldens did not change, because
the old border was black too and nothing drew over it during that stretch.

Every new golden was also reproduced bit-exactly by the ASan host and by the SH simulator (`make test-sim-full`, C-only
ELF).

## 2026-10-10 — demo sync: vanilla game logic where CGDoom deviated (DEMO1-3 of doom1.wad play in sync)

**Changed:** 21 of the 24 goldens; `e1m8-finale`, `map30-cast` and `title` are unchanged. One golden was **added**
(`e1m1x-auto`). The engine now plays id's v1.9 demos tic for tic like linuxdoom-1.10. Every old output with monsters
in it was non-vanilla. The fixes are listed in `notes/engine-changes.md`, "Demo sync".

| golden | frames (old → new) | first differing frame | cause (checked by reverting one fix at a time) |
|---|---|---|---|
| demo1, demo1-low | 2041 → 2041 | 84 | sound-pick `P_Random` calls |
| demo2-1 | 2746 → 2705 | 379 | sound-pick `P_Random`; Freedoom2 DEMO1 now plays in sync to its end: the player dies at tic 2602, so the old respawn and its wipe are gone |
| demo2-1-auto | 1381 → 1357 | 194 | as demo2-1 |
| e1m1, e1m1-low, e1m1-view7 | 941 → 941 | 131 | sound-pick `P_Random` |
| e1m1-exit | 921 → 921 | 131 | sound-pick `P_Random` |
| e1m1-options | 1241 → 1241 | 145 | sound-pick `P_Random` |
| e1m1-auto | 377 → 377 | 53 | sound-pick `P_Random` |
| e1m2 | 941 → 941 | 110 | sound-pick `P_Random` |
| e1m3 | 978 → 974 | 108 | sound-pick `P_Random` (the bot dies at another tic) |
| e2m2, e2m2-lowflat | 641 → 641 | 383, 387 | sound-pick `P_Random` |
| e2m4-limits | 741 → 741 | 207 | sound-pick `P_Random` |
| e3m4, e3m4-low | 641 → 641 | 127 | sound-pick `P_Random`, and nightmare demons and missiles (skill 5) |
| map01, map01-flat | 941 → 941 | 201 | sound-pick `P_Random`, and the super shotgun's refire in `A_CloseShotgun2` |
| map07, map07-low | 741 → 741 | 399 | sound-pick `P_Random` |

**Why the old output was not vanilla:**
- **Sound picks.** CGDoom removed sound, and with it three game-state `P_Random` calls: the random sight sound
  (`A_Look`), the active sound (`A_Chase`, every chase step of a monster that has one) and the death sound
  (`A_Scream`). It also zeroed every sound field in `info.c`. Every later random number was shifted.
- **`A_CloseShotgun2`** lost its `A_ReFire` with the sound, so a held fire button did not refire the super shotgun.
- **Nightmare** (skill 5) did not halve the demon states' tics or speed up the imp, cacodemon and baron missiles,
  because `states[]`/`mobjinfo[]` are const.
- **Not visible in any golden:**
  - `P_LookForPlayers` is restored to the vanilla loop. Reverting it alone changes no golden, but DEMO1 of doom1.wad
    desyncs at tic 67 without it.
  - the menu no longer pauses demo playback.
  - `-playdemo` stops at the demo marker without running one more tic.
  - `respawnparm`, and a player who is dead when a level loads is reborn.

**Why a new golden `e1m1x-auto`:** `demo2-1-auto` covered the automatic frameskip across a mid-run wipe, the respawn
after the old desynced death. That wipe is gone now. `e1m1x-auto` is `e1m1-exit` with `-autoframeskip 2 -rendercost 1.6
-ticcost 0.2`: a level exit, the intermission and the next level's wipe. Its game state (`-gamelog`) is identical to
`e1m1-exit`'s.

**Checks:**
- `make test-demosync` (with `CGD_DOOM1_WAD`): the per-tic traces of DEMO1-3 equal those of the linuxdoom-1.10 oracle
  on every tic.
- The single-player Freedoom demos (freedoom1 DEMO1, freedoom2 DEMO1 and DEMO3) match the oracle on every tic too.
- 22 bot demos recorded with this engine, from cheat-free `builtin:walk`/`builtin:random` runs, match the oracle. They
  cover doom1 E1M1-E1M9, freedoom1 E1M9, E2M3, E3M6, E4M2 and E4M5, and freedoom2 MAP01, 07, 11, 15, 20, 24, 29 and
  30, at skills 1-5.
- `make test-sim-full`, `make test-asan`: every golden is reproduced.
