# gint platform layer review (src/gint)

Review log for the fx-CG100 / fx-CG50 platform layer. None of this can run
here (no hardware, no emulator): the fixes are checked by `fxsdk build-cg`
(warning-free) and by the host unit tests in `tests/gint-host/`
(`sh tests/gint-host/run.sh`), which compile the platform sources against
stubbed gint headers.

## gint review round 1

17 findings were confirmed. 15 were fixed in `src/gint` and `CMakeLists.txt`.
Two are engine-side; this round adds only their platform half and a
workaround. Build: `fxsdk build-cg` passes without warnings. Host tests:
`gint-host: 8 passed, 0 failed`. A mutation check showed that the new
file-flush and UTLB-repair tests fail if those fixes are undone.

### 1+2. UTLB protection did not cover the OS-world window (tlb.c:12; medium/high, verdicts low/medium/medium)

**Finding.** `gint_world_switch_out()` installs the OS's VBR (drv_cpu
hrestore). gint's VBR comes back only in `gint_world_switch_in()`. Between
the two, a lot of our own P0 code runs and takes TLB misses through the
OS's handler, not our VBR+0x400 handler:

- `gint_call(call)`, which runs our os_* callbacks. For example
  `os_query_blocks` makes about 830 `Bfile_GetBlockAddress` calls.
- the on-chip memory restore.
- the canary check.
- the funbind loop.

The tlb.c comment claimed the handler covered this window. It did not.

**Fix.** The image pages are now pinned (tlb.c, tlbplan.c, tlbh.S):

- **Pinning at startup.** `gtlb_init` takes the snapshot and installs the
  handler as before. It then moves the UTLB entries of the N image pages
  (N = ceil(image size / 64 KiB), 5 today) into the N slots just below
  MMUCR.URB (49..53 with MPM's URB=54). It then lowers URB to 49. LDTLB
  with URC, used both by the OS's replacement and by our handler, only
  selects slots below URB, so the OS can no longer evict our code in
  either world. gint never touches MMUCR, so the OS runs with our URB.
- **The executor.** All UTLB writes go through a new assembly executor,
  `cgd_tlb_apply`, called through its P2 alias with interrupts masked:
  - It performs no TLB-translated fetch or data access.
  - Before each valid load, it invalidates every other valid entry of
    the same 64 KiB page. The UTLB therefore never holds two entries for
    one address (a multiple hit resets the CPU), even if the miss handler
    changed the UTLB while the plan was being made.
- **Refusal cases.** Pinning is refused, and nothing changes, when:
  - fewer than 16 replaceable slots would remain;
  - an image page is missing from the snapshot;
  - a target slot holds an image page.
- **Repair.** `gtlb_repair` (after every `gfs_os_call`) now plans with the
  same logic:
  - It moves pinned pages back to their slots.
  - It restores missing pages, iterating until stable.
  - It restores URB if the OS changed it.
  - It counts repairs, logs them through PL_Log, and shows them on the
    Diagnostics system page.
  - The old direct ldtlb path is kept as a fallback if the executor
    cannot be set up.
- **Exit.** The original URB is restored at exit (`atexit`).
- **Docs.** The tlb.c and tlbh.S comments are corrected. The design note
  still says that layer 1 protects `gint_world_switch` itself. That
  sentence should be corrected; design.md was out of scope for this
  round.

`CGD_TLB_PIN=0` disables pinning. Pinning still relies on the OS using
URC-based replacement; an OS that writes explicit slots is only covered by
the repair. The planned hardware probe still applies (critique §E item 5:
UTLB dump before and after the block-map switch, and the repair count in
Diagnostics).

**Tests.** `test_tlb.c` runs against a simulated MPM UTLB layout. A
step-by-step model of the executor checks after every single ldtlb that no
two valid entries overlap. The tests cover:

- the pinning layout;
- each refusal case;
- repair cases: a pinned page moved back, an unpinned reload left in
  place, a cascade where a reload sits in another page's slot, and a
  foreign 4 KiB entry overlapping our page;
- 200 fuzz rounds of random OS loads, explicit overwrites, handler reloads
  and repairs (pinned pages stay pinned, repair is idempotent, URC stays
  below URB);
- the MMUCR field math.

### 3. Unbounded release wait (input.c:53; low)

**Fix.** `ginp_wait_release()` gives up after 3 s (`GINP_RELEASE_TIMEOUT_US`),
or after 2000 polls if `PL_GetMicros()` does not advance. A new
`ginp_wait_any_key(timeout)` was added.

**Tests.** `test_input.c` covers: a clean release, a release after 1 s, a
stuck key (bounded at 3 s), and a frozen clock (poll cap).

### 4. Pre-main allocations could reach the OS heap (memory.c:115; low)

**Fix.**
- New `src/gint/no_osheap.c` defines `kmalloc_arena_osheap`, so the linker
  never pulls gint's arena_osheap.o. Checked in the map: the object is
  absent.
- On the fx-CG100 (`gint[HWCALC]`, valid from `hw_detect()` on, before
  kinit) the arena returns NULL and ignores frees. On the fx-CG50 it
  forwards to `__malloc/__realloc/__free`, because the same g3a runs on
  both models.
- `gmem_init` still removes "_os" on the fx-CG100.
- New link-time guard: `src/gint/uram_check.ld` is passed as an implicit
  linker script (CMakeLists.txt, with LINK_DEPENDS). It asserts
  `_euram <= 0x0814c000`, which keeps at least 192 KiB of _uram for
  kinit. It was verified to fail the link with a lowered threshold.
  `_euram` is currently 0x081139e0.

**Tests.** `test_memory.c`: the arena returns NULL with no OS call on
CG100, and forwards on CG50.

### 5. Pre-loadinfo mpm.bin builds were treated as dynload (memory.c:141; low)

**Fix.** These are builds up to a1a2cc3: no `_ld1`, user RAM at 0x8c780000,
code at a fixed 0x8c400000. They are now reported as "mpm.bin pre-Feb-2025"
(`GMPM_PRELOADINFO`) and get a second private arena "cgd_ram2" for
[code end, 0x8c700000) on top of [0x8c200000, 0x8c400000). The new arena
is in the zone and diagnostics lists, and the Diagnostics page shows it.
The 0x8c700000 ceiling is unchanged.

**Tests.** `test_memory.c` covers all five loader layouts:

| Layout | Result |
|---|---|
| pre-loadinfo | both ranges, 4.8 MiB total |
| dynload | unchanged, single range |
| Feb-2025 | unchanged |
| v1.1 | unchanged |
| huge image | no second range |

### 6 + 16b. WAD base name cut to 6 characters (main.c:662; medium)

**Fix.** `run_game` now passes the whole base name (new
`gnames_wad_base()`, src/gint/wadnames.c, buffer `GFS_LIST_NAME`). The
engine's `CGD_FilePrefix` hashes the whole name. The stale "6 characters"
comment is gone.

**Tests.** `test_names.c`, linked with the engine's cgdoom-names.c, checks
that 8 WADs give 8 distinct save names (freedoom1 FRE0P_0.DSG, freedoom2
FREXS_0.DSG, plutonia PLUFN_0.DSG, plutonia2 PLUIR_0.DSG).

### 7. Recreate path left a 0xFF-filled or half-written file (files.c:360; low)

**Fix.** In `os_flush`, a failed open or write after a successful
`create()` now removes the partial file. The old file is still removed
before `create()`. We kept remove-before-create on purpose, as the
verifier advised: create plus rename would need room for both files when
storage is nearly full.

**Tests.** `test_files.c` (the implementer's harness, moved from the
scratchpad) gained failure injection:
- a write failure after create: no file is left;
- a failure in the second 32 KiB chunk: no file is left;
- a create failure: reported by name;
- the in-place fast path still works.

### 8. PL_Fatal flushed with the WAD map live (main.c:781; low)

**Fix.** `PL_WadClose()` now runs before `CGD_FlushDelayedWrites()` in
`PL_Fatal`, as `run_game` does. Not host-testable, since main.c needs the
engine.

### 9. Strips DMA without synco (video.c:216; low)

**Fix.** `synco()` now follows each strip expansion, before
`dma_transfer_wait`/`dma_transfer_async`. The header comment was updated.
This is a hardware ordering issue and cannot be tested on the host.

### 10. PL_VideoReset blanked the game area (video.c:334; low)

**Fix.** `PL_VideoReset()` now calls `gvid_release()` and marks the
borders dirty. The next frame clears the borders and repaints the game
area, so save/load no longer shows a black flash. `gvid_clear_lcd()` is
kept for the launcher and diagnostics.

### 11. Controls editor accepted BACK/HOME/AC and the menu key (main.c:384; medium, verdict low)

**Fix.** New `gkm_rebind_check()` in keymaps.c:

- BACK/HOME on the CG100, or EXIT/MENU on the CG50, cancels the capture
  and keeps the old binding.
- AC/ON and the Doom menu key are refused with a message.
- The prompt names the cancel keys.

The engine now exports `CGD_EscapeKeycode`, so on the CG100 the menu key
is BACK and OK can be bound.

**Tests.** `test_keymaps.c` (CG100 and CG50 cases).

### 12. Save-name entry on the CG100 (engine cgdoom-kbd.c:251; medium, engine-side)

**Platform side (done).** `gkm_apply_engine_hooks()` sets a weak
`int CGD_AlphaKeycode` to ALPHA (0x72) on the CG100, once the engine
exports it. The CG50 keeps 0x71. **Tests:** test_keymaps built with and
without the symbol.

**Still open in the engine:**
- Export `int CGD_AlphaKeycode = KEYCODE_ALPHA;` (declared in
  cgdoom-kbd.h) and use it in `KeyDown()`.
- Drop the `(uint8_t)` casts in `I_StartTic` (i_system.c:300). They make
  digits and space dead on every model.
- The letter table still uses CG50 key positions. The CG100 alpha legends
  are not documented anywhere in ref/ or notes/, so no CG100 letter table
  was invented. This needs a hardware photo or keyboard legend check.

### 13. DEL-unbound keys came back after restart (engine cgdoom.c:199; low, engine-side)

**Workaround (done).** New `gkm_apply_unbound()` re-applies
`Keymap.<action>=NONE` lines after `CGD_LoadSettings()`. It is called from
`load_platform_settings`, only for the settings version the engine
accepts, and handles CRLF.

**Still open in the engine:** `CGD_LoadSettings` should accept the literal
NONE. The workaround is then redundant but harmless.

**Tests.** `test_keymaps.c` (NONE, CRLF, wrong version, unknown action,
"NONEX").

### 14. Stale `prev` after the game (input.c:150; low)

**Fix.** `ginp_wait_release()` now runs before `dev_info_after()`.
`ginp_reset()` is not needed, as the verifier noted: after a release every
scan is empty.

### 15. "See Diagnostics > Log next time" (main.c:746; low)

**Fix.** New `gfs_last_failure()` records the first failing 8.3 name and
its error code. The exit message now says "Could not write <name> (error
<n>)[ and other files]. Storage full?". **Tests:** test_files checks the
recorded name and code.

