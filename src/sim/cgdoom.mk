# cgdoom.mk - CGDoom under the SH simulator (included by the root Makefile after sim.mk).
#
#   make sim            build-sim/cgdoom-sim.elf: src/engine (+ src/engine/sh4 when present) and
#                       src/host, sh-elf-gcc -m4-nofpu -mb -O2 -DPL_SIM, no RANGECHECK (as on the
#                       calculator), linked with the src/sim runtime and src/sim/cgdoom-hooks.c
#   make test-sim       short bit-exactness check: the first SIM_TEST_TICS tics of a few scenarios
#                       must reproduce the host goldens' first frames (src/sim/test-sim.sh)
#   make test-sim-full  every scenario of tests/scenarios.txt in the simulator (several minutes)
#   make perf-sim       per-frame instruction/cycle baseline + profiles (src/sim/perf-sim.sh)
#   make test-demosync-sim  DEMO1-3 of $CGD_DOOM1_WAD in the simulator: the per-tic traces must
#                       match tests/demosync/*.ckpt like the host (tests/demo-sync.sh)
#
# The default ELF uses the SH4 assembly drawers of src/engine/sh4 (the engine selects them under
# __sh__, r_draw.h CGD_SH4), as the calculator build does. Variants (use another SIM_BUILD):
#   CGSIM_SH4=0         C drawers only (-DCGD_NO_SH4, no src/engine/sh4 objects): the C reference,
#                       e.g.  make SIM_BUILD=build-sim-c CGSIM_SH4=0 sim
#   CGSIM_ILRAM=1       hot drawers in ".ilram" (the calculator layout; sim.ld maps it into plain RAM
#                       after .text) instead of the default -DCGD_SH4_NO_ILRAM (.text): the
#                       simulator has no IL memory, the two layouts must render the same frames.
#
# Needs from the root Makefile: ROOT, ENGINE_SRC, HOST_SRC, RUN_SCEN.

CGSIM_SH4      ?= 1
CGSIM_ILRAM    ?= 0
CGSIM_ELF      := $(SIM_BUILD)/cgdoom-sim.elf
CGSIM_CFLAGS   := $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -I$(ROOT)/src/host
ifeq ($(CGSIM_SH4),0)
CGSIM_ASM_SRC  :=
CGSIM_C_SRC    := $(ENGINE_SRC) $(HOST_SRC)
CGSIM_CFLAGS   += -DCGD_NO_SH4
else
CGSIM_ASM_SRC  := $(wildcard $(ROOT)/src/engine/sh4/*.s $(ROOT)/src/engine/sh4/*.S)
CGSIM_C_SRC    := $(ENGINE_SRC) $(HOST_SRC) $(wildcard $(ROOT)/src/engine/sh4/*.c)
endif
# .S files: readable DSP mnemonics (ldrs/ldre/ldrc need the DSP ISA, -Wa,--dsp); the raw-opcode
# fallback of sh4_asm.h stays byte-identical (make -C src/engine/sh4/test asmcheck).
CGSIM_SFLAGS   := $(CGSIM_CFLAGS) -Wa,--dsp -DCGD_SH4_DSP_MNEMONICS \
                  $(if $(filter 1,$(CGSIM_ILRAM)),,-DCGD_SH4_NO_ILRAM)
CGSIM_OBJ      := $(CGSIM_C_SRC:$(ROOT)/src/%.c=$(SIM_BUILD)/%.o) \
                  $(patsubst $(ROOT)/src/%,$(SIM_BUILD)/%.o,$(CGSIM_ASM_SRC)) \
                  $(SIM_BUILD)/sim/cgdoom-hooks.o
# Objects are rebuilt when the variant of this SIM_BUILD changes (the stamp is rewritten only then)
CGSIM_VARIANT  := sh4=$(CGSIM_SH4) ilram=$(CGSIM_ILRAM)
CGSIM_STAMP    := $(SIM_BUILD)/.cgsim-variant
CGSIM_STAMP_OK := $(shell mkdir -p $(SIM_BUILD) && \
                    { [ "`cat $(CGSIM_STAMP) 2>/dev/null`" = "$(CGSIM_VARIANT)" ] || \
                      echo "$(CGSIM_VARIANT)" > $(CGSIM_STAMP); } && echo ok)
$(CGSIM_OBJ): $(CGSIM_STAMP)
CGSIM_WRAP     := -Wl,--wrap=main,--wrap=PL_FinishUpdate,--wrap=R_RenderPlayerView,--wrap=G_Ticker,--wrap=P_SetupLevel

SIM_TEST_TICS  ?= 60

.PHONY: sim test-sim test-sim-full perf-sim test-demosync-sim

sim: $(CGSIM_ELF)

$(CGSIM_ELF): $(CGSIM_OBJ) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(CGSIM_WRAP) $(CGSIM_OBJ))

$(SIM_BUILD)/engine/%.o: $(ROOT)/src/engine/%.c
	@mkdir -p $(dir $@)
	$(SIM_CC) $(CGSIM_CFLAGS) -MMD -MP -c $< -o $@
$(SIM_BUILD)/host/%.o: $(ROOT)/src/host/%.c
	@mkdir -p $(dir $@)
	$(SIM_CC) $(CGSIM_CFLAGS) -MMD -MP -c $< -o $@
$(SIM_BUILD)/engine/sh4/%.s.o: $(ROOT)/src/engine/sh4/%.s
	@mkdir -p $(dir $@)
	$(SIM_AS) $(SIM_ASFLAGS) $< -o $@
$(SIM_BUILD)/engine/sh4/%.S.o: $(ROOT)/src/engine/sh4/%.S
	@mkdir -p $(dir $@)
	$(SIM_CC) $(CGSIM_SFLAGS) -MMD -MP -c $< -o $@
$(SIM_BUILD)/sim/cgdoom-hooks.o: $(SIM_DIR)/cgdoom-hooks.c $(SIM_DIR)/sim.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(CGSIM_CFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard $(SIM_BUILD)/engine/*.d $(SIM_BUILD)/engine/sh4/*.d $(SIM_BUILD)/host/*.d)

test-sim: $(CGSIM_ELF)
	@sh $(SIM_DIR)/test-sim.sh -bin $(CGSIM_ELF) -tics $(SIM_TEST_TICS)

test-sim-full: $(CGSIM_ELF)
	@sh $(RUN_SCEN) -mode test -runner $(SIM_RUNNER) -bin $(CGSIM_ELF) -out $(SIM_BUILD)/tests

# DEMO1-3 of $(CGD_DOOM1_WAD) in the simulator, same traces as the host (tests/demo-sync.sh)
test-demosync-sim: $(CGSIM_ELF)
	@sh $(ROOT)/tests/demo-sync.sh -runner $(SIM_RUNNER) -bin $(CGSIM_ELF) -out $(SIM_BUILD)/tests/demosync

perf-sim: $(CGSIM_ELF)
	@sh $(SIM_DIR)/perf-sim.sh -bin $(CGSIM_ELF)
