/*
 * Ki Blast Arena - entry point
 *
 * PS3 homebrew port of "Power of Pong": a Dragon Ball-style 1v1 arena fighter
 * mixing pong/paddle mechanics with charged ki energy blasts.
 *
 * Phase 4 (core combat): port of BalanceOfPower.cs. The fight is a power
 * tug-of-war, not a health-to-zero race: a single balance bar starts at 50; a
 * melee hit TRANSFERS power (attacker +1.5*levelPower, opponent -the same) and
 * grants the attacker +7 ki. Fill the bar to 100 to win the round; first to 3
 * round wins takes the match. Ki charges while holding the charge button (faster
 * when standing still) and drains slowly otherwise. Collisions are hand-rolled
 * (XZ distance), no Rigidbody. Ki blasts (Projectile.cs) come in Phase 5.
 *
 * Two controllers: pad on port 0 drives P1, port 1 drives P2 (same layout each).
 * Controls (per pad):
 *   Left stick / D-pad ... move      hold Cross ... charge ki
 *   Circle ............... fire ki blast (spends 30/60/80 ki -> tier 1/2/3)
 *   Square ............... melee (in contact)
 *   Start ................ pause (in fight) / rematch (match over)
 *   Select + Start ....... quit to XMB
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
#include "audio.h"

#define SCREEN_WIDTH  848
#define SCREEN_HEIGHT 512

/* RGBA (0xRRGGBBAA) */
#define COLOR_WHITE   0xFFFFFFFF
#define COLOR_CYAN    0x00FFFFFF
#define COLOR_GRAY    0xA0A0A0FF
#define COLOR_ORANGE  0xFF8800FF
#define COLOR_YELLOW  0xFFD23FFF
#define COLOR_P1      0x3FA9F5FF   /* fighter 1 (blue) */
#define COLOR_P2      0xF5503FFF   /* fighter 2 (red)  */
#define COLOR_SHADOW  0x00000080
#define BAR_BG        0x0A1420FF
#define BAR_BORDER    0x405068FF

#define SKY_CLEAR     0xff0E1A2E   /* tiny3d_Clear wants 0xAARRGGBB */

/* Arena world bounds (PongPaddle.cs: X +/-12, Z +/-6). */
#define ARENA_X   12.0f
#define ARENA_Z   6.0f
#define FIGHTER_W 2.4f
#define FIGHTER_H 3.4f
/* Billboard is flat in Z, so only X gets a footprint half-extent. */
#define FIGHTER_HALF_X (FIGHTER_W * 0.5f)
#define FIGHTER_HALF_Z 0.0f

/* Combat model (BalanceOfPower.cs). Each fighter's strength comes from its
 * selected character's levelPower (1..10); see p1_power / p2_power. */
#define DEFAULT_POWER 5.0f
#define BAL_MAX       100.0f
#define BAL_START     50.0f
#define KI_MAX        100.0f
#define KI_MIN        1.0f
#define KI_START      35.0f
#define MELEE_KIGAIN  7.0f
#define MELEE_RANGE   3.4f      /* center-to-center XZ contact distance */
#define MELEE_CD      14        /* frames between melee hits */
#define KNOCKBACK     1.6f      /* world units pushed on hit */
#define ROUND_BANNER  110       /* frames the round-over banner shows */
#define FRAME_DT      (1.0f / 60.0f)

/* Ki blasts (Projectile.cs / LaunchKiBlast). Circle fires the highest tier the
 * current ki affords; the blast flies straight toward where the opponent was and
 * transfers power on hit (like melee, but stronger and ranged). */
#define KI_COST1      30.0f
#define KI_COST2      60.0f
#define KI_COST3      80.0f
#define BLAST_SPEED   11.0f     /* world units/sec */
#define BLAST_LIFE    240       /* frames (~4s, matches Projectile.lifeTime) */
#define BLAST_Y       1.7f      /* chest height */
#define BLAST_OFFSET  1.5f      /* spawn this far in front of the shooter */
#define BLAST_HIT     1.9f      /* hit radius vs the opponent (XZ) */
#define BLAST_DMGBASE 6.0f      /* transfer = DMGBASE*tier + levelPower*1.25 */
#define MAX_BLASTS    12
#define MAX_EXPL      12
#define EXPL_LIFE     18
/* Explosion knockback (Projectile.cs OverlapSphere(radius 5) on impact). The
 * original pushed every rigidbody toward the world origin; we push radially out
 * from the blast (intuitive), scaled by proximity. */
#define EXPL_KNOCK_RADIUS 5.0f
#define EXPL_KNOCK_FORCE  2.2f   /* max world units pushed at the blast centre */

/* CPU AI (CPUController.cs / FMS.cs). Difficulty scales with the CPU's character
 * power (p2_power); the rates are computed at runtime (see cpu_*_rate below). */
#define CPU_SIGHT      10.0f                           /* chase within this   */

#define STICK_DEAD    32

/* Software camera (deterministic pinhole). */
#define FOCAL  520.0f
#define CAM_CX (SCREEN_WIDTH / 2.0f)
#define CAM_CY (SCREEN_HEIGHT / 2.0f)

static int running = 1;
static u32 *ttf_texture = NULL;

/* Camera basis. */
static float cam_eye[3]    = { 0.0f, 9.0f, -18.0f };
static float cam_target[3] = { 0.0f, 1.5f,   0.0f };
static float cam_r[3], cam_u[3], cam_f[3];

/* Fighters at world (x, 0, z). */
static float f1x = -9.0f, f1z = 0.0f;
static float f2x =  9.0f, f2z = 0.0f;

/* Combat state. */
static float balance;            /* 0..100; >=100 P1 wins, <=0 P2 wins */
static float ki1, ki2;           /* per-fighter ki 0..100 */
static int   score1, score2;
static int   melee_cd1, melee_cd2;
static int   charging1, charging2;
static int   moving1, moving2;
static int   p1_conn, p2_conn;   /* controllers present on ports 0 / 1 */

/* Ki blast projectiles + explosion effects (simple fixed pools). */
typedef struct { int active; float x, y, z, vx, vz; int owner, tier, life; } Blast;
typedef struct { int active; float x, y, z; int owner, t; } Expl;
static Blast blasts[MAX_BLASTS];
static Expl  expls[MAX_EXPL];

enum { GS_FIGHT, GS_ROUND, GS_MATCH };
static int gstate;
static int banner_timer;
static int round_winner;         /* 1 or 2 */
static int match_winner;
static int paused;

/* CPU AI state — drives fighter 2 when no second controller is present. */
enum { CPU_PATROL, CPU_CHASE, CPU_CHARGE, CPU_MELEE };
static int   cpu_state;
static int   cpu_destpoint;
static float cpu_destx, cpu_destz;
static float cpu_shoot_t, cpu_punch_t, cpu_charge_t;
static const float patrol_pts[4][2] = { { 8, 4 }, { -8, 4 }, { -8, -4 }, { 8, -4 } };

/* --- characters, modes, app flow ----------------------------------------- */
/* The 30-fighter roster (names from the original prefabs; levelPower from the
 * prefabs where extractable, interpolated on the same rising curve elsewhere). */
typedef struct { const char *name; float power; } Character;
static const Character roster[30] = {
	{ "Goku Inicio", 1.0f },        { "Piccolo Inicio", 1.0f },   { "Kid Gohan", 1.1f },
	{ "Vegeta Inicio", 1.6f },      { "Freezer Forma 1", 2.0f },  { "Freeze Forma 2", 2.4f },
	{ "Piccolo Nail", 2.5f },       { "Vegeta Namek", 2.7f },     { "Freezer Forma 4", 2.9f },
	{ "Goku SS", 3.1f },            { "Piccolo End", 3.2f },      { "Cell Forma 1", 3.3f },
	{ "Cell Foma 2", 3.4f },        { "Super Vegeta", 3.6f },     { "Cell Foma Perfecta", 3.9f },
	{ "Joven Gohan SS 2", 4.3f },   { "Goku SS2", 4.6f },         { "Majin Vegeta", 4.9f },
	{ "Super Buu", 5.1f },          { "Gohan Definitivo", 5.2f }, { "Kid Buu", 5.3f },
	{ "Goku SS3", 5.4f },           { "Goku End", 5.7f },         { "Vegeta End", 6.2f },
	{ "Bills", 6.8f },              { "Goku SSG", 7.3f },         { "Goku SSGSS", 7.6f },
	{ "Golden Freezer", 7.8f },     { "Vegeta SSGSS", 8.2f },     { "Whis", 8.6f },
};
#define ROSTER_N 30