### 16. Demo replay picked the wrong IWAD (main.c:531; low, verdict medium)

**Fix.** New `gnames_demo_wad()` matches demos in this order:

1. The exact engine name `<CGD_FilePrefix(base)>D<nn>.LMP`, compared
   case-insensitively.
2. Otherwise, the longest WAD base name followed by a separator.
3. Otherwise -1. In that case the launcher uses the only WAD, or asks the
   user to pick one with the new `screen_pick`. It no longer falls back
   to WAD 0.

**Tests.** `test_names.c`:
- every WAD's demo slots map back to it, including lower case;
- DOOM1D03 goes to doom1.wad, not doom.wad;
- legacy names and unknown names.

### 17. No idle timeout (main.c:467; low)

**Fix.**
- `ginp_getkey()` calls `ginp_idle_hook` after 10 minutes without a UI
  key, then returns GK_NONE so that every caller redraws.
  `ui_message`/`ui_confirm` were reworked to redraw on GK_NONE.
- main.c's hook shows "Quitting to MPM in 60 s. Press any key to stay".
  If no key is pressed, it saves the settings and longjmps to the exit
  path, which flushes pending writes. The hook is disabled in the exit
  path and in PL_Fatal.

The fix does not dim the backlight:
- `r61525_brightness_set()` writes R61524 registers 0x5a1/0x5a2, which
  are not known to be valid on the fx-CG100's new LCD controller (video.c
  already avoids `r61524_win_set` for that reason).
- gint has no PowerOff syscall for OS 02.00.

**Not covered:** a paused game. It needs an engine "paused" signal, and
quitting a game would lose progress. **Tests:** test_input checks that the
hook fires after the idle time, and does not fire when a key arrives
first.

### Files

| Status | Files |
|---|---|
| Changed | src/gint/{tlb.c, tlbh.S, cgd_gint.h, memory.c, input.c, ui.c, main.c, files.c, video.c, keymaps.c, diag.c}, CMakeLists.txt |
| New | src/gint/{tlbplan.c, no_osheap.c, wadnames.c, uram_check.ld}, tests/gint-host/{run.sh, stub/, test_files.c, test_tlb.c, test_memory.c, test_input.c, test_keymaps.c, test_names.c, test_expand.c} |

Host-test hooks in production code are `#ifdef CGD_HOST_TEST`. They cover
the KIUDATA address in input.c and the image-size linker symbols in
memory.c.

## gint review round 2

12 findings were confirmed. All 12 are fixed in `src/gint`; `CMakeLists.txt`
did not need changes. Build: `fxsdk build-cg` passes with no warnings, and
`.dtors` holds `gtlb_quit` (checked in `build-cg/cgdoom.map`). Host tests:
`sh tests/gint-host/run.sh` reports `gint-host: 9 passed, 0 failed`; the new
suite is `wadmap`.

Mutation check: each of the following, applied alone, makes the new tests
fail:
- `gtlb_plan_unpin` becomes a no-op: 10238 failures.
- `gtlb_track_image` becomes a no-op: 52 failures.
- the EOF check in `PL_WadMap` is removed: 23 failures.
- `gfs_failure_text` ignores earlier failures: 2 failures.

The harness already lived in `tests/gint-host/` (round 1). This round adds
`test_wadmap.c` and extends `test_tlb.c`, `test_files.c`, `test_input.c`
and `test_keymaps.c`. `test_files.c` now `#include`s files.c with malloc
interposed.

