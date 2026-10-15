# src/sim: run big freestanding SH4 programs under the GDB SH simulator

`sh-elf-run` (GDB 15.2 SH simulator) executes ELF programs linked with this directory's runtime:
`crt0.s` + `syscalls.c` + `libc-glue.c` + `sim.ld`, on top of fxlibc (`libc.a`), `libopenlibm.a` and
`libgcc.a` from `toolchain/`. Programs get `argc/argv`, `stdout`/`stderr`/`stdin`, host file I/O
(`fopen`, `fread`, `fseek`, `fwrite`, ...), `malloc`/`realloc`/`free`/`calloc`/`memalign`, `exit()` codes
and instruction/cycle statistics. Everything is GPLv2-compatible plain C/asm; nothing here is calculator code.

```sh
make -C src/sim sim-tool         # (once, 5 s) patched simulator build-sim/tools/sh-elf-run64, see section 6
make -C src/sim sim-selftest     # build + run the self test on freedoom1.wad: must print SELFTEST PASS
make -C src/sim sim-bench        # build + run the micro-benchmark example (exact insts/cycles per call)
src/sim/simrun --stats prog.elf args...      # run any ELF, then print SIMSTATS insts=.. cycles=..
```

`export PATH=$PWD/toolchain/bin:$PATH` is **not** needed: the makefile fragment
and the scripts use absolute tool paths.

## 1. Using it from the root Makefile

The root Makefile includes `sim.mk` after its own default goal (the fragment preserves it), then
`cgdoom.mk`, the client that builds CGDoom (§9). `cgdoom.mk` is the reference for writing another one:
it compiles its objects into `$(SIM_BUILD)` (with its own flags, or with `$(call sim-cc,<src>,<obj>)`
for plain `$(SIM_CFLAGS)`), links them with `$(call sim-link,<elf>,<objs>)`, which adds the runtime
objects and libraries and writes `<elf>.map`, and runs the ELF through `$(SIM_RUNNER)`. The rules for
`make test-fixed-sim` / `make test-div-sim` in the root Makefile are smaller examples of the same pattern.

Variables defined by `sim.mk` (set `SIM_BUILD` etc. *before* the include; append with `+=` after it):

| variable | default / meaning |
|---|---|
| `SIM_BUILD` | `<root>/build-sim` (the root Makefile sets `build-sim$(VSUFFIX)` first: one directory per screen format, §9); runtime objects go to `$(SIM_BUILD)/sim/` |
| `SIM_CC`, `SIM_AS`, `SIM_AR`, `SIM_OBJDUMP`, `SIM_SIZE`, `SIM_RUN` | `toolchain/bin/sh-elf-*` (absolute) |
| `SIM_CFLAGS` | `-m4-nofpu -mb -O2 -std=gnu11 -fno-strict-aliasing -DPL_SIM -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -I src/sim` |
| `SIM_ARCH`, `SIM_OPT` | `-m4-nofpu -mb`, `-O2`. `-m4a-nofpu` works too (the simulator decodes SH4A opcodes) |
| `SIM_ASFLAGS` | `-big -isa=sh4al-dsp` (also required for `.s` files that use DSP opcodes, see 5) |
| `SIM_LDFLAGS` | `$(SIM_ARCH) -nostdlib -nostartfiles -T src/sim/sim.ld -Wl,--gc-sections ...` |
| `SIM_LIBS` | `-Wl,--start-group -lc -lopenlibm -lgcc -Wl,--end-group` |
| `SIM_RUNTIME_OBJS` | `crt0.o syscalls.o libc-glue.o` (always linked, listed first, built by `sim.mk`) |
| `SIM_RAM_END`, `SIM_STACK_SIZE` | optional hex, e.g. `SIM_RAM_END=0x00800000` emulates an 8 MiB machine |
| `SIM_RUNNER`, `SIM_BENCH`, `SIM_PROF` | the `simrun`, `simbench`, `simprof` scripts |
| `sim-cc`, `sim-as`, `sim-link` | `$(call ...)` recipe helpers (see above) |