/* Per-character sprites (bin2o from data/charNN.png). */
#define DECL_CHAR(n) extern const uint8_t char##n##_png[]; extern const uint32_t char##n##_png_size;
DECL_CHAR(00) DECL_CHAR(01) DECL_CHAR(02) DECL_CHAR(03) DECL_CHAR(04)
DECL_CHAR(05) DECL_CHAR(06) DECL_CHAR(07) DECL_CHAR(08) DECL_CHAR(09)
DECL_CHAR(10) DECL_CHAR(11) DECL_CHAR(12) DECL_CHAR(13) DECL_CHAR(14)
DECL_CHAR(15) DECL_CHAR(16) DECL_CHAR(17) DECL_CHAR(18) DECL_CHAR(19)
DECL_CHAR(20) DECL_CHAR(21) DECL_CHAR(22) DECL_CHAR(23) DECL_CHAR(24)
DECL_CHAR(25) DECL_CHAR(26) DECL_CHAR(27) DECL_CHAR(28) DECL_CHAR(29)
#define CHAR_ENT(n) { char##n##_png, &char##n##_png_size }
typedef struct { const uint8_t *png; const uint32_t *size; } CharPng;
static const CharPng char_png[ROSTER_N] = {
	CHAR_ENT(00), CHAR_ENT(01), CHAR_ENT(02), CHAR_ENT(03), CHAR_ENT(04),
	CHAR_ENT(05), CHAR_ENT(06), CHAR_ENT(07), CHAR_ENT(08), CHAR_ENT(09),
	CHAR_ENT(10), CHAR_ENT(11), CHAR_ENT(12), CHAR_ENT(13), CHAR_ENT(14),
	CHAR_ENT(15), CHAR_ENT(16), CHAR_ENT(17), CHAR_ENT(18), CHAR_ENT(19),
	CHAR_ENT(20), CHAR_ENT(21), CHAR_ENT(22), CHAR_ENT(23), CHAR_ENT(24),
	CHAR_ENT(25), CHAR_ENT(26), CHAR_ENT(27), CHAR_ENT(28), CHAR_ENT(29),
};
static ya2d_Texture *char_tex[ROSTER_N];

static void load_char_textures(void)
{
	for (int i = 0; i < ROSTER_N; i++)
		char_tex[i] = ya2d_loadPNGfromBuffer((void *)char_png[i].png, *char_png[i].size);
}

/* Arena backgrounds (bin2o from data/arenaN.jpg). */
#define DECL_ARENA(n) extern const uint8_t arena##n##_jpg[]; extern const uint32_t arena##n##_jpg_size;
DECL_ARENA(0) DECL_ARENA(1) DECL_ARENA(2) DECL_ARENA(3) DECL_ARENA(4) DECL_ARENA(5) DECL_ARENA(6)
#define ARENA_N 7
static const struct { const uint8_t *jpg; const uint32_t *size; const char *name; } arena_src[ARENA_N] = {
	{ arena0_jpg, &arena0_jpg_size, "Namek" },
	{ arena1_jpg, &arena1_jpg_size, "Cell Ring" },
	{ arena2_jpg, &arena2_jpg_size, "Desert" },
	{ arena3_jpg, &arena3_jpg_size, "Diablo Desert" },
	{ arena4_jpg, &arena4_jpg_size, "Kamehouse" },
	{ arena5_jpg, &arena5_jpg_size, "Planet Supreme Kaio" },
	{ arena6_jpg, &arena6_jpg_size, "Torneo Mundial" },
};
static ya2d_Texture *arena_tex[ARENA_N];
static int arena_sel = 0;

static void load_arena_textures(void)
{
	for (int i = 0; i < ARENA_N; i++)
		arena_tex[i] = ya2d_loadJPGfromBuffer((void *)arena_src[i].jpg, *arena_src[i].size);
}

/* Per-fighter strength (from the chosen character). */
static float p1_power = DEFAULT_POWER, p2_power = DEFAULT_POWER;
static int   p1_char = 0, p2_char = ROSTER_N - 1;

/* Modes (instancePrefabs.cs flags). */
enum { MODE_BATTLE, MODE_TOURNAMENT, MODE_MISSION };
static int mode = MODE_BATTLE;
static int round_target = 1;     /* round wins to take the match (Battle 1, Tour 3) */
static int p2_is_cpu = 1;

/* App flow. */
enum { APP_MENU, APP_CHARSEL, APP_FIGHT };
static int app_state = APP_MENU;
static int menu_sel = 0;         /* mode-menu cursor   */
static int sel_cursor = 0;       /* character grid cursor */
static int sel_phase = 0;        /* 0 = picking P1, 1 = picking P2 */

/* Missions: a fixed enemy + arena (and a flavour title) per mission. */
typedef struct { const char *title; int enemy; int arena; } Mission;
static const Mission missions[5] = {
	{ "Saiyan Saga",   3,  3 },   /* vs Vegeta Inicio   @ Diablo Desert */
	{ "Namek",         8,  0 },   /* vs Freezer Forma 4 @ Namek         */
	{ "Android/Cell",  14, 1 },   /* vs Cell Perfecta   @ Cell Ring     */
	{ "Majin Buu",     20, 6 },   /* vs Kid Buu         @ Torneo Mundial*/
	{ "Gods",          29, 5 },   /* vs Whis            @ Supreme Kaio   */
};
#define MISSION_N 5
static int mission_sel = 0;
static int missions_unlocked = 1;   /* sequential unlock (beat N to open N+1) */
static int tour_tier = 1;           /* Tournament difficulty tier 1..3 */

/* --- unified pad polling (shared by all app states) ----------------------- */
enum {
	PB_UP = 1u, PB_DOWN = 2u, PB_LEFT = 4u, PB_RIGHT = 8u,
	PB_CROSS = 16u, PB_CIRCLE = 32u, PB_SQUARE = 64u,
	PB_START = 128u, PB_SELECT = 256u, PB_L1 = 512u, PB_R1 = 1024u,
	PB_L2 = 2048u, PB_R2 = 4096u
};
static padData g_pd[2];      /* retained current pad state */
static int     g_conn[2];
static u32     g_pressed[2]; /* edge bits this frame       */

static void sys_callback(u64 status, u64 param, void *userdata)
{
	(void)param; (void)userdata;
	if (status == SYSUTIL_EXIT_GAME)
		running = 0;
}

