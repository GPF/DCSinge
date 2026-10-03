/* KallistiOS ##version##

   sndserver.h
   (c)2000 Megan Potter
*/

#ifndef __SNDSERVER_H
#define __SNDSERVER_H

#include <sys/cdefs.h>
__BEGIN_DECLS

/* Starts the MP3 server thread */
int mp3_init(void);

/* Starts a song playing */
int mp3_start(const char *fn, int loop);

/* Stops the playing song */
int mp3_stop(void);

/* Pauses the playing song without unloading decoder state */
int mp3_pause(void);

/* Resumes a paused song */
int mp3_resume(void);

/* Returns non-zero if playback is currently paused */
int mp3_is_paused(void);

/* Shuts down the MP3 server thread */
int mp3_shutdown(void);

/* Controls volume level of stream */
void mp3_volume(int vol);

__END_DECLS

#endif	/* __SNDSERVER_H */
