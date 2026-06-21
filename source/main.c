/*
 * Ki Blast Arena - entry point
 *
 * PS3 homebrew port of "Power of Pong": a Dragon Ball-style 1v1 arena fighter
 * mixing pong/paddle mechanics with charged ki energy blasts.
 *
 * Phase 2 (input): DualShock3 via the PSL1GHT pad API. The player-1 fighter
 * moves with the left stick / D-pad, clamped to the arena bounds, and an
 * on-screen readout reacts to the action buttons. Control map ported from the
 * Unity originals (SimpleController.cs / BalanceOfPower.cs):
 *
 *   Left stick / D-pad ... move (Horizontal/Vertical axes)
 *   Cross (X), hold ...... charge ki      ("X PO"  GetButton)
 *   Circle (O) ........... launch ki blast ("O PO" GetButtonDown)
 *   Square ............... melee attack    ("4 PO" GetButtonDown)
 *   Start ................ pause (toggle)
 *   Select + Start ....... quit to XMB
 *
 * Movement/combat are only visualized here (chips light up, fighter slides).
 * The real ki/health/damage simulation lands in Phases 4-5 (todo/ROADMAP.md).
 */

#include <stdio.h>
#include <stdlib.h>
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
#define COLOR_GREEN   0x46D45AFF
#define COLOR_P1      0x3FA9F5FF   /* player 1 fighter (blue) */
#define COLOR_P2      0xF5503FFF   /* player 2 / CPU fighter (red) */

#define SKY_CLEAR     0xff0E1A2E   /* tiny3d_Clear wants 0xAARRGGBB */
#define ARENA_FILL    0x14243CFF
#define ARENA_BORDER  0x3A5A86FF
#define CHIP_OFF      0x1E2C3CFF
#define CHIP_BORDER   0x405068FF

/* Arena playfield rectangle (screen space). The original world bounds are
 * X +/-12, Z +/-6 (PongPaddle.cs); here we clamp the fighter to this box and
 * map the real world-space arena in Phase 3. */
#define AR_LEFT    80
#define AR_TOP     110
#define AR_RIGHT   (SCREEN_WIDTH - 80)
#define AR_BOTTOM  382
#define AR_INSET   10

#define PAD_W  16
#define PAD_H  84
#define MOVE_SPEED   4.0f   /* px/frame; ki-scaled speed comes with the ki system */
#define STICK_DEAD   32     /* analog deadzone (0..127 from center) */
#define FLASH_FRAMES 12     /* how long a tap lights its chip */

static int running = 1;
static padInfo pad_info;
static padData pad_data;
static u32 *ttf_texture = NULL;

