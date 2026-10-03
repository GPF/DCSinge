/* KallistiOS ##version##

   sndmp3.h
   (c)2000 Megan Potter
*/

#ifndef __SNDMP3_H
#define __SNDMP3_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/* Wait until the MP3 thread is started and ready */
void sndmp3_wait_start(void);

/* Initialize the MP3 driver; takes an input filename and starts the
   decoding process. */
int sndmp3_start(const char *fn, int loop);

/* Stop playback (implies song unload) */
void sndmp3_stop(void);

/* Pause playback without unloading decoder state */
void sndmp3_pause(void);

/* Resume paused playback */
void sndmp3_resume(void);

/* Returns non-zero if playback is currently paused */
int sndmp3_is_paused(void);

/* Shut everything down */
void sndmp3_shutdown(void);

/* Controls volume of MP3 stream */
void sndmp3_volume(int vol);

/* The main loop for the sound server */
void sndmp3_mainloop(void);

__END_DECLS

#endif	/* __SNDMP3_H */
