# Final whole-system review: confirmed findings, fixes and tests (2026-10-10)

Eighteen confirmed findings from the whole-system review, with the
severities from the verdicts. One TLB finding got a split verdict (one
reviewer said "not a defect", the other "medium"). It was handled as the
optional hardening that both reviewers named. Each entry gives the
finding, the fix and the regression test. Paths are relative to the
repository root.

Goldens: no golden changed (host 35/35, ASan 35/35, sim-full 35/35), so
tests/golden/CHANGES.md has no new entry.

## 1. DelaySaves=0: a refused save was reported as "game saved." (low)

- **Finding.** `G_DoSaveGame` dropped the `PL_FileWrite` result. On gint,
  the write is refused when the private-copy malloc fails while the WAD
  block map is live. That refusal (files.c) did not count as a failure,
  so the exit screen was silent too. `G_CheckDemoStatus` also dropped
  `CGD_WriteDelayedFile`'s result.
- **Fix.**
  - g_game.c: when the write returns <0, the player sees
    `GGSAVEFAILED` ("save failed: file not written!") instead of "game
    saved.", and `PL_Log` records `G_DoSaveGame: <name> not written`. A
    failed immediate demo write is logged in the same way.
  - files.c: the refusal calls `note_failure(name, GFS_ERR_NO_MEMORY)`,
    so `gfs_failure_text()` names the file at exit ("no memory to keep
    it"). The advice it gives is "Set DelaySaves=1 in CGDOOM.CFG".
- **Tests.**
  - Host: new `-nodelaysaves` option. tests/save-entry.sh saves with
    `DelaySaves=0` into a read-only `-data` directory (expects the "not
    written" log and no file) and into a writable one (expects the file
    and no failure).
  - gint-host test_files `test_refusal_counted`: a block map is live
    (P0 block pointers) and malloc fails, so the write is refused. The
    test checks that the refusal is counted, named with
    `GFS_ERR_NO_MEMORY`, and that the exit text says "NOT SAVED",
    "savegame" and "lost".

## 2. fx-CG50 without experimental RAM cannot run doom1.wad, but the dialog said "may" (medium → low)

- **Fix.** main.c `run_game` now estimates the zone the way
  `PL_GetZones` builds it: experimental RAM, plus the heap above the
  320 KiB CG50 reserve.
  - Below `CGD_CG50_MIN_GAME_ZONE` (700 KiB: E1M6 peaks at 637 KiB in
    the simulator), the "Not enough memory" dialog says Doom will stop
    with a Z_Malloc error, and gives the remedy for this calculator:
    - OS 3.70 or later: there is no experimental RAM, so Doom cannot run.
    - Option off: enable 'Use experimental RAM'.
    - Option on but nothing found: reset the calculator (see 4).
  - "Start anyway?" stays available. The numbers come from the
    simulator, and a smaller WAD may fit.
  - Between that limit and `CGD_MIN_TOTAL_HEAP`, the milder "may run out
    of memory" dialog remains.
- **Docs.** design.md §7 now says that the CG50 bonus target needs
  experimental RAM for doom1.wad.
- **Tests.** None. The dialog is UI-only, and the arithmetic matches
  `PL_GetZones` (memory.c).

## 3. The 136-byte pad after every RAM-image segment duplicated ~150 KB (low)

- **Fix.** wad.c `seg_chained()`: a segment that is file-adjacent to the
  next one, with a size that is a multiple of 4, is placed directly
  before that segment. It gets no pad, no alignment gap and no memcpy.
  Its pad bytes are then the next segment's true bytes, and the last
  segment of each chained run keeps a real pad.
  - The total, the final offsets, the memset/pad-read list and the
    pad-copy pass all apply the same rule.
  - Lookups are unchanged (one table entry per segment, `seg_find`).
- **Measured** with gint-host test_files on
  ref/extra/GBADoom/GbaWadUtil/doom1.wad:

  | WAD | Before | After |
  |---|---|---|
  | unstripped | 3,547,872 B (3464 KiB) | 3320 KiB |
  | wadtool-packed | 3,545,856 B (3462 KiB) | 3318 KiB |

  Both save about 144 KiB.
- **Tests.**
  - gint-host test_files `test_packed_image`: a synthetic PWAD whose
    lump sizes mix multiples of 4 with other sizes. The image size must
    equal the rule's model and be smaller than the unchained one. Every
    `PL_WadRead` at every offset (lengths 0..300) and every
    `PL_WadMap(lump, size+136)` must return the file's bytes, 4-aligned.
  - The freedoom RAM-image checks, which cover content and pad for every
    lump, still pass.

## 4. fx-CG50 experimental RAM was not zeroed at exit, so the next launch found none (medium → low)

- **Fix.** memory.c:
  - `PL_GetZones` marks the probed range as handed out.
  - New `gmem_release_extra_ram()` drops the P1 cache lines of that
    range (ocbi), then zeroes it through the uncached P2 alias that the
    probe reads. It runs once.
  - main.c calls it in `exit_path()` after the flush, and in the
    `platform_quit()` destructor, which covers abort().
  - run_game's dialog: with the option on and nothing found, it says
    "reset the calculator" instead of "enable the option" (see 2).
- **Tests.** gint-host test_memory `test_extra_ram`, using a host buffer
  as the P2 view:
  - the probe finds 512 KiB;
  - the release before handing out is a no-op;
  - `PL_GetZones` returns the 0x8c200000 zone;
  - after the engine dirties it, the release zeroes exactly the zone
    and keeps the OS data after it;
  - the next probe finds 512 KiB again;
  - the release is idempotent;
  - dirty RAM is not found (the old behaviour);
  - no probe when the option is not allowed.

## 5. The cgdoom.h exit contract had the WAD closed after the delayed writes (low)

- **Fix.** cgdoom.h now lists `PL_WadClose()` and then
  `CGD_FlushDelayedWrites()`, and gives the reason: a storage write
  invalidates a flash mapping, and the platform may have to refuse a
  write it cannot copy.
- host_main.c follows the same order.
- The main.c header comment now describes the destructor:
  `gwad_quit`, the experimental-RAM release and `gtlb_quit` all run
  before kquit, on the abort() path too.
- **Tests.** The host save and demo tests (saveentry, roundtrip,
  unbegun) and the goldens pass with the new host order.

## 6. Start map or episode the WAD does not have; Doom II MAP33+ killed the add-in (medium → low)

- **Fix.**
  - The launcher caps the episode at 1..4 (the engine has no more).
    Maps still go up to 99, because PWADs can go past MAP32.
  - On a direct start, after `PL_WadOpen`, `run_game` asks the new
    wad.c `gwad_has_lump()` (a case-insensitive directory scan through
    `PL_WadRead`, in 128-entry chunks) for the lump the engine will
    load. That is `MAPnn` if the WAD has MAP01, otherwise `EeMm`, using
    the same rule as d_main.c's gamemode detection.
  - If the lump is missing, the WAD is closed and a message names the
    map and the valid range. The player returns to the launcher instead
    of getting the fatal screen and an exit to MPM, or a silent clamp.
- **Tests.** gint-host test_files `test_wad` checks `gwad_has_lump()` on
  the freedoom WADs:
  - the first and last directory names are found, in either case;
  - ZZZZZZZZ, MAP33 and E5M1 are not found;
  - exactly one of MAP01 and E1M1 is present.

## 7. "press y or n." while Y and N cannot be pressed (medium → low)

- **Fix.** d_englsh.h `PRESSYN` is now "press exe to confirm.", like
  DOSY. This covers ENDGAME, NIGHTMARE, QSPROMPT and QLPROMPT.
- **Tests.** Goldens unchanged (no scenario shows these prompts).

## 8. The fx-CG100 OK key did nothing in the Doom menu (low)

- **Fix.**
  - Engine hook `int CGD_EnterKeycode2 = -1` (cgdoom-kbd.c/.h). In
    `KeyDown(KEY_ENTER)`, that key also counts, unless a keymap entry
    uses it.
  - gint `gkm_enter_key2()` returns OK (0x84) on the fx-CG100 when BACK
    is the menu key. `gkm_apply_engine_hooks()` sets the hook through a
    weak reference.
  - The Controls note says "Enter: EXE or OK (if unbound)".
- **Tests.** gint-host test_keymaps `test_enter`:
  - fx-CG100: EXE and OK are Enter, BACK and no key are not, OK bound to
    Fire is not Enter, and EXE still is;
  - fx-CG50: no second Enter key (0x84 is MENU there).

## 9. The per-launch Skip rewrote the saved "Title screen"; the Skill row was dead (low)

- **Fix.** main.c `launch_t` now has three fields:
  - `title`: this launch;
  - `title_pref`: the saved setting, which only the Title row changes;
  - `start_set`: an episode or map was chosen.

  How they are used:
  - `launcher_commit()` saves `title_pref`.
  - `run_game` puts this launch's choice into `CGD_Options` for the
    engine, and `end_game_session()` restores the saved setting before
    the settings are queued.
  - Setting the record slot back to None restores `title_pref`, unless a
    start map was chosen.
  - Notes under the Title row: "Skill: used only with Skip (the Doom
    menu asks)", or "Skip for this game only (saved: ...)".
