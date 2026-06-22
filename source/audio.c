/*
 * Ki Blast Arena - audio (Phase 8)
 *
 * MikMod-based audio: a looping tracker module for the battle theme, plus short
 * PCM sound effects synthesized in-memory (no external SFX assets needed) and
 * loaded as MikMod samples through an in-memory reader.
 *
 * Everything is defensive: if any step fails, audio_ok stays 0 and every entry
 * point becomes a no-op, so a bad audio init can never hang the PS3.
 *
 * NOTE: the battle module (data/battle.s3m) is a PLACEHOLDER (the "Haiku" S3M
 * from the PS3 homebrew template). MikMod plays tracker modules, not the
 * original game's OGG/MP3, so real music needs a sourced/authored module later.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>

#include <ppu-types.h>
#include <mikmod.h>

#include "audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Embedded battle module (bin2o symbol from data/battle.s3m). */
extern const unsigned char battle_s3m[];
extern const unsigned int  battle_s3m_size;

static int     audio_ok = 0;
static MODULE *music = NULL;
static SAMPLE *sfx_hit = NULL, *sfx_blast = NULL, *sfx_expl = NULL;

/* ---- in-memory MREADER (so MikMod can load from embedded buffers) -------- */
typedef struct { MREADER core; const unsigned char *data; long size; long pos; } MemReader;

static BOOL mr_eof(MREADER *r)  { MemReader *m = (MemReader *)r; return m->pos >= m->size; }
static long mr_tell(MREADER *r) { return ((MemReader *)r)->pos; }

static int mr_get(MREADER *r)
{
	MemReader *m = (MemReader *)r;
	if (m->pos >= m->size) return EOF;
	return m->data[m->pos++];
}

static BOOL mr_read(MREADER *r, void *dst, size_t n)
{
	MemReader *m = (MemReader *)r;
	long rem = m->size - m->pos;
	if ((long)n > rem) {                       /* short read -> failure (0) */
		if (rem > 0) { memcpy(dst, m->data + m->pos, rem); m->pos += rem; }
		return 0;
	}
	memcpy(dst, m->data + m->pos, n);
	m->pos += n;
	return 1;
}

static BOOL mr_seek(MREADER *r, long off, int whence)
{
	MemReader *m = (MemReader *)r;
	long base = (whence == SEEK_SET) ? 0 : (whence == SEEK_CUR) ? m->pos : m->size;
	long np = base + off;
	if (np < 0 || np > m->size) return -1;     /* non-zero -> error */
	m->pos = np;
	return 0;
}

static void mem_reader_init(MemReader *m, const void *data, long size)
{
	m->core.Seek = mr_seek; m->core.Tell = mr_tell; m->core.Read = mr_read;
	m->core.Get = mr_get;   m->core.Eof = mr_eof;
	m->data = (const unsigned char *)data; m->size = size; m->pos = 0;
}

/* ---- SFX synthesis: build a mono 16-bit PCM WAV in memory, load as SAMPLE -- */
#define SR 22050
static short         synth_buf[SR / 2];                  /* up to 0.5 s        */
static unsigned char wav_buf[44 + sizeof(synth_buf)];

static void put_u16le(unsigned char *b, unsigned v) { b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; }
static void put_u32le(unsigned char *b, unsigned v)
{ b[0] = v & 0xFF; b[1] = (v >> 8) & 0xFF; b[2] = (v >> 16) & 0xFF; b[3] = (v >> 24) & 0xFF; }

/* WAV is little-endian by spec; emit bytes explicitly (PPU is big-endian). */
static long make_wav(int nsamples, int rate)
{
	long data_bytes = (long)nsamples * 2;
	memcpy(wav_buf, "RIFF", 4);          put_u32le(wav_buf + 4, 36 + data_bytes);
	memcpy(wav_buf + 8, "WAVE", 4);
	memcpy(wav_buf + 12, "fmt ", 4);     put_u32le(wav_buf + 16, 16);
	put_u16le(wav_buf + 20, 1);          /* PCM   */
	put_u16le(wav_buf + 22, 1);          /* mono  */
	put_u32le(wav_buf + 24, rate);
	put_u32le(wav_buf + 28, rate * 2);   /* byte rate  */
	put_u16le(wav_buf + 32, 2);          /* block align*/
	put_u16le(wav_buf + 34, 16);         /* bits  */
	memcpy(wav_buf + 36, "data", 4);     put_u32le(wav_buf + 40, data_bytes);
	for (int i = 0; i < nsamples; i++)
		put_u16le(wav_buf + 44 + i * 2, (unsigned)((unsigned short)synth_buf[i]));
	return 44 + data_bytes;
}

