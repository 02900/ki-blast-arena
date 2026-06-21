/*
 * Ki Blast Arena - entry point
 *
 * PS3 homebrew port of "Power of Pong": a Dragon Ball-style 1v1 arena fighter
 * mixing pong/paddle mechanics with charged ki energy blasts.
 *
 * Phase 3 (arena & rendering): the playfield is now a real world-space arena
 * drawn in perspective. Fighters live at world coordinates (x, 0, z) and are
 * clamped to the original PongPaddle.cs bounds (X +/-12, Z +/-6); they render as
 * depth-scaled billboards over a projected floor grid. A small software pinhole
 * projection (camera lookAt + perspective divide) feeds Tiny3D's 2D primitives,
 * so the camera is fully deterministic; true Tiny3D meshes are a later upgrade.
 *
 * Controls (ported from SimpleController.cs / BalanceOfPower.cs):
 *   Left stick / D-pad ... move fighter 1 (Horizontal -> world X, Vertical -> Z)
 *   Right stick .......... move fighter 2 (local 2P placeholder until the CPU AI)
 *   Cross (X), hold ...... charge ki  (aura)    Circle (O) ... ki blast
 *   Square ............... melee                Start ........ pause
 *   Select + Start ....... quit to XMB
 *
 * Movement uses SimpleController's speed formula; curKi is a fixed placeholder
 * until the ki system goes live in Phase 5. No damage simulation yet.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
#define COLOR_P1      0x3FA9F5FF   /* fighter 1 (blue) */
#define COLOR_P2      0xF5503FFF   /* fighter 2 (red)  */
#define COLOR_SHADOW  0x00000080
#define CHIP_OFF      0x1E2C3CFF
#define CHIP_BORDER   0x405068FF

#define SKY_CLEAR     0xff0E1A2E   /* tiny3d_Clear wants 0xAARRGGBB */

/* Arena world bounds (PongPaddle.cs: X +/-12, Z +/-6). */
#define ARENA_X   12.0f
#define ARENA_Z   6.0f
#define FIGHTER_W 2.4f
#define FIGHTER_H 3.4f
/* Ground footprint half-extents: the fighter's EDGE stops at the arena bound,
 * not its center, so no part of the body pokes past the floor. The billboard is
 * a flat plane facing the camera: it has visible width in X but no depth in Z,
 * so the Z footprint is 0 (the base can reach the front/back border). When real
 * meshes replace the billboards, give Z a real half-depth. */
#define FIGHTER_HALF_X (FIGHTER_W * 0.5f)
#define FIGHTER_HALF_Z 0.0f

/* Movement model (SimpleController.cs). levelPower 1..10; curKi is a placeholder
 * until Phase 5 wires the real ki bar. moveSpeed = 0.2 + levelPower/50, then the
 * per-step translate scales up with current ki. */
#define LEVEL_POWER  5.0f
#define PLACEHOLDER_KI 35.0f
#define STICK_DEAD   32
#define FLASH_FRAMES 12

/* Software camera (deterministic pinhole; no Tiny3D matrices). */
#define FOCAL  520.0f
#define CAM_CX (SCREEN_WIDTH / 2.0f)
#define CAM_CY (SCREEN_HEIGHT / 2.0f)

static int running = 1;
static padInfo pad_info;
static padData pad_data;
static u32 *ttf_texture = NULL;

/* Camera basis (right, up, forward) + eye, computed once. */
static float cam_eye[3]   = { 0.0f, 9.0f, -18.0f };
static float cam_target[3]= { 0.0f, 1.5f,   0.0f };
static float cam_r[3], cam_u[3], cam_f[3];

/* Two fighters at world (x, 0, z). */
static float f1x = -9.0f, f1z = 0.0f;
static float f2x =  9.0f, f2z = 0.0f;

/* P1 action readout state. */
static int moving = 0, charging = 0, blast_flash = 0, melee_flash = 0, paused = 0;

