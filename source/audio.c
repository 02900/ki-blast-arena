/*
 * Ki Blast Arena - audio (Phase 8)
 *
 * MikMod-based audio using the ORIGINAL game's assets, converted to mono 16-bit
 * PCM WAV (via ffmpeg) and embedded with bin2o:
 *   - Music: the original "battle ambient" track (OGG -> WAV), played as a
 *     looping sample (MikMod plays modules or samples; the OGG can't be a
 *     module, so it loops as one long PCM sample).
 *   - SFX: the original meleehit1 / basicbeam_fire / kiplosion WAVs.
 *
 * Everything is defensive: if any step fails, audio_ok stays 0 and every entry
 * point becomes a no-op, so a bad audio init can never hang the PS3.
 */

#include <stdio.h>
#include <string.h>

#include <ppu-types.h>
#include <mikmod.h>

#include "audio.h"

/* Embedded WAVs (bin2o symbols from the data/ folder). */
extern const unsigned char music_bin[];      extern const unsigned int music_bin_size;
extern const unsigned char sfx_hit_bin[];    extern const unsigned int sfx_hit_bin_size;
extern const unsigned char sfx_blast_bin[];  extern const unsigned int sfx_blast_bin_size;
extern const unsigned char sfx_expl_bin[];   extern const unsigned int sfx_expl_bin_size;

static int     audio_ok = 0;
static SAMPLE *music = NULL, *sfx_hit = NULL, *sfx_blast = NULL, *sfx_expl = NULL;
static SBYTE   music_voice = -1;

/* ---- in-memory MREADER (so MikMod can load WAVs from embedded buffers) ---- */
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

static SAMPLE *load_sample(const void *data, long size)
{
	MemReader mr;
	mem_reader_init(&mr, data, size);
	return Sample_LoadGeneric(&mr.core);   /* may be NULL */
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
	MikMod_SetNumVoices(0, 16);              /* all voices for samples (music + sfx) */
	if (MikMod_EnableOutput()) { MikMod_Exit(); return; }

	sfx_hit   = load_sample(sfx_hit_bin,   (long)sfx_hit_bin_size);
	sfx_blast = load_sample(sfx_blast_bin, (long)sfx_blast_bin_size);
	sfx_expl  = load_sample(sfx_expl_bin,  (long)sfx_expl_bin_size);

	music = load_sample(music_bin, (long)music_bin_size);
	if (music) {
		music->flags |= SF_LOOP;
		music->loopstart = 0;
		music->loopend = music->length;
		/* SFX_CRITICAL keeps the looping music voice from being stolen by SFX. */
		music_voice = Sample_Play(music, 0, SFX_CRITICAL);
		if (music_voice >= 0) Voice_SetVolume(music_voice, 150);  /* duck under SFX */
	}

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
	if (music_voice >= 0) Voice_Stop(music_voice);
	if (music)     Sample_Free(music);
	if (sfx_hit)   Sample_Free(sfx_hit);
	if (sfx_blast) Sample_Free(sfx_blast);
	if (sfx_expl)  Sample_Free(sfx_expl);
	MikMod_DisableOutput();
	MikMod_Exit();
}

void audio_play_hit(void)       { if (audio_ok && sfx_hit)   Sample_Play(sfx_hit, 0, 0); }
void audio_play_blast(void)     { if (audio_ok && sfx_blast) Sample_Play(sfx_blast, 0, 0); }
void audio_play_explosion(void) { if (audio_ok && sfx_expl)  Sample_Play(sfx_expl, 0, 0); }
