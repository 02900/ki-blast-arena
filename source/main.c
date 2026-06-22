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

/* Combat model (BalanceOfPower.cs). */
#define LEVEL_POWER   5.0f
#define BAL_MAX       100.0f
#define BAL_START     50.0f
#define KI_MAX        100.0f
#define KI_MIN        1.0f
#define KI_START      35.0f
#define MELEE_DMG     (1.5f * LEVEL_POWER)
#define MELEE_RANGE   3.4f      /* center-to-center XZ contact distance */
#define MELEE_KIGAIN  7.0f
#define MELEE_CD      14        /* frames between melee hits */
#define KNOCKBACK     1.6f      /* world units pushed on hit */
#define ROUND_WINS    3         /* first to this many round wins = match */
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

/* CPU AI (CPUController.cs / FMS.cs). Difficulty scales with LEVEL_POWER (1..10). */
#define CPU_SHOOT_RATE (0.9f - LEVEL_POWER / 75.0f)    /* sec between blasts  */
#define CPU_PUNCH_RATE (0.20f - LEVEL_POWER / 100.0f)  /* sec between melees  */
#define CPU_SPEED_BASE (10.0f + LEVEL_POWER / 2.0f)    /* world units/second  */
#define CPU_SIGHT      10.0f                           /* chase within this   */

#define STICK_DEAD    32

/* Software camera (deterministic pinhole). */
#define FOCAL  520.0f
#define CAM_CX (SCREEN_WIDTH / 2.0f)
#define CAM_CY (SCREEN_HEIGHT / 2.0f)

static int running = 1;
static padInfo pad_info;
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
static int move_fighter(float *x, float *z, float mx, float mz, float ki)
{
	const float moveSpeed = 0.2f + LEVEL_POWER / 50.0f;
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
		balance += MELEE_DMG;
		ki1 += MELEE_KIGAIN; clampf(&ki1, KI_MIN, KI_MAX);
		knockback(&f2x, &f2z, f1x, f1z);
		melee_cd1 = MELEE_CD;
	} else {
		balance -= MELEE_DMG;
		ki2 += MELEE_KIGAIN; clampf(&ki2, KI_MIN, KI_MAX);
		knockback(&f1x, &f1z, f2x, f2z);
		melee_cd2 = MELEE_CD;
	}
}

