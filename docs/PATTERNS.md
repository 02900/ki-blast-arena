# Patterns & Gotchas — PS3 / PSL1GHT homebrew

Hard-won conventions from porting this game. They are written to be reusable in
future PS3 homebrew (and, where noted, any game port). Each entry is **what to do**,
**why**, and the **trap it avoids**.

> Rule of thumb that underlies most of this: **you cannot run the build on the dev
> host** — only the user can, on a real PS3 / RPCS3. So favour code you can fully
> reason about, change in small reversible steps, build green in the toolchain image,
> and mark on-hardware behaviour as *unverified* until someone plays it.

---

## 1. Build & verify workflow

- **Build through the Docker toolchain**, never assume a local PSL1GHT:
  ```bash
  DOCKER_DEFAULT_PLATFORM=linux/amd64 ./scripts/build.sh        # -> src.self
  PS3_IP=192.168.x.x ./scripts/deploy.sh                        # ps3load to console
  ```
  `DOCKER_DEFAULT_PLATFORM` is read by the Docker CLI itself, so the scripts need no
  `--platform` flag (the image is x86_64; Apple Silicon emulates it).
- **Output is named after the mount dir.** Mounted at `/src`, the Makefile's
  `TARGET := $(notdir $(CURDIR))` makes `src.elf` / `src.self`. Deploy/CI match by glob
  or `src.self`, not a repo-named file.
- **Keep `-Wall` clean.** The build uses `-Wall`; treat warnings as failures. Common
  one: `-Wmisleading-indentation` from `if (a) x; if (b) y;` on one line — split it.
- **Small, reversible increments.** Build green after every change; a wrong matrix/
  color/sign often shows only as a black or garbled screen the user has to catch.
- **Transient toolchain segfaults are normal under emulation.** The PPU gcc 7.2
  (`cc1` / `collect2`) sporadically crashes with "internal compiler error: Segmentation
  fault" when the x86_64 image runs emulated on Apple Silicon. It's not your code —
  just re-run. `scripts/build.sh` retries automatically; don't go hunting a code cause
  unless the failure is *deterministic* (same error every run).

## 2. Input — DualShock via the PSL1GHT pad API

### 2.1 `padData.len == 0` means "no new data" — retain the last packet ⚠️

This bit this project **three times** (phantom flailing, false "disconnected", ki
charge stalling while still). `cellPadGetData` sets `len = 0` on frames with no input
change (e.g. a button held while the sticks are still). If you zero the struct every
frame and use it as-is, held buttons read as released on those frames.

**Do:** read into a temp; refresh a retained per-port copy only when `len > 0`; reuse
it otherwise so held inputs stay held.

```c
static padData held[2];                 /* persists across frames */
padData pd[2]; int conn[2] = {0,0};
ioPadGetInfo(&pad_info);
for (int i = 0; i < 2; i++) {
    padData tmp; memset(&tmp, 0, sizeof tmp);          /* zero-init: no stack garbage */
    if (pad_info.status[i] && ioPadGetData(i, &tmp) == 0) {
        conn[i] = 1;
        if (tmp.len > 0) held[i] = tmp;                /* fresh data: remember it     */
        pd[i] = held[i];                               /* else reuse last known state */
    } else {
        conn[i] = 0; memset(&held[i], 0, sizeof held[i]);  /* forget on disconnect    */
    }
}
```

**Don't:** gate *connection* on `len > 0` (a motionless pad would read as disconnected),
and **don't** read into an uninitialized struct (a phantom/unconfigured port — common on
RPCS3 — leaves stack garbage that sends a fighter flailing into a corner).

### 2.2 Two controllers = two ports

`padInfo.status[i]` flags each connected port; `ioPadGetData(i, &data)` reads a specific
one. Port 0 = player 1, port 1 = player 2 — same layout each. Accept menu/quit buttons
from *either* pad. On RPCS3, mapping the *same* physical pad to two players makes both
fighters move together; that's expected, not a bug.

### 2.3 Edge-trigger actions, level-read holds

One-shot actions (fire, melee, menu select) must be edge-detected so a held button does
not repeat; continuous actions (charge, move) read the current level.

```c
u32 cur = (pd.BTN_SQUARE?1:0) | (pd.BTN_CIRCLE?2:0);
u32 pressed = cur & ~prev;   prev = cur;   /* pressed = this frame's new presses */
```

### 2.4 Analog deadzone + the "(0,0) = no data" guard

Map a stick byte (0..255, centre 128) through a deadzone. Treat **both axes exactly 0**
as "no analog data this frame" (digital pad / not read), not full down-left — otherwise
the character drifts on startup.