### R2-1. Exit left pinned UTLB entries in slots 49..53 (tlb.c:161; critical, verdicts critical/high)

**Finding.** The public Feb-2025 mpm.bin (68e7f6e), and a1a2cc3 and 0fb6cec:
- map ROM pages only into slots 0..47;
- never set MMUCR.TI before a launch;
- never write slots 48..54.

At exit, `restore_urb()` only reset URB. Our pinned copies of pages 0..4
therefore survived in 49..53. On the next launch of any add-in, MPM loads
the same VPNs into slots 0..4. Two UTLB entries then match one address,
which is a multiple hit and a CPU reset.

The early exits (unsupported OS, not enough memory) had the same problem,
because pinning happens before them. The `abort()` paths also skip
`atexit`.

**Fix.** `gtlb_init` now saves each page's original slot in `g_slot_orig`.
The new destructor `gtlb_quit()` (`__attribute__((destructor))`, replacing
the `atexit(restore_urb)` call) runs `gtlb_plan_unpin()` and then restores
the original URB:
- `gtlb_plan_unpin()` (tlbplan.c) loads every snapshot page back into its
  original slot. The executor's dedup drops every other entry of that
  64 KiB page: the pinned copy, a copy the miss handler made, or a foreign
  overlapping entry. Pages already alone in their slot are skipped.
- gint's `start2` runs `.dtors` before `kquit` on both the `exit()` path
  and the `abort()`/`_Exit` path, so all exits are covered.
- It also runs when nothing was pinned. That cleans up miss-handler
  reloads into slots 48..53, the rarer unpinned variant of the same
  problem.
- Afterwards `g_slot` is reset to the MPM layout, so a later repair cannot
  re-pin anything.

Comments were corrected in tlb.c and tlbh.S: the "0..54, MPM remaps every
slot" claim is true only for v1.1 (which calls `invalidateTLB`) and dynload.

**Tests.** test_tlb.c now models three MPM layouts: v1.1 (TI flush and 55
pages), dynload (55 pages) and Feb-2025 (48 pages).
- `test_relaunch`, for each layout:
  - round 1's URB-only exit followed by MPM's next launch: duplicates under
    Feb-2025 only, as expected;
  - pin, OS activity (foreign entries, a 4 KiB overlap, a handler copy in
    slot 48), unpin, next launch: no duplicate VPN; every page alone in its
    original slot; URB 54; unpin is idempotent;
  - an unpinned run with a handler copy in slot 50: the next launch is
    clean.
- The fuzz test rotates the three layouts. Each of its 200 rounds ends with
  unpin, a normalisation check, and a replay of the next launch.

### R2-2. The repair counted MPM's pages beyond the image (tlbplan.c:145; medium, verdict low)

**Finding.** The snapshot holds about 50 non-image pages. The repair kept
rewriting their slots over the OS's fresh entries, so `repairs` and the
"UTLB: n repaired" log line were nonzero after nearly every world switch.
The counter therefore could not show whether our image pages were being
evicted.

**Fix.** The new `gtlb_track_image(slot, n)` restricts `slot[]` to the
image pages 0..n-1, whether or not pinning is on. With n=0 (image size
unknown), every page is still tracked. The `table[]` entries stay, so the
miss handler reloads the other pages on demand, and the unpin step uses
`g_slot_orig`. `repairs` now counts only image pages, plus URB resets.

**Tests.** In test_tlb.c:
- With n=5 pinned, an OS entry over page 30 is left alone (0 repairs) and
  so is a handler reload of page 30.
- The cascade and "unpinned page restored" cases now use an unpinned
  45-page image.
- A non-image page 48 is not restored.
- With n=0, every page is still repaired.
- `test_pin_basic` expects `slot[q] == -1` for q >= 5.

### R2-3/R2-5. The direct-LDTLB fallback (tlb.c:147; low, verdicts low/low)

**Finding.** Two problems when the executor is missing:
- The fallback did not de-duplicate, although the planner assumes it does.
  A foreign entry overlapping the page would become a multiple hit.
- It ran `ldtlb` from P0, where an ITLB refill between the URC write and
  `ldtlb` can move URC.

**Fix.** The fallback was removed:
- `apply()` only calls the P2 executor.
- `gtlb_repair()` and `gtlb_quit()` return when `g_apply` is NULL. The
  address-array invalidation needs P1/P2 code, which is the executor.
- Without the executor, only the miss handler writes the UTLB, which covers
  the gint world.

The static op buffer is gone. **Tests:** none (hardware path).

### R2-4. Startup error screens, PL_Fatal and the exit message had no idle timeout (main.c:883; low)

**Fix.**
- `ginp_idle_hook = idle_quit` is now set right after `g_exit_ready`. This
  covers the Unsupported OS and Not enough memory screens and the
  Experimental OS confirm.
- `save_settings_if_changed()` does nothing until `load_settings()` has
  run. This guards against the verifier's concern that defaults would be
  written over the user's file.
- `PL_Fatal`: `ginp_wait_release()` then `ginp_wait_any_key(GINP_IDLE_MS)`,
  then continue as if a key had been pressed.
- exit_path: the failure message uses the new
  `ui_message_timeout(..., GINP_IDLE_MS)` (ui.c).
- `ginp_wait_raw_key()` now takes a timeout and returns -1 on expiry. The
  controls editor's key capture uses `GINP_IDLE_MS`; -1 cancels the
  capture, and the controls screen's idle hook takes over.

**Tests.** test_input `test_wait_raw_key`: a key press is returned; it
times out after 60 s; it also times out with a frozen clock.

### R2-6. Failed no-memory writes were never shown (main.c:756; low)

**Finding.** PL_FileWrite's no-memory path flushes at once and leaves
nothing pending. exit_path only reported failures when something was still
pending, so these failures were never shown.

**Fix.** The new `gfs_failure_text(text, size, flush_result)` (files.c)
builds the exit message from every failure of the session:
- `g_flush_failures` and the last failure's name;
- or a generic text when the exit flush returned nonzero without counting,
  which happens when the map is live or the OS is unsupported.

exit_path shows it whenever it returns true. Nothing reports failures
earlier, so there is no double report.

**Tests.** test_files:
- `test_failure_text_clean`: nothing to report at first; the generic text
  for a refused flush.
- `test_nomem`: malloc fails above 64 KiB and `create()` fails. Checks:
  - `PL_FileWrite` returns -1;
  - nothing is pending;
  - the failure is counted and named;
  - the exit flush returns 0, and the exit text still names FRE0P_3.DSG;
  - the immediate write succeeds when storage has room.

### R2-7. Block-map lump at EOF exposed flash bytes (wad.c:791; low)

**Fix.** In GWAD_MAP mode, `PL_WadMap` declines any range whose end
reaches `W.size`. The engine clamped such a range, so the renderer may read
up to W_LUMP_PAD bytes past it. The engine then makes its zero-padded heap
copy.

Not covered on the platform side: a clamp at an engine `wad_end` below
`W.size`, meaning trailing bytes after the last lump and the directory.
The platform cannot see that clamp, and RAM mode behaves the same way.

**Engine follow-up (recommended, not done here).** In w_wad.c, call
`PL_WadMap` only when `padded >= size + W_LUMP_PAD`.

