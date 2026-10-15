# src/engine/sh4/test/Makefile - bit-exactness tests and benchmarks of the SH4 routines on the
# GDB SH simulator (src/sim harness).
#
#   make -C src/engine/sh4/test run       asmcheck + build + randomized comparison vs C in every
#                                         screen format (exit 0 = PASS)
#   make -C src/engine/sh4/test asmcheck  the raw-opcode .S build (no --dsp) = the --dsp mnemonic build
#   make -C src/engine/sh4/test bench     instruction counts C vs asm (needs sim64: builds it)
#   make -C src/engine/sh4/test pipe      static SH-4A dual-issue estimates of the inner loops
#   make -C src/engine/sh4/test sched     modsched.py: shipped loops vs the model, minimum IIs (python only)
#   make -C src/engine/sh4/test sizes     code size per section (ILRAM budget)
#   make -C src/engine/sh4/test all       everything above
#   N=20000 make ... run                  more cases per drawer (default 3000)
#
# Screen formats (src/platform/cgd_config.h). The drawers are assembled for one format, so the
# test is built once per format: TV= (empty: 320x200 8-bit, the calculator build), wide (396x224
# 8-bit), tc (320x200 RGB565) and hd (396x224 RGB565). `run`, `bench`, `pipe` and `sizes` cover
# them through sub-makes with TV set; TV=<name> <target> runs one format only.

ROOT      := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../../..)
SH4       := $(ROOT)/src/engine/sh4
SIM_BUILD ?= $(ROOT)/build-sim
include $(ROOT)/src/sim/sim.mk

TV        ?=
TVS       := wide tc hd
TVDEFS_   :=
TVDEFS_wide := -DCGD_WIDESCREEN=1
TVDEFS_tc := -DCGD_TRUECOLOR=1
TVDEFS_hd := -DCGD_HD=1
ifeq ($(filter $(TV),$(TVS)),$(TV))
TVDEFS    := $(TVDEFS_$(TV))
else
$(error TV must be empty, wide, tc or hd)
endif

OBJ       := $(SIM_BUILD)/sh4test$(if $(TV),/$(TV))
ELF       := $(OBJ)/test_sh4.elf
ELF_TEXT  := $(OBJ)/test_sh4_text.elf
N         ?= 3000
CONFIG_H  := $(ROOT)/src/platform/cgd_config.h

# Same flags as the calculator build for the assembly (fxSDK: -m4-nofpu -mb -Wa,--dsp)
ASFLAGS_SH4 := $(SIM_ARCH) -Wa,--dsp -I$(SH4) -I$(ROOT)/src/platform $(TVDEFS)
TCFLAGS   := $(SIM_CFLAGS) -I$(ROOT)/src/platform $(TVDEFS)

SH4_S     := r_draw_sh4 r_draw16_sh4 expand_sh4
SH4_OBJS  := $(SH4_S:%=$(OBJ)/%.o) $(OBJ)/r_draw_sh4_c.o
TEST_OBJS := $(OBJ)/test_sh4.o $(OBJ)/ref_draw.o $(OBJ)/abi_call.o

.PHONY: all run run-tv asmcheck bench bench-tv pipe pipe8 pipe16 sched sizes sizes-tv clean
all: run bench pipe sched sizes

$(OBJ)/%.o: $(SH4)/%.S $(SH4)/sh4_asm.h $(CONFIG_H)
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -c $< -o $@
$(OBJ)/text_%.o: $(SH4)/%.S $(SH4)/sh4_asm.h $(CONFIG_H)
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -DCGD_SH4_NO_ILRAM -c $< -o $@
$(OBJ)/r_draw_sh4_c.o: $(SH4)/r_draw_sh4_c.c $(SH4)/cgd_sh4.h $(CONFIG_H)
	@mkdir -p $(OBJ)
	$(SIM_CC) $(TCFLAGS) -MMD -MP -c $< -o $@
$(OBJ)/test_sh4.o: test_sh4.c test_globals.h $(SH4)/cgd_sh4.h $(CONFIG_H)
	@mkdir -p $(OBJ)
	$(SIM_CC) $(TCFLAGS) -MMD -MP -c $< -o $@