/* --- tiny vector helpers -------------------------------------------------- */
static void v_sub(const float a[3], const float b[3], float o[3])
{ o[0]=a[0]-b[0]; o[1]=a[1]-b[1]; o[2]=a[2]-b[2]; }
static float v_dot(const float a[3], const float b[3])
{ return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
static void v_cross(const float a[3], const float b[3], float o[3])
{ o[0]=a[1]*b[2]-a[2]*b[1]; o[1]=a[2]*b[0]-a[0]*b[2]; o[2]=a[0]*b[1]-a[1]*b[0]; }
static void v_norm(float v[3])
{ float l=sqrtf(v_dot(v,v)); if(l>1e-6f){v[0]/=l;v[1]/=l;v[2]/=l;} }

static void camera_setup(void)
{
	const float up[3] = { 0.0f, 1.0f, 0.0f };
	v_sub(cam_target, cam_eye, cam_f); v_norm(cam_f);
	v_cross(up, cam_f, cam_r);         v_norm(cam_r);
	v_cross(cam_f, cam_r, cam_u);
}

static int project(float wx, float wy, float wz, float *sx, float *sy, float *scale)
{
	const float p[3] = { wx, wy, wz };
	float d[3];
	v_sub(p, cam_eye, d);
	float vz = v_dot(d, cam_f);
	if (vz < 0.05f) return 0;
	*sx = CAM_CX + FOCAL * v_dot(d, cam_r) / vz;
	*sy = CAM_CY - FOCAL * v_dot(d, cam_u) / vz;
	*scale = FOCAL / vz;
	return 1;
}

/* --- init ----------------------------------------------------------------- */
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

/* --- combat helpers ------------------------------------------------------- */
static float axis(u8 v)
{
	int d = (int)v - 128;
	if (d > -STICK_DEAD && d < STICK_DEAD) return 0.0f;
	return (float)d / 127.0f;
}
static void clampf(float *v, float lo, float hi)
{ if (*v < lo) *v = lo; else if (*v > hi) *v = hi; }

static void clamp_pos(float *x, float *z)
{
	clampf(x, -ARENA_X + FIGHTER_HALF_X, ARENA_X - FIGHTER_HALF_X);
	clampf(z, -ARENA_Z + FIGHTER_HALF_Z, ARENA_Z - FIGHTER_HALF_Z);
}

/* Read a player's movement intent (left stick + D-pad) from one pad. */
static void read_move(const padData *pd, float *mx, float *mz)
{
	*mx = 0.0f; *mz = 0.0f;
	/* Both axes exactly 0 = no analog data (digital pad / not read yet). */
	if (!(pd->ANA_L_H == 0 && pd->ANA_L_V == 0)) {
		*mx = axis(pd->ANA_L_H);
		*mz = -axis(pd->ANA_L_V);   /* stick up -> +Z (away) */
	}
	if (pd->BTN_LEFT)  *mx -= 1.0f;
	if (pd->BTN_RIGHT) *mx += 1.0f;
	if (pd->BTN_UP)    *mz += 1.0f;
	if (pd->BTN_DOWN)  *mz -= 1.0f;
	clampf(mx, -1.0f, 1.0f);
	clampf(mz, -1.0f, 1.0f);
}

/* SimpleController translation + PongPaddle clamp. Returns 1 if it moved. */
static int move_fighter(float *x, float *z, float mx, float mz, float ki, float power)
{
	const float moveSpeed = 0.2f + power / 50.0f;
	const float step = moveSpeed + (moveSpeed * 0.5f * ki / 100.0f);
	*x += mx * step;
	*z += mz * step;
	clamp_pos(x, z);
	return (mx != 0.0f || mz != 0.0f);
}

/* chargeKi(): hold to fill (faster when still), else drain. */
static void charge_ki(float *ki, int holding, int moving)
{
	if (holding && *ki < KI_MAX)
		*ki += (moving ? 10.0f : 25.0f) * FRAME_DT;
	else if (*ki > KI_MIN)
		*ki -= 3.0f * FRAME_DT;
	clampf(ki, KI_MIN, KI_MAX);
}

static int fighters_in_contact(void)
{
	float dx = f1x - f2x, dz = f1z - f2z;
	return (dx*dx + dz*dz) <= (MELEE_RANGE * MELEE_RANGE);
}

static void knockback(float *tx, float *tz, float fromx, float fromz)
{
	float dx = *tx - fromx, dz = *tz - fromz;
	float len = sqrtf(dx*dx + dz*dz);
	if (len < 1e-3f) { dx = 1.0f; dz = 0.0f; len = 1.0f; }
	*tx += dx / len * KNOCKBACK;
	*tz += dz / len * KNOCKBACK;
	clamp_pos(tx, tz);
}

/* Push one fighter radially away from an explosion at (ex, ez), scaled by how
 * close it is (full force at the centre, zero at EXPL_KNOCK_RADIUS). */
static void blast_push(float *fx, float *fz, float ex, float ez)
{
	float dx = *fx - ex, dz = *fz - ez;
	float dist = sqrtf(dx*dx + dz*dz);
	if (dist >= EXPL_KNOCK_RADIUS) return;
	float falloff = 1.0f - dist / EXPL_KNOCK_RADIUS;
	if (dist < 1e-3f) { dx = 1.0f; dz = 0.0f; dist = 1.0f; }
	float push = EXPL_KNOCK_FORCE * falloff;
	*fx += dx / dist * push;
	*fz += dz / dist * push;
	clamp_pos(fx, fz);
}

/* OverlapSphere shove: every fighter within range is knocked back on impact. */
static void explosion_knockback(float ex, float ez)
{
	blast_push(&f1x, &f1z, ex, ez);
	blast_push(&f2x, &f2z, ex, ez);
}

/* Attack(): transfer power, grant ki, knock the opponent back. */
static void melee(int by_p1)
{
	if (by_p1) {
		balance += 1.5f * p1_power;
		ki1 += MELEE_KIGAIN; clampf(&ki1, KI_MIN, KI_MAX);
		knockback(&f2x, &f2z, f1x, f1z);
		melee_cd1 = MELEE_CD;
	} else {
		balance -= 1.5f * p2_power;
		ki2 += MELEE_KIGAIN; clampf(&ki2, KI_MIN, KI_MAX);
		knockback(&f1x, &f1z, f2x, f2z);
		melee_cd2 = MELEE_CD;
	}
	audio_play_hit();
}

static void spawn_expl(float x, float y, float z, int owner)
{
	audio_play_explosion();
	for (int i = 0; i < MAX_EXPL; i++) {
		if (!expls[i].active) {
			expls[i].active = 1;
			expls[i].x = x; expls[i].y = y; expls[i].z = z;
			expls[i].owner = owner; expls[i].t = EXPL_LIFE;
			return;
		}
	}
}

/* LookAt(opponent) + straight shot: aim at the opponent's position at launch. */
static void spawn_blast(int owner, int tier)
{
	float sx = (owner == 1) ? f1x : f2x, sz = (owner == 1) ? f1z : f2z;
	float tx = (owner == 1) ? f2x : f1x, tz = (owner == 1) ? f2z : f1z;
	float dx = tx - sx, dz = tz - sz;
	float len = sqrtf(dx*dx + dz*dz);
	if (len < 1e-3f) { dx = (owner == 1) ? 1.0f : -1.0f; dz = 0.0f; len = 1.0f; }
	dx /= len; dz /= len;
	audio_play_blast();
	for (int i = 0; i < MAX_BLASTS; i++) {
		if (!blasts[i].active) {
			blasts[i].active = 1;
			blasts[i].x = sx + dx * BLAST_OFFSET;
			blasts[i].z = sz + dz * BLAST_OFFSET;
			blasts[i].y = BLAST_Y;
			blasts[i].vx = dx * BLAST_SPEED;
			blasts[i].vz = dz * BLAST_SPEED;
			blasts[i].owner = owner; blasts[i].tier = tier; blasts[i].life = BLAST_LIFE;
			return;
		}
	}
}

/* LaunchKiBlast(): fire the strongest tier the current ki affords, spend it. */
static void try_fire(int owner)
{
	float *ki = (owner == 1) ? &ki1 : &ki2;
	int tier; float cost;
	if (*ki > KI_COST3)      { tier = 2; cost = KI_COST3; }
	else if (*ki > KI_COST2) { tier = 1; cost = KI_COST2; }
	else if (*ki > KI_COST1) { tier = 0; cost = KI_COST1; }
	else return;   /* not enough energy */
	*ki -= cost;
	clampf(ki, KI_MIN, KI_MAX);
	spawn_blast(owner, tier);
}

/* Advance blasts + explosions; apply ki-blast damage (a power transfer). */
static void update_effects(void)
{
	for (int i = 0; i < MAX_BLASTS; i++) {
		Blast *b = &blasts[i];
		if (!b->active) continue;
		b->x += b->vx * FRAME_DT;
		b->z += b->vz * FRAME_DT;
		b->life--;
		if (b->life <= 0 ||
		    b->x < -ARENA_X - 3 || b->x > ARENA_X + 3 ||
		    b->z < -ARENA_Z - 3 || b->z > ARENA_Z + 3) {
			b->active = 0;
			spawn_expl(b->x, b->y, b->z, b->owner);
			continue;
		}
		float ox = (b->owner == 1) ? f2x : f1x;
		float oz = (b->owner == 1) ? f2z : f1z;
		float dx = b->x - ox, dz = b->z - oz;
		if (dx*dx + dz*dz <= BLAST_HIT * BLAST_HIT) {
			float opw = (b->owner == 1) ? p1_power : p2_power;
			float amount = BLAST_DMGBASE * (b->tier + 1) + opw * 1.25f;
			if (b->owner == 1) balance += amount; else balance -= amount;
			explosion_knockback(b->x, b->z);   /* OverlapSphere shove on impact */
			b->active = 0;
			spawn_expl(b->x, b->y, b->z, b->owner);
		}
	}
	for (int i = 0; i < MAX_EXPL; i++)
		if (expls[i].active && --expls[i].t <= 0)
			expls[i].active = 0;
}

static void clear_effects(void)
{
	for (int i = 0; i < MAX_BLASTS; i++) blasts[i].active = 0;
	for (int i = 0; i < MAX_EXPL; i++)   expls[i].active = 0;
}

/* --- CPU AI (FSM port of CPUController.cs) --------------------------------- */
static void cpu_goto_next_point(void)
{
	cpu_destx = patrol_pts[cpu_destpoint][0];
	cpu_destz = patrol_pts[cpu_destpoint][1];
	cpu_destpoint = (cpu_destpoint + 1) % 4;
}

static void cpu_reset(void)
{
	cpu_state = CPU_PATROL;
	cpu_destpoint = 0;
	cpu_shoot_t = cpu_punch_t = 0.0f;
	cpu_charge_t = 4.0f;
	cpu_goto_next_point();
}

/* CPU ki only charges in the ChargeKi state (BalanceOfPower.chargeKi CPU branch). */
static void charge_ki_cpu(void)
{
	if (cpu_state == CPU_CHARGE && ki2 < KI_MAX) ki2 += 15.0f * FRAME_DT;
	else if (ki2 > KI_MIN)                       ki2 -= 3.0f * FRAME_DT;
	clampf(&ki2, KI_MIN, KI_MAX);
}

/* CPU difficulty rates from its character power (CPUController.Initialize). */
static float cpu_shoot_rate(void) { return 0.9f - p2_power / 75.0f; }
static float cpu_punch_rate(void) { return 0.20f - p2_power / 100.0f; }
static float cpu_speed_base(void) { return 10.0f + p2_power / 2.0f; }

/* Seek the current destination at the ki-scaled CPU speed (stops within 0.5). */
static void cpu_move(void)
{
	float dx = cpu_destx - f2x, dz = cpu_destz - f2z;
	float d = sqrtf(dx*dx + dz*dz);
	if (d <= 0.5f) { moving2 = 0; return; }
	float base = cpu_speed_base();
	float speed = (base + base * 0.5f * ki2 / 100.0f) * FRAME_DT;
	float step = (speed < d) ? speed : d;
	f2x += dx / d * step;
	f2z += dz / d * step;
	clamp_pos(&f2x, &f2z);
	moving2 = 1;
}

/* FSM tick: Patrol -> Chase -> Melee / ChargeKi. Drives fighter 2 each frame. */
static void cpu_update(void)
{
	float dx = f2x - f1x, dz = f2z - f1z;
	float dist = sqrtf(dx*dx + dz*dz);
	cpu_shoot_t += FRAME_DT;
	cpu_punch_t += FRAME_DT;

	switch (cpu_state) {
	case CPU_PATROL: {
		float bx = cpu_destx - f2x, bz = cpu_destz - f2z;
		if (sqrtf(bx*bx + bz*bz) < 2.5f) cpu_goto_next_point();
		else if (dist <= CPU_SIGHT) cpu_state = CPU_CHASE;
		break;
	}
	case CPU_CHASE:
		cpu_destx = f1x; cpu_destz = f1z;                 /* seek the player   */
		if (dist <= 2.5f) cpu_state = CPU_MELEE;
		else if (dist > CPU_SIGHT) { cpu_goto_next_point(); cpu_state = CPU_PATROL; }
		else if (dist >= 4.0f && dist <= 9.0f && (rand() % 1000) < 14)
			cpu_state = CPU_CHARGE;                       /* occasionally retreat to charge */
		if (cpu_shoot_t >= cpu_shoot_rate()) { try_fire(2); cpu_shoot_t = 0.0f; }
		break;
	case CPU_CHARGE:
		cpu_destx = -f1x; cpu_destz = -f1z;               /* retreat to the mirror point */
		cpu_charge_t -= FRAME_DT * 2.0f;
		if (cpu_charge_t <= 0.0f) { cpu_charge_t = 4.0f; cpu_state = CPU_PATROL; }
		break;
	case CPU_MELEE:
		cpu_destx = f1x; cpu_destz = f1z;
		if (dist > 3.0f) cpu_state = CPU_CHASE;
		break;
	}

	charge_ki_cpu();
	cpu_move();

	/* Auto-melee on contact (BalanceOfPower.OnTriggerStay, CPU branch). */
	if (fighters_in_contact() && cpu_punch_t >= cpu_punch_rate()) {
		melee(0);
		cpu_punch_t = 0.0f;
	}

	charging2 = (cpu_state == CPU_CHARGE) ? 1 : 0;
}

static void reset_positions(void)
{ f1x = -9.0f; f1z = 0.0f; f2x = 9.0f; f2z = 0.0f; }

static void reset_round(void)
{
	balance = BAL_START;
	reset_positions();
	clear_effects();
	cpu_reset();
	melee_cd1 = melee_cd2 = 0;
	gstate = GS_FIGHT;
}

static void reset_match(void)
{
	score1 = score2 = 0;
	ki1 = ki2 = KI_START;
	reset_round();
}

static void end_round(void)
{
	if (score1 >= round_target || score2 >= round_target) {
		match_winner = (score1 >= round_target) ? 1 : 2;
		gstate = GS_MATCH;
		/* Beating a mission unlocks the next one (sequential progression). */
		if (mode == MODE_MISSION && match_winner == 1 &&
		    mission_sel == missions_unlocked - 1 && missions_unlocked < MISSION_N)
			missions_unlocked++;
	} else {
		gstate = GS_ROUND;
		banner_timer = ROUND_BANNER;
	}
}

/* --- unified pad poll (retain-last packet; shared by every app state) ------ */
static void poll_pads(void)
{
	static padData held[2];
	static u32 prev[2] = { 0, 0 };
	padInfo info;
	ioPadGetInfo(&info);
	for (int i = 0; i < 2; i++) {
		padData tmp;
		memset(&tmp, 0, sizeof(tmp));
		if (info.status[i] && ioPadGetData(i, &tmp) == 0) {
			g_conn[i] = 1;
			if (tmp.len > 0) held[i] = tmp;   /* refresh only on fresh data */
			g_pd[i] = held[i];
		} else {
			g_conn[i] = 0;
			memset(&held[i], 0, sizeof(held[i]));
			memset(&g_pd[i], 0, sizeof(g_pd[i]));
		}
		padData *p = &g_pd[i];
		u32 cur = 0;
		if (g_conn[i]) {
			if (p->BTN_UP)     cur |= PB_UP;
			if (p->BTN_DOWN)   cur |= PB_DOWN;
			if (p->BTN_LEFT)   cur |= PB_LEFT;
			if (p->BTN_RIGHT)  cur |= PB_RIGHT;
			if (p->BTN_CROSS)  cur |= PB_CROSS;
			if (p->BTN_CIRCLE) cur |= PB_CIRCLE;
			if (p->BTN_SQUARE) cur |= PB_SQUARE;
			if (p->BTN_START)  cur |= PB_START;
			if (p->BTN_SELECT) cur |= PB_SELECT;
			if (p->BTN_L1)     cur |= PB_L1;
			if (p->BTN_R1)     cur |= PB_R1;
			if (p->BTN_L2)     cur |= PB_L2;
			if (p->BTN_R2)     cur |= PB_R2;
		}
		g_pressed[i] = cur & ~prev[i];
		prev[i] = cur;
	}
	p1_conn = g_conn[0];
	p2_conn = g_conn[1];
}

/* True if Select+Start is held on either pad (quit to XMB). */
static int quit_combo(void)
{
	for (int i = 0; i < 2; i++)
		if (g_conn[i] && g_pd[i].BTN_SELECT && g_pd[i].BTN_START) return 1;
	return 0;
}

/* Menu navigation edges from pad 0 (D-pad + left stick), with auto-centering. */
static u32 nav_edges(void)
{
	u32 e = g_pressed[0] & (PB_UP | PB_DOWN | PB_LEFT | PB_RIGHT);
	/* fold the left stick into discrete edges */
	static int sx = 0, sz = 0;
	int nx = 0, nz = 0;
	if (g_conn[0] && !(g_pd[0].ANA_L_H == 0 && g_pd[0].ANA_L_V == 0)) {
		int h = (int)g_pd[0].ANA_L_H - 128, v = (int)g_pd[0].ANA_L_V - 128;
		if (h < -48) nx = -1; else if (h > 48) nx = 1;
		if (v < -48) nz = -1; else if (v > 48) nz = 1;
	}
	if (nx == -1 && sx != -1) e |= PB_LEFT;
	if (nx ==  1 && sx !=  1) e |= PB_RIGHT;
	if (nz == -1 && sz != -1) e |= PB_UP;
	if (nz ==  1 && sz !=  1) e |= PB_DOWN;
	sx = nx; sz = nz;
	return e;
}

/* Configure and enter a match for the chosen mode. */
static void start_fight(void)
{
	p1_power = roster[p1_char].power;
	p2_power = roster[p2_char].power;
	if (mode == MODE_TOURNAMENT)   round_target = 3;
	else                           round_target = 1;   /* Battle / Mission */
	/* P2 is human only in Battle with a second pad; otherwise it's the CPU. */
	p2_is_cpu = !(mode == MODE_BATTLE && p2_conn);
	reset_match();
	app_state = APP_FIGHT;
}

/* Mode-select menu (StartOptions.cs). */
static void update_menu(void)
{
	u32 e = nav_edges();
	if (e & PB_UP)   menu_sel = (menu_sel + 3) % 4;
	if (e & PB_DOWN) menu_sel = (menu_sel + 1) % 4;
	if (g_pressed[0] & PB_CROSS) {
		switch (menu_sel) {
		case 0: mode = MODE_BATTLE;     break;   /* arena is player-picked (L2/R2) */
		case 1: mode = MODE_TOURNAMENT; arena_sel = 6; break;                 /* Torneo Mundial */
		case 2: mode = MODE_MISSION;    arena_sel = missions[mission_sel].arena; break;
		case 3: running = 0; return;   /* Quit */
		}
		sel_phase = 0;
		app_state = APP_CHARSEL;
	}
}

/* Character grid select (SelectCharacters.cs / ChooseYourPlayer.cs). */
static void update_charsel(void)
{
	u32 e = nav_edges();
	/* L1/R1 picks the mission (unlocked only) or the tournament difficulty tier. */
	if (sel_phase == 0 && mode == MODE_MISSION) {
		if (g_pressed[0] & PB_L1) mission_sel = (mission_sel + missions_unlocked - 1) % missions_unlocked;
		if (g_pressed[0] & PB_R1) mission_sel = (mission_sel + 1) % missions_unlocked;
		arena_sel = missions[mission_sel].arena;
	} else if (sel_phase == 0 && mode == MODE_TOURNAMENT) {
		if (g_pressed[0] & PB_L1) tour_tier = (tour_tier > 1) ? tour_tier - 1 : 3;
		if (g_pressed[0] & PB_R1) tour_tier = (tour_tier < 3) ? tour_tier + 1 : 1;
	} else if (sel_phase == 0 && mode == MODE_BATTLE) {
		if (g_pressed[0] & PB_L2) arena_sel = (arena_sel + ARENA_N - 1) % ARENA_N;
		if (g_pressed[0] & PB_R2) arena_sel = (arena_sel + 1) % ARENA_N;
	}
	if (e & PB_LEFT)  sel_cursor = (sel_cursor + ROSTER_N - 1) % ROSTER_N;
	if (e & PB_RIGHT) sel_cursor = (sel_cursor + 1) % ROSTER_N;
	if (e & PB_UP)    sel_cursor = (sel_cursor + ROSTER_N - 10) % ROSTER_N;
	if (e & PB_DOWN)  sel_cursor = (sel_cursor + 10) % ROSTER_N;

	if (g_pressed[0] & PB_CIRCLE) {   /* back */
		if (sel_phase == 1) sel_phase = 0;
		else app_state = APP_MENU;
		return;
	}
	if (g_pressed[0] & PB_CROSS) {
		if (sel_phase == 0) {
			p1_char = sel_cursor;
			if (mode == MODE_BATTLE) { sel_phase = 1; }       /* pick P2 next */
			else if (mode == MODE_TOURNAMENT) {
				/* random opponent within the tier's index range (instancePrefabs.cs). */
				if (tour_tier == 1)      p2_char = rand() % 7;        /*  0-6  */
				else if (tour_tier == 2) p2_char = 7 + rand() % 7;    /*  7-13 */
				else                     p2_char = 14 + rand() % 6;   /* 14-19 */
				start_fight();
			} else { /* MODE_MISSION */
				p2_char = missions[mission_sel].enemy; start_fight();
			}
		} else {
			p2_char = sel_cursor;
			start_fight();
		}
	}
}

/* --- per-frame fight update (APP_FIGHT) ----------------------------------- */
/* Pads are already polled (poll_pads) into g_pd / g_conn / g_pressed. */
static void update_game(void)
{
	u32 sq1 = g_pressed[0] & PB_SQUARE, sq2 = g_pressed[1] & PB_SQUARE;
	u32 bl1 = g_pressed[0] & PB_CIRCLE, bl2 = g_pressed[1] & PB_CIRCLE;
	u32 start = (g_pressed[0] | g_pressed[1]) & PB_START;

	if (melee_cd1 > 0) melee_cd1--;
	if (melee_cd2 > 0) melee_cd2--;

	if (gstate == GS_ROUND) {
		if (--banner_timer <= 0) reset_round();
		return;
	}
	if (gstate == GS_MATCH) {
		if (start) app_state = APP_MENU;          /* result -> back to menu */
		return;
	}

	/* GS_FIGHT */
	if (start) paused = !paused;
	if (paused) {
		if (g_pressed[0] & PB_CIRCLE) { paused = 0; app_state = APP_MENU; }  /* quit to menu */
		moving1 = moving2 = charging1 = charging2 = 0;
		return;
	}

	/* Fighter 1: human on pad 0. */
	float m1x = 0.0f, m1z = 0.0f;
	if (g_conn[0]) read_move(&g_pd[0], &m1x, &m1z);
	moving1 = move_fighter(&f1x, &f1z, m1x, m1z, ki1, p1_power);
	charging1 = (g_conn[0] && g_pd[0].BTN_CROSS) ? 1 : 0;
	charge_ki(&ki1, charging1, moving1);

	/* Fighter 2: human on pad 1 (Battle 2P) or the CPU. */
	if (!p2_is_cpu && g_conn[1]) {
		float m2x = 0.0f, m2z = 0.0f;
		read_move(&g_pd[1], &m2x, &m2z);
		moving2 = move_fighter(&f2x, &f2z, m2x, m2z, ki2, p2_power);
		charging2 = g_pd[1].BTN_CROSS ? 1 : 0;
		charge_ki(&ki2, charging2, moving2);
	} else {
		cpu_update();
	}

	/* Melee on contact (edge-triggered, cooldown-gated). */
	if (fighters_in_contact()) {
		if (sq1 && melee_cd1 == 0) melee(1);
		if (!p2_is_cpu && sq2 && melee_cd2 == 0) melee(0);
	}

	/* Ki blasts (Circle), then advance all projectiles/explosions. */
	if (bl1) try_fire(1);
	if (!p2_is_cpu && bl2) try_fire(2);
	update_effects();

	/* Round end: someone filled the balance bar. */
	if (balance >= BAL_MAX) { balance = BAL_MAX; round_winner = 1; score1++; end_round(); }
	else if (balance <= 0.0f) { balance = 0.0f; round_winner = 2; score2++; end_round(); }
}

/* --- rendering ------------------------------------------------------------ */
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

static void floor_quad(void)
{
	float sx[4], sy[4], sc;
	int ok = 1;
	ok &= project(-ARENA_X, 0, -ARENA_Z, &sx[0], &sy[0], &sc);
	ok &= project( ARENA_X, 0, -ARENA_Z, &sx[1], &sy[1], &sc);
	ok &= project( ARENA_X, 0,  ARENA_Z, &sx[2], &sy[2], &sc);
	ok &= project(-ARENA_X, 0,  ARENA_Z, &sx[3], &sy[3], &sc);
	if (!ok) return;
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
	float ax, ay, bx, by, sc;
	const u32 line = 0x35587FFF, mid = 0x6FA0D0FF;
	tiny3d_SetPolygon(TINY3D_LINES);
	for (int i = -12; i <= 12; i += 3) {
		u32 c = (i == 0) ? mid : line;
		if (!project((float)i, 0, -ARENA_Z, &ax, &ay, &sc)) continue;
		if (!project((float)i, 0,  ARENA_Z, &bx, &by, &sc)) continue;
		tiny3d_VertexPos(ax, ay, 0); tiny3d_VertexColor(c);
		tiny3d_VertexPos(bx, by, 0); tiny3d_VertexColor(c);
	}
	for (int j = -6; j <= 6; j += 3) {
		u32 c = (j == 0) ? mid : line;
		if (!project(-ARENA_X, 0, (float)j, &ax, &ay, &sc)) continue;
		if (!project( ARENA_X, 0, (float)j, &bx, &by, &sc)) continue;
		tiny3d_VertexPos(ax, ay, 0); tiny3d_VertexColor(c);
		tiny3d_VertexPos(bx, by, 0); tiny3d_VertexColor(c);
	}
	tiny3d_End();
}

static void draw_fighter(float wx, float wz, u32 color, int aura, int char_id)
{
	float bx, by, sc;
	if (!project(wx, 0.0f, wz, &bx, &by, &sc)) return;
	float h = FIGHTER_H * sc;
	ya2d_Texture *tex = (char_id >= 0 && char_id < ROSTER_N) ? char_tex[char_id] : NULL;

	if (tex) {
		/* Real character sprite (alpha cutout), aspect-preserved. */
		float aspect = (float)tex->imageWidth / (float)tex->imageHeight;
		float sw = h * aspect, sh = h;
		int sx = (int)(bx - sw * 0.5f), sy = (int)(by - sh);
		int shw = (int)(sw * 1.1f);
		ya2d_drawFillRectZ((int)(bx - shw * 0.5f), (int)by - 3, 0, shw, 6, COLOR_SHADOW);
		if (aura) {
			ya2d_drawRectZ(sx - 3, sy - 3, 0, (int)sw + 6, (int)sh + 6, COLOR_ORANGE);
			ya2d_drawRectZ(sx - 2, sy - 2, 0, (int)sw + 4, (int)sh + 4, COLOR_ORANGE);
		}
		ya2d_drawTextureEx(tex, (float)sx, (float)sy, 0, sw, sh);
		return;
	}

	/* Fallback: the old colored billboard if the texture failed to load. */
	float w = FIGHTER_W * sc;
	int rx = (int)(bx - w * 0.5f), ry = (int)(by - h);
	int rw = (int)w, rh = (int)h;
	int shw = (int)(w * 1.05f);
	ya2d_drawFillRectZ((int)(bx - shw * 0.5f), (int)by - 3, 0, shw, 6, COLOR_SHADOW);
	if (aura) {
		ya2d_drawRectZ(rx - 3, ry - 3, 0, rw + 6, rh + 6, COLOR_ORANGE);
		ya2d_drawRectZ(rx - 2, ry - 2, 0, rw + 4, rh + 4, COLOR_ORANGE);
	}
	ya2d_drawFillRectZ(rx, ry, 0, rw, rh, color);
	ya2d_drawRectZ(rx, ry, 0, rw, rh, COLOR_WHITE);
	int hw = rw / 2;
	ya2d_drawFillRectZ((int)(bx - hw * 0.5f), ry - hw, 0, hw, hw, color);
}

/* Draw a character sprite at screen (cx, base_y feet) with a given height. */
static void draw_char_sprite(int id, float cx, float base_y, float height)
{
	if (id < 0 || id >= ROSTER_N || !char_tex[id]) return;
	ya2d_Texture *t = char_tex[id];
	float w = height * ((float)t->imageWidth / (float)t->imageHeight);
	ya2d_drawTextureEx(t, cx - w * 0.5f, base_y - height, 0, w, height);
}

/* A glowing energy orb (concentric squares): faint glow, mid, white core. */
static void draw_orb(float bx, float by, float sc, int tier, int owner)
{
	float r = sc * (0.5f + 0.18f * tier);
	u32 glow = (owner == 1) ? 0x3FA9F555 : 0xFF884455;
	u32 mid  = (owner == 1) ? 0x6FD0FFFF : 0xFFC040FF;
	int R = (int)r, M = (int)(r * 0.6f), C = (int)(r * 0.3f);
	if (R < 2) R = 2;
	if (C < 1) C = 1;
	ya2d_drawFillRectZ((int)bx - R, (int)by - R, 0, 2 * R, 2 * R, glow);
	ya2d_drawFillRectZ((int)bx - M, (int)by - M, 0, 2 * M, 2 * M, mid);
	ya2d_drawFillRectZ((int)bx - C, (int)by - C, 0, 2 * C, 2 * C, COLOR_WHITE);
}

static void draw_blasts(void)
{
	float sx, sy, sc;
	for (int i = 0; i < MAX_BLASTS; i++)
		if (blasts[i].active && project(blasts[i].x, blasts[i].y, blasts[i].z, &sx, &sy, &sc))
			draw_orb(sx, sy, sc, blasts[i].tier, blasts[i].owner);
}

static void draw_expls(void)
{
	float sx, sy, sc;
	for (int i = 0; i < MAX_EXPL; i++) {
		if (!expls[i].active) continue;
		if (!project(expls[i].x, expls[i].y, expls[i].z, &sx, &sy, &sc)) continue;
		float frac = 1.0f - (float)expls[i].t / EXPL_LIFE;   /* 0 -> 1 over life */
		int R = (int)(sc * (0.4f + 2.6f * frac));
		u8 a = (u8)(220.0f * (1.0f - frac));
		u32 base = (expls[i].owner == 1) ? 0x9FE0FF00 : 0xFFC08000;
		u32 col = base | a;
		ya2d_drawRectZ((int)sx - R, (int)sy - R, 0, 2 * R, 2 * R, col);
		ya2d_drawRectZ((int)sx - R + 1, (int)sy - R + 1, 0, 2 * R - 2, 2 * R - 2, col);
	}
}

/* Full-screen arena background (drawn before the floor). */
static void draw_background(int idx)
{
	if (idx >= 0 && idx < ARENA_N && arena_tex[idx])
		ya2d_drawTextureEx(arena_tex[idx], 0.0f, 0.0f, 0.0f, SCREEN_WIDTH, SCREEN_HEIGHT);
}

static void draw_arena(void)
{
	draw_background(arena_sel);
	floor_quad();
	floor_grid();

	float d1[3], d2[3], p1[3] = { f1x, 0, f1z }, p2[3] = { f2x, 0, f2z };
	v_sub(p1, cam_eye, d1); v_sub(p2, cam_eye, d2);
	if (v_dot(d1, cam_f) >= v_dot(d2, cam_f)) {
		draw_fighter(f1x, f1z, COLOR_P1, charging1, p1_char);
		draw_fighter(f2x, f2z, COLOR_P2, charging2, p2_char);
	} else {
		draw_fighter(f2x, f2z, COLOR_P2, charging2, p2_char);
		draw_fighter(f1x, f1z, COLOR_P1, charging1, p1_char);
	}

	draw_blasts();
	draw_expls();
}

/* --- HUD ------------------------------------------------------------------ */
static int text_w(const char *s, int sw, int sh)
{ return display_ttf_string(0, 0, s, 0, 0, sw, sh); }

static void centered(int y, const char *s, u32 color, int sw, int sh)
{ display_ttf_string((SCREEN_WIDTH - text_w(s, sw, sh)) / 2, y, s, color, 0, sw, sh); }

static void draw_bar(int x, int y, int w, int h, float frac, u32 col, int anchor_right)
{
	if (frac < 0) frac = 0;
	if (frac > 1) frac = 1;
	ya2d_drawFillRectZ(x, y, 0, w, h, BAR_BG);
	int fw = (int)(w * frac);
	int fx = anchor_right ? x + (w - fw) : x;
	if (fw > 0) ya2d_drawFillRectZ(fx, y, 0, fw, h, col);
	ya2d_drawRectZ(x, y, 0, w, h, BAR_BORDER);
}

static void draw_balance_bar(void)
{
	const int x = 174, y = 22, w = 500, h = 22;
	float frac = balance / BAL_MAX;
	int bluew = (int)(w * frac);
	ya2d_drawFillRectZ(x, y, 0, w, h, BAR_BG);
	if (bluew > 0)     ya2d_drawFillRectZ(x, y, 0, bluew, h, COLOR_P1);
	if (bluew < w)     ya2d_drawFillRectZ(x + bluew, y, 0, w - bluew, h, COLOR_P2);
	ya2d_drawFillRectZ(x + w / 2 - 1, y - 3, 0, 2, h + 6, COLOR_WHITE);  /* center tick */
	ya2d_drawRectZ(x, y, 0, w, h, BAR_BORDER);
	display_ttf_string(x - 28, y + 3, "P1", COLOR_P1, 0, 14, 18);
	display_ttf_string(x + w + 8, y + 3, "P2", COLOR_P2, 0, 14, 18);
}

static void draw_hud(void)
{
	draw_balance_bar();

	/* Ki bars in the corners, with tier-cost ticks at 30/60/80. */
	const int kw = 130;
	const int kx2 = SCREEN_WIDTH - 28 - kw;
	draw_bar(28, 22, kw, 14, ki1 / KI_MAX, COLOR_CYAN, 0);
	draw_bar(kx2, 22, kw, 14, ki2 / KI_MAX, COLOR_CYAN, 1);
	const float costs[3] = { KI_COST1, KI_COST2, KI_COST3 };
	for (int k = 0; k < 3; k++) {
		int t1 = 28 + (int)(kw * (costs[k] / KI_MAX));
		int t2 = kx2 + (int)(kw * (1.0f - costs[k] / KI_MAX));
		ya2d_drawFillRectZ(t1 - 1, 22, 0, 1, 14, 0xFFFFFF66);
		ya2d_drawFillRectZ(t2, 22, 0, 1, 14, 0xFFFFFF66);
	}
	display_ttf_string(28, 38, "KI", COLOR_GRAY, 0, 11, 15);
	display_ttf_string(SCREEN_WIDTH - 28 - 22, 38, "KI", COLOR_GRAY, 0, 11, 15);

	/* Score, round/match result panels and PAUSED are drawn by the Clay UI
	 * overlay (build_and_render_ui) — see Phase 6. */

	/* Fighter names (P1 left, P2 right, with a CPU tag). */
	display_ttf_string(28, 52, roster[p1_char].name, COLOR_P1, 0, 12, 16);
	{
		char nm[48];
		snprintf(nm, sizeof nm, "%s%s", roster[p2_char].name, p2_is_cpu ? " (CPU)" : "");
		display_ttf_string(SCREEN_WIDTH - 28 - text_w(nm, 12, 16), 52, nm, COLOR_P2, 0, 12, 16);
	}
	if (!p1_conn)
		centered(150, "Connect controller 1 (P1)", COLOR_P1, 16, 22);

	display_ttf_string(28, SCREEN_HEIGHT - 28,
		"move | hold X charge | O blast | Square melee | Start pause (O quit to menu) | Sel+Start exit",
		COLOR_GRAY, 0, 10, 14);
}

/* --- Clay UI overlay (score + result panels) ------------------------------ */
/* Build a Clay_String from a C-string for runtime text (CLAY_STRING is literals
 * only). The buffer must outlive clay_render, so callers use static storage. */
static Clay_String clay_str(const char *s)
{
	Clay_String r;
	r.isStaticallyAllocated = false;
	r.length = (int32_t)strlen(s);
	r.chars = s;
	return r;
}

/* The score pill (ScorePanel.cs) and the round/match result card
 * (PongEndPanel.cs) are laid out with Clay and drawn over the scene. */
static void build_and_render_ui(void)
{
	static char score_buf[32], title_buf[48], sub_buf[32];
	snprintf(score_buf, sizeof score_buf, "%d   -   %d", score1, score2);

	const Clay_Color white  = { 255, 255, 255, 255 };
	const Clay_Color dim    = { 160, 160, 160, 255 };
	const Clay_Color yellow = { 255, 210,  63, 255 };
	const Clay_Color p1c    = {  63, 169, 245, 255 };
	const Clay_Color p2c    = { 245,  80,  63, 255 };
	const Clay_Color pillbg = {  12,  20,  34, 210 };
	const Clay_Color pillbd = {  64,  80, 104, 255 };
	const Clay_Color panbg  = {  10,  16,  28, 235 };

	int show_result = (gstate == GS_ROUND || gstate == GS_MATCH);
	int show_pause  = (gstate == GS_FIGHT && paused);
	Clay_Color win_col = white;
	if (gstate == GS_ROUND) {
		snprintf(title_buf, sizeof title_buf, "PLAYER %d WINS THE ROUND", round_winner);
		win_col = (round_winner == 1) ? p1c : p2c;
	} else if (gstate == GS_MATCH) {
		snprintf(title_buf, sizeof title_buf, "PLAYER %d WINS THE MATCH!", match_winner);
		snprintf(sub_buf, sizeof sub_buf, "Final  %d - %d", score1, score2);
		win_col = (match_winner == 1) ? p1c : p2c;
	}

	Clay_SetLayoutDimensions((Clay_Dimensions){ SCREEN_WIDTH, SCREEN_HEIGHT });
	Clay_BeginLayout();

	CLAY(CLAY_ID("UIRoot"), {
		.layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
			.padding = { 0, 0, 58, 0 },               /* push the pill below the bars */
			.layoutDirection = CLAY_TOP_TO_BOTTOM,
			.childAlignment = { .x = CLAY_ALIGN_X_CENTER }
		}
	}) {
		CLAY(CLAY_ID("ScorePill"), {
			.layout = { .padding = { 20, 20, 6, 6 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } },
			.backgroundColor = pillbg,
			.border = { .color = pillbd, .width = CLAY_BORDER_OUTSIDE(1) }
		}) {
			CLAY_TEXT(clay_str(score_buf), CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 22 }));
		}

		CLAY(CLAY_ID("SpcTop"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}

		if (show_result) {
			CLAY(CLAY_ID("ResultPanel"), {
				.layout = {
					.padding = CLAY_PADDING_ALL(24), .childGap = 12,
					.layoutDirection = CLAY_TOP_TO_BOTTOM,
					.childAlignment = { .x = CLAY_ALIGN_X_CENTER }
				},
				.backgroundColor = panbg,
				.border = { .color = win_col, .width = CLAY_BORDER_OUTSIDE(3) }
			}) {
				CLAY_TEXT(clay_str(title_buf), CLAY_TEXT_CONFIG({ .textColor = win_col, .fontSize = 30 }));
				if (gstate == GS_MATCH) {
					CLAY_TEXT(clay_str(sub_buf), CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 18 }));
					CLAY_TEXT(CLAY_STRING("Press START to rematch"),
					          CLAY_TEXT_CONFIG({ .textColor = yellow, .fontSize = 18 }));
				}
			}
		} else if (show_pause) {
			CLAY(CLAY_ID("PausePanel"), {
				.layout = {
					.padding = CLAY_PADDING_ALL(24), .childGap = 10,
					.layoutDirection = CLAY_TOP_TO_BOTTOM,
					.childAlignment = { .x = CLAY_ALIGN_X_CENTER }
				},
				.backgroundColor = panbg,
				.border = { .color = white, .width = CLAY_BORDER_OUTSIDE(2) }
			}) {
				CLAY_TEXT(CLAY_STRING("PAUSED"), CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 30 }));
				CLAY_TEXT(CLAY_STRING("Start: resume    O: quit to menu"),
				          CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 16 }));
			}
		}

		CLAY(CLAY_ID("SpcBot"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
	}

	Clay_RenderCommandArray cmds = Clay_EndLayout(0.0f);
	clay_render(cmds);
}