static void sys_callback(u64 status, u64 param, void *userdata)
{
	(void)param; (void)userdata;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

/* --- tiny vector helpers -------------------------------------------------- */
static void v_sub(const float a[3], const float b[3], float o[3])
{
	o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2];
}
static float v_dot(const float a[3], const float b[3])
{
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}
static void v_cross(const float a[3], const float b[3], float o[3])
{
	o[0] = a[1]*b[2] - a[2]*b[1];
	o[1] = a[2]*b[0] - a[0]*b[2];
	o[2] = a[0]*b[1] - a[1]*b[0];
}
static void v_norm(float v[3])
{
	float len = sqrtf(v_dot(v, v));
	if (len > 1e-6f) { v[0]/=len; v[1]/=len; v[2]/=len; }
}

/* Build the right/up/forward basis for a lookAt camera (up = +Y). */
static void camera_setup(void)
{
	const float up[3] = { 0.0f, 1.0f, 0.0f };
	v_sub(cam_target, cam_eye, cam_f); v_norm(cam_f);   /* forward */
	v_cross(up, cam_f, cam_r);         v_norm(cam_r);   /* right   */
	v_cross(cam_f, cam_r, cam_u);                       /* true up */
}

/* Project a world point to screen. Returns 0 if behind the camera; otherwise
 * fills *sx,*sy and *scale (pixels per world unit at that depth). */
static int project(float wx, float wy, float wz, float *sx, float *sy, float *scale)
{
	const float p[3] = { wx, wy, wz };
	float d[3];
	v_sub(p, cam_eye, d);
	float vz = v_dot(d, cam_f);
	if (vz < 0.05f) return 0;
	float vx = v_dot(d, cam_r);
	float vy = v_dot(d, cam_u);
	*sx = CAM_CX + FOCAL * vx / vz;
	*sy = CAM_CY - FOCAL * vy / vz;
	*scale = FOCAL / vz;
	return 1;
}

