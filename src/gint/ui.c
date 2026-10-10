/* CGDoom gint platform: launcher drawing helpers.

   A port of the spirit of CGDoom's immediate-mode layout (cgdoom-ui.c) to
   gint's renderer on the full 396x224 panel: a title bar, a list of
   focusable "label ... value" lines with scrolling, dimmed notes and a
   footer. Drawing goes to gint's VRAM; every screen starts with
   gvid_release() (frame DMA finished, full LCD window) and ends with
   dupdate(). Keys come from input.c (raw scans, never gint's getkey). */

#include "cgd_gint.h"

#include <gint/display.h>
#include <stdio.h>
#include <string.h>

#define UI_W        DWIDTH
#define UI_H        DHEIGHT
#define TITLE_H     18
#define MARGIN_X    12

static int g_lh = 12;      /* line height of items */
static int g_fh = 9;       /* font height */

void ui_init(void)
{
	font_t const *f = dfont_default();
	if(f && f->line_height > 0)
		g_fh = f->line_height;
	g_lh = g_fh + 4;
	if(g_lh < 12)
		g_lh = 12;
}

int ui_line_height(void)
{
	return g_lh;
}

void ui_begin(void)
{
	gvid_release();
	dclear(UI_BG);
}

void ui_end(void)
{
	gvid_release();
	dupdate();
}

void ui_text(int x, int y, int color, int halign, char const *str)
{
	dtext_opt(x, y, color, C_NONE, halign, DTEXT_TOP, str, -1);
}

void ui_printf(int x, int y, int color, int halign, char const *fmt, ...)
{
	char buf[160];
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf, sizeof buf, fmt, args);
	va_end(args);
	ui_text(x, y, color, halign, buf);
}

static void title_bar(char const *title)
{
	drect(0, 0, UI_W - 1, TITLE_H - 1, UI_TITLE);
	if(title)
		ui_text(UI_W / 2, (TITLE_H - g_fh) / 2, UI_BG, DTEXT_CENTER, title);
}

static void footer(char const *text)
{
	if(!text)
		return;
	int y = UI_H - g_lh;
	drect(0, y - 2, UI_W - 1, UI_H - 1, UI_HEAD);
	ui_text(UI_W / 2, y, UI_BG, DTEXT_CENTER, text);
}

int ui_wrap(int x, int y, int w, int color, char const *text)
{
	font_t const *f = dfont_default();
	while(text && *text) {
		/* Longest prefix ending at a word boundary that fits */
		int best = 0, i = 0;
		while(text[i] && text[i] != '\n') {
			int j = i;
			while(text[j] && text[j] != ' ' && text[j] != '\n')
				j++;
			int tw, th;
			dnsize(text, j, f, &tw, &th);
			if(tw > w && best > 0)
				break;
			best = j;
			if(tw > w)
				break;
			i = j;
			while(text[i] == ' ')
				i++;
		}
		if(best == 0) {
			/* Single word too long: hard cut */
			best = 1;
			int tw, th;
			while(text[best] && text[best] != '\n') {
				dnsize(text, best + 1, f, &tw, &th);
				if(tw > w)
					break;
				best++;
			}
		}
		dtext_opt(x, y, color, C_NONE, DTEXT_LEFT, DTEXT_TOP, text, best);
		y += g_fh + 3;
		text += best;
		while(*text == ' ')
			text++;
		if(*text == '\n')
			text++;
	}
	return y;
}

//---
// Layout
//---

static int top_y(void)    { return TITLE_H + 6; }
static int bottom_y(void) { return UI_H - g_lh - 4; }

void ui_layout_init(ui_layout_t *l)
{
	memset(l, 0, sizeof *l);
	l->focus = 0;
}

void ui_frame_start(ui_layout_t *l, char const *title)
{
	ui_begin();

	/* Scroll so that the focused item (position from the previous frame)
	   is visible */
	if(l->count > 0 && l->focus >= 0 && l->focus < UI_MAX_ITEMS) {
		int fy = l->item_y[l->focus];
		int visible = bottom_y() - top_y() - g_lh;
		if(fy - l->scroll > top_y() + visible)
			l->scroll = fy - top_y() - visible;
		if(fy - l->scroll < top_y())
			l->scroll = fy - top_y();
		if(l->focus == 0)
			l->scroll = 0;
		if(l->scroll < 0)
			l->scroll = 0;
	}

	l->y = top_y();
	l->count = 0;
	title_bar(title);
}

static bool row_visible(int y)
{
	return y >= top_y() - 2 && y + g_lh <= bottom_y() + 2;
}

