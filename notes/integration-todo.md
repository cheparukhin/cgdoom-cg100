# Integration TODO (tracked by the orchestrator)

## Engine-side items raised by the gint review (src/engine, after engine-perf-c finishes)
- [x] Export/use weak-referenced `int CGD_AlphaKeycode` (gint sets ALPHA = 0x72 on fx-CG100) for save-name text entry.
- [x] Digits and space untypable in save-name entry: `(uint8_t)` casts in `I_StartTic` (i_system.c ~300).
- [x] fx-CG100 letters: no documented alpha legends → prefill empty save slots with the map name (e.g. "E1M3")
      so EXE saves without typing; keep typing for those who can.
- [x] `CGD_LoadSettings` must accept `Keymap.<action>=NONE` (gint has a workaround that re-applies it).
- [x] w_wad.c: reference a lump in place only if the full `size + W_LUMP_PAD` range is mappable (not clamped at EOF);
      otherwise copy with zero-filled pad (uniform across platforms; gint already refuses EOF-reaching ranges).
- [x] Optional: expose a "paused/idle" signal so the platform can apply its idle timeout in-game.
      (Done: `int CGD_IsPaused(void)` in cgdoom.h; fx-CG100 letters: the CG50 matrix table matches the
      CG100 manual, so no per-model table; see engine-changes.md "Follow-ups".)
- [x] `CGD_EscapeKeycode` added by engine step 2; `CGD_AutoFrameskip` added by step 3 (value = max consecutive skips).

## Integration of SH4 drawers (after sh4-asm-review + engine-perf-c)
- [x] Wire *_SH4 drawers via colfunc/spanfunc/... pointers under `__sh__` (single marked place), call
      `cgd_sh4_set_colormaps()` in R_InitColormaps, add sh4 sources to sim build (cgdoom.mk) — CMake already globs them.
      (Done: `CGD_SH4` in r_draw.h (__sh__ && !CGD_NO_SH4), R_ExecuteSetViewSize picks the 8 *_SH4 drawers (potato
      floors stay C), R_InitColormaps registers a 32-byte-aligned PU_STATIC rotated copy; engine-changes.md "SH4 integration".)
- [x] test-sim-full must match host goldens bit-exactly with SH4 drawers; perf-sim before/after.
      (Done: test-sim 3/3, test-sim-full 35/35 in both section layouts; 24 mode runs identical to the C-only ELF;
      sim insts/frame −15..−29%, modelled drawer cycles 2.3–3.5× faster: tests/perf-baseline.md "After the SH4 drawers".)
- [x] Loop schedules (after sh4-asm-review finishes; exact search in scratchpad/msched/bounds_neg.py):
      R_DrawSpanLow has a II=12 schedule (3.0 c/px) vs shipped II=13 (3.25) under modsched's own model → implement
      it, keep bit-exact. R_DrawSpan: modsched's model says min II=12 but README claims 11 (5.5 c/px, scheduled
      by hand/pipesim) → reconcile models, fix whichever is wrong. col/collow are at the resource floor (no gain).
      Replace modsched.py's naive backtracking with the constraint-propagation search (solves all kernels in ms).
      Done (sh4 round 3, notes/sh4-review.md): modsched.py is the exact search (+ --px, --shipped, --stage1, "max"
      latencies, `make -C src/engine/sh4/test sched`); SpanLow loop now II=12 = 3.0 c/px under pess/opt/gcc,
      bit-exact; Span: the model was wrong (WAR pairs given latency 1), the shipped 11 is the floor.
      Reviewed (sh4-review.md "SH4 loop schedules review"): asm OK; fixed modsched --stage1 exactness + listing order.
- [x] Builds: add `-Wa,--dsp` and `-MMD -MP` to src/sim/cgdoom.mk's `.S` rule and define `CGD_SH4_DSP_MNEMONICS` in
      BOTH sim and CMake builds (readable mnemonics; raw-opcode LDRS/LDRE/LDRC macros stay as fallback, asmcheck keeps
      them byte-identical). sh4 review: notes/sh4-review.md (translated-column .bss overwrite fixed; 1-px fast path).
      (Done: sim `.S` rule + `-DCGD_SH4_NO_ILRAM` default, CGSIM_SH4=0 / CGSIM_ILRAM=1 variants; CMake sets the define on
      the .S files (fxSDK passes -m4-nofpu -mb -Wa,--dsp); g3a .ilram 1552 B of 4 KiB (drawers 1352, gint 176).)
