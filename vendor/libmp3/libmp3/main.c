/* KallistiOS ##version##

   libmp3 main.c
   (c)2000-2001 Megan Potter

*/

#include <stdio.h>
#include <kos/thread.h>
#include <dc/sound/stream.h>
#include "sndmp3.h"

void *sndserver_thread(void *blagh) {
	printf("sndserver: started [DEBUG]\r\n");
	printf("sndserver: pid is %d; capabilities: MP3, SFX\r\n", thd_get_current()->tid);

	sndmp3_mainloop();
	
	printf("sndserver exited\r\n");
	return NULL;
}

int mp3_init(void) {
	if (snd_stream_init() < 0)
		return -1;
	if (thd_create(1, sndserver_thread, NULL) != NULL) {
		sndmp3_wait_start();
		return 0;
	} else
		return -1;
}

int mp3_start(const char *fn, int loop) {
	return sndmp3_start(fn, loop);
}

int mp3_stop() {
	sndmp3_stop();
	return 0;
}

int mp3_pause() {
	sndmp3_pause();
	return 0;
}

int mp3_resume() {
	sndmp3_resume();
	return 0;
}

int mp3_is_paused() {
	return sndmp3_is_paused();
}

int mp3_shutdown() {
	sndmp3_shutdown();
	return 0;
}

void mp3_volume(int vol) {
       sndmp3_volume(vol);
}
