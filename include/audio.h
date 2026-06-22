#ifndef AUDIO_H
#define AUDIO_H

/* Audio subsystem (MikMod): looping module music + synthesized PCM SFX.
 * All calls are safe — if init fails, everything degrades to silence and
 * never hangs the console. */

void audio_init(void);       /* set up MikMod, start music, build SFX */
void audio_update(void);     /* call once per frame to feed the mixer  */
void audio_shutdown(void);   /* stop + free at exit                    */

void audio_play_hit(void);       /* melee impact   */
void audio_play_blast(void);     /* ki blast fired */
void audio_play_explosion(void); /* blast detonate */

#endif