- [x] Engine: "recording has begun" flag (gint-review r3.3 Open): G_BeginRecording sets it, G_Ticker/G_CheckDemoStatus
      require it (title, D_DoomLoop's final call, I_Error in the first level load); recording buffer kept apart from
      title demos; no store after an ESC-ended recording. (Done: tests/demo-unbegun.sh in test-unit and test-asan.)
      Wiring review: a BEGUN recording also wrote into a played title demo (End Game, demos on) -> G_DoPlayDemo
      now ends it first (demo-unbegun.sh case "endgame"); the ESC/MENU end of a recording is unreachable
      (M_Responder eats KEY_ESCAPE). engine-changes.md "Wiring/integration review".
- [x] gint video: drop the redundant per-line `ocbwb` loop after expansion (both the asm and the weak C expansion
      already `movca.l`+`ocbp` each P1 line); keep a final `synco` before starting DMA.
      (Done: verified in expand_sh4.S/gen_expand.py (ocbp per line incl. the last) and the weak C; present_dma now
      expands then `synco()`; gint-review.md "Integration follow-ups".)
- [x] gint video: persistent pal32 table built in PL_SetPalette with `cgd_expand_build_pal32`, use
      `cgd_expand_p8_rgb565_p32` (avoid 1 KiB stack table rebuild per call).
      (Done: DMA + strips use `_p32`, CPU path reads the same table; weak C fallbacks of both in video.c; the g3a
      links the SH4 strong versions; `_p32` has no repeat loops.)

## gint platform follow-ups
- [x] gint: use `CGD_IsPaused()` (engine, mask: 1 paused, 2 menu/save entry, 4 no level, 8 demo) in the in-game idle
      check; drop the now-redundant `gkm_apply_unbound()` workaround and the `-Wl,-U,_CGD_AlphaKeycode` flag in
      tests/gint-host/run.sh (engine defines CGD_AlphaKeycode now; tests/gint-host/test_keymaps.c no longer defines it).
      (Done: `gtime_game_idle_limit_ms()` (host-tested): demos/title 10 min, the player's game incl. paused/menu 30 min,
      exit via the normal exit path; workaround, its test, WITH_HOOKS build and the -U flag removed.)
- [x] Verify tests/gint-host/run.sh still passes after gint-review-r3 (engine-followups touched test_keymaps.c).
      (Done: `gint-host: 9 passed, 0 failed` — 9, not 10: the keymaps-hooks build is merged into keymaps.)
- [x] `CGD_AutoFrameskip` value: gint sets `(g_opt.frameskip < 0)` = 1 → use `CGD_AUTOFRAMESKIP_DEFAULT` (3).
      (Done in gint review round 3.3; verified in run_game().)
- [x] Launcher options for new engine settings: LowDetail, FlatFloors, ViewSize (already saved in CGDOOM.CFG).
      (Done: Options screen; in-game changes are saved on every exit path but the 2 s abort() hang fallback.)
- [x] Verify WAD base name passed untruncated and demo list matches `<P>D<nn>.LMP` via `CGD_FilePrefix()`.
      (Verified: gnames_wad_base keeps everything up to the first '.', buffer 40 = longest listed name;
      gnames_demo_wad matches CGD_FilePrefix + "D<nn>.LMP"; test_names covers both.)
- [x] sim build: src/sim/cgdoom.mk `.S` rule must assemble src/engine/sh4/*.S (DSP isa / -Wa,--dsp, -DCGD_SH4_NO_ILRAM),
      then re-run test-sim / test-sim-full on the DEFAULT ELF (steps 1-3 only verified a C-only ELF).
      (Done: default ELF = SH4 drawers; test-sim 3/3, test-sim-full 35/35; src/sim/README.md §9.)
- [x] Implement `PL_WadIsSlow()` (1 in block-map/BFile modes, 0 in RAM-image mode) once engine step 3 adds it.
      (Round 3.3; semantics checked vs w_wad.c: read once in W_InitMultipleFiles after PL_WadOpen, constant per open.)
- [x] Notes corrections: design.md §7 TLB coverage wording; research-critique.md §A4/§B9 abort chord (now implemented
      by our tic interrupt).
      (Done; also design.md §8 stale 6-char/PL_WadIsSlow/weak-frameskip wording and research-critique.md §C7.)

## Renderer polish (2026-10-10)
- [x] r_segs.c: skip `R_GetColumn` + `colfunc` for empty mid-texture columns (`yh < yl`, 26% of column calls in
      e1m1); top/bottom tiers were already guarded. Bit-exact. (Done; also the lighting/`dc_iscale` division only
      before the first drawn tier. engine-changes.md "Renderer polish".)
- [x] SH4 drawers' count < 0 path: no call site passes count < 0 any more (0 C-fallback calls in all sim profiles),
      so the asm is left unchanged rather than lengthening every call's set-up (sh4 README §7.4).
- [x] r_things.c `R_DrawMaskedColumn`: loop state in locals (bit-exact; −21% of its instructions in demo1).
- [x] Docs drift: sh4 README (sim `.S` flags, `CGD_SH4` from r_draw.h, presenter `synco` only, §7 as built),
      cgd_sh4.h:71, sh4_asm.h and sh4 test/Makefile comments.
- [x] gint: drop the redundant `gdemo_begun()` check in `end_game_session()` (engine `demorecbegun` covers it);
      helper, externs and its gint-host checks removed; gint-host 9/9.
- [x] perf-sim before/after (default + low detail): tests/perf-baseline.md "After the renderer polish"
      (−0.7% to −5.8% insts/frame, all CRCs ok). Full gate list green.

## Final
- [ ] Whole-system review (cross-cutting: engine↔platform contracts, memory budget, startup/exit sequence).
- [ ] README: install MPM + add-in, wadtool pack, controls (CG100 + CG50), options, diagnostics, troubleshooting,
      dev loop (fxlink -pw build-cg/cgdoom.bin), building from source, credits/licenses.
- [x] Final build + size check + full test suite. (2026-10-10, clean rebuild of all build dirs: test-host 35/35,
      test-asan 35/35, test-unit, check-sh, sh4 test 6507+600 cases 0 failures, gint-host 10/10, test-sim 3/3,
      test-sim-full 35/35, fxsdk build-cg warning-free. CGDoom.g3a 373,252 B; .ilram 1,552 B of 4,096;
      _euram 0x08113e90 vs uram_check limit 0x0814c000 = 224 KiB margin.)
- [x] Release bundle: dist/ (CGDoom.g3a, cgdoom.bin, tools/wadtool.py, README.md, LICENSE, CREDITS.md, SHA256SUMS; sums verified).
- [ ] Still open, needs a human/hardware: whole-system review; README final content check (a README.md exists, 26 KB,
      shipped as is); on-calculator verification (MPM + CG100/CG50: speed, keys, WAD load) -- never run on hardware here.

## Later
- [ ] Restore OS 02.10 once MPM's installer supports it: recipe and the open `BFile_Write` address in
      notes/design.md §9.
