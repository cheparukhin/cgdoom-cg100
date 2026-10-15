# CGDoom for fx-CG100: host builds and tests (GNU make 3.81 compatible).
#
#   make host        build-host/cgdoom-host        (clang -O2 -g, RANGECHECK on)
#   make host-asan   build-host-asan/cgdoom-host   (ASan + UBSan)
#   make golden      regenerate tests/golden/*.crc from tests/scenarios.txt
#   make test-host   run all scenarios, compare with goldens (PASS/FAIL)
#   make test-asan   run all scenarios under ASan/UBSan, also compared to goldens
#                    (and the save-entry, demo-unbegun, map-limit and robust tests)
#   make test-fixed  FixedMul/FixedDiv vs vanilla exactness test (PASS/FAIL)
#   make test-fixed-sim  the same test built for SH and run in the simulator
#   make test-div    exact division helpers vs the C operators (host)
#   make test-div-sim    the same test in the SH simulator
#   make test-names  8.3 file-name mapping test (src/engine/cgdoom-names.c)
#   make test-roundtrip  record a demo (8.3 name) and play it back identically
#   make test-settings   CGDOOM.CFG save/load (NONE, exact keys, garbage)
#   make test-saveentry  save-name entry: pre-fill, typing, escape, load
#   make test-demo-unbegun  a demo recording that never began (title, title
#                    demos, I_Error in the first level load): no crash, no save
#   make test-robust damaged, foreign and oversized savegames, damaged demos:
#                    clean errors, no access outside a buffer (run under ASan
#                    by test-asan; tests/save-demo-robustness.sh)
#   make test-unit   test-fixed + test-div + test-names + test-roundtrip +
#                    test-settings + test-saveentry + test-demo-unbegun +
#                    test-robust
#   make test-demosync  DEMO1-3 of the shareware doom1.wad given by
#                    CGD_DOOM1_WAD play in sync (per-tic game-state traces vs
#                    tests/demosync/*.ckpt, checked against linuxdoom-1.10);
#                    skipped when CGD_DOOM1_WAD is not set
#   make test-map-limit  -warp 33 starts MAP32 (the engine's map tables end at 32);
#                    also part of test-unit, and test-asan runs it under ASan
#   make test-timedemo  -timedemo plays every demo, one frame per tic (also in
#                    test-unit and, under ASan, test-asan)
#   make check-sh    compile every engine file with sh-elf-gcc (no link)
#   make sim         build-sim/cgdoom-sim.elf (engine + src/engine/sh4 asm drawers + src/host for
#                    sh-elf-run, src/sim/cgdoom.mk; CGSIM_SH4=0 for a C-only ELF)
#   make test-sim    short sim run of a few scenarios vs the host goldens (bit-exact)
#   make test-sim-full / perf-sim   all scenarios in the sim / per-frame cost baseline
#   make test-demosync-sim  test-demosync with cgdoom-sim.elf in the simulator
#
# The calculator build is CMake/fxSDK (CMakeLists.txt), not this Makefile.

ROOT        := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

all: host
.PHONY: all host host-asan golden test-host test-asan test-fixed test-names \
        test-roundtrip test-unit check-sh clean test-settings test-saveentry \
        test-demo-unbegun test-demosync test-robust