/* Fighter + action state driven by the pad. */
static float p1x, p1y;          /* player-1 fighter top-left (screen space) */
static int moving = 0;          /* stick/d-pad deflected this frame */
static int charging = 0;        /* Cross held */
static int blast_flash = 0;     /* Circle tapped (counts down) */
static int melee_flash = 0;     /* Square tapped (counts down) */
static int paused = 0;          /* Start toggles */

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
	/* Clay UI backend is wired up (Phase 1) so menus/HUD can build on it later. */
	clay_backend_init(SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* Map an analog axis byte (0..255, center 128) to -1..1 with a deadzone. */
static float axis(u8 v)
{
	int d = (int)v - 128;
	if (d > -STICK_DEAD && d < STICK_DEAD)
		return 0.0f;
	return (float)d / 127.0f;
}

static void clampf(float *v, float lo, float hi)
{
	if (*v < lo) *v = lo;
	else if (*v > hi) *v = hi;
}

/* Read the pad and update fighter position + action flags.
 * Returns 0 to request quit (Select+Start). */
static int update_input(void)
{
	static u32 prev = 0;   /* for edge-detected actions */
	u32 cur = 0;
	int connected;

	ioPadGetInfo(&pad_info);
	connected = pad_info.status[0];
	if (connected)
		ioPadGetData(0, &pad_data);

	if (!connected) {
		moving = charging = 0;
		prev = 0;
		return 1;
	}

	/* Quit combo takes priority so it never doubles as a pause toggle. */
	if (pad_data.BTN_SELECT && pad_data.BTN_START)
		return 0;

	cur = (pad_data.BTN_CIRCLE ? 1u : 0u)
	    | (pad_data.BTN_SQUARE ? 2u : 0u)
	    | (pad_data.BTN_START  ? 4u : 0u);
	u32 pressed = cur & ~prev;
	prev = cur;

	if (pressed & 4u) paused = !paused;

	/* Action readout (only while running). */
	charging = (!paused && pad_data.BTN_CROSS) ? 1 : 0;
	if (!paused && (pressed & 1u)) blast_flash = FLASH_FRAMES;
	if (!paused && (pressed & 2u)) melee_flash = FLASH_FRAMES;
	if (blast_flash > 0) blast_flash--;
	if (melee_flash > 0) melee_flash--;

	/* Movement: left stick + D-pad, clamped to the arena box. */
	float mx = 0.0f, my = 0.0f;
	if (!paused) {
		/* Both axes exactly 0 means "no analog data yet" (digital pad / not
		 * read), NOT full up-left -> ignore to avoid phantom drift. */
		if (!(pad_data.ANA_L_H == 0 && pad_data.ANA_L_V == 0)) {
			mx = axis(pad_data.ANA_L_H);
			my = axis(pad_data.ANA_L_V);
		}
		if (pad_data.BTN_LEFT)  mx -= 1.0f;
		if (pad_data.BTN_RIGHT) mx += 1.0f;
		if (pad_data.BTN_UP)    my -= 1.0f;
		if (pad_data.BTN_DOWN)  my += 1.0f;
		clampf(&mx, -1.0f, 1.0f);
		clampf(&my, -1.0f, 1.0f);
	}
	moving = (mx != 0.0f || my != 0.0f);

	p1x += mx * MOVE_SPEED;
	p1y += my * MOVE_SPEED;
	clampf(&p1x, AR_LEFT + AR_INSET, AR_RIGHT - AR_INSET - PAD_W);
	clampf(&p1y, AR_TOP + AR_INSET, AR_BOTTOM - AR_INSET - PAD_H);

	return 1;
}

/* Standard 2D frame setup (clear + alpha/blend + Project2D). */
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

static void draw_arena(void)
{
	const int w = AR_RIGHT - AR_LEFT;
	const int h = AR_BOTTOM - AR_TOP;

	ya2d_drawFillRectZ(AR_LEFT, AR_TOP, 0, w, h, ARENA_FILL);
	ya2d_drawRectZ(AR_LEFT, AR_TOP, 0, w, h, ARENA_BORDER);
	ya2d_drawFillRectZ(SCREEN_WIDTH / 2 - 1, AR_TOP + 8, 0, 2, h - 16, ARENA_BORDER);

	/* Player 1 (moves with the pad); a charge "aura" outline when holding Cross. */
	ya2d_drawFillRectZ((int)p1x, (int)p1y, 0, PAD_W, PAD_H, COLOR_P1);
	if (charging) {
		ya2d_drawRectZ((int)p1x - 3, (int)p1y - 3, 0, PAD_W + 6, PAD_H + 6, COLOR_ORANGE);
		ya2d_drawRectZ((int)p1x - 2, (int)p1y - 2, 0, PAD_W + 4, PAD_H + 4, COLOR_ORANGE);
	}

	/* Player 2 / CPU placeholder, parked at the right boundary (static for now). */
	const int p2x = AR_RIGHT - AR_INSET - 24 - PAD_W;
	const int p2y = AR_TOP + (h - PAD_H) / 2;
	ya2d_drawFillRectZ(p2x, p2y, 0, PAD_W, PAD_H, COLOR_P2);
}

/* A labelled status "chip" that lights up when its action is active. */
static void draw_chip(int x, int y, int w, const char *label, int active, u32 on_color)
{
	u32 fill = active ? on_color : CHIP_OFF;
	ya2d_drawFillRectZ(x, y, 0, w, 30, fill);
	ya2d_drawRectZ(x, y, 0, w, 30, active ? on_color : CHIP_BORDER);
	display_ttf_string(x + 10, y + 7, label, active ? 0x000000FF : COLOR_GRAY, 0, 12, 18);
}

static void draw_hud(void)
{
	display_ttf_string(80, 28, "KI BLAST ARENA", COLOR_ORANGE, 0, 26, 36);
	display_ttf_string(82, 70, "Phase 2 - input test (move with stick / D-pad)", COLOR_GRAY, 0, 13, 18);

	const int y = AR_BOTTOM + 14;
	int x = AR_LEFT;
	draw_chip(x, y, 86, "MOVE",   moving,           COLOR_P1);     x += 96;
	draw_chip(x, y, 100, "CHARGE", charging,         COLOR_ORANGE); x += 110;
	draw_chip(x, y, 90, "BLAST",  blast_flash > 0,  COLOR_CYAN);   x += 100;
	draw_chip(x, y, 90, "MELEE",  melee_flash > 0,  COLOR_GREEN);  x += 100;
	draw_chip(x, y, 90, "PAUSE",  paused,           COLOR_WHITE);

	display_ttf_string(80, SCREEN_HEIGHT - 34,
		"Cross: charge   Circle: blast   Square: melee   Start: pause   Select+Start: quit",
		COLOR_GRAY, 0, 12, 16);

	if (paused)
		display_ttf_string(SCREEN_WIDTH / 2 - 60, AR_TOP + (AR_BOTTOM - AR_TOP) / 2 - 18,
			"PAUSED", COLOR_WHITE, 0, 28, 40);
}

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	printf("\n=== Ki Blast Arena (Phase 2 input test) ===\n");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
	init_screen();
	ioPadInit(7);

	/* Start the fighter near the left boundary, vertically centered. */
	p1x = AR_LEFT + AR_INSET + 24;
	p1y = AR_TOP + ((AR_BOTTOM - AR_TOP) - PAD_H) / 2;

	while (running) {
		if (!update_input())
			running = 0;

		begin_2d_frame();
		draw_arena();
		draw_hud();
		tiny3d_Flip();

		sysUtilCheckCallback();
	}

	printf("Exiting...\n");
	ya2d_deinit();
	ioPadEnd();
	return 0;
}