/* Arena floor backdrop for the menu / select screens. */
static void draw_backdrop(void)
{
	draw_background(arena_sel);
	floor_quad();
	floor_grid();
	if (app_state == APP_MENU) {
		draw_fighter(-9.0f, 0.0f, COLOR_P1, 0, p1_char);
		draw_fighter( 9.0f, 0.0f, COLOR_P2, 0, p2_char);
	}
	/* In char-select the previews are drawn by render_charsel. */
}

/* Mode-select menu (Clay). */
static void render_menu(void)
{
	static const char *items[4] = { "BATTLE", "TOURNAMENT", "MISSION", "QUIT" };
	const Clay_Color white = { 255, 255, 255, 255 }, dim = { 160, 160, 160, 255 };
	const Clay_Color orange = { 255, 136, 0, 255 }, rowc = { 40, 60, 84, 255 };
	const Clay_Color rowf = { 64, 92, 128, 255 }, focus = { 255, 200, 50, 255 };

	Clay_SetLayoutDimensions((Clay_Dimensions){ SCREEN_WIDTH, SCREEN_HEIGHT });
	Clay_BeginLayout();
	CLAY(CLAY_ID("MenuRoot"), {
		.layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
			.padding = CLAY_PADDING_ALL(40), .childGap = 14,
			.layoutDirection = CLAY_TOP_TO_BOTTOM, .childAlignment = { .x = CLAY_ALIGN_X_CENTER }
		}
	}) {
		CLAY_TEXT(CLAY_STRING("KI BLAST ARENA"), CLAY_TEXT_CONFIG({ .textColor = orange, .fontSize = 40 }));
		CLAY_TEXT(CLAY_STRING("Select a mode"), CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 16 }));
		CLAY(CLAY_ID("MSpc"), { .layout = { .sizing = { CLAY_SIZING_FIXED(0), CLAY_SIZING_FIXED(12) } } }) {}
		for (int i = 0; i < 4; i++) {
			int f = (i == menu_sel);
			CLAY(CLAY_IDI("MItem", i), {
				.layout = {
					.sizing = { CLAY_SIZING_FIXED(360), CLAY_SIZING_FIXED(46) },
					.padding = { 20, 20, 8, 8 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }
				},
				.backgroundColor = f ? rowf : rowc,
				.border = { .color = focus, .width = CLAY_BORDER_OUTSIDE(f ? 3 : 0) }
			}) {
				CLAY_TEXT(clay_str(items[i]), CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 24 }));
			}
		}
		CLAY(CLAY_ID("MSpc2"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
		CLAY_TEXT(CLAY_STRING("D-pad: move    X: select    Select+Start: exit"),
		          CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 14 }));
	}
	clay_render(Clay_EndLayout(0.0f));
}

