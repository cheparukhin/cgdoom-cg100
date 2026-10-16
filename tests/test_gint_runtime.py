"""Behavioral tests of the runtime built by tools/build-toolchain.sh gint.

No firmware or hardware is required. The build is a prerequisite so this tests
the exported pinned fork code rather than a duplicate implementation.
"""
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path(os.environ.get(
    "CGDOOM_GINT_SOURCE", ROOT / "toolchain-build/gint-cgdoom-src"))
DISPLAY_SOURCE = SOURCE / "src/r61524/r61524.c"


def callback(source, name):
    """Use the exported implementation, not a duplicate of the guard logic."""
    matches = re.findall(
        rf"^static void {name}\([^\n]+\)\n\{{\n.*?^\}}", source, re.M | re.S)
    if len(matches) != 1:
        raise AssertionError(f"Expected exactly one exported {name} callback")
    return matches[0]


HOST_SUPPORT = r"""
#include <assert.h>
#include <stdint.h>

typedef struct { uint16_t HSA, HEA, VSA, VEA; } r61524_state_t;
static uint16_t lcd_version;
static unsigned reads, writes;
static r61524_state_t hardware;

static uint16_t r61524_get_version(void) { return lcd_version; }
static void r61524_win_get(uint16_t *a, uint16_t *b,
                          uint16_t *c, uint16_t *d)
{
    reads++;
    *a = hardware.HSA; *b = hardware.HEA;
    *c = hardware.VSA; *d = hardware.VEA;
}
static void r61524_win_set(uint16_t a, uint16_t b, uint16_t c, uint16_t d)
{
    writes++;
    hardware = (r61524_state_t){a, b, c, d};
}
static void assert_state(r61524_state_t a, r61524_state_t b)
{
    assert(a.HSA == b.HSA && a.HEA == b.HEA);
    assert(a.VSA == b.VSA && a.VEA == b.VEA);
}
"""

HOST_MAIN = r"""
int main(void)
{
    const r61524_state_t os_window = {6, 389, 0, 215};
    const r61524_state_t addin_window = {38, 357, 12, 211};
    const r61524_state_t sentinel = {0x1111, 0x2222, 0x3333, 0x4444};
    const uint16_t new_lcds[] = {0x32, 0x52};
    for(unsigned i = 0; i < sizeof new_lcds / sizeof *new_lcds; i++) {
        lcd_version = new_lcds[i];
        reads = writes = 0;
        hardware = os_window;
        r61524_state_t state = sentinel;
        for(int transition = 0; transition < 3; transition++) {
            hsave(&state);
            hrestore(&state);
        }
        assert(reads == 0 && writes == 0);
        assert_state(state, sentinel);
        assert_state(hardware, os_window);
    }

    const uint16_t old_lcds[] = {0, 0x24, 0xffff};
    for(unsigned i = 0; i < sizeof old_lcds / sizeof *old_lcds; i++) {
        lcd_version = old_lcds[i];
        reads = writes = 0;
        r61524_state_t os_state = sentinel, addin_state = sentinel;
        hardware = os_window;
        hsave(&os_state);
        assert_state(os_state, os_window);
        hardware = addin_window;
        hsave(&addin_state);
        assert_state(addin_state, addin_window);
        hrestore(&os_state);
        assert_state(hardware, os_window);
        hrestore(&addin_state);
        assert_state(hardware, addin_window);
        assert(reads == 2 && writes == 2);
    }
    return 0;
}
"""


@unittest.skipUnless(shutil.which("cc"), "Host C compiler unavailable")
class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not DISPLAY_SOURCE.is_file():
            raise AssertionError("Run tools/build-toolchain.sh gint before runtime tests")

    def test_kernel_selects_exact_os_before_startup(self):
        source = (SOURCE / "src/kernel/kernel.c").read_text()
        start = source.index('	char *version = (void *)0x80020020;')
        end = source.index('	#endif', start)
        selection = source[start:end].replace(
            '	char *version = (void *)0x80020020;', '')
        harness = r"""
#include <assert.h>
#include <string.h>
#define HWCALC 0
#define HWCALC_FXCG100 9
int gint[1];
int selected;
void gint_set_CASIOWIN_API(int api) { selected = api; }
void select_os(char const *version) {
""" + selection + r"""
}
int main(void) {
    gint[HWCALC] = HWCALC_FXCG100;
    select_os("02.00.0000"); assert(selected == 2);
    select_os("02.20.0000"); assert(selected == 3);
    select_os("02.00.0010"); assert(selected == 0);
    select_os("02.20.0010"); assert(selected == 0);
    select_os("02.10.0000"); assert(selected == 0);
    select_os("03.00.0000"); assert(selected == 0);
    gint[HWCALC] = 5;
    select_os("02.00.0000"); assert(selected == 0);
    select_os("02.20.0000"); assert(selected == 0);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "kernel.c"
            exe = Path(tmp) / "kernel"
            path.write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            path, "-o", exe], check=True)
            subprocess.run([exe], check=True)

    def test_exported_callbacks_preserve_old_lcd_and_avoid_new_lcd_commands(self):
        source = DISPLAY_SOURCE.read_text()
        harness = "\n".join([HOST_SUPPORT, callback(source, "hsave"),
                              callback(source, "hrestore"), HOST_MAIN])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test-display.c"
            exe = Path(tmp) / "test-display"
            path.write_text(harness)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            path, "-o", exe], check=True)
            subprocess.run([exe], check=True)


if __name__ == "__main__":
    unittest.main()