**Tests.** New `tests/gint-host/test_wadmap.c`, from the reviewer's
simulated-flash harness. It covers:
- adversarial `Bfile_GetBlockAddress` layouts (P0, P1, P2, NULL,
  unaligned, failing queries, fragmented);
- every range around block edges, plus a fuzz test; the fuzz now expects
  ranges that reach EOF to stay unmapped;
- an EOF lump: unmapped in MAP mode, and with zero padding in the RAM
  image;
- a range ending 1 byte before EOF, which still maps.

### R2-8. DMA display mislabelled when gvid_init() fails (main.c:873; low)

**Fix.**
- `main()` checks `gvid_init()` and logs a failure.
- `gvid_mode_name(GDISP_DMA)` returns "DMA n/a: CPU" when there is no
  frame buffer. This label appears in the launcher options and the
  developer info.
- The new `gvid_dma_available()` makes the benchmark skip the DMA push. The
  page then prints "DMA n/a" instead of a CPU time under the DMA label.

Not done: converting a P2 malloc result to P1. Mixing cached and uncached
aliases of heap memory needs care, and the fallback is now visible.
**Tests:** test_expand still builds video.c; no new logic to test.

### R2-9. The gint freeze-abort chord can never fire (input.c:12; medium, verdict low)

**Finding.** gint's `keydown()` reads keydev state that only advances when
events are dequeued. Nothing dequeues them, so the chord cannot fire.
Draining the queue would not help during a hang anyway, because no code
would run to drain it.

**Fix.** time.c's 35 Hz tic interrupt (now a custom `tic_callback`) checks
the same chord on a raw KIUDATA scan, through the new
`ginp_abort_chord()`, and calls `abort()`. That is what gint's keyboard
timer does too. It works even when the engine hangs, as long as interrupts
are enabled.
- SHIFT is (8,1) on the fx-CG50 and (7,1) on the fx-CG100. 7, 3 and AC/ON
  are 0x41, 0x23 and 0x07 on both models.
- `abort()` runs gint's `.dtors`, including the UTLB normalisation, and
  then `kquit`. Unsaved data is lost.
- AC/ON stays unbindable, so no preset can trigger the chord.

Comments were updated in input.c and keymaps.c. **Docs to update outside
src/gint:** research-critique.md:79 and §A4/§B9. The live chord is now
CGDoom's own, in time.c; gint's chord is inactive.

**Tests.** test_input `test_abort_chord`:
- all four keys on each model abort;
- any three of the four do not;
- the other model's SHIFT does not count;
- the chord is also detected through a simulated KIUDATA scan.

### R2-10. fx-CG50: EXIT could not be bound (keymaps.c:229; low)

**Fix.** On the fx-CG50 only MENU cancels a capture. MENU is the Doom menu
key and is never bindable. EXIT binds again, so the presets that use it
can be restored by hand. The prompt now reads "MENU: cancel". The fx-CG100
is unchanged: BACK and HOME cancel.

**Tests.** test_keymaps: "CG50 EXIT binds" (it used to expect cancel), and
MENU still cancels.

### R2-11. PL_WaitTics lost-wakeup window (time.c:103; low)

**Fix.** Interrupt-cancellable sleep, following gint's own pattern in
dma.c:
- `cpu_csleep_t` lives on the stack and is re-initialised for each
  iteration.
- It is published in `g_tic_ics`, and the condition is re-tested before
  `cpu_csleep`.
- `tic_callback` cancels it.

Without a tic timer, the function busy-waits on the micros-derived tics,
as before. **Tests:** none (needs gint's ICS code); the build is checked.

### R2-12. Benchmarks without a profiler timer (diag.c:93; low)

**Fix.** The new `gtime_prof_ok()`. When it is false, `run_bench()` returns
at once, and the benchmark page shows "Profiler timer unavailable: no
benchmarks". **Tests:** none (diagnostics UI).

### Files (round 2)

| Status | Files |
|---|---|
| Changed | src/gint/{tlb.c, tlbplan.c, tlbh.S, cgd_gint.h, main.c, files.c, wad.c, video.c, diag.c, time.c, input.c, keymaps.c, ui.c}, tests/gint-host/{run.sh, test_tlb.c, test_files.c, test_input.c, test_keymaps.c} |
| New | tests/gint-host/test_wadmap.c |

## gint review round 3.1

7 findings were confirmed and all 7 are fixed in `src/gint`;
`CMakeLists.txt` did not need changes. Build: `fxsdk build-cg` passes with
no warnings. `.dtors` now holds main.c's `platform_quit` (checked in
`build-cg/cgdoom.map`). The new P1 MMUCR writer sits inside the handler
block, which is 0x74 bytes (limit 0x200).

