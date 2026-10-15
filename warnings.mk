# Warnings of every build: -Wall -Wextra and the checks the code passes at
# no cost, as errors in the test builds (host, ASan, unit tests, check-sh,
# simulator). Off: unused parameters (callback signatures), unused-but-set
# parameters (CGDoom's "ticks = 0" idiom) and deprecated declarations
# (macOS's SDK deprecates the sprintf that DOOM uses). The calculator build
# (CMakeLists.txt, which parses the CGD_WARN line) drops -Werror: a newer
# compiler's new warning must not stop it; check-sh, with the same compiler,
# catches it here. Included by the root Makefile and src/sim/sim.mk (the
# simulator runtime and the SH4 harness); tests/gint-host/run.sh reads it.
CGD_WARN    := -Wall -Wextra -Wno-unused-parameter -Wno-unused-but-set-parameter \
               -Wno-deprecated-declarations \
               -Wshadow -Wstrict-prototypes -Wimplicit-fallthrough -Wformat=2 \
               -Wvla -Wundef -Wpointer-arith -Wdouble-promotion -Werror