static void spawn_expl(float x, float y, float z, int owner)
{
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
			float amount = BLAST_DMGBASE * (b->tier + 1) + LEVEL_POWER * 1.25f;
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

/* Seek the current destination at the ki-scaled CPU speed (stops within 0.5). */
static void cpu_move(void)
{
	float dx = cpu_destx - f2x, dz = cpu_destz - f2z;
	float d = sqrtf(dx*dx + dz*dz);
	if (d <= 0.5f) { moving2 = 0; return; }
	float speed = (CPU_SPEED_BASE + CPU_SPEED_BASE * 0.5f * ki2 / 100.0f) * FRAME_DT;
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
		if (cpu_shoot_t >= CPU_SHOOT_RATE) { try_fire(2); cpu_shoot_t = 0.0f; }
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
	if (fighters_in_contact() && cpu_punch_t >= CPU_PUNCH_RATE) {
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
	if (score1 >= ROUND_WINS || score2 >= ROUND_WINS) {
		match_winner = (score1 >= ROUND_WINS) ? 1 : 2;
		gstate = GS_MATCH;
	} else {
		gstate = GS_ROUND;
		banner_timer = ROUND_BANNER;
	}
}

/* --- per-frame game update ------------------------------------------------ */
/* Edge state per pad: bit0 = Square (melee), bit1 = Start, bit2 = Circle (blast). */
#define BTN_SQ 1u
#define BTN_ST 2u
#define BTN_CI 4u

/* Returns 0 to request quit. */
static int update_game(void)
{
	static u32 prev[2] = { 0, 0 };
	static padData held[2];   /* last good packet per port (persists across frames) */
	padData pd[2];
	int conn[2] = { 0, 0 };

	ioPadGetInfo(&pad_info);
	for (int i = 0; i < 2; i++) {
		/* ioPadGetData sets len = 0 on frames with NO new data (e.g. a button held
		 * while the sticks are still). Using that frame as-is would read the held
		 * button as released and stall ki charging. So refresh our retained state
		 * only on a fresh packet (len > 0) and reuse it otherwise; a held input
		 * stays held. Zero-init avoids stack garbage from a phantom port. */
		padData tmp;
		memset(&tmp, 0, sizeof(tmp));
		if (pad_info.status[i] && ioPadGetData(i, &tmp) == 0) {
			conn[i] = 1;
			if (tmp.len > 0) held[i] = tmp;   /* fresh data: remember it */
			pd[i] = held[i];                  /* else reuse last known state */
		} else {
			conn[i] = 0;
			memset(&held[i], 0, sizeof(held[i]));   /* forget on disconnect */
			memset(&pd[i], 0, sizeof(pd[i]));
		}
	}
	p1_conn = conn[0];
	p2_conn = conn[1];

	/* Quit: Select+Start on either pad. */
	for (int i = 0; i < 2; i++)
		if (conn[i] && pd[i].BTN_SELECT && pd[i].BTN_START) return 0;

	/* Per-pad edge detection (Square + Start). */
	u32 pressed_sq[2] = { 0, 0 };
	u32 pressed_blast[2] = { 0, 0 };
	u32 pressed_start = 0;
	for (int i = 0; i < 2; i++) {
		u32 cur = 0;
		if (conn[i]) cur = (pd[i].BTN_SQUARE ? BTN_SQ : 0u)
		                 | (pd[i].BTN_START  ? BTN_ST : 0u)
		                 | (pd[i].BTN_CIRCLE ? BTN_CI : 0u);
		u32 p = cur & ~prev[i];
		prev[i] = cur;
		pressed_sq[i] = p & BTN_SQ;
		pressed_blast[i] = p & BTN_CI;
		pressed_start |= (p & BTN_ST);
	}

	if (melee_cd1 > 0) melee_cd1--;
	if (melee_cd2 > 0) melee_cd2--;

	if (gstate == GS_ROUND) {
		if (--banner_timer <= 0) reset_round();
		return 1;
	}
	if (gstate == GS_MATCH) {
		if (pressed_start) reset_match();
		return 1;
	}

	/* GS_FIGHT */
	if (pressed_start) paused = !paused;
	if (paused) { moving1 = moving2 = charging1 = charging2 = 0; return 1; }

	/* Fighter 1: always the human on pad 0. */
	float m1x = 0.0f, m1z = 0.0f;
	if (conn[0]) read_move(&pd[0], &m1x, &m1z);
	moving1 = move_fighter(&f1x, &f1z, m1x, m1z, ki1);
	charging1 = (conn[0] && pd[0].BTN_CROSS) ? 1 : 0;
	charge_ki(&ki1, charging1, moving1);

	/* Fighter 2: human on pad 1, or the CPU AI when no second controller. */
	if (p2_conn) {
		float m2x = 0.0f, m2z = 0.0f;
		read_move(&pd[1], &m2x, &m2z);
		moving2 = move_fighter(&f2x, &f2z, m2x, m2z, ki2);
		charging2 = pd[1].BTN_CROSS ? 1 : 0;
		charge_ki(&ki2, charging2, moving2);
	} else {
		cpu_update();   /* drives f2: move, charge, blasts, melee */
	}

	/* Melee on contact (edge-triggered, cooldown-gated). */
	if (fighters_in_contact()) {
		if (pressed_sq[0] && melee_cd1 == 0) melee(1);
		if (pressed_sq[1] && melee_cd2 == 0) melee(0);
	}

	/* Ki blasts (Circle), then advance all projectiles/explosions. */
	if (pressed_blast[0]) try_fire(1);
	if (pressed_blast[1]) try_fire(2);
	update_effects();

	/* Round end: someone filled the balance bar. */
	if (balance >= BAL_MAX) { balance = BAL_MAX; round_winner = 1; score1++; end_round(); }
	else if (balance <= 0.0f) { balance = 0.0f; round_winner = 2; score2++; end_round(); }

	return 1;
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

static void draw_fighter(float wx, float wz, u32 color, int aura)
{
	float bx, by, sc;
	if (!project(wx, 0.0f, wz, &bx, &by, &sc)) return;
	float w = FIGHTER_W * sc, h = FIGHTER_H * sc;
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

static void draw_arena(void)
{
	floor_quad();
	floor_grid();

	float d1[3], d2[3], p1[3] = { f1x, 0, f1z }, p2[3] = { f2x, 0, f2z };
	v_sub(p1, cam_eye, d1); v_sub(p2, cam_eye, d2);
	if (v_dot(d1, cam_f) >= v_dot(d2, cam_f)) {
		draw_fighter(f1x, f1z, COLOR_P1, charging1);
		draw_fighter(f2x, f2z, COLOR_P2, charging2);
	} else {
		draw_fighter(f2x, f2z, COLOR_P2, charging2);
		draw_fighter(f1x, f1z, COLOR_P1, charging1);
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

	/* Controller presence: P1 needs a pad; P2 is the CPU when pad 2 is absent. */
	if (!p1_conn)
		centered(150, "Connect controller 1 (P1)", COLOR_P1, 16, 22);
	if (!p2_conn)
		display_ttf_string(SCREEN_WIDTH - 92, 52, "P2: CPU", COLOR_P2, 0, 12, 16);

	display_ttf_string(28, SCREEN_HEIGHT - 28,
		"KI BLAST ARENA - P7 | P1=pad1  P2=pad2 or CPU | move | hold X charge | O blast | Square melee | Start pause",
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
				.layout = { .padding = CLAY_PADDING_ALL(24), .childAlignment = { .x = CLAY_ALIGN_X_CENTER } },
				.backgroundColor = panbg,
				.border = { .color = white, .width = CLAY_BORDER_OUTSIDE(2) }
			}) {
				CLAY_TEXT(CLAY_STRING("PAUSED"), CLAY_TEXT_CONFIG({ .textColor = white, .fontSize = 30 }));
			}
		}

		CLAY(CLAY_ID("SpcBot"), { .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {}
	}

	Clay_RenderCommandArray cmds = Clay_EndLayout(0.0f);
	clay_render(cmds);
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	printf("\n=== Ki Blast Arena (Phase 7 CPU AI) ===\n");

	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sys_callback, NULL);
	init_screen();
	ioPadInit(7);
	camera_setup();
	reset_match();

	while (running) {
		if (!update_game())
			running = 0;

		begin_2d_frame();
		draw_arena();
		draw_hud();              /* custom: balance + ki bars, hints */
		build_and_render_ui();   /* Clay: score pill + result/pause panels */
		tiny3d_Flip();

		sysUtilCheckCallback();
	}

	printf("Exiting...\n");
	ya2d_deinit();
	ioPadEnd();
	return 0;
}