- **Tests.** None. This is launcher UI state, and no gint-host harness
  covers main.c.

## 10. WAD and fatal-error messages were not actionable (low)

- **Fix.**
  - `PL_WadOpen` returns a distinct code for each cause:
    `GWAD_ERR_FS/NAME/OPEN/SHORT/MAGIC/TRUNCATED`. TRUNCATED is new: the
    directory lies past the end of the file, the usual sign of an
    incomplete copy. `gwad_error_text()` gives one message per code, and
    run_game shows it.
  - The no-WAD note now says to connect to a PC over USB, copy a WAD to
    the top folder, and optionally run `tools/wadtool.py pack` on the PC
    first.
  - New src/gint/hints.c `gfatal_hint()`. `PL_Fatal` shows one recovery
    line under the engine text:
    - Z_Malloc: model-specific memory advice;
    - "Demo is from" or M_ReadFile of a .LMP: delete or re-record the
      demo;
    - W_GetNumForName: the WAD is incomplete or not playable.
- **Tests.**
  - gint-host test_files `test_wad_errors`: short, bad-magic, truncated,
    missing and over-long names each return their own code, and the six
    texts are distinct and non-empty.
  - New tests/gint-host/test_hints.c checks the engine's exact strings
    and that other errors get no hint.
  - test_wadmap's synthetic files now carry a valid empty directory.