static void init_fonts(void)
{
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
	clay_backend_init(SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* --- input ---------------------------------------------------------------- */
static float axis(u8 v)
{
	int d = (int)v - 128;
	if (d > -STICK_DEAD && d < STICK_DEAD) return 0.0f;
	return (float)d / 127.0f;
}
static void clampf(float *v, float lo, float hi)
{
	if (*v < lo) *v = lo; else if (*v > hi) *v = hi;
}

/* Apply SimpleController's translation with PongPaddle's clamp. */
static void move_fighter(float *x, float *z, float mx, float mz)
{
	const float moveSpeed = 0.2f + LEVEL_POWER / 50.0f;
	const float step = moveSpeed + (moveSpeed * 0.5f * PLACEHOLDER_KI / 100.0f);
	*x += mx * step;
	*z += mz * step;
	/* Clamp the fighter's footprint edge (not its center) to the arena bounds. */
	clampf(x, -ARENA_X + FIGHTER_HALF_X, ARENA_X - FIGHTER_HALF_X);
	clampf(z, -ARENA_Z + FIGHTER_HALF_Z, ARENA_Z - FIGHTER_HALF_Z);
}

/* Returns 0 to request quit (Select+Start). */
static int update_input(void)
{
	static u32 prev = 0;
	u32 cur = 0;
	int connected;

	ioPadGetInfo(&pad_info);
	connected = pad_info.status[0];
	if (connected) ioPadGetData(0, &pad_data);
	if (!connected) { moving = charging = 0; prev = 0; return 1; }

	if (pad_data.BTN_SELECT && pad_data.BTN_START) return 0;

	cur = (pad_data.BTN_CIRCLE ? 1u : 0u)
	    | (pad_data.BTN_SQUARE ? 2u : 0u)
	    | (pad_data.BTN_START  ? 4u : 0u);
	u32 pressed = cur & ~prev;
	prev = cur;

	if (pressed & 4u) paused = !paused;
	charging = (!paused && pad_data.BTN_CROSS) ? 1 : 0;
	if (!paused && (pressed & 1u)) blast_flash = FLASH_FRAMES;
	if (!paused && (pressed & 2u)) melee_flash = FLASH_FRAMES;
	if (blast_flash > 0) blast_flash--;
	if (melee_flash > 0) melee_flash--;

	if (paused) { moving = 0; return 1; }

	/* Fighter 1: left stick + D-pad. Both axes exactly 0 = no analog data. */
	float m1x = 0.0f, m1z = 0.0f;
	if (!(pad_data.ANA_L_H == 0 && pad_data.ANA_L_V == 0)) {
		m1x = axis(pad_data.ANA_L_H);
		m1z = -axis(pad_data.ANA_L_V);   /* stick up -> +Z (away) */
	}
	if (pad_data.BTN_LEFT)  m1x -= 1.0f;
	if (pad_data.BTN_RIGHT) m1x += 1.0f;
	if (pad_data.BTN_UP)    m1z += 1.0f;
	if (pad_data.BTN_DOWN)  m1z -= 1.0f;
	clampf(&m1x, -1.0f, 1.0f);
	clampf(&m1z, -1.0f, 1.0f);
	moving = (m1x != 0.0f || m1z != 0.0f);
	move_fighter(&f1x, &f1z, m1x, m1z);

	/* Fighter 2: right stick (local 2P placeholder until the CPU AI, Phase 7). */
	float m2x = 0.0f, m2z = 0.0f;
	if (!(pad_data.ANA_R_H == 0 && pad_data.ANA_R_V == 0)) {
		m2x = axis(pad_data.ANA_R_H);
		m2z = -axis(pad_data.ANA_R_V);
	}
	move_fighter(&f2x, &f2z, m2x, m2z);

	return 1;
}

/* --- 3D arena rendering (software-projected, drawn with Tiny3D 2D prims) --- */
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

/* A projected quad (4 world ground points) as a single filled polygon. */
static void floor_quad(void)
{
	float sx[4], sy[4], sc;
	int ok = 1;
	ok &= project(-ARENA_X, 0, -ARENA_Z, &sx[0], &sy[0], &sc);
	ok &= project( ARENA_X, 0, -ARENA_Z, &sx[1], &sy[1], &sc);
	ok &= project( ARENA_X, 0,  ARENA_Z, &sx[2], &sy[2], &sc);
	ok &= project(-ARENA_X, 0,  ARENA_Z, &sx[3], &sy[3], &sc);
	if (!ok) return;

	/* Brighter at the front (near) edge, darker at the back, for depth. */
	const u32 near_c = 0x1C3050FF, far_c = 0x0E1C30FF;
	tiny3d_SetPolygon(TINY3D_QUADS);
	tiny3d_VertexPos(sx[0], sy[0], 0); tiny3d_VertexColor(near_c);
	tiny3d_VertexPos(sx[1], sy[1], 0); tiny3d_VertexColor(near_c);
	tiny3d_VertexPos(sx[2], sy[2], 0); tiny3d_VertexColor(far_c);
	tiny3d_VertexPos(sx[3], sy[3], 0); tiny3d_VertexColor(far_c);
	tiny3d_End();
}

static void floor_grid(void)
{
	float sx, sy, sc, ax, ay, bx, by;
	const u32 line = 0x35587FFF, mid = 0x6FA0D0FF;

	tiny3d_SetPolygon(TINY3D_LINES);
	/* Lines parallel to Z (constant X). */
	for (int i = -12; i <= 12; i += 3) {
		u32 c = (i == 0) ? mid : line;
		if (!project((float)i, 0, -ARENA_Z, &ax, &ay, &sc)) continue;
		if (!project((float)i, 0,  ARENA_Z, &bx, &by, &sc)) continue;
		tiny3d_VertexPos(ax, ay, 0); tiny3d_VertexColor(c);
		tiny3d_VertexPos(bx, by, 0); tiny3d_VertexColor(c);
	}
	/* Lines parallel to X (constant Z). */
	for (int j = -6; j <= 6; j += 3) {
		u32 c = (j == 0) ? mid : line;
		if (!project(-ARENA_X, 0, (float)j, &ax, &ay, &sc)) continue;
		if (!project( ARENA_X, 0, (float)j, &bx, &by, &sc)) continue;
		tiny3d_VertexPos(ax, ay, 0); tiny3d_VertexColor(c);
		tiny3d_VertexPos(bx, by, 0); tiny3d_VertexColor(c);
	}
	(void)sx; (void)sy;
	tiny3d_End();
}

/* Depth-scaled billboard standing on the floor at (wx, wz). */
static void draw_fighter(float wx, float wz, u32 color, int aura)
{
	float bx, by, sc;
	if (!project(wx, 0.0f, wz, &bx, &by, &sc)) return;

	float w = FIGHTER_W * sc;
	float h = FIGHTER_H * sc;
	int rx = (int)(bx - w * 0.5f);
	int ry = (int)(by - h);
	int rw = (int)w;
	int rh = (int)h;

	/* Floor shadow (a flattened ellipse approximated by a thin rect). */
	int shw = (int)(w * 1.05f);
	ya2d_drawFillRectZ((int)(bx - shw * 0.5f), (int)by - 3, 0, shw, 6, COLOR_SHADOW);

	if (aura) {
		ya2d_drawRectZ(rx - 3, ry - 3, 0, rw + 6, rh + 6, COLOR_ORANGE);
		ya2d_drawRectZ(rx - 2, ry - 2, 0, rw + 4, rh + 4, COLOR_ORANGE);
	}
	ya2d_drawFillRectZ(rx, ry, 0, rw, rh, color);
	ya2d_drawRectZ(rx, ry, 0, rw, rh, COLOR_WHITE);
	/* A "head" cap so the billboard reads as a fighter, not a slab. */
	int hw = rw / 2;
	ya2d_drawFillRectZ((int)(bx - hw * 0.5f), ry - hw, 0, hw, hw, color);
}

static void draw_arena(void)
{
	floor_quad();
	floor_grid();

	/* Painter's algorithm: draw the farther fighter first. Depth = forward dot. */
	float d1[3], d2[3], p1[3] = { f1x, 0, f1z }, p2[3] = { f2x, 0, f2z };
	v_sub(p1, cam_eye, d1);
	v_sub(p2, cam_eye, d2);
	float vz1 = v_dot(d1, cam_f), vz2 = v_dot(d2, cam_f);

	if (vz1 >= vz2) {
		draw_fighter(f1x, f1z, COLOR_P1, charging);
		draw_fighter(f2x, f2z, COLOR_P2, 0);
	} else {
		draw_fighter(f2x, f2z, COLOR_P2, 0);
		draw_fighter(f1x, f1z, COLOR_P1, charging);
	}
}

/* --- HUD ------------------------------------------------------------------ */
static void draw_chip(int x, int y, int w, const char *label, int active, u32 on_color)
{
	u32 fill = active ? on_color : CHIP_OFF;
	ya2d_drawFillRectZ(x, y, 0, w, 28, fill);
	ya2d_drawRectZ(x, y, 0, w, 28, active ? on_color : CHIP_BORDER);
	display_ttf_string(x + 9, y + 6, label, active ? 0x000000FF : COLOR_GRAY, 0, 11, 17);
}

static void draw_hud(void)
{
	display_ttf_string(28, 24, "KI BLAST ARENA", COLOR_ORANGE, 0, 24, 34);
	display_ttf_string(30, 62, "Phase 3 - world-space arena (X +/-12, Z +/-6)", COLOR_GRAY, 0, 12, 17);

	const int y = SCREEN_HEIGHT - 64;
	int x = 28;
	draw_chip(x, y, 80, "MOVE",   moving,          COLOR_P1);     x += 90;
	draw_chip(x, y, 94, "CHARGE", charging,        COLOR_ORANGE); x += 104;
	draw_chip(x, y, 84, "BLAST",  blast_flash > 0, COLOR_CYAN);   x += 94;
	draw_chip(x, y, 84, "MELEE",  melee_flash > 0, COLOR_GREEN);  x += 94;
	draw_chip(x, y, 84, "PAUSE",  paused,          COLOR_WHITE);

	display_ttf_string(28, SCREEN_HEIGHT - 28,
		"L-stick: P1   R-stick: P2   X: charge   O: blast   Sq: melee   Start: pause   Sel+Start: quit",
		COLOR_GRAY, 0, 11, 15);

	if (paused)
		display_ttf_string(SCREEN_WIDTH / 2 - 55, 250, "PAUSED", COLOR_WHITE, 0, 26, 38);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	printf("\n=== Ki Blast Arena (Phase 3 arena) ===\n");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
	init_screen();
	ioPadInit(7);
	camera_setup();

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
