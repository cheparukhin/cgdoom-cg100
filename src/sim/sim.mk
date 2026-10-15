# sim.mk - build/run support for programs executed by sh-elf-run (GDB SH simulator).
#
# Include from the root Makefile (GNU make 3.81 compatible):
#     -include src/sim/sim.mk
#
# Building an ELF from your own objects:
#     $(SIM_BUILD)/foo.o: foo.c ; $(call sim-cc,$<,$@)            # compile (adds -MMD)
#     $(SIM_BUILD)/foo.elf: $(OBJS) $(SIM_RUNTIME_OBJS) ; $(call sim-link,$@,$(OBJS))
# Running it:
#     $(SIM_RUNNER) [--stats] $(SIM_BUILD)/foo.elf args...      (see src/sim/README.md)
#
# Variables you may use or override (set them before the include, or append with += after it):
#   SIM_BUILD   build directory (default <root>/build-sim); runtime objects go to $(SIM_BUILD)/sim
#   SIM_ARCH    CPU flags (default -m4-nofpu -mb; the only multilib shipped with the toolchain)
#   SIM_OPT     optimisation flags (default -O2)
#   SIM_CC SIM_AS SIM_AR SIM_OBJDUMP SIM_SIZE SIM_RUN SIM_RUNNER   tools
#   SIM_CFLAGS  C flags (includes -DPL_SIM and -I src/sim, so <sim.h> is available)
#   SIM_RAM_END, SIM_STACK_SIZE   optional hex values shrinking RAM / stack (default 64 MiB / 1 MiB)
#   SIM_ASFLAGS assembler flags; SIM_LDFLAGS link flags; SIM_LIBS libraries; SIM_RUNTIME_OBJS crt0+glue
# Targets: sim-runtime, sim-tool (patched simulator), sim-selftest, sim-bench, sim-clean

SIM_PREV_GOAL := $(.DEFAULT_GOAL)

SIM_DIR       := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
SIM_ROOT      := $(abspath $(SIM_DIR)/../..)
SIM_TOOLCHAIN ?= $(SIM_ROOT)/toolchain
SIM_BIN       ?= $(SIM_TOOLCHAIN)/bin
SIM_BUILD     ?= $(SIM_ROOT)/build-sim
SIM_OBJ       := $(SIM_BUILD)/sim

SIM_CC        ?= $(SIM_BIN)/sh-elf-gcc
SIM_AS        ?= $(SIM_BIN)/sh-elf-as
SIM_AR        ?= $(SIM_BIN)/sh-elf-ar
SIM_OBJDUMP   ?= $(SIM_BIN)/sh-elf-objdump
SIM_SIZE      ?= $(SIM_BIN)/sh-elf-size
SIM_RUN       ?= $(SIM_BIN)/sh-elf-run
SIM_RUNNER    ?= $(SIM_DIR)/simrun
SIM_BENCH     ?= $(SIM_DIR)/simbench
SIM_PROF      ?= $(SIM_DIR)/simprof
SIM_RUN64     ?= $(SIM_BUILD)/tools/sh-elf-run64

SIM_ARCH      ?= -m4-nofpu -mb
SIM_OPT       ?= -O2
SIM_CFLAGS    ?= $(SIM_ARCH) $(SIM_OPT) -std=gnu11 -fno-strict-aliasing -DPL_SIM \
                 -ffunction-sections -fdata-sections -fno-asynchronous-unwind-tables -I$(SIM_DIR)
# crt0 is assembled with the DSP ISA so the final ELF is tagged sh4al-dsp (this is what the
# libraries are tagged with anyway): the simulator then executes SH4AL-DSP opcodes (repeat loops,
# padd/pmuls...) instead of FPU opcodes.  Without it, DSP code would trap as illegal instructions.
SIM_ASFLAGS   ?= -big -isa=sh4al-dsp
# SIM_RAM_END / SIM_STACK_SIZE (hex, optional) shrink the usable RAM / the stack, e.g. SIM_RAM_END=0x00800000
# emulates an 8 MiB machine (heap = what is left between .bss and the stack).
SIM_LD_EXTRA   = $(if $(SIM_RAM_END),-Xlinker --defsym=___sim_ram_end=$(SIM_RAM_END)) \
                 $(if $(SIM_STACK_SIZE),-Xlinker --defsym=___sim_stack_size=$(SIM_STACK_SIZE))
SIM_LDFLAGS   ?= $(SIM_ARCH) -nostdlib -nostartfiles -T $(SIM_DIR)/sim.ld \
                 -Wl,--gc-sections -Wl,--no-warn-rwx-segments $(SIM_LD_EXTRA)
# -lc must come before -lopenlibm/-lgcc and they reference each other: use a group.
SIM_LIBS      ?= -Wl,--start-group -lc -lopenlibm -lgcc -Wl,--end-group

