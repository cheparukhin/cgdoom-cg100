/* Host unit test for src/gint/keymaps.c (gint review round 1):
   - gkm_rebind_check(): BACK/HOME (fx-CG50: MENU) cancel a capture, AC/ON and
     the Doom menu key are refused, anything else binds;
   - gkm_apply_engine_hooks(): the Doom menu key (CGD_EscapeKeycode) and the
     save-name ALPHA modifier (CGD_AlphaKeycode) on each model.
   Linked with the engine's cgdoom-kbd.c, which defines both hooks.
   "Keymap.<action>=NONE" (actions unbound with DEL) is the engine's
   CGD_LoadSettings() job since the engine follow-ups (tests/settings_test.c,
   make test-settings); gint's re-apply workaround and its test are gone. */

#include "cgd_gint.h"
#include "cgdoom.h"
#include "cgdoom-kbd.h"
#include <stdio.h>
#include <string.h>

/* The engine headers redefine printf (on-calculator logging) */
#undef printf

static int fails = 0;
#define CHECK(c, ...) do { if(!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while(0)

bool g_is_cg100 = true;
CGDoom_Options CGD_Options;
/* Keys reported by the next scans */
static uint8_t g_scan[12];
void PL_ScanKeyboard(uint8_t s[12]) { memcpy(s, g_scan, 12); }
/* CGD_EscapeKeycode and CGD_AlphaKeycode are defined by the engine
   (cgdoom-kbd.c) */

static void test_rebind(void)
{
	char const *why;
	g_is_cg100 = true;
	CHECK(gkm_rebind_check(0x82, &why) == GKM_BIND_CANCEL, "BACK cancels");
	CHECK(gkm_rebind_check(0x92, &why) == GKM_BIND_CANCEL, "HOME cancels");
	CHECK(gkm_rebind_check(-1, &why) == GKM_BIND_CANCEL, "no key cancels");
	CHECK(gkm_rebind_check(KEYCODE_ACON, &why) == GKM_BIND_RESERVED && why, "AC refused");
	CHECK(gkm_rebind_check(0x71, &why) == GKM_BIND_OK && !why, "SHIFT binds");
	CHECK(gkm_rebind_check(KEYCODE_EXE, &why) == GKM_BIND_OK, "EXE binds");
	CHECK(gkm_rebind_check(0x74, &why) == GKM_BIND_OK, "CG100 DOWN binds");
	/* The engine exports CGD_EscapeKeycode: the menu key is BACK (already
	   a cancel key) and OK is free */
	CHECK(gkm_menu_key_configurable() && gkm_menu_key() == 0x82, "menu key BACK");
	CHECK(gkm_rebind_check(0x84, &why) == GKM_BIND_OK, "OK binds when BACK is the menu key");
	g_is_cg100 = false;
	/* Round 2: EXIT is bound by CG50 presets, so it must be bindable */
	CHECK(gkm_rebind_check(KEYCODE_EXIT, &why) == GKM_BIND_OK && !why, "CG50 EXIT binds");
	CHECK(gkm_rebind_check(KEYCODE_MENU, &why) == GKM_BIND_CANCEL, "MENU cancels");
	CHECK(gkm_rebind_check(KEYCODE_ACON, &why) == GKM_BIND_RESERVED, "CG50 AC refused");
	CHECK(gkm_rebind_check(KEYCODE_SHIFT, &why) == GKM_BIND_OK, "CG50 SHIFT binds");
	g_is_cg100 = true;
}

static void test_hooks(void)
{
	g_is_cg100 = true;
	gkm_apply_engine_hooks();
	CHECK(CGD_EscapeKeycode == 0x82, "escape -> BACK (%02x)", CGD_EscapeKeycode);
	CHECK(CGD_AlphaKeycode == 0x72, "alpha -> CG100 ALPHA (%02x)", CGD_AlphaKeycode);
	g_is_cg100 = false;
	gkm_apply_engine_hooks();
	CHECK(CGD_AlphaKeycode == KEYCODE_ALPHA, "alpha stays on CG50");
	CHECK(CGD_EscapeKeycode == KEYCODE_MENU, "escape stays MENU on CG50");
	g_is_cg100 = true;
}