Targets: `sim-runtime`, `sim-tool`, `sim-selftest`, `sim-selftest-stock`, `sim-bench`, `sim-clean`.
`make -C src/sim` alone runs `sim-selftest`.

Engine code that prints floating point with `printf` must call `__printf_enable_fp()` (`<fxlibc/printf.h>`)
once, exactly as on the calculator. fxlibc has no `<strings.h>`: `strcasecmp`/`strncasecmp` are in `<string.h>`.

## 2. Memory map (sim.ld)

The simulator has one flat RAM at address 0: **2^30 bytes (1 GiB) by default**, allocated lazily with
`mmap` (a program argument `N` in 1..31 would select 2^N bytes, see the digit-argument pitfall in 3). There is no MMU, no cache and no P1/P2/P3 aliasing. Everything below the RAM size is plain memory,
everything above raises a bus error. The linker script uses the first 64 MiB:

```
0x00000000-0x00000fff  unused gap; SIM64 simulator: any access here is a bus error (NULL guard)
0x00001000             .text      _start (crt0), code, .pretext, GILRAM (".ilram") functions; 32-byte aligned
                       .rodata
                       .init_array (constructors; fxlibc needs them for stdin/stdout/stderr)
                       .data      (+ GXRAM/GYRAM ".xyram" objects, plain RAM here)  32-byte aligned
                       .bss       (NOLOAD, NOT cleared by crt0: simulator RAM starts zeroed) 32-byte aligned
___bss_end/_end        == ___heap_start (C: __heap_start)
                       malloc arena: [__heap_start, __heap_end)   ~63 MiB with the defaults
0x03f00000             __heap_end == __stack_limit
0x03f00000-0x03ffffff  main stack, 1 MiB, grows down from __sim_stack_top = 0x04000000 (no guard page)
```

(The toolchain prefixes C symbols with `_`, so the linker script spells the C symbol `__heap_start` as
`___heap_start`; assembler code uses the same prefixed names.) Selftest example: `.text` 49 KiB,
`.bss` 93 KiB, heap start 0x28a40. To mimic the calculator's smaller heap pass `SIM_RAM_END=...` or
simply allocate your own zone from `malloc`.

The program image is loaded with the ELF's LMA = VMA; execution starts at `_start` (0x1000).
**All ELFs are tagged `sh4al-dsp`** (crt0 is assembled with `-isa=sh4al-dsp`, and the libraries carry
that tag too, so the linked ELF gets it): this makes the simulator accept SH4AL-DSP opcodes
(`setrc`/`ldrs`/`ldre`/`ldrc` repeat loops, `padd`, `movs`, ...). GCC `-m4-nofpu`/`-m4a-nofpu` code never
uses the opcode range DSP mode takes over from the FPU.

## 3. Start-up, exit, arguments