## 11. Failed delayed writes were reported without saying what was lost (low)

- **Fix.** `gfs_failure_text()` now says, for example: "NOT SAVED:
  savegame FRE0P_3.DSG and other files (error -4, storage full?). They
  are lost. Free some storage space before playing again."
  - The file kind (savegame, demo or settings) comes from the extension.
  - The no-memory case gives the DelaySaves advice instead (see 1).
  - The generic text also says the files are lost.
  - exit_path's buffer is now 256 bytes.
- No free-space check and no DelaySaves option were added: there is no
  safe BFile free-space call, and writes cannot be made immediate while
  the map is live (see the verdict).
- **Tests.** The test_files nomem and refusal tests check the new text.
  `test_failure_text_clean` still finds "Some files".

## 12. Demo picker: 0 KiB sizes, no WAD shown, silent list caps, silent overwrite (low)

- **Fix.**
  - main.c `size_text()` shows "N B" under 1 KiB, KiB under 1 MB, and
    "N.NM" from 1 MB up. It is used by the picker and by the WAD rows.
  - The replay picker shows each demo's WAD (`gnames_demo_wad`) and its
    size.
  - `gfs_list` sets `gfs_list_more` when a list stops at its maximum
    with entries left. The launcher and the picker then show "More ...
    exist: only N listed".
  - Recording on a slot that already holds a demo for this WAD
    (`CGD_DemoFileName` + `PL_FileSize`, checked before the WAD load)
    asks "Record over it?".
- **Tests.** gint-host test_files `test_list_more`: a list cut one entry
  short is reported, and a list that is exactly full or not full is not.

## 13 and 17. Diagnostics could not show the storage, UTLB and timer probes (low)

- **Fix.** diag.c:
  - Storage page:
    - With no WAD opened, it says so.
    - EXE/OK runs a read-only probe of a WAD from the launcher list
      (UP/DOWN picks it). The probe forces block-map mode, without the
      RAM image and without `gwad_force_bfile`. It takes a UTLB snapshot
      before and after `PL_WadOpen`, then calls `PL_WadClose`. Nothing
      is written.
    - The page then shows the open result or error text, the block-map
      statistics (and "map did not match" if 16 rejected the map), the
      number of changed UTLB slots with their slot list, and URB before
      and after (critique §E items 4 and 5).
  - Memory page: says the zones are set up when a game starts.
  - System page: adds "Tic timer: TMU n (35 Hz)" or "none (polled
    clock)", plus "SAFE START" (see 15).
  - New helpers: tlb.c `gtlb_read_view()` and time.c
    `gtime_tic_timer()`. `gdiag_screen()` now takes the WAD list.