static SAMPLE *load_synth(int nsamples)
{
	MemReader mr;
	long len = make_wav(nsamples, SR);
	mem_reader_init(&mr, wav_buf, len);
	return Sample_LoadGeneric(&mr.core);   /* may be NULL */
}

static unsigned rng = 0x1234567u;
static float noisef(void)
{
	rng = rng * 1664525u + 1013904223u;
	return ((float)((rng >> 9) & 0x7FFFFF) / 4194303.0f) - 1.0f;   /* -1..1 */
}

/* melee thud: low sine + a touch of noise, fast decay. */
static SAMPLE *make_hit(void)
{
	int n = SR * 8 / 100;   /* 80 ms */
	for (int i = 0; i < n; i++) {
		float t = (float)i / SR, env = expf(-t * 22.0f);
		float s = sinf(2.0f * (float)M_PI * 150.0f * t) * 0.7f + noisef() * 0.3f;
		synth_buf[i] = (short)(env * s * 22000.0f);
	}
	return load_synth(n);
}

/* ki blast: descending sine sweep "pew". */
static SAMPLE *make_blast(void)
{
	int n = SR * 18 / 100;  /* 180 ms */
	float dur = (float)n / SR;
	for (int i = 0; i < n; i++) {
		float t = (float)i / SR, env = expf(-t * 9.0f);
		float f = 900.0f - 700.0f * (t / dur);
		synth_buf[i] = (short)(env * sinf(2.0f * (float)M_PI * f * t) * 22000.0f);
	}
	return load_synth(n);
}

/* explosion: noise burst + low rumble, exponential decay. */
static SAMPLE *make_expl(void)
{
	int n = SR * 35 / 100;  /* 350 ms */
	for (int i = 0; i < n; i++) {
		float t = (float)i / SR, env = expf(-t * 7.0f);
		float s = noisef() * 0.8f + sinf(2.0f * (float)M_PI * 70.0f * t) * 0.3f;
		synth_buf[i] = (short)(env * s * 23000.0f);
	}
	return load_synth(n);
}

/* ---- public API ---------------------------------------------------------- */
void audio_init(void)
{
	if (audio_ok) return;

	MikMod_RegisterAllDrivers();
	MikMod_RegisterAllLoaders();

	md_mode = DMODE_STEREO | DMODE_16BITS | DMODE_SOFT_MUSIC | DMODE_SOFT_SNDFX;
	md_mixfreq = 48000;

	if (MikMod_Init("")) return;             /* stays silent on failure */
	MikMod_SetNumVoices(-1, 8);              /* reserve voices for SFX  */
	if (MikMod_EnableOutput()) { MikMod_Exit(); return; }

	MemReader mr;
	mem_reader_init(&mr, battle_s3m, (long)battle_s3m_size);
	music = Player_LoadGeneric(&mr.core, 64, 0);
	if (music) {
		music->wrap = 1;                     /* loop the module forever */
		Player_SetVolume(100);
		Player_Start(music);
	}

	sfx_hit   = make_hit();
	sfx_blast = make_blast();
	sfx_expl  = make_expl();

	audio_ok = 1;
}

void audio_update(void)
{
	if (audio_ok) MikMod_Update();
}

void audio_shutdown(void)
{
	if (!audio_ok) return;
	audio_ok = 0;
	Player_Stop();
	if (music) { Player_Free(music); music = NULL; }
	if (sfx_hit)   Sample_Free(sfx_hit);
	if (sfx_blast) Sample_Free(sfx_blast);
	if (sfx_expl)  Sample_Free(sfx_expl);
	MikMod_DisableOutput();
	MikMod_Exit();
}

void audio_play_hit(void)       { if (audio_ok && sfx_hit)   Sample_Play(sfx_hit, 0, 0); }
void audio_play_blast(void)     { if (audio_ok && sfx_blast) Sample_Play(sfx_blast, 0, 0); }
void audio_play_explosion(void) { if (audio_ok && sfx_expl)  Sample_Play(sfx_expl, 0, 0); }