$(OBJ)/abi_call.o: abi_call.S
	@mkdir -p $(OBJ)
	$(SIM_CC) $(SIM_ARCH) -c $< -o $@
$(OBJ)/ref_draw.o: ref_draw.c test_globals.h $(CONFIG_H)
	@mkdir -p $(OBJ)
	$(SIM_CC) $(TCFLAGS) -fwrapv -c $< -o $@

$(ELF): $(SH4_OBJS) $(TEST_OBJS) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(SH4_OBJS) $(TEST_OBJS))
# .text variant (CGD_SH4_NO_ILRAM) to check that the switch builds and behaves the same
TEXT_OBJS := $(SH4_S:%=$(OBJ)/text_%.o) $(OBJ)/r_draw_sh4_c.o
$(ELF_TEXT): $(TEXT_OBJS) $(TEST_OBJS) $(SIM_RUNTIME_OBJS)
	$(call sim-link,$@,$(TEXT_OBJS) $(TEST_OBJS))

# P1 variant: the simulator has no P1 alias, so this build forces the movca.l/ocbp code paths
ELF_P1    := $(OBJ)/test_sh4_p1.elf
P1_OBJS   := $(OBJ)/r_draw_sh4.o $(OBJ)/r_draw16_sh4.o $(OBJ)/p1_expand_sh4.o $(OBJ)/r_draw_sh4_c.o
$(OBJ)/p1_expand_sh4.o: $(SH4)/expand_sh4.S $(SH4)/sh4_asm.h
	@mkdir -p $(OBJ)
	$(SIM_CC) $(ASFLAGS_SH4) -DCGD_SH4_TEST_FORCE_P1 -c $< -o $@
$(ELF_P1): $(P1_OBJS) $(TEST_OBJS) $(SIM_RUNTIME_OBJS)
	$(call sim-link,$@,$(P1_OBJS) $(TEST_OBJS))