Host tests: `sh tests/gint-host/run.sh` reports `gint-host: 10 passed, 0
failed`. The new suite is `time` (`test_time.c` `#include`s time.c, with
gint's timer, profiler, clock and cancellable sleep stubbed). There are new
stubs `gint/{timer,prof,clock}.h`; `cpu.h` gained the csleep API, and the
stub `GINT_CALL` now accepts 0 or 1 arguments.

Mutation check: each of the following, applied alone, makes the tests fail
(number of failures in brackets):
- tic does not clear `g_tic_ics` (8);
- abort not one-shot (4);
- abort on the first chord tic (6);
- taking the request does not disarm (2);
- 32-bit fallback microseconds (2^32 us wrap) (3);
- idle counts "any key down" instead of a change (2);
- level-triggered `ginp_wait_any_key` (7);
- no quit poll in `ginp_getkey_timeout` (2);
- `gtlb_plan_contain_urb` never lowers (7).

### R3.1-1. No executor: handler reloads in 48..53 survived the exit (tlb.c:177; low)

**Finding.** When `setup_executor()` fails, the miss handler stays
installed, but `gtlb_repair`/`gtlb_quit` returned early. The handler
reloads at MMUCR.URC (0..53) and nothing can drop those copies at exit.
Under Feb-2025, a copy in 48..53 duplicates a VPN at the next launch,
which is the same reset as R2-1.

**Fix.** The handler is kept: gint's own handler would jump to the fx-CG50
syscall gate. Its reloads are contained instead:
- The handler block now also carries `cgd_tlbh_set_mmucr`, a 6-instruction
  P1 routine. It does an MMUCR store and then `icbi`, with no `ldtlb` and
  nothing translated. tlb.c calls its VBR-area copy with interrupts masked.
- When the executor is unavailable, `gtlb_init` lowers URB to
  `gtlb_plan_contain_urb(g_slot_orig, urb)` (tlbplan.c, pure). That is the
  count of leading slots that held snapshot pages at startup, which are the
  slots MPM's next launch rewrites:
  - Feb-2025: 48;
  - v1.1/dynload: 55, so nothing changes;
  - URB=0 is treated as 64;
  - nothing changes if fewer than `GTLB_MIN_REPLACEABLE` slots would remain.
- `gtlb_repair` without the executor puts URB back under the cap if the OS
  raised it, and counts that in `urb_fixes`.
- `gtlb_quit` does **not** restore URB in this mode, which deviates from
  the suggested fix. kquit still runs from our image with the handler
  installed, and a reload there must not land in 48..53 either. The cap is
  only ever set under the Feb-2025 family. In that family (loader.c of
  68e7f6e, a1a2cc3, 0fb6cec and e99a7d1), `MMU_SetEnabled(false)` after
  `run_addin` writes the whole MMUCR (URB 63), and `MMU_SetEnabled(true)`
  sets URB 54 before every launch.
- Diagnostics: new `gtlb_stats.executor` and `urb_cap` fields. The page
  shows "UTLB executor off: not pinned, URB 48 (was 54)", and the log
  records it.

**Tests.** `test_tlb` `test_contain`:
- the cap for each layout, for URB 0, and for an URB already inside;
- a gap in the leading slots, too few slots, and an empty snapshot;
- 40 random executor-less sessions per layout: OS loads and handler
  reloads at URC < URB, more reloads during kquit, then MPM's next launch.
  None of them has a duplicate.
- The same sessions with URB 54 under Feb-2025 reset in 39 of 40, which
  reproduces the finding.

### R3.1-2/3. The chord's abort() lost the session's saves; dangling `g_tic_ics` (time.c:49, :43; low/low)

**Fix.** The chord is now handled in two stages (time.c, main.c):
- **Latch only.** `tic_callback` sets a sticky request. The foreground
  takes it with `gtime_take_quit()` in `gquit_poll()` (main.c). That runs
  from `PL_GetTics()`, which the engine calls every tic, and from every
  wait loop in input.c (`ginp_getkey_timeout`, `ginp_wait_release`,
  `ginp_wait_any_key`, `ginp_wait_raw_key`).
- **Exit through exit_path.** `leave_to_exit_path(4)` does what PL_Fatal
  does after its message. In a game it calls `PL_WadClose()` and
  `CGD_FlushDelayedWrites()`; otherwise it calls
  `save_settings_if_changed()`. It then longjmps, and `exit_path()` writes
  everything.
- **Guards.** The new `g_in_exit` flag, `g_in_fatal`, `!g_exit_ready`, and a
  busy flag against re-entry.
- **Hang fallback.** `abort()` from the interrupt runs only when the chord
  stays held for `GTIME_ABORT_HOLD_TICS` (70 tics, 2 s) and nobody took the
  request. It fires once (`g_abort_armed`). Taking the request,
  `PL_Fatal()` and `exit_path()` disarm it, so it never cuts the exit's
  file writes; every wait on those paths is bounded.
- **No dangling sleep.** `tic_callback` clears `g_tic_ics` before
  cancelling it, so tics after an abort out of `PL_WaitTics` never write
  into the dead stack frame.
- **WAD handle on the abort path.** The new `gwad_quit()` (wad.c) closes
  `W.fd` through `gfs_os_call` and frees nothing else. `gtlb_quit()` is no
  longer a destructor itself. A single destructor, main.c's
  `platform_quit()`, calls `gwad_quit()` and then `gtlb_quit()`. The order
  is explicit because the world switch needs the pinning that `gtlb_quit`
  removes, and fxcg50.ld does not sort `.dtors` priorities.

Not changed: an abort that interrupts a `malloc`, which gint's own chord
shares (finding point 3).

**Tests.** `test_time`:
- the request is latched, sticky and taken once;
- three keys do nothing;
- once taken, holding the chord 10x longer never aborts;
- abort comes exactly at 70 tics and only once, even with the chord
  re-pressed;
- the hold count restarts on release;
- disarmed means no abort;
- `PL_WaitTics` wake-ups clear the published sleep, and `PL_GetTics` is a
  poll point;
- an abort from the tic during `PL_WaitTics` (simulated longjmp) leaves no
  dangling sleep, and 210 later tics neither cancel it nor abort again.

`test_input`: `ginp_getkey_timeout` polls.

### R3.1-4. Timerless PL_GetTics wrapped after 71.6 min (time.c:131; low)

**Fix.** The 32.32 accumulator is split into `g_us_total`, a 64-bit count
of whole microseconds, and `g_us_frac`, a 0.32 fraction. `PL_GetMicros`
still returns the low 32 bits. The fallback tics use all 64 bits. Also,
fallback `PL_WaitTics` returns at once when there is no clock at all (no
timer and no profiler); before, it waited forever.

**Tests.** `test_time`:
- 50 steps of 100 s (5000 s, past 2^32 us) give monotonic tics, ending at
  175000;
- the fallback wait works;
- with no clock, the wait returns.

### R3.1-5. No idle exit in a game (main.c:847; medium, verdict low)

**Fix.**
- `tic_callback` records the tic of the last **change** of the raw scan
  (`gtime_idle_tics`/`gtime_idle_reset`). A change, not "a key is down", so
  a pinned key cannot keep the unit on forever. This deviates from the
  verdict's "including a held key". A player holding one key unchanged for
  30 minutes only gets the warning.
- `gquit_poll()` (from `PL_GetTics`) runs `game_idle_check()` while
  `g_game_running`:
  - demo playback or the title loop (`demoplayback ||
    gamestate == GS_DEMOSCREEN`, nothing to lose): `GINP_IDLE_MS`
    (10 min);
  - real play: `GINP_GAME_IDLE_MS` (30 min); only explicit saves survive,
    as with Quit.
- It shows "Quitting to MPM in 60 s. Press any key to stay" and waits with
  the edge-triggered `ginp_wait_any_key`.
  - On a key: idle reset, `PL_VideoReset()` (the next frame repaints the
    game area and borders), then a release wait so the game never sees the
    key. That wait polls again, so a chord pressed here is taken before the
    2 s fallback.
  - Otherwise: `leave_to_exit_path(3)`.
- The stall is harmless to the engine: `singletics` loop, and
  `I_AutoPace` caps the lag. The idle clock starts at game start.

**Tests.** `test_time` `test_idle`:
- a press resets;
- a pinned key keeps counting;
- a release resets;
- no timer gives 0.

The game-side overlay is not host-tested (UI and engine state).

### R3.1-6. Idle exit lost the launcher's title-screen choice (main.c:855; low)

**Fix.** The new `launcher_commit()` sets `CGD_Options.AutoStart`/
`EnableDemos` from the title choice. It is called before every
`ginp_getkey()` of the launcher loop and after the loop. The idle exit and
the chord leave from inside `ginp_getkey()`, so they see current options.
**Tests:** none (launcher UI).

### R3.1-7. Level-triggered any-key waits (input.c:103; low)

**Fix.** `ginp_wait_any_key()` is edge-triggered. It takes a snapshot on
entry and returns true only for a bit set that is not in the baseline;
released keys leave the baseline, so pressing them again counts. The
effects:
- `idle_quit()` and the in-game warning really quit under a pinned key;
- `PL_Fatal` is not dismissed by a key still held after the 3 s release
  wait.

**Tests.** `test_input`:
- a pinned key does not count and times out after 60 s;
- a second key while one is held counts;
- releasing a held key and pressing it again counts;
- a still-held key does not dismiss PL_Fatal;
- `ginp_getkey` with a pinned EXE reaches the hook, and the hook's wait
  returns false.

### Docs outside src/gint to update (not edited here)

- integration-todo.md and §17 above: "a paused game needs an engine paused
  signal" no longer applies.
- research-critique.md §A4/§B9: the chord now keeps saves and closes the
  WAD. abort() is only the 2 s hang fallback.

### Files (round 3.1)

| Status | Files |
|---|---|
| Changed | src/gint/{tlb.c, tlbh.S, tlbplan.c, cgd_gint.h, diag.c, time.c, input.c, main.c, wad.c}, tests/gint-host/{run.sh, test_tlb.c, test_input.c, stub/gint/cpu.h, stub/gint/gint.h} |
| New | tests/gint-host/{test_time.c, stub/gint/timer.h, stub/gint/prof.h, stub/gint/clock.h} |

## gint review round 3.2

5 confirmed findings, all fixed in `src/gint`. Two findings were the same
bug (demo lost on the chord and on the in-game idle exit), so there are 4
fixes. `CMakeLists.txt` did not need changes. Build: `fxsdk build-cg`
passes with no warnings. Host tests: `sh tests/gint-host/run.sh` reports
`gint-host: 10 passed, 0 failed`, with new regression tests in `input` and
`wadmap`.

Mutation check: the round-1 `ginp_getkey()` (a per-call timeout) fails 4
of the new `input` checks: pinned arrow, its timing, non-UI activity, and
the hook after the capture timeout. Removing either WAD poll fails the
`wadmap` poll checks.

### R3.2-1. The 2 s abort() fallback fired during slow loads (time.c:86; medium, verdict low)

**Fix.** Every engine WAD read and map is a safe point. `PL_WadRead()` and
`PL_WadMap()` call the new `gquit_poll_load()` (main.c) on entry, before
they touch anything. In a game it takes a pending chord request exactly
like `gquit_poll()` and goes to `leave_to_exit_path(4)`. It never runs the
idle exit, so it never draws in the middle of a frame or a load. It does
nothing outside a game, so `PL_WadOpen()` construction is never cut. It
also does nothing on the exit and fatal paths, or while another poll is
busy. Leaving from there is as safe as leaving from `PL_GetTics`: the
engine state is dropped either way, the zone is consistent (a
`W_CacheLumpNum` `Z_Malloc` has returned), and `end_game_session()` closes
the WAD before anything is written. This covers R_Init and the sprite
scan, P_SetupLevel with R_PrecacheLevel, and G_DoLoadGame, because they
read lumps all along.

Not done, with reasons:
- **"No poll for N tics" condition.** A poll that runs while the request is
  latched always takes it, unless its guards block it. The guards are the
  exit path and PL_Fatal (abort already disarmed), `g_poll_busy` (the idle
  warning, which returns on the first key edge of the chord) and
  `!g_exit_ready` (nothing to save yet). A poll heartbeat would add a
  mechanism and close no case.
- **Longer hold.** Not needed.

`PL_WadOpen()` is not a poll point. Its long parts (RAM image copy, block
queries) are single world switches, and the tic timer does not run while
the OS does (gint swaps TMU state), so the hold cannot accumulate there.

The time.c header now states the remaining abort() cases: a hang, or a
phase that neither reads the WAD nor asks for the time for 2 s of gint
time.

**Tests.** `test_wadmap` case 9:
- `PL_WadOpen` never polls;
- a read, a map and a bad map each poll once;
- a poll that longjmps (a taken request) leaves before anything is read,
  with the statistics untouched and the WAD state intact.

### R3.2-2. Chord and in-game idle exit dropped a demo being recorded (main.c:889/888; low, verdicts low/low)

**Fix.** The new `end_game_session()` (main.c) is the one way out of a
game. It is used by the normal return in `run_game()`, by `PL_Fatal()`, and
by `leave_to_exit_path()` for the chord and the idle exit. It:
1. drops the WAD map;
2. calls `G_CheckDemoStatus()` when `demorecording && !demoplayback &&
   !timingdemo`, once (a static guard, like I_Error's `in_error`);
3. calls `CGD_FlushDelayedWrites()`;
4. queues the settings (R3.2-3).

`G_CheckDemoStatus()` is the only place a recording becomes a delayed save,
so it must run before step 3. It must also run after `PL_WadClose()`: with
`DelaySaves` off it writes at once, and `PL_FileWrite` refuses its
fallback while the map is live. It is skipped for a timedemo, which ends
in `I_Error`, and during playback, where it would start the next demo. If
it fails, the failure goes to I_Error and then PL_Fatal, where
`g_game_running` is already false and the guard holds. On the I_Error path
`demorecording` is already false, and the call is still there for any
future direct `PL_Fatal` caller (there is none today). **Tests:** none, the
code is coupled to the engine.

### R3.2-3. In-game option changes were never saved (main.c:755; medium, verdict low)

**Fix.** `end_game_session()` ends with `save_settings_if_changed()`, so
every exit from a game now queues the settings: the normal return,
PL_Fatal, the chord and the idle exit. The `memcmp` against
`g_saved_engine_opts` sees the in-game ViewSize, LowDetail and FlatFloors
changes. The write stays pending until `exit_path()`'s `gfs_flush()`. On
PL_Fatal it is best effort: a `CGD_malloc` failure goes to I_Error and then
re-enters PL_Fatal, which leaves at once through `longjmp(2)`. The delayed
saves are queued before it, and `exit_path` still writes them.
engine-changes.md (about line 732) said "saving the options after the game
keeps them". That is now true.

### R3.2-4. A held arrow defeated the launcher idle timeout (input.c:272; low)

**Fix.** `ginp_getkey()` now measures idleness as "no change of the keyboard
state". That is time.c's tic scan (`gtime_idle_tics`), the same measure as
the in-game idle exit, and it replaces the per-call timeout that every
returned key restarted.
- It waits with `ginp_getkey_timeout(remaining)`. On `GK_NONE` it
  re-checks, because a non-UI key or a release may have restarted the idle
  time.
- An auto-repeating pinned arrow is not activity: the hook runs
  `GINP_IDLE_MS` after the press. Its wait is edge-triggered (R3.1-7), so
  the add-in really quits.
- After the hook returns, `gtime_idle_reset()` starts a fresh period, so a
  hook that returns at once does not run on every call.
- Without a tic timer (new `gtime_ticking()`), the old per-call timeout
  applies.

The secondary slip is fixed by the same change. Time spent in other waits
counts, so after the controls capture times out (`ginp_wait_raw_key`, 10
min), the hook runs at the next `ginp_getkey()`, as the screen_controls
comment says, instead of 10 minutes later. Auto-repeat itself is unchanged
(no cap), so holding an arrow to scroll is not affected.

**Tests.** `test_input` `test_idle_activity`, with a tic-scan stub:
- a pinned UP auto-repeats more than 5000 times, and the hook still comes
  600 s after the press (within 0.1 s);
- after the hook, the next call returns a repeat without a second hook;
- a non-UI key pressed and released is activity: the hook comes 600 s
  after the release;
- after a capture timeout, the hook runs at once;
- with no timer, the hook comes after `GINP_IDLE_MS`.

### Docs outside src/gint to update (not edited here)

- research-critique.md §A4/§B9 and the R3.1-2 text: the abort() fallback
  is now reached only by a real hang, or by a phase that neither reads the
  WAD nor asks for the time. The chord keeps a demo being recorded and the
  in-game settings.

### Files (round 3.2)

| Status | Files |
|---|---|
| Changed | src/gint/{main.c, input.c, time.c, wad.c, cgd_gint.h}, tests/gint-host/{test_input.c, test_wadmap.c, test_files.c} |

## gint review round 3.3

5 confirmed findings. Three were the same bug, a demo recording finished
before it had begun, so there are 3 fixes. All are in `src/gint`, and
`CMakeLists.txt` did not need changes. Build: `fxsdk build-cg` passes with
no warnings. Host tests: `sh tests/gint-host/run.sh` reports
`gint-host: 10 passed, 0 failed`, with new regression tests in `files` and
`wadmap`.

Mutation check: without the new size cap in `PL_FileWrite`, `files`
crashes in `test_demo_exit` (the ~1.9e9-byte copy).

### R3.3-1. end_game_session() finished a demo recording that had not begun (main.c:679; medium x3)

The cause: `G_RecordDemo()` sets `demorecording` (d_main.c, before the
title or the first `G_DeferedInitNew`). `demo_p` is first set by
`G_BeginRecording()`, at the end of `G_DoNewGame()`, after the level load.
Before that, `demo_p` is NULL (bss: the engine runs once per add-in
session, main() leaves after one game). R3.2-2's `G_CheckDemoStatus()` call
then stored `DEMOMARKER` through NULL and set the size to
`demo_p - demobuffer`, about 1.9e9. With no memory for a copy, `os_flush()`
removed the slot's previous demo and then failed in `create()`. The window
was reachable in two ways:
- with autostart, the chord during the first level load (R3.2-1's WAD
  polls);
- without autostart, the chord or the 10-minute idle exit on the title.

After a title demo, `G_DoPlayDemo()` had replaced `demobuffer`/`demo_p`
with the demo lump. "Recording" then wrote into the lump and saved it under
the user's slot.

**Fix, in three layers:**
1. **Begun check.** `end_game_session()` calls `G_CheckDemoStatus()` only
   when `gdemo_begun(demobuffer, demo_p, demoend)` holds (cgd_gint.h: all
   non-NULL, `demobuffer < demo_p <= demoend`). An armed recording that has
   not begun is dropped: there is nothing to save. `demorecording = false`
   then also keeps I_Error() out of it if the flush that follows fails. The
   zone, and the buffer with it, goes with the next `Z_Init`.
2. **Recording implies a direct start.** This removes the stale-pointer
   case, which no pointer check can tell apart. `launcher_normalize()`
   forces Title = Skip, and so no title demos, whenever a record slot is
   set. Choosing a slot selects Skip, and moving Title off Skip clears the
   slot. `run_game()` sets `autostart = AutoStart || recording`, as
   vanilla's `-record` does. It also never records during a replay
   (`CGD_RecordDemoSlot = -1` when `L->demo >= 0`): `G_DoPlayDemo()` would
   replace the buffer. Recording then only runs from a direct start. The
   only "not begun" window left is the first level load, and the begun
   check covers it.
3. **Size cap.** `PL_FileWrite()` refuses `size > GFS_WRITE_MAX` (16 MiB)
   before touching the file. This covers the engine's own unguarded
   callers, which src/gint cannot change: `I_Error()` during the first
   level load (i_system.c:138) and, in principle, `D_DoomLoop`'s final
   `G_CheckDemoStatus()`. The NULL store itself remains there, but the
   slot's previous demo survives. Every legitimate write comes from RAM and
   is far smaller: settings, saves, and demos of at most 128 KiB.

The suggested engine flag (`G_BeginRecording` sets it, `G_CheckDemoStatus`
checks it) is the complete fix for the engine paths. It is outside
src/gint and is listed below.

*Later (renderer polish, 2026-10-10):* the engine flag is in
(`g_game.c` `demorecbegun`, tests/demo-unbegun.sh), so layer 1 was
removed as redundant: `end_game_session()` now calls `G_CheckDemoStatus()`
directly, which drops an unbegun recording itself; `gdemo_begun()` and its
checks in `test_demo_exit` are gone (only the "Demo recording had not
begun" log line is lost). Layers 2 and 3 stay.

**Tests.** `test_files` `test_demo_exit`:
- `gdemo_begun` for NULL / no header / header / full / past the end /
  nothing armed;
- the engine's bogus size is refused on the copy path and on the
  no-memory path, nothing is queued, no failure is counted, and the
  slot's demo keeps its size;
- a full 128 KiB demo is still written.

The launcher and `end_game_session()` are in main.c, which is not host
built.

### R3.3-2. Auto frameskip ran with N=1 instead of 3 (main.c:758; medium)

`CGD_AutoFrameskip = (g_opt.frameskip < 0) ? CGD_AUTOFRAMESKIP_DEFAULT : 0`.
Before, it was the boolean 1, so the engine skipped at most one frame in a
row and slowed the game after one tic of lag. The weak declaration and
the "engine without auto frameskip" note are gone: the engine defines the
symbol (cgdoom.c), and `CGD_AUTOFRAMESKIP_DEFAULT` comes from cgdoom.h. The
Options item reads "< Auto (max 3) >", which matches the in-game frameskip
key's "Frameskip: auto (max 3)".

### R3.3-3. PL_WadIsSlow() was missing (wad.c:757; low)

`PL_WadIsSlow()` returns 1 for `GWAD_MAP` and `GWAD_BFILE`, and 0 for the
RAM image or with no WAD, as platform.h specifies. It is stable between
open and close: `gwad_remap()` only turns MAP into BFILE. The engine reads
it once in `W_InitMultipleFiles`, after `run_game()`'s `PL_WadOpen()`. The
gain is in MAP mode: hot lumps are copied out of NOR flash and each level
is precached. In BFILE mode, which already copies every lump, it only
enables the level-start purge and the bounded precache.

**Tests.** `test_wadmap` case 10 checks the value with no WAD, in MAP, in
MAP after the fallback to BFILE, after close, in forced BFILE, and with
the RAM image.

### Outside src/gint to update (not edited here)

- Engine: add a "recording begun" flag. `G_BeginRecording()` sets it.
  `G_CheckDemoStatus()` requires it for the recording branch: without it,
  the branch clears `demorecording` and writes nothing. That closes the
  NULL store for `I_Error()` during the first level load (i_system.c:138),
  which is still possible, and for `D_DoomLoop`'s final call (d_main.c:309).
  Separately, `G_WriteDemoTiccmd` still writes 4 bytes after its own
  ESC-triggered `G_CheckDemoStatus()`. That is vanilla behaviour, and the
  bytes stay inside the buffer.
- integration-todo.md: tick "use CGD_AUTOFRAMESKIP_DEFAULT" (line 38) and
  "PL_WadIsSlow" (line 43).
- User docs: recording a demo now starts the game directly (no title
  screen), as vanilla `-record` does.

### Files (round 3.3)

| Status | Files |
|---|---|
| Changed | src/gint/{main.c, files.c, wad.c, cgd_gint.h}, tests/gint-host/{test_files.c, test_wadmap.c} |

## Integration follow-ups (after the engine follow-ups and the SH4 drawers)

Items of notes/integration-todo.md ("gint platform follow-ups" and the two gint video items). Build: `fxsdk
build-cg` warning-free, CGDoom.g3a 361,656 bytes. Host tests: `sh tests/gint-host/run.sh` reports `gint-host: 9
passed, 0 failed` (the keymaps-hooks build was merged into keymaps).

### Video: persistent pal32 table, no ocbwb loop (video.c, cgd_gint.h)
- `PL_SetPalette()` builds a static 1 KiB `g_pal32` with `cgd_expand_build_pal32()` (layout of cgd_sh4.h:
  `pal32[k] = pal[k ^ 0x80] * 0x10001`). It replaces the 512-byte RGB565 copy (net +512 bytes of .bss).
- DMA and strip paths call `cgd_expand_p8_rgb565_p32()`; the CPU path reads the same table (`PAL565`). The plain
  entry built the table on its stack at every call (~870 modelled cycles, 1 KiB stack; 20 calls a frame in strip
  mode, ≈0.15 ms/frame at 118 MHz per src/engine/sh4/README.md).
- Weak C fallbacks of both functions stay in video.c (same contract, write nothing unless n is a positive multiple
  of 16). The g3a links the strong SH4 versions (map: `r_draw_sh4_c.c` build_pal32, `expand_sh4.S` _p32; the weak
  sections are discarded). The old weak `cgd_expand_p8_rgb565` is gone (unused; the asm one remains).
- ocbwb loop removed after verifying that every P1 line is `movca.l`-allocated and `ocbp`-purged, including the
  last line (`.Lxp1_last` ends with `ocbp @r4`), in expand_sh4.S / gen_expand.py, and in the weak C. present_dma
  keeps one `synco()` before `r61524_start_frame()` + DMA; strips keep their per-strip `synco()`.
- `_p32` has no `ldrs`/`ldre`/`ldrc` (only `dt`/`bf` loops), no DSP or MAC registers: interrupt-safe. It runs in
  the foreground today.
- Test: test_expand (table layout, whole frame, 20 strips, palette change, invalid counts write nothing);
  mutation-checked (wrong low-half index, missing count check).

### Idle: CGD_IsPaused() (main.c, time.c)
- `gtime_game_idle_limit_ms(paused, usergame, title, &lose)` (pure, time.c): nothing to lose → `GINP_IDLE_MS`
  (10 min): a demo (`CGD_IDLE_DEMO`), a title page (`GS_DEMOSCREEN`), or no level outside a user game
  (`CGD_IDLE_NOLEVEL && !usergame`). Otherwise (playing, automap, paused, menu or save-name entry, intermission or
  finale of the player's game) → `GINP_GAME_IDLE_MS` (30 min), and the warning says the game is not saved.
- `game_idle_check()` (from `gquit_poll()` ← PL_GetTics, foreground only) reads the engine state only once the
  shorter limit has passed. The engine keeps calling PL_GetTics while paused or in the menu, so every state is
  covered. Quitting goes through `leave_to_exit_path(3)` → `end_game_session()` (delayed saves, demo, settings) →
  `exit_path()`. A paused game is not saved automatically (no slot is ours); it offers to quit with 60 s to stay.
- `GIDLE_*` mirror `CGD_IDLE_*` (static assert in main.c). Test: test_time `test_idle_policy`.

### Keymaps
- `gkm_apply_unbound()` removed (engine `CGD_LoadSettings()` accepts `Keymap.<action>=NONE`, covered by
  tests/settings_test.c). `CGD_AlphaKeycode` is a plain reference (engine defines it); the `-Wl,-U` flag and the
  WITH_HOOKS double build are gone; test_keymaps checks both hooks on both models.

### Launcher renderer options (main.c Options screen)
- View size (3..11, wraps; 10 "Status bar", 11 "Full screen"; values outside 3..11 shown and cycled as 10, as the
  engine reads them), Detail (High/Low), Flat floors. Stored in CGD_Options, saved in CGDOOM.CFG by the engine.
- In-game changes (view size keys, detail menu → `CGD_Set*` update CGD_Options) are queued by
  `end_game_session()` → `save_settings_if_changed()` on every exit: normal quit, PL_Fatal, chord, in-game idle
  exit. Not on the 2 s `abort()` hang fallback (by design).

### Verified, no change
- WAD base name: `gnames_wad_base()` keeps everything before the first '.', in a `GFS_LIST_NAME` (40) buffer, the
  longest name the listing accepts. Demo → WAD: exact `CGD_FilePrefix(base) + "D<nn>.LMP"` (case-insensitive),
  then the legacy separator rule; test_names covers every WAD's slots 0/33/66/99.
- `PL_WadIsSlow()`: 1 for MAP and BFILE, 0 for the RAM image / no WAD. w_wad.c reads it once in
  `W_InitMultipleFiles` (after run_game's PL_WadOpen); `gwad_remap()` only turns MAP into BFILE, so it is constant
  between open and close, as platform.h requires.
- `CGD_AutoFrameskip = CGD_AUTOFRAMESKIP_DEFAULT` for Auto (round 3.3).

### Open
- The CPU display path still does one table lookup per pixel straight to the LCD register (bus-bound, ~8 ms).
- sh4 README / cgd_sh4.h still say "the caller's ocbwb + synco is still fine" (true, just no longer done).

## gint follow-ups review (adversarial review of "Integration follow-ups")

No confirmed defects; nothing changed in src/gint or tests/gint-host. Checked:
- Video: the linked `cgd_expand_build_pal32` (r_draw_sh4_c.c, disassembled) builds `pal[k ^ 0x80] * 0x10001`; `_p32`
  (`xtrct` of two table words) puts the left pixel in the high half; `PAL565` (CPU path) takes the low half of
  `pal32[b ^ 0x80]` = `pal[b]`. The map shows video.c's weak `.text.cgd_expand_*` discarded and the ELF's
  PL_FinishUpdate has `synco` before the DMA start and no `ocb*`. Every P1 line is `movca.l`/`ocbp` in expand_sh4.S
  and in gen_expand.py (last line included), so dropping the ocbwb loop is safe; the CPU fallback after a failed DMA
  start re-reads purged lines from RAM. The weak C compiled for SH keeps all seven stores between `movca.l` and
  `ocbp`. Strip banks (0xe5...) take the plain-store path. test_expand and test_time also pass under
  ASan+UBSan at -O0/-O2/-O3.
- Idle policy against the real engine: a scratch host build printed (CGD_IsPaused, usergame, GS_DEMOSCREEN) on every
  change. Title page 4/0/1 and demo 8/0/0 → 10 min; before the first level 4/0/0 → 10 min; level 0/1/0 and
  intermission 4/1/0 → 30 min. The only !usergame state with mask 0 is the one tic between a demo's end and
  D_DoAdvanceDemo, where the policy errs long (30 min). G_RecordDemo clears usergame but G_InitNew sets it again
  before the level.
- Launcher options: the view-size cycling wraps 3..11 both ways; `R_Init` reads out-of-range values as 10, as
  shown. `CGD_Set*` are the only engine writers of CGD_Options, and every exit path but the abort() fallback
  reaches `save_settings_if_changed()`.
- Notes: the four TLB layers in design.md §7 match tlb.c; the §A4 chord positions (SHIFT (7,1)/(8,1)) match
  input.c; §8 is consistent with main.c, wadnames.c and wad.c.
- Gates: gint-host 9/0, fxsdk build-cg warning-free (361,656 bytes), sh4 test 63966/6507/600 PASS, check-sh,
  test-unit, test-host 35/0, test-asan 35/0.
- Still open (outside src/gint): the sh4 README (§"cache_writeback() … redundant", and line 80) and cgd_sh4.h:71 still
  describe the caller's ocbwb+synco as current.