```c
if (!(pd.ANA_L_H == 0 && pd.ANA_L_V == 0)) { mx = axis(pd.ANA_L_H); mz = -axis(pd.ANA_L_V); }
```

## 3. Rendering (Tiny3D + ya2d)

### 3.1 Colour format mismatch ⚠️

- `tiny3d_Clear()` takes **ARGB** (`0xAARRGGBB`).
- `ya2d_*` fills and `tiny3d_VertexColor()` take **RGBA** (`0xRRGGBBAA`).

Mixing them silently produces wrong colours (e.g. a "blue" clear coming out red). Keep
clear constants separate and comment the byte order at the define.

### 3.2 Prefer a deterministic software camera when you can't iterate on-device

Tiny3D has a full matrix pipeline (`tiny3d_Project3D`, `SetProjectionMatrix`,
`SetMatrixModelView`, `matrix.h`), but its multiply order / FOV conventions are easy to
get subtly wrong, and a wrong camera = a black screen you can only debug on hardware.
A small, fully-understood **pinhole projection** (lookAt basis + perspective divide)
feeding Tiny3D's 2D primitives is predictable and reviewable:

```c
/* basis: f = norm(target-eye); r = norm(cross(up,f)); u = cross(f,r) */
float d[3]; v_sub(p, eye, d);
float vz = v_dot(d, f); if (vz < 0.05f) return 0;        /* behind camera */
*sx = CX + FOCAL * v_dot(d, r) / vz;
*sy = CY - FOCAL * v_dot(d, u) / vz;
*scale = FOCAL / vz;                                     /* px per world unit at depth */
```

General principle: **when feedback loops are slow/expensive, choose the approach you can
verify by reading, not by running.**

### 3.3 No z-buffer in 2D mode → painter's algorithm

Draw far-to-near. Sort objects by camera-forward depth (`dot(pos - eye, forward)`),
draw the larger-depth one first. Fine for a handful of actors.

### 3.4 Clamp the footprint edge, not the centre

Clamp `pos ± half-extent` to the bounds so the body stops at the wall, not its centre.
A camera-facing **billboard is flat in Z**, so its Z footprint is 0 (it can reach the
front/back edge); give X a real half-width. Revisit when real meshes replace billboards.

### 3.5 Fonts come from `/dev_flash`

System TTFs (`/dev_flash/data/font/SCE-PS3-*.TTF`) are present on real consoles and
RPCS3. Load them via the `ttf_render` helper; don't ship your own for basic UI.

## 4. Game loop & simulation

- **Clean XMB exit:** `sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, cb, NULL)` once, then
  `sysUtilCheckCallback()` **every frame**; set a `running = 0` flag on `SYSUTIL_EXIT_GAME`.
  Without the per-frame check the console can't reclaim the app.
- **Fixed timestep.** The loop runs at vsync; use a constant `FRAME_DT = 1/60` for
  rates (charge/sec, speeds) instead of measuring wall time. Simple and deterministic.
- **Fixed pools for transient objects** (projectiles, explosions): a small `struct{int active; …}` array
  with an `active` flag. No per-frame allocation, trivial lifetime, cache-friendly.
- **Embed assets with `bin2o`.** Files in `data/*.png|jpg|bin|mod|s3m` become extern
  symbols (`foo_png` / `foo_png_size`) linked into the `.self`, so they work over
  `ps3load` with no PKG install. Bulky assets go in `pkgfiles/assets/` (PKG only).

## 5. Audio (MikMod)

MikMod plays tracker **modules** and **samples** (PCM) — it **cannot decode OGG/MP3**.
Drive it with `MikMod_Update()` once per frame and reserve voices with
`MikMod_SetNumVoices(music, sfx)` (sample playback needs sfx voices reserved).

### 5.1 Synthesize SFX in code — no assets needed

You can generate sound effects procedurally: build a PCM waveform with math (a decaying
sine for a "thud", a falling sweep for a "pew", noise for an explosion), wrap it as a
little-endian WAV in memory, and load it as a MikMod sample. Self-contained (no asset
files), great for prototyping, placeholders, or a stylized retro feel.

```c
#define SR 22050
short pcm[SR/2];
int n = SR * 8 / 100;                       /* 80 ms */
for (int i = 0; i < n; i++) {
    float t = (float)i / SR, env = expf(-t * 22.0f);     /* fast decay */
    float s = sinf(2*M_PI*150.0f*t) * 0.7f + noisef() * 0.3f;
    pcm[i] = (short)(env * s * 22000.0f);
}
/* -> make_wav(pcm, n) into a byte buffer, then Sample_LoadGeneric (see 5.2/5.3) */
```