-include $(wildcard $(OBJ)/*.d)

# Every .S must assemble without -Wa,--dsp (the sim game's C flags, $(SIM_CC) $(CGSIM_CFLAGS) -c)
# and give exactly the section bytes and relocations of the DSP-mnemonic form
# (-DCGD_SH4_DSP_MNEMONICS -Wa,--dsp, what the CMake and sim game builds assemble), in both
# section layouts and every screen format. sh4_asm.h encodes ldrs/ldre/ldrc as raw opcodes for this.
CGSIM_FLAGS_COPY := $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -I$(ROOT)/src/host
ASMCHECK := $(SIM_BUILD)/sh4test/asmcheck
asmcheck: $(SH4_S:%=$(SH4)/%.S) $(SH4)/sh4_asm.h $(CONFIG_H)
	@mkdir -p $(ASMCHECK)
	@set -e; for f in $(SH4_S); do for d in "" $(foreach t,$(TVS),"$(TVDEFS_$(t))"); do \
	 for v in "" -DCGD_SH4_NO_ILRAM; do \
	  $(SIM_CC) $(CGSIM_FLAGS_COPY) $$d $$v -c $(SH4)/$$f.S -o $(ASMCHECK)/$$f.sim.o; \
	  $(SIM_CC) $(SIM_ARCH) -Wa,--dsp -I$(SH4) -I$(ROOT)/src/platform $$d -DCGD_SH4_DSP_MNEMONICS $$v \
	    -c $(SH4)/$$f.S -o $(ASMCHECK)/$$f.dsp.o; \
	  $(SIM_OBJDUMP) -s -r $(ASMCHECK)/$$f.sim.o | sed 1,2d > $(ASMCHECK)/$$f.sim.txt; \
	  $(SIM_OBJDUMP) -s -r $(ASMCHECK)/$$f.dsp.o | sed 1,2d > $(ASMCHECK)/$$f.dsp.txt; \
	  if ! cmp -s $(ASMCHECK)/$$f.sim.txt $(ASMCHECK)/$$f.dsp.txt; then \
	    echo "asmcheck: $$f.S $$d $$v: raw-opcode build differs from the mnemonic build"; \
	    diff $(ASMCHECK)/$$f.sim.txt $(ASMCHECK)/$$f.dsp.txt | head -20; exit 1; fi; \
	done; done; done; echo "asmcheck: $(SH4_S:%=%.S) build without -Wa,--dsp, identical to the --dsp mnemonic build (all formats)"

# run: asmcheck, then the default format (ILRAM, .text and forced-P1 builds), then the others;
# with TV set, asmcheck and that format only (likewise bench and sizes)
run: asmcheck run-tv
	$(if $(TV),,@set -e; for t in $(TVS); do $(MAKE) --no-print-directory TV=$$t run-tv; done)
run-tv: $(ELF) $(ELF_TEXT) $(if $(TV),,$(ELF_P1))
	$(SIM_RUNNER) --stats $(ELF) test $(N)
	$(SIM_RUNNER) $(ELF_TEXT) test 300
	$(if $(TV),,$(SIM_RUNNER) $(ELF_P1) expand 300)

$(SIM_BUILD)/tools/sh-elf-run64:
	$(MAKE) -C $(ROOT)/src/sim sim-tool

bench: bench-tv
	$(if $(TV),,@set -e; for t in wide hd; do $(MAKE) --no-print-directory TV=$$t bench-tv; done)
bench-tv: $(ELF) $(SIM_BUILD)/tools/sh-elf-run64
	$(SIM_RUNNER) $(ELF) bench

PIPE = python3 $(CURDIR)/pipesim.py $(ELF) $(PIPE_FLAGS)
# pess/opt: the README models; gcc: GCC sh4a.md latencies (all loads 3), the pessimistic bound.
# Pairing: 4-byte-aligned first instruction (empirical); add PIPE_FLAGS="--pairing manual" for
# the SH4AL-DSP manual's rule alone (same 1 KiB page). Without TV: the 8-bit loops from the default
# build, then the RGB565 loops (r_draw16_sh4.S) from the tc build; with TV: that format's loops.
PIPE_FLAGS ?=
ifeq ($(TV),)
pipe: pipe8
	@$(MAKE) --no-print-directory TV=tc pipe16
else ifneq ($(filter $(TV),tc hd),)
pipe: pipe16
else
pipe: pipe8
endif
pipe8: $(ELF)
	@set -e; for m in pess opt gcc; do \
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
pipe16: $(ELF)
	@set -e; for m in pess opt gcc; do \
	  $(PIPE) cgd_c16_rs cgd_c16_end --per 2 --model $$m; \
	  $(PIPE) cgd_cl16_rs cgd_cl16_end --per 4 --model $$m; \
	  $(PIPE) cgd_s16_rs cgd_s16_end --per 2 --model $$m; \
	  $(PIPE) cgd_sl16_rs cgd_sl16_end --per 4 --model $$m; \
	  $(PIPE) cgd_t16_rs cgd_t16_end --per 1 --model $$m; \
	  $(PIPE) cgd_f16_rs cgd_f16_end --per 1 --model $$m; \
	  $(PIPE) cgd_fl16_rs cgd_fl16_end --per 2 --model $$m; \
	done

# modsched.py (exact modulo-schedule search, README §3): the loops of r_draw_sh4.S and
# r_draw16_sh4.S must meet the model under pess and opt (and those that run stall-free under gcc,
# under gcc too); then the minimum II of each kernel under "max" (the elementwise max of the
# three latency tables).
MODSCHED = python3 $(CURDIR)/modsched.py
sched:
	@set -e; for k in col collow span spanlow col16m collow16 span16 spanlow16; do for m in pess opt; do \
	  $(MODSCHED) $$k $$m --shipped -q; done; done
	@set -e; for k in span spanlow col16m; do $(MODSCHED) $$k gcc --shipped -q; done
	@set -e; for k in col collow span spanlow col16m collow16 span16 spanlow16; do $(MODSCHED) $$k max -q; done

sizes: sizes-tv
	$(if $(TV),,@$(MAKE) --no-print-directory TV=hd sizes-tv)
sizes-tv: $(SH4_S:%=$(OBJ)/%.o)
	@$(SIM_BIN)/sh-elf-size -A $^ | grep -E '^(\.ilram|\.text|\.rodata|\.data|\.bss|[^ ]*\.o)'
	@$(SIM_BIN)/sh-elf-nm -S --size-sort $^ | grep -i ' t '

clean:
	rm -rf $(SIM_BUILD)/sh4test