/* Character grid select (Clay). */
static void render_charsel(void)
{
	static char cellnames[ROSTER_N][3];
	static int cellnames_ready = 0;
	static char title[24], info[72], p1line[80], mline[128], tline[80], aline[80];

	if (!cellnames_ready) {
		for (int i = 0; i < ROSTER_N; i++) snprintf(cellnames[i], 3, "%02d", i);
		cellnames_ready = 1;
	}

	const Clay_Color white = { 255, 255, 255, 255 }, dim = { 160, 160, 160, 255 };
	const Clay_Color p1c = { 63, 169, 245, 255 }, p2c = { 245, 80, 63, 255 }, yellow = { 255, 210, 63, 255 };
	const Clay_Color cell = { 30, 44, 60, 255 }, cellf = { 64, 92, 128, 255 }, focus = { 255, 200, 50, 255 };

	const Character *c = &roster[sel_cursor];
	Clay_Color hdr = (sel_phase == 0) ? p1c : p2c;
	snprintf(title, sizeof title, "SELECT %s", sel_phase == 0 ? "P1" : "P2");
	snprintf(info, sizeof info, "%02d  %s   -   PWR %.1f", sel_cursor, c->name, (double)c->power);
	snprintf(p1line, sizeof p1line, "P1: %s", roster[p1_char].name);
	const Mission *ms = &missions[mission_sel];
	snprintf(mline, sizeof mline, "Mission %d/%d (%d unlocked): %s   vs %s   (L1/R1)",
	         mission_sel + 1, MISSION_N, missions_unlocked, ms->title, roster[ms->enemy].name);
	snprintf(tline, sizeof tline, "Tournament  Tier %d/3   (L1/R1 change)", tour_tier);
	snprintf(aline, sizeof aline, "Arena: %s%s", arena_src[arena_sel].name,
	         mode == MODE_BATTLE ? "   (L2/R2)" : "");

	/* Big character previews behind the grid: cursor (and the chosen P1 in phase 2). */
	if (sel_phase == 0) {
		draw_char_sprite(sel_cursor, SCREEN_WIDTH / 2.0f, 478.0f, 240.0f);
	} else {
		draw_char_sprite(p1_char,    300.0f, 470.0f, 220.0f);
		draw_char_sprite(sel_cursor, 548.0f, 470.0f, 220.0f);
	}

	Clay_SetLayoutDimensions((Clay_Dimensions){ SCREEN_WIDTH, SCREEN_HEIGHT });
	Clay_BeginLayout();
	CLAY(CLAY_ID("CSRoot"), {
		.layout = {
			.sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
			.padding = CLAY_PADDING_ALL(22), .childGap = 12,
			.layoutDirection = CLAY_TOP_TO_BOTTOM, .childAlignment = { .x = CLAY_ALIGN_X_CENTER }
		}
	}) {
		CLAY_TEXT(clay_str(title), CLAY_TEXT_CONFIG({ .textColor = hdr, .fontSize = 28 }));

		for (int r = 0; r < 3; r++) {
			CLAY(CLAY_IDI("Row", r), { .layout = { .childGap = 6, .layoutDirection = CLAY_LEFT_TO_RIGHT } }) {
				for (int col = 0; col < 10; col++) {
					int idx = r * 10 + col;
					int f = (idx == sel_cursor);
					int isP1 = (sel_phase == 1 && idx == p1_char);
					CLAY(CLAY_IDI("Cell", idx), {
						.layout = {
							.sizing = { CLAY_SIZING_FIXED(42), CLAY_SIZING_FIXED(34) },
							.childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER }
						},
						.backgroundColor = f ? cellf : cell,
						.border = { .color = isP1 ? p1c : focus, .width = CLAY_BORDER_OUTSIDE((f || isP1) ? 3 : 0) }
					}) {
						CLAY_TEXT(clay_str(cellnames[idx]),
						          CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 18 }));
					}
				}
			}
		}

		CLAY_TEXT(clay_str(info), CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 22 }));
		if (sel_phase == 1)
			CLAY_TEXT(clay_str(p1line), CLAY_TEXT_CONFIG({ .textColor = p1c, .fontSize = 16 }));
		if (mode == MODE_MISSION && sel_phase == 0)
			CLAY_TEXT(clay_str(mline), CLAY_TEXT_CONFIG({ .textColor = yellow, .fontSize = 16 }));
		if (mode == MODE_TOURNAMENT && sel_phase == 0)
			CLAY_TEXT(clay_str(tline), CLAY_TEXT_CONFIG({ .textColor = yellow, .fontSize = 16 }));
		if (sel_phase == 0)
			CLAY_TEXT(clay_str(aline), CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 15 }));

		CLAY(CLAY_ID("CSpc"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
		CLAY_TEXT(CLAY_STRING("D-pad: move    X: select    O: back"),
		          CLAY_TEXT_CONFIG({ .textColor = dim, .fontSize = 14 }));
	}
	clay_render(Clay_EndLayout(0.0f));
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	printf("\n=== Ki Blast Arena (Phase 9 menus & modes) ===\n");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
	init_screen();
	load_char_textures();    /* 30 character sprites (after ya2d_init) */
	load_arena_textures();   /* 7 arena backgrounds */
	ioPadInit(7);
	camera_setup();
	audio_init();            /* MikMod music + SFX (silent if it fails) */
	reset_match();

	while (running) {
		poll_pads();
		if (quit_combo()) running = 0;          /* Select+Start on either pad */

		switch (app_state) {
		case APP_MENU:    update_menu();    break;
		case APP_CHARSEL: update_charsel(); break;
		case APP_FIGHT:   update_game();    break;
		}

		audio_update();         /* feed the MikMod mixer each frame */

		begin_2d_frame();
		if (app_state == APP_FIGHT) {
			draw_arena();
			draw_hud();              /* custom: balance + ki bars, names */
			build_and_render_ui();   /* Clay: score pill + result/pause panels */
		} else {
			draw_backdrop();
			if (app_state == APP_MENU) render_menu();
			else                       render_charsel();
		}
		tiny3d_Flip();

		sysUtilCheckCallback();
	}

	printf("Exiting...\n");
	audio_shutdown();
	ya2d_deinit();
	ioPadEnd();
	return 0;
}
