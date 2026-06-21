/*
 * Ki Blast Arena - entry point
 *
 * PS3 homebrew port of "Power of Pong": a Dragon Ball-style 1v1 arena fighter
 * mixing pong/paddle mechanics with charged ki energy blasts.
 *
 * Phase 1 (engine bring-up): initialize the PSL1GHT graphics/text/UI stack and
 * render a static "blank arena" frame — the arena box, two placeholder fighters,
 * and a title — then exit cleanly on START or an XMB exit request. No gameplay
 * yet; movement, combat, ki blasts and AI come in later phases (todo/ROADMAP.md).
 */

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <io/pad.h>
#include <sysutil/sysutil.h>

#include <tiny3d.h>
#include <ya2d/ya2d.h>
#include "ttf_render.h"
#include "clay.h"
#include "clay_renderer.h"
#include "clay_nav.h"

#define SCREEN_WIDTH  848
#define SCREEN_HEIGHT 512

/* RGBA (0xRRGGBBAA) */
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_CYAN    0x00FFFFFF
#define COLOR_GRAY    0xA0A0A0FF
#define COLOR_ORANGE  0xFF8800FF
#define COLOR_P1      0x3FA9F5FF   /* player 1 fighter (blue) */
#define COLOR_P2      0xF5503FFF   /* player 2 / CPU fighter (red) */

#define SKY_CLEAR     0xff0E1A2E   /* tiny3d_Clear wants 0xAARRGGBB */
#define ARENA_FILL    0x14243CFF
#define ARENA_BORDER  0x3A5A86FF

static int running = 1;
static padInfo pad_info;
static padData pad_data;
static u32 *ttf_texture = NULL;

static void sys_callback(u64 status, u64 param, void *userdata)
{
	(void)param;
	(void)userdata;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

static void init_fonts(void)
{
	/* PS3 system fonts shipped in /dev_flash (present on real consoles + RPCS3). */
	TTFLoadFont(0, "/dev_flash/data/font/SCE-PS3-SR-R-LATIN2.TTF", NULL, 0);
	TTFLoadFont(1, "/dev_flash/data/font/SCE-PS3-DH-R-CGB.TTF", NULL, 0);
	TTFLoadFont(2, "/dev_flash/data/font/SCE-PS3-SR-R-JPN.TTF", NULL, 0);

	ttf_texture = (u32 *)init_ttf_table((u16 *)ya2d_texturePointer);
	ya2d_texturePointer = ttf_texture;
}

static void init_screen(void)
{
	tiny3d_Init(1024 * 1024);
	ya2d_init();
	init_fonts();
	set_ttf_window(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
	/* Clay UI backend is wired up now (Phase 1) so menus/HUD can build on it later. */
	clay_backend_init(SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* Standard 2D frame setup (clear + alpha/blend + Project2D), mirroring the
 * PS3 template. Leaves the frame ready for ya2d / ttf draws. */
static void begin_2d_frame(void)
{
	tiny3d_Clear(SKY_CLEAR, TINY3D_CLEAR_ALL);
	tiny3d_AlphaTest(1, 0x10, TINY3D_ALPHA_FUNC_GEQUAL);
	tiny3d_BlendFunc(1,
		TINY3D_BLEND_FUNC_SRC_RGB_SRC_ALPHA | TINY3D_BLEND_FUNC_SRC_ALPHA_SRC_ALPHA,
		TINY3D_BLEND_FUNC_DST_RGB_ONE_MINUS_SRC_ALPHA | TINY3D_BLEND_FUNC_DST_ALPHA_ZERO,
		TINY3D_BLEND_RGB_FUNC_ADD | TINY3D_BLEND_ALPHA_FUNC_ADD);
	tiny3d_Project2D();
	reset_ttf_frame();
}

/* Draw the placeholder arena: a bordered playfield with two fighters parked at
 * the left/right boundaries. Stands in for the real 3D arena until Phase 3. */
static void draw_arena(void)
{
	const int margin_x = 80;
	const int top = 150;
	const int bottom = SCREEN_HEIGHT - 90;
	const int w = SCREEN_WIDTH - 2 * margin_x;
	const int h = bottom - top;

	/* Playfield */
	ya2d_drawFillRectZ(margin_x, top, 0, w, h, ARENA_FILL);
	ya2d_drawRectZ(margin_x, top, 0, w, h, ARENA_BORDER);

	/* Center divider */
	ya2d_drawFillRectZ(SCREEN_WIDTH / 2 - 1, top + 8, 0, 2, h - 16, ARENA_BORDER);

	/* Placeholder fighters (paddles) at the arena bounds */
	const int pad_w = 16, pad_h = 90;
	const int pad_y = top + (h - pad_h) / 2;
	ya2d_drawFillRectZ(margin_x + 24, pad_y, 0, pad_w, pad_h, COLOR_P1);
	ya2d_drawFillRectZ(SCREEN_WIDTH - margin_x - 24 - pad_w, pad_y, 0, pad_w, pad_h, COLOR_P2);
}

static void draw_overlay(void)
{
	display_ttf_string(80, 60, "KI BLAST ARENA", COLOR_ORANGE, 0, 30, 40);
	display_ttf_string(82, 105, "PS3 port of Power of Pong", COLOR_GRAY, 0, 14, 20);
	display_ttf_string(80, SCREEN_HEIGHT - 60,
		"Phase 1 - engine bring-up (no gameplay yet)", COLOR_CYAN, 0, 14, 20);
	display_ttf_string(80, SCREEN_HEIGHT - 36,
		"Press START to exit", COLOR_WHITE, 0, 14, 20);
}

/* Edge-triggered START so a held button doesn't immediately re-trigger. */
static int start_pressed(void)
{
	static u32 prev = 0;
	u32 cur = 0;

	ioPadGetInfo(&pad_info);
	if (pad_info.status[0]) {
		ioPadGetData(0, &pad_data);
		cur = pad_data.BTN_START;
	}
	u32 pressed = cur & ~prev;
	prev = cur;
	return pressed != 0;
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	printf("\n=== Ki Blast Arena (Phase 1 bring-up) ===\n");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
	init_screen();
	ioPadInit(7);

	while (running) {
		if (start_pressed())
			running = 0;

		begin_2d_frame();
		draw_arena();
		draw_overlay();
		tiny3d_Flip();

		sysUtilCheckCallback();
	}

	printf("Exiting...\n");
	ya2d_deinit();
	ioPadEnd();
	return 0;
}