void ui_frame_end(ui_layout_t *l, char const *footer_text)
{
	if(l->focus >= l->count)
		l->focus = l->count ? l->count - 1 : 0;

	/* Scroll indicators */
	if(l->scroll > 0)
		ui_text(UI_W - 6, top_y() - 2, UI_DIM, DTEXT_RIGHT, "^");
	if(l->y - l->scroll > bottom_y())
		ui_text(UI_W - 6, bottom_y() - g_fh, UI_DIM, DTEXT_RIGHT, "v");

	drect(0, bottom_y() + 2, UI_W - 1, UI_H - 1, UI_BG);
	footer(footer_text);
	ui_end();
}

int ui_item(ui_layout_t *l, char const *label, char const *fmt, ...)
{
	int i = l->count++;
	if(i < UI_MAX_ITEMS)
		l->item_y[i] = l->y;

	int y = l->y - l->scroll;
	if(row_visible(y)) {
		bool focused = (i == l->focus);
		if(focused)
			drect(4, y - 2, UI_W - 5, y + g_lh - 3, UI_HEAD);
		int fg = focused ? UI_BG : UI_FG;
		ui_text(MARGIN_X, y, fg, DTEXT_LEFT, label);

		if(fmt && fmt[0]) {
			char buf[96];
			va_list args;
			va_start(args, fmt);
			vsnprintf(buf, sizeof buf, fmt, args);
			va_end(args);
			ui_text(UI_W - MARGIN_X, y, fg, DTEXT_RIGHT, buf);
		}
	}
	l->y += g_lh;
	return i;
}

void ui_note(ui_layout_t *l, int color, char const *fmt, ...)
{
	int y = l->y - l->scroll;
	if(row_visible(y)) {
		char buf[128];
		va_list args;
		va_start(args, fmt);
		vsnprintf(buf, sizeof buf, fmt, args);
		va_end(args);
		ui_text(MARGIN_X + 8, y, color, DTEXT_LEFT, buf);
	}
	l->y += g_lh;
}

void ui_space(ui_layout_t *l, int pixels)
{
	l->y += pixels;
}

bool ui_nav(ui_layout_t *l, int key)
{
	if(l->count <= 0)
		return false;
	if(key == GK_UP) {
		l->focus = (l->focus > 0) ? l->focus - 1 : l->count - 1;
		return true;
	}
	if(key == GK_DOWN) {
		l->focus = (l->focus < l->count - 1) ? l->focus + 1 : 0;
		return true;
	}
	return false;
}

//---
// Full-screen messages
//---

void ui_status(char const *title, char const *line1, char const *line2)
{
	ui_begin();
	title_bar(title);
	int y = UI_H / 2 - g_lh;
	if(line1)
		ui_text(UI_W / 2, y, UI_FG, DTEXT_CENTER, line1);
	if(line2)
		ui_text(UI_W / 2, y + g_lh + 2, UI_DIM, DTEXT_CENTER, line2);
	ui_end();
}

static void message_draw(char const *title, char const *text, bool prompt)
{
	ui_begin();
	title_bar(title);
	ui_wrap(MARGIN_X, top_y() + 4, UI_W - 2 * MARGIN_X, UI_FG, text);
	if(prompt)
		footer("Press EXE / OK");
	ui_end();
}

void ui_message(char const *title, char const *text, bool wait)
{
	/* Redrawn when ginp_getkey() returns GK_NONE (the idle hook drew
	   over the screen) */
	int k = GK_NONE;
	do {
		if(k == GK_NONE)
			message_draw(title, text, wait);
		if(!wait)
			return;
		k = ginp_getkey();
	}
	while(k != GK_OK && k != GK_BACK && k != GK_QUIT);
}

void ui_message_timeout(char const *title, char const *text, int timeout_ms)
{
	/* For the screens shown with the idle hook off (exit path): the add-in
	   owns the interrupts, so an unbounded wait here would keep the
	   calculator on until the battery is flat (gint review round 2) */
	message_draw(title, text, true);
	uint32_t start = PL_GetMicros();
	uint32_t limit = (uint32_t)timeout_ms * 1000u;
	while(1) {
		uint32_t elapsed = PL_GetMicros() - start;
		if(elapsed >= limit)
			return;
		int k = ginp_getkey_timeout((int)((limit - elapsed) / 1000u) + 1);
		if(k == GK_OK || k == GK_BACK || k == GK_QUIT)
			return;
	}
}

bool ui_confirm(char const *title, char const *text)
{
	int k = GK_NONE;
	while(1) {
		if(k == GK_NONE) {
			ui_begin();
			title_bar(title);
			ui_wrap(MARGIN_X, top_y() + 4, UI_W - 2 * MARGIN_X, UI_FG,
				text);
			footer(g_is_cg100 ? "OK/EXE: yes    BACK: no"
				: "EXE: yes    EXIT: no");
			ui_end();
		}
		k = ginp_getkey();
		if(k == GK_OK)
			return true;
		if(k == GK_BACK || k == GK_QUIT)
			return false;
	}
}