- **Tests.** None. These are UI pages. The probe reuses
  `PL_WadOpen`/`PL_WadClose`, which test_wadmap and test_files cover.

## 14. Controls editor: duplicate bindings, and a reserved-key text naming the wrong key (low)

- **Fix.**
  - keymaps.c `gkm_find_binding()`. When a key is rebound, main.c clears
    the older binding of the same key and says "<key> was bound to
    <action>, which is now unbound".
  - The reserved-menu text now names the actual menu key
    (`gkm_key_name(code)`). In a real build that branch cannot fire,
    because the menu key is a cancel key on both models (comment
    added).
- **Tests.** gint-host test_keymaps `test_bindings`:
  - no preset binds a key twice (both models);
  - `gkm_find_binding` finds the other entry, skips the edited entry,
    and ignores unbound and free keys;
  - the menu key cancels on both models.

## 15. UTLB pinning had no runtime opt-out (high → split: not a defect / medium)

- Handled as the hardening that both verdicts named, not as a design
  change. Pinning stays the default, and before the first world switch,
  because reading a setting from CGDOOM.CFG is itself a world switch.
- **Fix: safe start.**
  - DEL (0x44, the same matrix position on both models, and not a launch
    key) held when the add-in starts is read from a raw KEYSC scan
    (input.c `ginp_safe_start()`), right after `gsys_init` and before
    `gmem_init`/`gtlb_init`.
  - `gtlb_init(true)` keeps the miss handler but does not set up the P2
    executor. It then takes the existing no-executor path: no pinning,
    no ldtlb, contained URB.
  - main.c also skips the private stack and runs the OS clock for that
    session (the setting is kept), and shows a "Safe start" message.
  - Diagnostics shows "SAFE START".
  - `CGD_TLB_PIN=0` still exists for builds.
- **Tests.** gint-host test_input `test_safe_start`: DEL held means safe
  start on both models; no key, or EXE/OK held, does not. The
  no-executor TLB path is already covered by test_tlb (contain).

## 16. The block map was accepted at open without checking it maps the file (medium → low)

- **Fix.** wad.c `map_verify()` runs right after `build_map()` in
  `PL_WadOpen`:
  - Block 0, if mapped, must hold the cached header.
  - The middle block, the last block and the directory's block, if
    mapped, must match a BFile_Read of their first 64 bytes. This is
    read-only, so the map stays valid.
  - On a mismatch: `map_free()`, BFile mode, `gwad_stats.map_rejected`,
    and a log line.
- **Tests.** gint-host test_wadmap case 11:
  - a corrupted flash copy of the header block, the middle block, the
    last block or the directory block each means BFile mode,
    `map_rejected`, the true bytes from `PL_WadRead`, and no map;
  - the intact layout keeps the map.

## 18. fx-CG50 was offered the CG100-tuned Ptune4 F5 (low)

- **Fix.**
  - clock.c `gclk_levels()`: 3 levels on the fx-CG100, 2 on the
    fx-CG50. `gclk_apply()` runs a level the model does not offer as F4,
    which covers a stored setting and the benchmark.
  - load_settings resets a stored F5 to F4 on the fx-CG50.
  - Options cycles through `gclk_levels()`.
  - The F5 warning now speaks of the fx-CG100 only.
- **Tests.** None. These are register writes and UI.

## Gates (all run after the fixes)

| Gate | Result |
|---|---|
| `make test-host` | 35/35 |
| `make test-asan` | 35/35, plus saveentry (with the new DelaySaves=0 checks) and unbegun |
| `make test-unit` | fixed, div, names, roundtrip, settings, saveentry, unbegun: PASS |
| `make check-sh` | all engine files compile |
| `make -C src/engine/sh4/test run` | PASS |
| `sh tests/gint-host/run.sh` | 10/10 (new: hints) |
| `make test-sim` | 3/3 |
| `make test-sim-full` | 35/35 |
| `fxsdk build-cg` | warning-free |