SIM_RUNTIME_OBJS := $(SIM_OBJ)/crt0.o $(SIM_OBJ)/syscalls.o $(SIM_OBJ)/libc-glue.o

# The test builds' warnings, as errors (CGD_WARN)
include $(SIM_ROOT)/warnings.mk

# $(call sim-cc,<src.c>,<out.o>)
define sim-cc
@mkdir -p $(dir $(2))
$(SIM_CC) $(SIM_CFLAGS) $(CGD_WARN) -MMD -MP -c $(1) -o $(2)
endef

# $(call sim-as,<src.s>,<out.o>)
define sim-as
@mkdir -p $(dir $(2))
$(SIM_AS) $(SIM_ASFLAGS) $(1) -o $(2)
endef

# $(call sim-link,<out.elf>,<objects>)   (runtime objects are added automatically; also writes <out>.map)
define sim-link
@mkdir -p $(dir $(1))
$(SIM_CC) $(SIM_LDFLAGS) -Wl,-Map=$(1).map -o $(1) $(SIM_RUNTIME_OBJS) $(2) $(SIM_LIBS)
endef

.PHONY: sim-runtime sim-tool sim-selftest sim-selftest-stock sim-bench sim-clean
sim-runtime: $(SIM_RUNTIME_OBJS)

# Patched simulator with 64-bit counters, in-program counters and PC profiles (a few seconds).
# simrun/simbench use it automatically once it exists.  See src/sim/README.md.
sim-tool:
	SIM_BUILD=$(SIM_BUILD) $(SIM_DIR)/gdbsim/build.sh

$(SIM_OBJ)/crt0.o: $(SIM_DIR)/crt0.s
	$(call sim-as,$<,$@)
$(SIM_OBJ)/%.o: $(SIM_DIR)/%.c $(SIM_DIR)/sim.h
	$(call sim-cc,$<,$@)
$(SIM_OBJ)/test/%.o: $(SIM_DIR)/test/%.c $(SIM_DIR)/sim.h
	$(call sim-cc,$<,$@)
$(SIM_OBJ)/test/%.o: $(SIM_DIR)/test/%.s
	$(call sim-as,$<,$@)
-include $(wildcard $(SIM_OBJ)/*.d $(SIM_OBJ)/test/*.d)

# ---- self test ------------------------------------------------------------------------------------
SIM_TEST_WAD   ?= $(SIM_ROOT)/test-wads/fd/freedoom-0.13.0/freedoom1.wad
SIM_SELFTEST   := $(SIM_BUILD)/selftest.elf

SIM_SELFTEST_OBJS := $(SIM_OBJ)/test/selftest.o $(SIM_OBJ)/test/insns.o
$(SIM_SELFTEST): $(SIM_SELFTEST_OBJS) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(SIM_SELFTEST_OBJS))

# Runs the program on the simulator; the WAD CRC is computed on the host and checked by the test.
sim-selftest: $(SIM_SELFTEST)
	@mkdir -p $(SIM_BUILD)/selftest-tmp
	$(SIM_RUNNER) --stats $(SIM_SELFTEST) $(SIM_TEST_WAD) $(SIM_BUILD)/selftest-tmp \
		$$(python3 -c 'import sys,zlib;print("%08x"%(zlib.crc32(open(sys.argv[1],"rb").read())&0xffffffff))' $(SIM_TEST_WAD) 2>/dev/null)

# Same with the unpatched toolchain/bin/sh-elf-run (32-bit counters, no in-program services).
sim-selftest-stock: $(SIM_SELFTEST)
	@mkdir -p $(SIM_BUILD)/selftest-tmp
	$(SIM_RUNNER) --stock --stats $(SIM_SELFTEST) $(SIM_TEST_WAD) $(SIM_BUILD)/selftest-tmp \
		$$(python3 -c 'import sys,zlib;print("%08x"%(zlib.crc32(open(sys.argv[1],"rb").read())&0xffffffff))' $(SIM_TEST_WAD) 2>/dev/null)

# ---- micro-benchmark example (differences of two runs) ---------------------------------------------
SIM_BENCH_EX := $(SIM_BUILD)/bench_example.elf
$(SIM_BENCH_EX): $(SIM_OBJ)/test/bench_example.o $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(SIM_OBJ)/test/bench_example.o)

sim-bench: $(SIM_BENCH_EX)
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) col_c
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) col_unrolled
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) memcpy256
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) memcpy255_unaligned
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) mul64
	$(SIM_BENCH) -b empty $(SIM_BENCH_EX) div32

sim-clean:
	rm -rf $(SIM_OBJ) $(SIM_SELFTEST) $(SIM_SELFTEST).map $(SIM_BENCH_EX) $(SIM_BENCH_EX).map $(SIM_BUILD)/selftest-tmp

# keep the root Makefile's default goal
.DEFAULT_GOAL := $(SIM_PREV_GOAL)
