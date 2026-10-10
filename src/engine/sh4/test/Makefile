# src/engine/sh4/test/Makefile - bit-exactness tests and benchmarks of the SH4 routines on the
# GDB SH simulator (src/sim harness).
#
#   make -C src/engine/sh4/test run       asmcheck + build + randomized comparison vs C (exit 0 = PASS)
#   make -C src/engine/sh4/test asmcheck  the raw-opcode .S build (no --dsp) = the --dsp mnemonic build
#   make -C src/engine/sh4/test bench     instruction counts C vs asm (needs sim64: builds it)
#   make -C src/engine/sh4/test pipe      static SH-4A dual-issue estimates of the inner loops
#   make -C src/engine/sh4/test sched     modsched.py: shipped loops vs the model, minimum IIs (python only)
#   make -C src/engine/sh4/test sizes     code size per section (ILRAM budget)
#   make -C src/engine/sh4/test all       everything above
#   N=20000 make ... run                  more cases per drawer (default 3000)

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
SH4       := $(ROOT)/src/engine/sh4
SIM_BUILD ?= $(ROOT)/build-sim
include $(ROOT)/src/sim/sim.mk

OBJ       := $(SIM_BUILD)/sh4test
ELF       := $(SIM_BUILD)/sh4test/test_sh4.elf
ELF_TEXT  := $(SIM_BUILD)/sh4test/test_sh4_text.elf
N         ?= 3000

# Same flags as the calculator build for the assembly (fxSDK: -m4-nofpu -mb -Wa,--dsp)
ASFLAGS_SH4 := $(SIM_ARCH) -Wa,--dsp -I$(SH4)

SH4_OBJS  := $(OBJ)/r_draw_sh4.o $(OBJ)/expand_sh4.o $(OBJ)/r_draw_sh4_c.o
TEST_OBJS := $(OBJ)/test_sh4.o $(OBJ)/ref_draw.o $(OBJ)/abi_call.o

.PHONY: all run asmcheck bench pipe sched sizes clean
all: run bench pipe sched sizes

$(OBJ)/%.o: $(SH4)/%.S $(SH4)/sh4_asm.h
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -c $< -o $@
$(OBJ)/text_%.o: $(SH4)/%.S $(SH4)/sh4_asm.h
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -DCGD_SH4_NO_ILRAM -c $< -o $@
$(OBJ)/r_draw_sh4_c.o: $(SH4)/r_draw_sh4_c.c $(SH4)/cgd_sh4.h
	$(call sim-cc,$<,$@)
$(OBJ)/test_sh4.o: test_sh4.c test_globals.h $(SH4)/cgd_sh4.h
	$(call sim-cc,$<,$@)
$(OBJ)/abi_call.o: abi_call.S
	@mkdir -p $(OBJ)
	$(SIM_CC) $(SIM_ARCH) -c $< -o $@
$(OBJ)/ref_draw.o: ref_draw.c test_globals.h
	@mkdir -p $(OBJ)
	$(SIM_CC) $(SIM_CFLAGS) -fwrapv -c $< -o $@

$(ELF): $(SH4_OBJS) $(TEST_OBJS) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(SH4_OBJS) $(TEST_OBJS))
# .text variant (CGD_SH4_NO_ILRAM) to check that the switch builds and behaves the same
$(ELF_TEXT): $(OBJ)/text_r_draw_sh4.o $(OBJ)/text_expand_sh4.o $(OBJ)/r_draw_sh4_c.o $(TEST_OBJS) $(SIM_RUNTIME_OBJS)
	$(call sim-link,$@,$(OBJ)/text_r_draw_sh4.o $(OBJ)/text_expand_sh4.o $(OBJ)/r_draw_sh4_c.o $(TEST_OBJS))

# P1 variant: the simulator has no P1 alias, so this build forces the movca.l/ocbp code paths
ELF_P1    := $(SIM_BUILD)/sh4test/test_sh4_p1.elf
$(OBJ)/p1_expand_sh4.o: $(SH4)/expand_sh4.S $(SH4)/sh4_asm.h
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -DCGD_SH4_TEST_FORCE_P1 -c $< -o $@
$(ELF_P1): $(OBJ)/r_draw_sh4.o $(OBJ)/p1_expand_sh4.o $(OBJ)/r_draw_sh4_c.o $(TEST_OBJS) $(SIM_RUNTIME_OBJS)
	$(call sim-link,$@,$(OBJ)/r_draw_sh4.o $(OBJ)/p1_expand_sh4.o $(OBJ)/r_draw_sh4_c.o $(TEST_OBJS))