ENGINE_SRC  := $(wildcard $(ROOT)/src/engine/*.c)
HOST_SRC    := $(wildcard $(ROOT)/src/host/*.c)

HOST_CC     ?= clang
HOST_INC    := -I$(ROOT)/src/platform -I$(ROOT)/src/engine -I$(ROOT)/src/host
# CGDoom/DOOM code triggers these a lot; keep the rest of -Wall -Wextra
HOST_WARN   := -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter \
               -Wno-unused-but-set-variable -Wno-unused-but-set-parameter \
               -Wno-missing-field-initializers -Wno-deprecated-declarations
HOST_CFLAGS := -std=gnu11 -O2 -g -DRANGECHECK $(HOST_WARN) $(HOST_INC)
HOST_LDFLAGS :=

# shift-base: DOOM left-shifts negative values everywhere (x<<FRACBITS); GCC
# defines this as the usual two's complement shift, so it is not checked.
ASAN_FLAGS  := -fsanitize=address,undefined -fno-sanitize=shift-base \
               -fno-sanitize-recover=all -fno-omit-frame-pointer
ASAN_CFLAGS := -std=gnu11 -O1 -g -DRANGECHECK $(HOST_WARN) $(HOST_INC) $(ASAN_FLAGS)

HOST_BUILD  := $(ROOT)/build-host
ASAN_BUILD  := $(ROOT)/build-host-asan

HOST_OBJ    := $(ENGINE_SRC:$(ROOT)/src/%.c=$(HOST_BUILD)/%.o) \
               $(HOST_SRC:$(ROOT)/src/%.c=$(HOST_BUILD)/%.o)
ASAN_OBJ    := $(ENGINE_SRC:$(ROOT)/src/%.c=$(ASAN_BUILD)/%.o) \
               $(HOST_SRC:$(ROOT)/src/%.c=$(ASAN_BUILD)/%.o)

#---
# Host builds
#---

host: $(HOST_BUILD)/cgdoom-host
host-asan: $(ASAN_BUILD)/cgdoom-host

$(HOST_BUILD)/cgdoom-host: $(HOST_OBJ)
	$(HOST_CC) $(HOST_CFLAGS) $(HOST_LDFLAGS) $^ -o $@

$(HOST_BUILD)/%.o: $(ROOT)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) -MMD -MP -c $< -o $@

$(ASAN_BUILD)/cgdoom-host: $(ASAN_OBJ)
	$(HOST_CC) $(ASAN_CFLAGS) $^ -o $@

$(ASAN_BUILD)/%.o: $(ROOT)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(ASAN_CFLAGS) -MMD -MP -c $< -o $@

-include $(wildcard $(HOST_BUILD)/*/*.d $(ASAN_BUILD)/*/*.d)

#---
# Golden-frame tests (scenarios in tests/scenarios.txt)
#---

SCENARIOS   := $(ROOT)/tests/scenarios.txt
RUN_SCEN    := $(ROOT)/tests/run-scenarios.sh

golden: host
	@sh $(RUN_SCEN) -mode golden -bin $(HOST_BUILD)/cgdoom-host

test-host: host
	@sh $(RUN_SCEN) -mode test -bin $(HOST_BUILD)/cgdoom-host

test-asan: host-asan
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(RUN_SCEN) -mode test -bin $(ASAN_BUILD)/cgdoom-host -out $(ASAN_BUILD)/tests
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/save-entry.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/saveentry
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/demo-unbegun.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/unbegun
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/save-demo-robustness.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/robust

test-fixed: $(HOST_BUILD)/fixed-exact
	$(HOST_BUILD)/fixed-exact

$(HOST_BUILD)/fixed-exact: $(ROOT)/tests/fixed_exact.c $(ROOT)/src/engine/m_fixed.c $(ROOT)/src/engine/m_fixed.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 $(HOST_WARN) $(HOST_INC) $(filter %.c,$^) -o $@

# Exact division helpers (m_fixed.h IntDivPos/IntCycleFill) vs C operators
test-div: $(HOST_BUILD)/div-exact
	$(HOST_BUILD)/div-exact

$(HOST_BUILD)/div-exact: $(ROOT)/tests/div_exact.c $(ROOT)/src/engine/m_fixed.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 $(HOST_WARN) $(HOST_INC) -fsanitize=undefined \
	  -fno-sanitize-recover=all $(filter %.c,$^) -o $@

# Small host tests that are not golden scenarios
test-unit: test-fixed test-div test-names test-roundtrip test-settings \
           test-saveentry test-demo-unbegun test-robust

test-roundtrip: host
	@sh $(ROOT)/tests/demo-roundtrip.sh -bin $(HOST_BUILD)/cgdoom-host

test-saveentry: host
	@sh $(ROOT)/tests/save-entry.sh -bin $(HOST_BUILD)/cgdoom-host

test-demo-unbegun: host
	@sh $(ROOT)/tests/demo-unbegun.sh -bin $(HOST_BUILD)/cgdoom-host

test-robust: host
	@sh $(ROOT)/tests/save-demo-robustness.sh -bin $(HOST_BUILD)/cgdoom-host

# Optional: needs CGD_DOOM1_WAD (the IWAD is not in the repository)
test-demosync: host
	@sh $(ROOT)/tests/demo-sync.sh -bin $(HOST_BUILD)/cgdoom-host

test-settings: $(HOST_BUILD)/settings-test
	$(HOST_BUILD)/settings-test

$(HOST_BUILD)/settings-test: $(ROOT)/tests/settings_test.c $(ROOT)/src/engine/cgdoom.c \
  $(ROOT)/src/engine/cgdoom-kbd.c $(ROOT)/src/engine/cgdoom.h $(ROOT)/src/engine/cgdoom-kbd.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g $(HOST_WARN) $(HOST_INC) $(ASAN_FLAGS) $(filter %.c,$^) -o $@

test-names: $(HOST_BUILD)/names-test
	$(HOST_BUILD)/names-test

$(HOST_BUILD)/names-test: $(ROOT)/tests/names_test.c $(ROOT)/src/engine/cgdoom-names.c $(ROOT)/src/engine/cgdoom.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O1 -g $(HOST_WARN) $(HOST_INC) $(ASAN_FLAGS) $(filter %.c,$^) -o $@

# Maps past MAP32 (tests/map-limit.sh)
.PHONY: test-map-limit test-map-limit-asan
test-unit: test-map-limit
test-asan: test-map-limit-asan

test-map-limit: host
	@sh $(ROOT)/tests/map-limit.sh -bin $(HOST_BUILD)/cgdoom-host

test-map-limit-asan: host-asan
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/map-limit.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/maplimit

# Special keys on the title screen, before any level (tests/title-keys.sh)
.PHONY: test-title-keys test-title-keys-asan
test-unit: test-title-keys
test-asan: test-title-keys-asan

test-title-keys: host
	@sh $(ROOT)/tests/title-keys.sh -bin $(HOST_BUILD)/cgdoom-host

test-title-keys-asan: host-asan
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/title-keys.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/titlekeys

# Timedemo: every demo of the WAD, one frame per tic (tests/timedemo.sh)
.PHONY: test-timedemo test-timedemo-asan
test-unit: test-timedemo
test-asan: test-timedemo-asan

test-timedemo: host
	@sh $(ROOT)/tests/timedemo.sh -bin $(HOST_BUILD)/cgdoom-host

test-timedemo-asan: host-asan
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/timedemo.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/timedemo

# Fast monsters after loading a save (tests/skill-load.sh)
.PHONY: test-skill-load test-skill-load-asan
test-unit: test-skill-load
test-asan: test-skill-load-asan

test-skill-load: host
	@sh $(ROOT)/tests/skill-load.sh -bin $(HOST_BUILD)/cgdoom-host

test-skill-load-asan: host-asan
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	 sh $(ROOT)/tests/skill-load.sh -bin $(ASAN_BUILD)/cgdoom-host \
	   -out $(ASAN_BUILD)/tests/skillload

#---
# Engine portability check with the SH compiler (big-endian, 32-bit)
#---

SH_CC       ?= $(ROOT)/toolchain/bin/sh-elf-gcc
SH_CHECK    := $(HOST_BUILD)/check-sh

check-sh:
	@mkdir -p $(SH_CHECK)
	@fail=0; for f in $(ENGINE_SRC); do \
	  $(SH_CC) -m4-nofpu -mb -O2 -std=gnu11 -fno-strict-aliasing -Wall \
	    -I$(ROOT)/src/platform -I$(ROOT)/src/engine \
	    -c $$f -o $(SH_CHECK)/`basename $$f .c`.o || fail=1; \
	done; \
	if [ $$fail = 0 ]; then echo "check-sh: all engine files compile"; \
	else echo "check-sh: FAILED"; exit 1; fi

clean:
	rm -rf $(HOST_BUILD) $(ASAN_BUILD)

#---
# Simulator builds (src/sim/sim.mk and src/sim/cgdoom.mk; optional)
#---

-include $(ROOT)/src/sim/sim.mk
-include $(ROOT)/src/sim/cgdoom.mk

#---
# FixedMul/FixedDiv exactness in the SH simulator (the SH FixedDiv is inline
# asm; tests/fixed_exact.c). FIXED_SIM_PAIRS random pairs, the soft-float
# double reference on one pair out of FIXED_SIM_DOUBLE_EVERY.
#---

FIXED_SIM_PAIRS        ?= 3000000
FIXED_SIM_DOUBLE_EVERY ?= 8
FIXED_SIM_ELF          := $(ROOT)/build-sim/tests/fixed-exact.elf
FIXED_SIM_OBJ          := $(ROOT)/build-sim/tests/fixed_exact.o $(ROOT)/build-sim/tests/m_fixed.o
.PHONY: test-fixed-sim

$(ROOT)/build-sim/tests/fixed_exact.o: $(ROOT)/tests/fixed_exact.c $(ROOT)/src/engine/m_fixed.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -c $< -o $@
$(ROOT)/build-sim/tests/m_fixed.o: $(ROOT)/src/engine/m_fixed.c $(ROOT)/src/engine/m_fixed.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -c $< -o $@
$(FIXED_SIM_ELF): $(FIXED_SIM_OBJ) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(FIXED_SIM_OBJ))

test-fixed-sim: $(FIXED_SIM_ELF)
	$(SIM_RUNNER) $(FIXED_SIM_ELF) $(FIXED_SIM_PAIRS) $(FIXED_SIM_DOUBLE_EVERY)

# The exact division helpers in the SH simulator (tests/div_exact.c; the
# references are libgcc's __sdivsi3_i4i and __divdi3)
DIV_SIM_PAIRS ?= 2000000
DIV_SIM_ELF   := $(ROOT)/build-sim/tests/div-exact.elf
DIV_SIM_OBJ   := $(ROOT)/build-sim/tests/div_exact.o
.PHONY: test-div test-div-sim

$(DIV_SIM_OBJ): $(ROOT)/tests/div_exact.c $(ROOT)/src/engine/m_fixed.h
	@mkdir -p $(dir $@)
	$(SIM_CC) $(SIM_CFLAGS) -Wall -I$(ROOT)/src/platform -I$(ROOT)/src/engine -c $< -o $@
$(DIV_SIM_ELF): $(DIV_SIM_OBJ) $(SIM_RUNTIME_OBJS) $(SIM_DIR)/sim.ld
	$(call sim-link,$@,$(DIV_SIM_OBJ))

test-div-sim: $(DIV_SIM_ELF)
	$(SIM_RUNNER) $(DIV_SIM_ELF) $(DIV_SIM_PAIRS)