`noisef()` is a tiny LCG mapped to −1..1. Vary frequency/decay/noise mix per effect.

### 5.2 Load audio from memory via an MREADER

MikMod's loaders (`Sample_LoadGeneric`, `Player_LoadGeneric`) take an **`MREADER`**, not a
raw pointer. Implement a ~5-function in-memory reader over an embedded buffer (from
`bin2o`) and pass `&reader.core`:

```c
typedef struct { MREADER core; const unsigned char *data; long size, pos; } MemReader;
/* Get -> byte or EOF; Read -> 1 on FULL read else 0; Seek -> 0 on success (non-zero
 * error); Tell -> pos; Eof -> pos>=size.  NOTE the inverted Read vs Seek conventions. */
SAMPLE *s = Sample_LoadGeneric(&mr.core);   /* NULL on failure */
```

Embed the `.wav` with `bin2o` by naming the file `*.bin` in `data/` (the Makefile's
`bin2o` rule keys on extension; MikMod parses the RIFF header, not the filename).

Convert real assets to a clean canonical WAV first (strips odd chunks that loaders trip
on), e.g. with ffmpeg:
```bash
ffmpeg -i in.ogg -ac 1 -ar 22050 -c:a pcm_s16le -map_metadata -1 -f wav data/music.bin
```

### 5.3 Hand-built WAV bytes must be little-endian (PPU is big-endian) ⚠️

WAV is little-endian by spec. If you **build** a WAV yourself (e.g. for synthesized SFX),
write the header fields *and* the int16 PCM as explicit LE bytes — a naive `u32`/`s16`
store is big-endian on the PPU and MikMod will misread it. (Real `.wav` files from ffmpeg
are already LE, so this only bites hand-built buffers.)

```c
static void put_u16le(unsigned char *b, unsigned v){ b[0]=v; b[1]=v>>8; }
static void put_u32le(unsigned char *b, unsigned v){ b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24; }
```

### 5.4 Loop long music as a sample (the OGG/MP3 way)

Since MikMod can't play OGG/MP3 and a module would mean re-authoring, decode the track to
PCM WAV on the host (ffmpeg) and play it as **one long looping sample**: set `SF_LOOP` +
`loopend`, and play with `SFX_CRITICAL` so transient SFX voices can't steal it.

```c
music->flags |= SF_LOOP; music->loopstart = 0; music->loopend = music->length;
SBYTE v = Sample_Play(music, 0, SFX_CRITICAL);
if (v >= 0) Voice_SetVolume(v, 150);        /* duck under SFX */
```

### 5.5 Init audio defensively — it can hang the console ⚠️

A bad audio init can **freeze a PS3** (double-init is a known hang), and you can't hear it
on the dev host. Guard every step; on any failure set an `audio_ok = 0` flag and make all
calls no-ops (degrade to silence). Isolate audio in its own file so it's easy to disable.

## 6. Porting from a Unity (or other engine) game

- **Read the original mechanic before assuming it.** This game's "health" is a
  *power tug-of-war* (a hit transfers power; you win by filling the bar to 100, with a
  single shared balance bar), not a health-to-zero race. Porting it as the latter would
  be wrong. Read the source script, don't infer from the genre.
- **Map engine input to the pad explicitly.** Unity `CrossPlatformInput` axes/buttons →
  concrete DualShock buttons; write the mapping down (header comment + HUD hint).
- **Replace `Rigidbody`/`Collider` with hand-rolled math.** Overlap = XZ distance vs a
  radius; "raycast" = step + proximity test. PS3 has no physics engine to lean on.
- **State machines port cleanly** (round/match flow, AI FSM) — keep them explicit.
- **Record every deviation.** Where the port simplifies the original (e.g. a single
  damage base instead of an inner/outer split, or skipping explosion knockback), say so
  in the commit and the roadmap so it's a known choice, not a silent bug.

---

## TL;DR checklist for a new input/render feature

1. Read input from the **retained** pad packet, never assume fresh data each frame.
2. **Edge-detect** one-shot actions; level-read holds.
3. Watch the **colour format** (clear = ARGB, ya2d/vertex = RGBA).
4. Keep the **camera math deterministic**; sort draws far-to-near.
5. Build **green under `-Wall`** in the toolchain image; mark on-hardware as unverified.
6. **Guard audio init defensively** — a bad init can hang the console, and you can't hear
   it on the dev host.
7. **Document deviations** from the source game.