# Review round 2: every .S must assemble without -Wa,--dsp (the sim game's C flags, $(SIM_CC)
# $(CGSIM_CFLAGS) -c, as that build used until it switched to the mnemonics) and give exactly the
# section bytes and relocations of the DSP-mnemonic form (-DCGD_SH4_DSP_MNEMONICS -Wa,--dsp, what
# the CMake and sim game builds now assemble), in both section layouts. sh4_asm.h encodes
# ldrs/ldre/ldrc as raw opcodes for this.
CGSIM_FLAGS_COPY := $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -I$(ROOT)/src/host
ASMCHECK := $(OBJ)/asmcheck
asmcheck: $(SH4)/r_draw_sh4.S $(SH4)/expand_sh4.S $(SH4)/sh4_asm.h
	@mkdir -p $(ASMCHECK)
	@set -e; for f in r_draw_sh4 expand_sh4; do for v in "" -DCGD_SH4_NO_ILRAM; do \
	  $(SIM_CC) $(CGSIM_FLAGS_COPY) $$v -c $(SH4)/$$f.S -o $(ASMCHECK)/$$f.sim.o; \
	  $(SIM_CC) $(ASFLAGS_SH4) -DCGD_SH4_DSP_MNEMONICS $$v -c $(SH4)/$$f.S -o $(ASMCHECK)/$$f.dsp.o; \
	  $(SIM_OBJDUMP) -s -r $(ASMCHECK)/$$f.sim.o | sed 1,2d > $(ASMCHECK)/$$f.sim.txt; \
	  $(SIM_OBJDUMP) -s -r $(ASMCHECK)/$$f.dsp.o | sed 1,2d > $(ASMCHECK)/$$f.dsp.txt; \
	  if ! cmp -s $(ASMCHECK)/$$f.sim.txt $(ASMCHECK)/$$f.dsp.txt; then \
	    echo "asmcheck: $$f.S $$v: raw-opcode build differs from the mnemonic build"; \
	    diff $(ASMCHECK)/$$f.sim.txt $(ASMCHECK)/$$f.dsp.txt | head -20; exit 1; fi; \
	done; done; echo "asmcheck: r_draw_sh4.S, expand_sh4.S build without -Wa,--dsp, identical to the --dsp mnemonic build"

run: asmcheck $(ELF) $(ELF_TEXT) $(ELF_P1)
	$(SIM_RUNNER) --stats $(ELF) test $(N)
	$(SIM_RUNNER) $(ELF_TEXT) test 300
	$(SIM_RUNNER) $(ELF_P1) expand 300

$(SIM_BUILD)/tools/sh-elf-run64:
	$(MAKE) -C $(ROOT)/src/sim sim-tool

bench: $(ELF) $(SIM_BUILD)/tools/sh-elf-run64
	$(SIM_RUNNER) $(ELF) bench

PIPE = python3 $(CURDIR)/pipesim.py $(ELF) $(PIPE_FLAGS)
# pess/opt: the README models; gcc: GCC sh4a.md latencies (all loads 3), the pessimistic bound.
# Pairing: 4-byte-aligned first instruction (empirical); add PIPE_FLAGS="--pairing manual" for
# the SH4AL-DSP manual's rule alone (same 1 KiB page).
PIPE_FLAGS ?=
pipe: $(ELF)
	@for m in pess opt gcc; do \
	  $(PIPE) cgd_col_rs cgd_col_end --per 2 --model $$m; \
	  $(PIPE) cgd_coll_rs cgd_coll_end --per 4 --model $$m; \
	  $(PIPE) cgd_span_rs cgd_span_end --per 2 --model $$m; \
	  $(PIPE) cgd_spanl_rs cgd_spanl_end --per 4 --model $$m; \
	  $(PIPE) cgd_tr_rs cgd_tr_end --per 1 --model $$m; \
	  $(PIPE) cgd_fz_rs cgd_fz_end --per 1 --model $$m; \
	  $(PIPE) cgd_x_loop cgd_x_end --per 16 --model $$m; \
	  $(PIPE) cgd_x32_loop cgd_x32_end --per 16 --model $$m; \
	  $(PIPE) cgd_x32_loop cgd_x32_next --per 16 --model $$m; \
	  $(PIPE) cgd_xt_loop cgd_xt_end --per 4 --model $$m; \
	  $(PIPE) cgd_xs_loop cgd_xs_end --per 1 --model $$m; \
	done

# modsched.py (exact modulo-schedule search, README §3): the loops in r_draw_sh4.S must meet the
# model under pess and opt (and the spans under gcc too: the columns are known to stall there);
# then the minimum II of each kernel under "max" (the elementwise max of the three latency tables).
MODSCHED = python3 $(CURDIR)/modsched.py
sched:
	@set -e; for k in col collow span spanlow; do for m in pess opt; do $(MODSCHED) $$k $$m --shipped -q; done; done
	@set -e; for k in span spanlow; do $(MODSCHED) $$k gcc --shipped -q; done
	@set -e; for k in col collow span spanlow; do $(MODSCHED) $$k max -q; done

sizes: $(OBJ)/r_draw_sh4.o $(OBJ)/expand_sh4.o
	@$(SIM_BIN)/sh-elf-size -A $(OBJ)/r_draw_sh4.o $(OBJ)/expand_sh4.o | grep -E '^(\.ilram|\.text|\.rodata|\.data|\.bss|[^ ]*\.o)'
	@$(SIM_BIN)/sh-elf-nm -S --size-sort $(OBJ)/r_draw_sh4.o $(OBJ)/expand_sh4.o | grep -i ' t '

clean:
	rm -rf $(OBJ)