/* Final review: OK is a second Enter key of the Doom menu on the fx-CG100
   (unless an action is bound to it); EXE still is one */
#define KEY_ENTER_ 13   /* doomdef.h KEY_ENTER */
static int enter_after(int code)
{
	memset(g_scan, 0, sizeof g_scan);
	CGD_UpdateKeyboardState();
	CGD_UpdateKeyboardState();
	if(code >= 0)
		PL_KeySet(g_scan, code);
	CGD_UpdateKeyboardState();
	int r = CGD_KeyWasJustPressed(KEY_ENTER_);
	memset(g_scan, 0, sizeof g_scan);
	CGD_UpdateKeyboardState();
	return r;
}

static void test_enter(void)
{
	int n;
	g_is_cg100 = true;
	CGD_CopyKeymap(CGD_Options.Keymap, gkm_presets(&n)[0].map);
	gkm_apply_engine_hooks();
	CHECK(CGD_EnterKeycode2 == 0x84, "CG100: OK is Enter 2 (%d)", CGD_EnterKeycode2);
	CHECK(enter_after(KEYCODE_EXE), "CG100: EXE is Enter");
	CHECK(enter_after(0x84), "CG100: OK is Enter");
	CHECK(!enter_after(0x82), "CG100: BACK is not Enter");
	CHECK(!enter_after(-1), "no key, no Enter");
	/* OK bound to an action: not Enter any more */
	CGD_Options.Keymap[5] = 0x84;
	CHECK(!enter_after(0x84), "CG100: OK bound to Fire is not Enter");
	CHECK(enter_after(KEYCODE_EXE), "CG100: EXE still Enter");
	CGD_CopyKeymap(CGD_Options.Keymap, gkm_presets(&n)[0].map);

	g_is_cg100 = false;
	CGD_CopyKeymap(CGD_Options.Keymap, gkm_presets(&n)[0].map);
	gkm_apply_engine_hooks();
	CHECK(CGD_EnterKeycode2 == -1, "CG50: no second Enter key");
	CHECK(!enter_after(0x84), "CG50: MENU (0x84) is not Enter");
	CHECK(enter_after(KEYCODE_EXE), "CG50: EXE is Enter");
	g_is_cg100 = true;
	CGD_CopyKeymap(CGD_Options.Keymap, gkm_presets(&n)[0].map);
}

/* Final review: one action per key. gkm_find_binding() finds the older
   binding (main.c clears it); no preset binds a key twice; the reserved
   text names the actual menu key */
static void test_bindings(void)
{
	for(int model = 0; model < 2; model++) {
		g_is_cg100 = model;
		int n;
		gkm_preset_t const *p = gkm_presets(&n);
		for(int k = 0; k < n; k++)
		for(int i = 0; i < CGDOOM_KEYMAP_SIZE; i++)
			CHECK(gkm_find_binding(p[k].map, p[k].map[i], i) < 0,
				"preset %s binds %s twice", p[k].name, gkm_key_name(p[k].map[i]));
	}
	g_is_cg100 = true;
	CGDoom_Keymap m;
	int n;
	CGD_CopyKeymap(m, gkm_presets(&n)[0].map);
	CHECK(gkm_find_binding(m, m[3], 7) == 3, "finds the other entry");
	CHECK(gkm_find_binding(m, m[3], 3) < 0, "skips the edited entry");
	CHECK(gkm_find_binding(m, -1, 0) < 0, "unbound is no binding");
	CHECK(gkm_find_binding(m, 0x92, 0) < 0, "HOME is free");
	/* The reserved-menu branch cannot fire with the engine hook: the
	   menu key is a cancel key on both models */
	char const *why;
	g_is_cg100 = true;
	CHECK(gkm_rebind_check(gkm_menu_key(), &why) == GKM_BIND_CANCEL, "CG100 menu key cancels");
	g_is_cg100 = false;
	CHECK(gkm_rebind_check(gkm_menu_key(), &why) == GKM_BIND_CANCEL, "CG50 menu key cancels");
	g_is_cg100 = true;
}

int main(void)
{
	test_rebind();
	test_hooks();
	test_enter();
	test_bindings();
	printf("%s (%d failures)\n", fails ? "FAILED" : "OK", fails);
	return fails != 0;
}