`crt0.s` loads `sp`, jumps to `__sim_start()` (syscalls.c), which runs the `.init_array` constructors,
fetches `argc/argv` with the simulator's `argc`/`argnlen`/`argn` syscalls (max 64 args, 16 KiB), calls
`main(argc, argv)` and `exit()`s with its result. `exit()` runs the `atexit` handlers, flushes
`stdout`/`stderr` (fxlibc's `exit` does not, ours does) and ends with `trapa #34` syscall 1.
The shell exit status of `sh-elf-run` is the program's exit status. Simulator-detected faults give
`sh-elf-run` status 10 (bus/address error: unaligned access, wild pointer), 4 (illegal instruction), 5
(trap), after a line `program stopped with signal N`. `abort()` prints `abort() called`, exits with 134.

**Pitfall: digit arguments.** `sh-elf-run` treats *every* argument that starts with a digit, including
program arguments such as `-skill 4` or `-tics 100`, as a RAM-size option (`4` means 2^4 bytes: instant bus
error). `simrun` protects them by prefixing a `\037` byte, which `__sim_start` removes again. Always run
programs through `simrun` (or the `$(SIM_RUNNER)` make variable); calling `sh-elf-run` directly is fine
as long as no argument starts with a digit. Arguments are visible to the program unmodified.

## 4. Syscall ABI and libc glue

`trapa #34`: `r4` = number, `r5,r6,r7` = arguments; result in `r0` (negative = error), newlib errno in `r1`
(`sim_syscall()` maps it to the fxlibc `errno`). Implemented by the simulator and used here:

| nr | name | notes |
|---|---|---|
| 1 | exit(status in r5) | |
| 3 / 4 | read / write (fd, buf, n) | fd 0/1/2 are the host's stdin/stdout/stderr; fd 1 is block-buffered by the simulator when it is a pipe, fd 2 is not (output of the two can interleave out of order under a pipe) |
| 5 | open(path, flags) | flags are translated from fxlibc (`O_CREAT 4, O_TRUNC 0x20, O_APPEND 0x10, O_EXCL 0x40`) to newlib (`0x200, 0x400, 0x8, 0x800`); new files get mode 0644; relative paths are relative to the host cwd |
| 6 | close | `close(0..2)` is ignored |
| 19 | lseek(fd, off, whence) | fxlibc has `SEEK_SET=3`; translated to 0. Offsets are 32-bit (files < 2 GiB) |
| 23 | time | host seconds; used by `time()`; `clock()` therefore has 1 s resolution |
| 38 | stat(path, buf) | wrapped by `stat()` (size, regular/dir flag, mtime); `fstat()` uses `lseek` |
| 172/173/174 | argc / argnlen / argn | start-up only |

Not implemented by the simulator (the wrapper fails with `ENOSYS`): `unlink`/`remove`, `mkdir`, `rmdir`,
`getcwd`, `chdir`, `isatty` (returns 0), `rename`, `fsync` (returns 0), directories (`opendir`), `getenv` (returns
NULL), `system`. Offsets in `ftell/fseek` and sizes are 32-bit, as on the calculator.

`libc-glue.c`:
- **Allocator** behind `fxlibc_hal_malloc/free/realloc` (and so `malloc/calloc/realloc/free`): dlmalloc-style
  boundary tags, 8-byte aligned payloads, 4 bytes of overhead per block, first-fit with splitting,
  O(1) coalescing, in-place `realloc` growth/shrink, `NULL` + `ENOMEM` when full. Extras declared in `sim.h`:
  `memalign`, `aligned_alloc`, `posix_memalign`, `malloc_usable_size`, `sim_heap_info()` (arena/used/peak/free/
  largest free chunk) and `sim_heap_check()` (walks the whole heap, returns 0 if consistent).
- `bsearch`, `strupr`, `strlwr` (declared by fxlibc / expected by DOOM sources but missing from `libc.a`; weak
  symbols, an engine definition wins), `getenv`, `system`, `abort`, `exit`, `_Exit`.
- `fxlibc_hal_rawtime/clock` from the host clock.
- fxlibc's `__cpucap` stays 0 as on the calculator (gint does not set it), so its `memcpy` takes the generic
  paths: 32-byte `mov.l` blocks if both pointers are 4-aligned, `mov.w` pairs if 2-aligned, otherwise a byte loop.
- `sim_micros()` (sim.h): deterministic microsecond clock = cycles / 118 (sim64 only, else host seconds).

stdio buffering works: stdout is line buffered, stderr unbuffered, files use `BUFSIZ` (512 byte) buffers. Note that fxlibc's
`fread` always bounces through the stream buffer (`memcpy` of every byte; with unaligned destinations that is
a byte loop), so `fread` of the 28 MB `freedoom1.wad` costs ~1 simulated instruction per byte (30 M instructions,
0.15 s of host time). Code that wants cheap bulk reads (the host platform's `PL_WadRead`) should use
`open()/lseek()/read()` directly, which map 1:1 onto the simulator syscall.

## 5. Statistics, benchmarking and profiling

### 5.1 Stock `sh-elf-run -v` (also what `simrun --stock --stats` parses)

```
$ toolchain/bin/sh-elf-run -v build-sim/bench_example.elf mul64 100
sh-elf-run build-sim/bench_example.elf

# instructions executed        8767
# cycles                      11869
# pipeline stalls                39
# misaligned load/store         868
# real time taken            0.0000
# virtual time taken         0.0003
```

`simrun --stats prog.elf args` hides the simulator's own text and prints the program output followed by
`SIMSTATS insts=N cycles=N stalls=N misaligned=N` on stderr (`--stats-file F` writes the same as key=value lines).

What the numbers mean (from `sim/sh/interp.c`):
- **instructions**: one per executed instruction, **except delay-slot instructions, which the stock simulator does
  not count**; the sim64 build counts them. `cycles` and `insts` differ by taken-branch penalties.
- **cycles**: `1` per instruction plus `2` per *taken* branch/jump/call/return (`bt bf bt/s bf/s bra braf bsr bsrf jmp
  jsr rts rte`), i.e. an SH-1-style model. A taken `bf/s` + delay slot costs 3. There is no dual issue, no
  load/use stall (`stalls` is a rarely-used register-lock counter that does not feed `cycles`), no cache or bus
  model, `mul`/`div`/`dmuls.l` cost 1. **Use cycles for relative comparisons of the same kind of code only.
  Real SH4A cycles can be ~0.5x (dual-issue loops) to several x (cache/RAM misses) of this number.**
- `misaligned load/store` is a memory-access heuristic of the simulator (accesses with `PC & 3`); ignore it.
- **The stock counters are 32-bit and wrap at 2^32** (4.29G cycles, about 12 s of simulation). `simrun` prints
  them as unsigned, but a longer run silently loses multiples of 2^32. sim64 fixes this.
- Simulation speed: ~350 million simulated instructions per second on this machine (the stock simulator),
  ~300 M with sim64. `real time`/`virtual time` lines are meaningless (`time()` has 1 s resolution, "virtual" assumes 36 MHz).

### 5.2 Micro-benchmarks: `simbench` (works with both simulators)

Measure a function by running the program twice with different iteration counts; the *difference* is the exact
cost of the extra iterations, start-up and exit cancel out:

```
$ make -C src/sim sim-bench
bench=col_c (minus empty)         iters=800 insts/iter=1306.00 cycles/iter=1405.00  (fixed overhead: insts=7953 cycles=9208)
bench=col_unrolled (minus empty)  iters=800 insts/iter=1025.00 cycles/iter=1049.00  (fixed overhead: ...)
bench=memcpy256 (minus empty)     iters=800 insts/iter=214.00  cycles/iter=232.00
bench=memcpy255_unaligned (...)   iters=800 insts/iter=1303.00 cycles/iter=1558.00
bench=mul64 (minus empty)         iters=800 insts/iter=15.00   cycles/iter=15.00
bench=div32 (minus empty)         iters=800 insts/iter=63.00   cycles/iter=68.00
```

To benchmark your own routine (e.g. an SH4 assembly column drawer against its C version):
1. copy `src/sim/test/bench_example.c`, add `{ "my_asm", my_asm_wrapper }` to `benchmarks[]`. Each kernel is a
   `void (*)(void)` called `n` times from `main` (`n` = `argv[2]`); keep it `noinline` and make it
   deterministic;
2. build it with `$(call sim-cc,...)`/`$(call sim-link,...)`; assembly files with `$(call sim-as,...)`;
3. `src/sim/simbench -b empty prog.elf my_asm` (`-b empty` subtracts the bare call+loop overhead), or
   with sim64 `src/sim/simrun prog.elf my_asm 1000 -i`, which prints
   `col_c: 1000 calls, per call (loop overhead subtracted): 1305.99 insts, 1403.99 cycles`
   measured inside the program with `sim_counters()` (so the setup code is excluded too).

Compare with the model above: 13 instructions and 14 cycles per pixel for the vanilla C column loop;
the point of the example is the method, not the absolute numbers. Check bit-exactness of an optimized variant
in the same program (compare the framebuffers and return a non-zero exit code on mismatch).

### 5.3 Per-PC profile and in-program counters (sim64)

```
$ src/sim/simrun --pcprof build-sim/prof.txt --stats build-sim/selftest.elf <wad> <tmpdir>
$ src/sim/simprof -n 6 build-sim/selftest.elf build-sim/prof.txt
total: 580610093 instructions, 646666732 cycles (simulator model)
      cycles      %        insts    c/i  function
   374966080  58.0%    346111336   1.08  main
   113485782  17.5%     97776788   1.16  test_malloc
    88874754  13.7%     74105411   1.20  naive_memcpy
    ...
$ src/sim/simprof --disasm naive_memcpy build-sim/selftest.elf build-sim/prof.txt
     count     cycles   address  instruction
000084b2 <_naive_memcpy>:
  14769327   14769327      84b2:	mov.b	@r5+,r0
  14769327   14769327      84b4:	dt	r6
  14769327   14769327      84b6:	mov.b	r0,@r4
  14769327   44049237      84b8:	bf.s	84b2 <_naive_memcpy>
  14769327     129372      84ba:	add	#1,r4
```

The profile is exact (every instruction), not sampled; it costs about 3% run time. Profile totals equal the
`SIMSTATS` totals. Run `simprof` on the same ELF the profile was recorded from. In the program,
`sim_pcprof(0/1)` pauses/resumes recording, `sim_counters(&c)` returns the cumulative 64-bit
`insts/cycles/stalls/memstalls`, and `sim_mark(id)` prints `SIMMARK <id> <insts> <cycles>` on stderr, e.g.
once per frame for per-frame cost curves (`grep SIMMARK`). On the stock simulator these three return -1.

## 6. The sim64 simulator (`make sim-tool`)

`src/sim/gdbsim/sim64.patch` (against `toolchain-build/gdb-15.2/sim/sh/interp.c` and `sh-sim.h`) is built by
`src/sim/gdbsim/build.sh` into `build-sim/tools/sh-elf-run64` (it recompiles only `interp.c` and relinks the
existing objects). `simrun` and `simbench` use it automatically when it exists (`--stock` or `SIM_RUN=...`
override). Differences from the stock simulator:

1. All statistics are 64-bit (no wrap at 4G cycles; needed for any DOOM run longer than ~700 frames).
2. Delay-slot instructions are counted in `insts`; `cycles` is unchanged.
3. Extra services through `trapa #34`: `0x7f00` read counters, `0x7f01` pause/resume profile, `0x7f02` mark
   (`sim_counters/sim_pcprof/sim_mark` in `sim.h`). Unknown numbers return -1 on the stock simulator.
4. `SIM_PCPROF=<file>` per-PC profile (see 5.3).
5. **NULL guard**: loads/stores below 0x1000 raise a bus error (`SIM_NULLTRAP=0` disables), like an unmapped page
   on the calculator.
6. A fault prints a diagnostic before the usual message, e.g.
   `sim: address error: read 32 bits at 0x00003721, pc=0x000022da` (unaligned `mov.l`),
   `sim: illegal instruction 0xffff at pc=...`. The stock simulator already enforces SH alignment rules (unaligned
   `mov.w/mov.l` and odd `jmp` targets give exit 10), so engine bugs that would fault on the calculator fault
   here too. `movua.l` is the legal way to load unaligned longs on SH4A.

## 7. Limitations

- **Timing**: see 5.1. No dual issue, load-use latency, cache or bus model; no I-cache, so code placement and
  ILRAM effects cannot be measured. Use the simulator to check **correctness**, to count **instructions**, and
  to compare variants; confirm real speed on hardware.
- No calculator hardware: no LCD, keys, RTC, DMA, interrupts, `world switch`, TLB/MMU, uncached P2 area, XRAM/YRAM/ILRAM
  (their sections are plain RAM here), `pref`/`ocbi`/`icbi`/`prefi`/`synco` are no-ops. Cache-coherency bugs cannot show up.
- No FPU in the ELF (we build `-m4-nofpu`); `double` math is soft-float from libgcc/openlibm and slow.
- SH4A/DSP instruction support is that of the GDB 15.2 simulator: all integer SH4/SH4A opcodes tested here
  (`movua.l`, `movli.l/movco.l`, `movca.l`, `prefi`, `synco`, `icbi`, `ocbp/ocbwb/ocbi`, `tas.b`, `dmuls.l/dmulu.l`,
  `xtrct`, `swap.w`; `div1`/`shad`/`shld` via libgcc and compiled code) and the DSP repeat loop (`ldrs/ldre/ldrc`) + `padd` work (`test/insns.s`).
  Interrupt/exception/TLB/FPU-precision details are not modelled.
- No stack-overflow guard; the stack is the top 1 MiB below the end of RAM, directly above the heap.
- Files: host paths, 32-bit offsets, no directories, no `remove`. The simulator calls the host's `open/read/write`; relative
  paths use the host working directory; `fopen(..., "w")` on macOS is case-insensitive like the host filesystem.
- A program killed by a host signal (e.g. a simulator crash from a huge bogus `read` size, which the simulator does not
  bounds-check) produces no statistics.
- `sh-elf-run` has no instruction tracing or single-step; use `sim_mark`, the diagnostics above, `printf`, and
  `simprof --disasm`. (No `sh-elf-gdb` is built in the toolchain.)

## 8. Files

| file | purpose |
|---|---|
| `crt0.s`, `sim.ld` | entry code and linker script (memory map above) |
| `syscalls.c` | `trapa #34` wrappers, fd functions (`open/read/write/lseek/close/stat`), start-up, `exit`, argv, counters |
| `libc-glue.c` | allocator, `memalign`, `bsearch`, `strupr`, `getenv`, fxlibc HAL pieces |
| `sim.h` | harness API (heap info, counters, marks, syscall numbers, memory-map symbols) |
| `sim.mk`, `Makefile` | make fragment for the root Makefile; standalone entry (`make -C src/sim`) |
| `cgdoom.mk` | the CGDoom client of `sim.mk`, included by the root Makefile: `make sim`, `test-sim`, `test-sim-full`, `test-demosync-sim`, `perf-sim` (§9) |
| `cgdoom-hooks.c` | sim-only measurement hooks linked into `cgdoom-sim.elf` through `-Wl,--wrap=` (`-simwin`, `-simstats`, `-simmarks`) |
| `test-sim.sh`, `perf-sim.sh` | the scripts behind `make test-sim` and `make perf-sim` |
| `simrun`, `simbench`, `simprof` | run wrapper (digit args, stats), differential benchmark driver, profile viewer (python3) |
| `gdbsim/sim64.patch`, `gdbsim/build.sh` | simulator patch and its build script |
| `test/selftest.c`, `test/insns.s` | the self test (stdio on the WAD, file write/append/update, 64-bit math, allocator stress with `sim_heap_check`, libc/libm, SH4A/DSP opcodes, sim64 counters) |
| `test/bench_example.c` | micro-benchmark example |

## 9. CGDoom in the simulator (`make sim`, root Makefile)

`src/sim/cgdoom.mk` (included by the root Makefile after `sim.mk`) builds `build-sim/cgdoom-sim.elf` from the same
`src/engine` + `src/host` sources as `make host` plus `src/engine/sh4/*.{s,S,c}`, with `SIM_CFLAGS`
(`-O2`, no `RANGECHECK`), linked with `cgdoom-hooks.c` through `-Wl,--wrap=` (main, PL_FinishUpdate, R_RenderPlayerView,
G_Ticker, P_SetupLevel), so neither the engine nor src/host contains sim code. The CLI is the host's, plus
`-simwin A B`, `-simstats`, `-simmarks` (see `cgdoom-hooks.c` and `tests/perf-baseline.md`).

The engine selects the SH4 assembly drawers itself under `__sh__` (`CGD_SH4` in `src/engine/r_draw.h`), so the default
ELF renders with them, as the calculator does, in every screen format. The `.S` files are assembled with `-Wa,--dsp -DCGD_SH4_DSP_MNEMONICS`
(readable `ldrs`/`ldre`/`ldrc`) and `-MMD -MP`, and with `-DCGD_SH4_NO_ILRAM` (the simulator has no IL memory; the hot
drawers go to `.text`). Variants, each in its own `SIM_BUILD` (objects are rebuilt when the variant of a build
directory changes, `.cgsim-variant`):

| variable | effect |
|---|---|
| `CGSIM_SH4=0` | C drawers only (`-DCGD_NO_SH4`, no `src/engine/sh4` objects), the C reference: `make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim` |
| `CGSIM_ILRAM=1` | the calculator's section layout (hot drawers in `.ilram`, which `sim.ld` places after `.text`) |
| `VARIANT=wide`, `VARIANT=hd` | the root Makefile's screen formats (396×224 8-bit, 396×224 RGB565; `src/platform/cgd_config.h`): their defines on every object, `SIM_BUILD` defaulting to `build-sim-wide` / `build-sim-hd`, and the goldens of `tests/golden-wide` / `tests/golden-hd` (`GOLDEN_DIR`) |

The tests compare with `$(GOLDEN_DIR)`, which a command-line `GOLDEN_DIR=<dir>` points elsewhere, e.g. at goldens
generated from the host build of the same variant (`tests/run-scenarios.sh -mode golden -bin
build-host-wide/cgdoom-host -golden <dir> -out <dir2>`, then `make VARIANT=wide GOLDEN_DIR=<dir> test-sim-full`). The
scripts run with `SIM_BUILD` in their environment, so `simrun` uses that build directory's `tools/sh-elf-run64`
(`make VARIANT=wide sim-tool` builds it there).

| target | what |
|---|---|
| `make sim` | build the ELF |
| `make test-sim` | `test-sim.sh`: first `SIM_TEST_TICS` (60) tics of e1m1, map01, demo1 must equal the first lines of the host goldens (~6 s; `-golden DIR`) |
| `make test-sim-full` | every scenario of `tests/scenarios.txt` through `tests/run-scenarios.sh -runner simrun` (35, ~245 s at 320×200) |
| `make test-demosync-sim` | `tests/demo-sync.sh` with the sim ELF: DEMO1-3 of the shareware `doom1.wad` given by `CGD_DOOM1_WAD` must give the same per-tic game-state traces as the host (`tests/demosync/*.ckpt`); skipped when `CGD_DOOM1_WAD` is not set |
| `make perf-sim` | `perf-sim.sh`: per-frame insts/cycles table + PC profiles (needs sim64), results in `$(SIM_BUILD)/perf/` (`-golden DIR`) |

The root Makefile also has two exactness tests for the simulator, outside `cgdoom.mk`. They are the gate
for any SH-specific change to `FixedMul`/`FixedDiv` or the division helpers of `m_fixed.h`:

| target | what |
|---|---|
| `make test-fixed-sim` | `tests/fixed_exact.c` built for SH: `FixedMul`/`FixedDiv` (the SH `FixedDiv` is inline asm in `m_fixed.c`) against the vanilla definitions, edge values plus `FIXED_SIM_PAIRS` (3M) random pairs |
| `make test-div-sim` | `tests/div_exact.c` built for SH: the `m_fixed.h` division helpers (`IntDivPos`, `IntCycleFill`) against the C operators (libgcc on SH) |
