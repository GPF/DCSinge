
// singe_dreamcast.c - Hypseus Singe 2 API port/simulation for Dreamcast
// Based on Hypseus Singe - https://github.com/DirtBagXon/hypseus-singe
// and Singe 2 - https://forge.duensing.digital/Public_Skunkworks/singe.git
// uses my dreamcast-fmv encoder for media creation - https://github.com/GPF/dreamcast-fmv


#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include <lfs/lfs.h>
#include <png/png.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <ft2build.h>
#include FT_FREETYPE_H

static mutex_t io_lock = MUTEX_INITIALIZER;
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd/zstd.h>
static ZSTD_DCtx *g_zstd_dctx = NULL;

#define LZ4_FAST_DEC_LOOP 1 
#define LZ4_FREESTANDING 1
#define LZ4_memcpy(d,s,n)  memcpy_fast((d),(s),(n))
#define LZ4_memmove(d,s,n) memmove_fast((d),(s),(n))
#define LZ4_memset(d,v,n)  memset_fast((d),(v),(n))
#include <lz4/lz4.h>

// ---------------------------------------------------------------------------
// 🎮 Singe Dreamcast runtime configuration (auto-loaded from singe.cfg)
// ---------------------------------------------------------------------------
#define SINGE_VERSION       2.10
char G_BASE_PATH[128]   = "/pc/data/";   // Auto-set: /pc/data/ or /cd/data/
char G_GAME_DIR[128]    = "Hologram_Time_Traveler_Singe_2/";
char G_GAME_NAME[128]   = "Hologram Time Traveler";
char G_VIDEO_FILE[128]  = "hologram.dcmv";
char G_SCRIPT_FILE[128] = "Script/timetraveler.singe";
char G_CHUNK_NAME[128]  = "@timetraveler.singe";

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define PREFETCH_AHEAD (MIN(NUM_BUFFERS, (int)(fps * 2.5f)))

#define SINGE_FAKE_DISC_LAG_TICKS 800

// Singe input switch constants
#define SWITCH_UP          0
#define SWITCH_LEFT        1
#define SWITCH_DOWN        2
#define SWITCH_RIGHT       3
#define SWITCH_START1      4
#define SWITCH_START2      5
#define SWITCH_BUTTON1     6
#define SWITCH_BUTTON2     7
#define SWITCH_BUTTON3     8
#define SWITCH_COIN1       9
#define SWITCH_COIN2       10
#define SWITCH_SKILL1      11
#define SWITCH_SKILL2      12
#define SWITCH_SKILL3      13
#define SWITCH_SERVICE     14
#define SWITCH_TEST        15
#define SWITCH_RESET       16
#define SWITCH_SCREENSHOT  17
#define SWITCH_QUIT        18
#define SWITCH_PAUSE       19
#define SWITCH_CONSOLE     20

// Font quality constants
#define FONT_QUALITY_SOLID 1
#define FONT_QUALITY_SHADED 2
#define FONT_QUALITY_BLENDED 3

// Overlay return value
#define OVERLAY_UPDATED 1

static char *GGameName = NULL;
static char *GGamePath = NULL;
static char *GDataDir = NULL;
static char *GGameDir = NULL;
static uint64_t GPreviousInputBits = 0;
static int GMouseX = 0;
static int GMouseY = 0;
static int GMouseRelX = 0;
static int GMouseRelY = 0;
// static int GHalted = 0;
static int g_is_paused = 0;  // 0 for not paused, 1 for paused
_Atomic int preload_paused = 0;
// static int GShowingSingleFrame = 0;
static _Atomic unsigned int GSeekGeneration = 0;
static uint64_t GClipStartTicks = 0;
static uint64_t GTicks = 0;
static uint64_t GTicksOffset = 0;
static int GDecoderActive = 0;
static int GSeeking = 0;
static int GSeekTargetFrame = -1;
static atomic_int seek_request = -1;
static int g_playback_started = 0;
static int g_overlay_ran_once = 0;
static int g_disc_skip_count = 0;
static int g_display_w = 0, g_display_h = 0;
static int g_offset_x = 0, g_offset_y = 0;

float  g_ratio_x = 1.0f;
float  g_ratio_y = 1.0f;
float  g_ratio_x_offset = 0.0f;
float  g_ratio_y_offset = 0.0f;
float  g_scale_x = 1.0f;
float  g_scale_y = 1.0f;

// UI coordinate system: Singe uses 640x480 logical overlay space.
#define UI_LOGICAL_W 640
#define UI_LOGICAL_H 480

// Overlay dimensions (controlled by Lua setOverlaySize)
static int GOverlayWidth  = 360;
static int GOverlayHeight = 240;

// Active video mode (set in singe_startup)
static int is_320 = 0;

// Scaling from logical overlay space (640x480) to actual display resolution.
static float UI_SCALE_X = 1.0f;
static float UI_SCALE_Y = 1.0f;
static int   UI_OFFSET_X = 0;
static int   UI_OFFSET_Y = 0;

// Font state
static FT_Library GFTLibrary = NULL;
static FT_Face GCurrentFont = NULL;
static int GFontQuality = FONT_QUALITY_BLENDED;
static uint8_t GFontColorR = 0, GFontColorG = 255, GFontColorB = 0, GFontColorA = 255;
static uint8_t GBGColorR = 0, GBGColorG = 0, GBGColorB = 0, GBGColorA = 0;

typedef struct FontManager {
    FT_Face *fonts;        // Array to store multiple fonts
    int font_count;        // Number of fonts loaded
    int current_font_idx;  // Index of the currently selected font
} FontManager;

static FontManager g_font_manager = { NULL, 0, -1 };  // Initialize font manager

typedef struct SingeSprite {
    unsigned long hash_id;  // Unique hash ID based on the content (e.g., name or text)
    char *name;             // Optional name for debugging
    int width;
    int height;
    pvr_ptr_t texture;
    pvr_poly_hdr_t hdr;
    struct SingeSprite *next;  // Pointer to the next sprite in the linked list
} SingeSprite;

static SingeSprite *get_cached_sprite(const char *name_or_hash);
static SingeSprite *get_cached_font_sprite(unsigned long hash_value);

typedef struct SingeSound {
    char *name;
    sfxhnd_t handle;
    struct SingeSound *next;
} SingeSound;

static SingeSprite *GSprites = NULL;
static SingeSound *GSounds = NULL;
static lua_State *GLua = NULL;

// Video decoder state (same as Singe)
#define DCMV_MAGIC "DCMV"
#define NUM_BUFFERS 24
#define RING_CAPACITY (NUM_BUFFERS + 1)

enum BufState {
    BUF_EMPTY = 0,
    BUF_LOADING = 1,
    BUF_READY = 2
};

typedef struct {
    int frame;
    int generation;
} PreloadJob;

static PreloadJob preload_ring[RING_CAPACITY];
static atomic_int preload_ring_head = 0;
static atomic_int preload_ring_tail = 0;

static file_t video_fd = -1;
static file_t audio_fd_left = -1, audio_fd_right = -1;
static long left_channel_size = 0;
static uint8_t *compressed_buffer = NULL;
static uint8_t *frame_buffer[NUM_BUFFERS];
static uint32_t *frame_offsets = NULL;
static uint16_t *frame_durations = NULL;
static int last_unique_frame_drawn = -1;
static atomic_int buf_ref_count[NUM_BUFFERS] = { 0 }; 
static _Atomic int displayed_total_frame = 0; 
static atomic_int frame_index = 0;
static float fps;
static int frame_type, video_width, video_height, content_width, content_height;
static int sample_rate, audio_channels;
static int num_unique_frames = 0, num_total_frames = 0;
static int video_frame_size, max_compressed_size, audio_offset;

static pvr_ptr_t pvr_txr;
static pvr_poly_hdr_t hdr;
static pvr_vertex_t vert[4];
static snd_stream_hnd_t stream;

static _Atomic double audio_start_time_ms = 0.0;
static _Atomic int audio_muted = 0;
static float frame_duration = 1.0f / 30.0f;
static double frame_timer_anchor = 0.0;
static _Atomic int buf_state[NUM_BUFFERS] = { BUF_EMPTY };

int soundbufferalloc = 4096;
static volatile int audio_started = 0;
int use_zstd = 0;

static _Atomic int g_audio_left_on  = 1;
static _Atomic int g_audio_right_on = 1;
// Optional: if you have a master movie volume setting
static _Atomic int g_audio_movie_vol = 255;  // 0..255 like KOS volume style

static uint32_t fps_num = 0, fps_den = 0;
static double frame_duration_ms = 0.0;
static int *GTotalToUnique = NULL;
static uint32_t vfd_last_end = 0;
static long last_audio_left_pos = -1;
static long last_audio_right_pos = -1;

// ============================================================================
// Dreamcast Singe Overlay RTT Implementation (non-twiddled ARGB1555)
// Maintains original Lua overlay coordinates (GOverlayWidth/GOverlayHeight)
// ============================================================================



void DC_log(const char *fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    printf("[DC] %s\n", buffer);  // Logs for Dreamcast C side
}


void Singe_log(const char *fmt, ...) {
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    printf("[Singe] %s\n", buffer);
    // dbglog(DBG_INFO, "%s\n\n", buffer);
}

static char* resolve_path(const char* filename) {
    char fullpath[512];
    if (strncmp(filename, G_GAME_DIR, strlen(G_GAME_DIR)) == 0)
        snprintf(fullpath, sizeof(fullpath), "%s%s", G_BASE_PATH, filename);
    else
        snprintf(fullpath, sizeof(fullpath), "%s%s%s", G_BASE_PATH, G_GAME_DIR, filename);
    return strdup(fullpath);
}


// Memory functions
// void *Singe_xmalloc(size_t len) {
//     void *retval = malloc(len);
//     if (!retval) {
//         printf("PANIC: Out of memory!\n");
//         exit(1);
//     }
//     return retval;
// }

void *Singe_xmalloc(size_t len) {
    void *retval = memalign(32, len);
    if (!retval) {
        printf("PANIC: Out of memory (aligned 32)!\n");
        exit(1);
    }
    return retval;
}


void *Singe_xcalloc(size_t nmemb, size_t len) {
    void *retval = calloc(nmemb, len);
    if (!retval) {
        printf("PANIC: Out of memory!\n");
        exit(1);
    }
    return retval;
}

char *Singe_xstrdup(const char *str) {
    char *retval = strdup(str);
    if (!retval) {
        printf("PANIC: Out of memory!\n");
        exit(1);
    }
    return retval;
}

static inline int next_pow2(int v) {
    int p = 1; 
    while (p < v) p <<= 1; 
    return p;
}
static bool overlay_ready = false;
static uint16_t *overlay_buf = NULL;
static pvr_ptr_t overlay_tex = NULL;


int overlay_tex_w = 0;
int overlay_tex_h = 0;

void font_init_char_cache();
static int g_char_cache_initialized = 0;

// static void overlay_init(void) {
//     Singe_log("[SINGE] Initializing overlay texture %dx%d", next_pow2(GOverlayWidth), next_pow2(GOverlayHeight));


//     // Free any previous memory if it exists
//     if (overlay_tex != NULL) {
//         pvr_mem_free(overlay_tex);
//         overlay_tex = NULL;
//     }

//     // Recalculate the next power-of-two size for the overlay
//     overlay_tex_w = next_pow2(GOverlayWidth);  // Set to next power of two
//     overlay_tex_h = next_pow2(GOverlayHeight); // Set to next power of two

//     // Allocate new VRAM for the overlay texture
//     overlay_tex = pvr_mem_malloc(overlay_tex_w * overlay_tex_h * 2);

//     // Allocate and clear the buffer
//     overlay_buf = memalign(32, overlay_tex_w * overlay_tex_h * 2);
//     memset(overlay_buf, 0, overlay_tex_w * overlay_tex_h * 2);
// }


void compute_global_ratios(void)
{
    g_ratio_x = ((double)GOverlayWidth / (double)GOverlayHeight) /
                ((double)g_display_w / (double)g_display_h); // 1.125
    g_ratio_y = 1.0;

    // Dreamcast display scaling (360x240 → 640x480)
    g_scale_x = (double)g_display_w / (double)GOverlayWidth;
    g_scale_y = (double)g_display_h / (double)GOverlayHeight;

    // Compute the same offsets Lua does
    double gunscale = 1.0; // or 100/vldp scale if you expose it
    g_ratio_x_offset = ((gunscale * g_ratio_x) - 1.0) * (GOverlayWidth / 2.0);
    g_ratio_y_offset = ((gunscale * g_ratio_y) - 1.0) * (GOverlayHeight / 2.0);

    Singe_log("[SINGE] ratio_x=%.3f offset_x=%.2f scale_x=%.3f\n",
           g_ratio_x, g_ratio_x_offset, g_scale_x);

    // if (overlay_tex == NULL ||
    //     overlay_tex_w != next_pow2(GOverlayWidth) ||
    //     overlay_tex_h != next_pow2(GOverlayHeight)) {
    //     overlay_init();
    // }  
}


// Simple hash function for strings (djb2)
unsigned long hash(const char *str) {
    unsigned long hash = 5381;
    int c;

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }

    return hash;
}

// Timer function
static inline float psTimer(void) {
    #define AICA_MEM_CLOCK 0x021000
    uint32_t jiffies = g2_read_32(SPU_RAM_UNCACHED_BASE + AICA_MEM_CLOCK);
    const float AICA_TICKS_PER_MS = 4.410f; 
    return jiffies / AICA_TICKS_PER_MS;
}

static inline int ms_to_total_frame_floor(uint32_t ms) {
    uint64_t num = (uint64_t)ms * (uint64_t)fps_num;
    uint64_t den = (uint64_t)fps_den * 1000ULL;
    int f = (int)(num / den);
    if (f < 0) f = 0;
    if (f >= (int)num_total_frames) f = (int)num_total_frames - 1;
    return f;
}

static void init_timebase_from_fps(float fpsf) {
    if (fabsf(fpsf - (24000.0f/1001.0f)) < 0.02f) { fps_num = 24000; fps_den = 1001; }
    else if (fabsf(fpsf - (30000.0f/1001.0f)) < 0.02f) { fps_num = 30000; fps_den = 1001; }
    else if (fabsf(fpsf - (60000.0f/1001.0f)) < 0.02f) { fps_num = 60000; fps_den = 1001; }
    else {
        fps_den = 1000;
        fps_num = (uint32_t)llroundf(fpsf * fps_den);
    }
    frame_duration_ms = (1000.0 * (double)fps_den) / (double)fps_num;
}

static inline int total_to_unique_frame(int total_frame) {
    if ((unsigned)total_frame >= (unsigned)num_total_frames)
        return num_unique_frames - 1;
    return GTotalToUnique[total_frame];
}

static SingeSprite *get_sprite_by_hash_id(unsigned long hash_id) {
    for (SingeSprite *sprite = GSprites; sprite != NULL; sprite = sprite->next) {
        if (sprite->hash_id == hash_id) {
            return sprite;
        }
    }
    return NULL;  // Return NULL if no sprite with that hash_id is found
}


static int is_pow2(int n) { return n > 0 && (n & (n - 1)) == 0; }

// Audio callback
static size_t audio_cb(snd_stream_hnd_t hnd, uintptr_t l, uintptr_t r, size_t req) {
    if (atomic_load(&audio_muted)) {
        memset((void *)l, 0, req);
        if (audio_channels == 2)
            memset((void *)r, 0, req);
        return req;
    }

    size_t half = req / 2;
    size_t lbytes = 0, rbytes = 0;

    // Left channel audio
    if (atomic_load(&g_audio_left_on)) {
        // If not muted, read from the current position
        mutex_lock(&io_lock);
        lbytes = fs_read(audio_fd_left, (void *)l, half);
        mutex_unlock(&io_lock);
        last_audio_left_pos += lbytes;  // Update position
    } else {
        memset((void *)l, 0, half);  // Mute the left channel
        lbytes = half;
        last_audio_left_pos += lbytes;  // Advance the position, even when muted
    }

    // Right channel audio
    if (atomic_load(&g_audio_right_on)) {
        // If not muted, read from the current position
        mutex_lock(&io_lock);
        rbytes = fs_read(audio_fd_right, (void *)r, half);
        mutex_unlock(&io_lock);
        last_audio_right_pos += rbytes;  // Update position
    } else {
        memset((void *)r, 0, half);  // Mute the right channel
        rbytes = half;
        last_audio_right_pos += rbytes;  // Advance the position, even when muted
    }

    return lbytes + rbytes;
}



// Frame loading
static int load_frame(int unique_frame, int buf_index) {
    uint32_t offset = frame_offsets[unique_frame];
    uint32_t next_offset = frame_offsets[unique_frame + 1];
    uint32_t compressed_size = next_offset - offset;
    
    if (vfd_last_end != (long)offset) {
        mutex_lock(&io_lock);
        fs_seek(video_fd, offset, SEEK_SET);
        mutex_unlock(&io_lock);
        vfd_last_end = offset;
    }
    mutex_lock(&io_lock);
    fs_read(video_fd, compressed_buffer, compressed_size);
    mutex_unlock(&io_lock);
    vfd_last_end = offset + compressed_size;
    
    if (use_zstd == 1) {
        ZSTD_DCtx_reset(g_zstd_dctx, ZSTD_reset_session_only);
        ZSTD_inBuffer in = { compressed_buffer, compressed_size, 0 };
        ZSTD_outBuffer out = { frame_buffer[buf_index], (size_t)video_frame_size, 0 };
        
        size_t ret = 1;
        while (ret != 0 && out.pos < out.size) {
            ret = ZSTD_decompressStream(g_zstd_dctx, &out, &in);
            if (ZSTD_isError(ret)) return -1;
        }
        if (out.pos != (size_t)video_frame_size) return -1;
    } else {
        int res = LZ4_decompress_fast(
            (const char *)compressed_buffer,
            (char *)frame_buffer[buf_index],
            video_frame_size);
        if (res < 0){
            Singe_log("LZ4_decompress_fast failed for frame %d (buf %d)", unique_frame, buf_index);
            return -1;
        }
    }

    // Set buffer state to BUF_READY after successfully loading the frame
    atomic_store(&buf_state[buf_index], BUF_READY);

    return 0;
}
// --- render_current_video(): always mark forward progress ---
static void render_current_video(void) {
    int cur_total = atomic_load(&frame_index);
    int unique = total_to_unique_frame(cur_total);
    int buf = unique % NUM_BUFFERS;
    int cur_gen = atomic_load(&GSeekGeneration);

    int state = atomic_load(&buf_state[buf]);

    // Always mark progress, even for repeated unique frames
    atomic_store(&displayed_total_frame, cur_total);

    if (unique == last_unique_frame_drawn) {
        // DC_log("[Render] Repeat frame %d (unique=%d)", cur_total, unique);
    } else if (state == BUF_READY) {
        // Upload new texture only when ready
        pvr_txr_load_dma(frame_buffer[buf], pvr_txr, video_frame_size, -1, NULL, 0);
        last_unique_frame_drawn = unique;
        // DC_log("[Render] Draw frame %d (unique=%d buf=%d gen=%d)", cur_total, unique, buf, cur_gen);
    } else {
        DC_log("[Render] Waiting frame %d buf=%d state=%d", cur_total, buf, state);
    }

    // Submit quad as before
    pvr_dr_state_t dr;
    pvr_dr_init(&dr);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &hdr, sizeof(hdr) / 32);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), vert, sizeof(vert) / 32);
    pvr_dr_commit(&dr);
}


bool schedule_frame_preload(int frame) {
    if (frame >= num_total_frames) return false;
    int unique_frame = total_to_unique_frame(frame);
    int buf = unique_frame % NUM_BUFFERS;
    
    if (atomic_load(&buf_state[buf]) != BUF_EMPTY) return false;
    
    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = (head + 1) % RING_CAPACITY;
    if (next_head == tail) return false;
    
    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        int queued_unique = total_to_unique_frame(preload_ring[i].frame);
        if (queued_unique == unique_frame) return false;
    }
    
    preload_ring[head].frame = frame;
    preload_ring[head].generation = atomic_load(&GSeekGeneration);
    atomic_store(&preload_ring_head, next_head);
    return true;
}

bool schedule_frame_preload_with_generation(int frame, int generation) {
    if (frame >= num_total_frames) return false;
    int unique_frame = total_to_unique_frame(frame);
    int buf = unique_frame % NUM_BUFFERS;

    if (atomic_load(&buf_state[buf]) != BUF_EMPTY) return false;

    int head = atomic_load(&preload_ring_head);
    int tail = atomic_load(&preload_ring_tail);
    int next_head = (head + 1) % RING_CAPACITY;
    if (next_head == tail) return false;

    for (int i = tail; i != head; i = (i + 1) % RING_CAPACITY) {
        int queued_unique = total_to_unique_frame(preload_ring[i].frame);
        if (queued_unique == unique_frame) return false;
    }

    preload_ring[head].frame = frame;
    preload_ring[head].generation = generation;
    atomic_store(&preload_ring_head, next_head);
    return true;
}



kthread_t *worker_thread_id;

// Worker thread for preloading
// Worker thread for preloading and stream maintenance
void *worker_thread(void *p) {
    int idle_ticks = 0;

    while (1) {
        if (atomic_load(&preload_paused)) {
            thd_sleep(2);
            continue;
        }

        int cur_gen = atomic_load(&GSeekGeneration);

        // --- Always poll audio, even if idle ---
        if (!atomic_load(&audio_muted))
            snd_stream_poll(stream);

        int tail = atomic_load(&preload_ring_tail);
        int head = atomic_load(&preload_ring_head);

        // --- 1. Process one queued preload job if available ---
        if (tail != head) {
            PreloadJob job = preload_ring[tail];
            atomic_store(&preload_ring_tail, (tail + 1) % RING_CAPACITY);

            // Skip stale generations
            if (job.generation != cur_gen)
                continue;

            int total_frame  = job.frame;
            int unique_frame = total_to_unique_frame(total_frame);
            int buf          = unique_frame % NUM_BUFFERS;

            int expected = BUF_EMPTY;
            if (atomic_compare_exchange_strong(&buf_state[buf], &expected, BUF_LOADING)) {
                int res = load_frame(unique_frame, buf);
                if (res == 0) {
                    atomic_store(&buf_state[buf], BUF_READY);
                    // DC_log("[Worker] Loaded frame %d (unique=%d buf=%d gen=%d)", total_frame, unique_frame, buf, cur_gen);
                } else {
                    atomic_store(&buf_state[buf], BUF_EMPTY);
                    DC_log("[Worker] load_frame failed for %d (unique=%d buf=%d)",total_frame, unique_frame, buf);
                }
            }

            idle_ticks = 0;
        }

        // --- 2. Maintain rolling preload window ahead of current frame ---
        int current = atomic_load(&frame_index);   // use live playback frame
        int max_preloads = MIN(NUM_BUFFERS, 16);
        int scheduled = 0;

        for (int i = 1; i <= max_preloads; i++) {
            int target = current + i;
            if (target >= num_total_frames)
                break;

            int unique = total_to_unique_frame(target);
            int buf = unique % NUM_BUFFERS;

            // If buffer empty, queue a preload job for it
            if (atomic_load(&buf_state[buf]) == BUF_EMPTY) {
                if (schedule_frame_preload_with_generation(target, cur_gen)) {
                    scheduled++;
                    // Uncomment for detailed queue trace:
                    // Singe_log("[Worker] Queued frame %d ahead of current=%d", target, current);
                }
            }
        }

        // --- 3. Detect idle or starvation and attempt auto-recovery ---
        if (scheduled == 0 && tail == head) {
            if (++idle_ticks > 120 && !g_is_paused) {
                int cur = atomic_load(&frame_index);
                DC_log("[Worker] Idle/stalled (cur=%d gen=%d). Re-seeding preload window.", cur, cur_gen);
                idle_ticks = 0;

                // Try to re-seed a few frames ahead to recover from ring starvation
                for (int j = 0; j < NUM_BUFFERS; j++) {
                    atomic_store(&buf_state[j], BUF_EMPTY);
                }

                atomic_store(&preload_ring_head, 0);
                atomic_store(&preload_ring_tail, 0);

                for (int k = 0; k < MIN(NUM_BUFFERS, 8); k++) {
                    int target = cur + k;
                    if (target >= num_total_frames) break;
                    schedule_frame_preload_with_generation(target, cur_gen);
                }
            }
        } else {
            idle_ticks = 0;
        }

        thd_sleep(1);
    }
}



// --- seek_to_frame(): flush ring + re-prime fresh preload jobs ---
void seek_to_frame(int new_frame) {
    if (new_frame < 0) new_frame = 0;
    if (new_frame >= num_total_frames) new_frame = num_total_frames - 1;

    atomic_store(&audio_muted, 1);
    atomic_store(&preload_paused, 1);

    DC_log("[Seek] >>> Begin seek_to_frame(%d)", new_frame);

    // Reset buffers and ring
    for (int i = 0; i < NUM_BUFFERS; i++)
        atomic_store(&buf_state[i], BUF_EMPTY);
    atomic_store(&preload_ring_head, 0);
    atomic_store(&preload_ring_tail, 0);
    memset(preload_ring, 0, sizeof(preload_ring));

    last_unique_frame_drawn = -1;
    atomic_store(&seek_request, -1);

    // Flush/reopen files (important for GD-ROM)
    mutex_lock(&io_lock);
    fs_close(video_fd);
    thd_sleep(10);
    video_fd = fs_open(GGamePath, O_RDONLY);
    mutex_unlock(&io_lock);

    int uf = total_to_unique_frame(new_frame);
    uint32_t off = frame_offsets[uf];
    mutex_lock(&io_lock);
    fs_seek(video_fd, off, SEEK_SET);
    mutex_unlock(&io_lock);
    vfd_last_end = off;

    // Compute and seek audio
    double samples_exact = ((double)new_frame * (double)sample_rate) / (double)fps;
    uint32_t samples_i = (uint32_t)(samples_exact + 0.5);
    uint32_t bytes_per_channel = (samples_i / 2);
    bytes_per_channel = (bytes_per_channel + 15) & ~0xF;

    long left_offset = audio_offset + (long)bytes_per_channel;
    if (left_offset > (audio_offset + left_channel_size))
        left_offset = audio_offset + left_channel_size;

    long right_offset = audio_offset + left_channel_size + (long)bytes_per_channel;
    long right_limit  = audio_offset + (long)left_channel_size * 2;
    if (right_offset > right_limit) right_offset = right_limit;

    mutex_lock(&io_lock);
    fs_close(audio_fd_left);
    audio_fd_left = fs_open(GGamePath, O_RDONLY);
    fs_seek(audio_fd_left, left_offset, SEEK_SET);

    if (audio_channels == 2) {
        fs_close(audio_fd_right);
        audio_fd_right = fs_open(GGamePath, O_RDONLY);
        fs_seek(audio_fd_right, right_offset, SEEK_SET);
    }
    mutex_unlock(&io_lock);

    last_audio_left_pos  = left_offset;
    last_audio_right_pos = right_offset;

    // Reset timers
    atomic_store(&frame_index, new_frame);
    atomic_store(&displayed_total_frame, 0);
    frame_timer_anchor = psTimer();
    atomic_store(&audio_start_time_ms,
        (double)new_frame * (1000.0 / (double)fps));

    // Increment generation and clear stale jobs
    atomic_fetch_add(&GSeekGeneration, 1);
    int cur_gen = atomic_load(&GSeekGeneration);

    for (int i = 0; i < RING_CAPACITY; i++) {
        preload_ring[i].frame = -1;
        preload_ring[i].generation = cur_gen;
    }

    DC_log("[Seek] Incremented GSeekGeneration -> %d (flushed ring)", cur_gen);

    // Prime fresh preload frames
    int max_preloads = MIN(NUM_BUFFERS / 2, 16);
    for (int i = 0; i < max_preloads; i++) {
        int target = new_frame + i;
        if (target >= num_total_frames) break;
        schedule_frame_preload(target);
        DC_log("[Seek] Scheduled preload for frame %d (gen=%d)", target, cur_gen);
    }

    thd_sleep(50);
    atomic_store(&preload_paused, 0);
    atomic_store(&audio_muted, 0);

    DC_log("[Seek] <<< Completed seek_to_frame(%d)", new_frame);
}



static void fmv_tick(uint64_t now_ms) {
    static double accumulated_frame_debt = 0.0;
    static int frames_dropped = 0;
    static int stall_count = 0;
    static double max_frame_time = 0.0;
    static double avg_frame_time = 0.0;
    static double frame_time_samples = 0.0;
    static int unique_display_count = 0;
    static int expected_display_count = 0;

    // Handle seek requests (this is where frame seeking happens)
    int req = atomic_exchange(&seek_request, -1);
    if (req >= 0) {
        seek_to_frame(req);
        atomic_store(&frame_index, req);
        atomic_store(&audio_muted, 0);
        
        // CRITICAL: Reset timing anchor after seek
        frame_timer_anchor = psTimer();
        // Reset audio base time to current time
        atomic_store(&audio_start_time_ms, psTimer() * 1000.0);
    }

    // Frame sync and timing
    int current_frame = atomic_load(&frame_index);
    double now = psTimer();
    double elapsed_ms = (now - frame_timer_anchor) * 1000.0; // Convert to ms
    double audio_base_ms = atomic_load(&audio_start_time_ms);
    double current_audio_time_ms = audio_base_ms + elapsed_ms;
    
    // Calculate the expected video time based on the frame index
    double expected_video_time = current_frame * frame_duration;

    // Sync directly to the frame duration
    double target_time_ms = expected_video_time;

    // Handle pause logic first
    if (g_is_paused) {
        // Keep redrawing the last frame if paused
        int current_frame = atomic_load(&frame_index);
        int unique_id = total_to_unique_frame(current_frame);
        int buf = unique_id % NUM_BUFFERS;

        if (atomic_load(&buf_state[buf]) == BUF_READY) {
            // Redraw the current frame (don't clear buffer)
            last_unique_frame_drawn = unique_id;
            // Just draw it again every tick, no frame advance
            // render_current_video(); // your existing render-to-PVR call
        }

        // Prevent time drift by resetting the anchor
        frame_timer_anchor = psTimer();
        return;  // Skip all timing and frame advance logic while paused
    }

    // Skip frames if the current audio time is ahead of the target video time
    int frames_to_skip = 0;
    // while (current_audio_time_ms > target_time_ms + frame_duration && frames_to_skip < 10) {
    //     frames_to_skip++;
    //     current_frame++;
    //     target_time_ms = current_frame * frame_duration;
    // }
    
    // If we skipped frames, update the frame index
    if (frames_to_skip > 0) {
        atomic_store(&frame_index, current_frame);
    }

    // Check if we should render and display the frame
    if (current_audio_time_ms >= target_time_ms) {
        int draw_total = current_frame;
        int unique_id = total_to_unique_frame(draw_total);
        int buf = unique_id % NUM_BUFFERS;
        int state = atomic_load(&buf_state[buf]);

        if (state == BUF_READY) {
            // Display this frame only if not already displayed
            if (unique_id != last_unique_frame_drawn) {
                last_unique_frame_drawn = unique_id;
                unique_display_count = 1;
                expected_display_count = frame_durations[unique_id];
            } else {
                unique_display_count++;
            }

            // Only clear the buffer when the game is not paused
            if (unique_display_count >= expected_display_count) {
                atomic_store(&buf_state[buf], BUF_EMPTY);  // Clear buffer after rendering
            }

            atomic_fetch_add(&frame_index, 1);  // Increment frame index
            atomic_fetch_add(&displayed_total_frame, 1);  // Increment displayed frame counter
        }
    }

    // Time handling after frame rendering
    double t1 = psTimer();
    double render_ms = (t1 - now) * 1000.0;

    if (render_ms > max_frame_time) max_frame_time = render_ms;
    avg_frame_time = (avg_frame_time * frame_time_samples + render_ms) / (frame_time_samples + 1.0);
    frame_time_samples += 1.0;

    // Adjust accumulated frame debt
    double overrun = render_ms - frame_duration;
    if (overrun > 0.0) accumulated_frame_debt -= overrun;
    else accumulated_frame_debt += (-overrun * 0.1);
    accumulated_frame_debt *= 0.95;

    // No need to adjust based on audio if we're syncing directly to the frame rate
    double wait_ms = target_time_ms - current_audio_time_ms;
    if (accumulated_frame_debt < -10.0) {
        wait_ms = MAX(0.0, wait_ms + accumulated_frame_debt * 0.1);
    }
}

//=============================================================================
// SINGE LUA API FUNCTIONS
//=============================================================================

// Disc control functions
// Global variable to hold the current iFrameEnd value
static int g_iFrameEnd = -1;  // -1 is an invalid initial value

// Disc control functions
static int sep_get_current_frame(lua_State *L) {
    int cur = atomic_load(&frame_index);
    // Singe_log("sep_get_current_frame(): current frame = %d psTimer=%.2f", cur, psTimer());
    // // Use the locally stored g_iFrameEnd instead of fetching it from Lua every time
    if (cur >= g_iFrameEnd && g_iFrameEnd > 0) {
        atomic_store(&audio_muted, 1);  // Mute audio once we reach iFrameEnd
        Singe_log("Reached iFrameEnd, muting audio.");
        // g_is_paused = 1;
        // preload_paused = 1; // Pause preloading when reaching the end frame
        // Reset g_iFrameEnd to -1 to stop muting audio after reaching the end frame
        g_iFrameEnd = -1;
    }
    else
    {
        atomic_store(&audio_muted, 0);  // Unmute audio if not at iFrameEnd  
    }   
    lua_pushinteger(L, cur);  // Push the current frame as result for Lua
    return 1;
}

// Handle seeking to a new frame (skip to the next FMV segment)
static int sep_skip_to_frame(lua_State *L) {
    int frame = (int)luaL_checknumber(L, 1);  // Get the frame number passed to Lua function
    Singe_log("discSkipToFrame(%d)", frame);
    
    g_is_paused = 0;
            // Compute global ratios for the next phase
        compute_global_ratios();
    // Read the new iFrameStart value from Lua
    // lua_getglobal(L, "iFrameStart");  // Push 'iFrameStart' onto the Lua stack
    // if (lua_isnumber(L, -1)) {
    //     int iFrameStart = (int)lua_tonumber(L, -1);  // Retrieve the number value of iFrameStart from Lua
    //     Singe_log("iFrameStart from Lua: %d", iFrameStart);
    // } else {
    //     Singe_log("iFrameStart not found or not a number in Lua.");
    // }
    // lua_pop(L, 1);  // Pop 'iFrameStart' off the Lua stack    

    // // Read the new iFrameEnd value from Lua
    lua_getglobal(L, "iFrameEnd");  // Push 'iFrameEnd' onto the Lua stack
    if (lua_isnumber(L, -1)) {
        int newiFrameEnd = (int)lua_tonumber(L, -1);  // Retrieve the number value of iFrameEnd from Lua
        if (newiFrameEnd != g_iFrameEnd) {
            Singe_log("Updating g_iFrameEnd from %d to %d", g_iFrameEnd, newiFrameEnd);
            g_iFrameEnd = newiFrameEnd+1;  // Update the global variable
        } else {
            g_iFrameEnd = -1;
            lua_pushnumber(L, -1);   // Push -1 to the Lua stack
            lua_setglobal(L, "iFrameEnd"); // Set the global iFrameEnd variable in Lua to -1
        }
        Singe_log("iFrameEnd from Lua: %d", g_iFrameEnd);
    } else {
        Singe_log("iFrameEnd not found or not a number in Lua.");
         
    }
    lua_pop(L, 1);  // Pop 'iFrameEnd' off the Lua stack

    // // Mute audio before skipping to the next frame
    
    // atomic_store(&audio_muted, 1);
    // snd_stream_reinit(stream, NULL);
    // snd_stream_stop(stream);  // Stop the audio stream temporarily
    // snd_stream_shutdown();
    
    // thd_destroy(worker_thread_id); // Stop the worker thread

    // thd_sleep(500);  // Allow audio to stabilize
    // // snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    // // // snd_stream_set_callback  _direct(stream, audio_cb);
    // // snd_stream_reinit(stream, &audio_cb);
    // snd_stream_init_ex(audio_channels, soundbufferalloc);
    // stream = snd_stream_alloc(NULL, soundbufferalloc);
    // snd_stream_set_callback_direct(stream, audio_cb);
    // snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);    
    // worker_thread_id = thd_create(0, worker_thread, NULL);
    // Proceed with skipping the frame
    atomic_store(&audio_muted, 1);  
    atomic_store(&seek_request, frame);
    // seek_to_frame(frame);  // Skip to the specified frame

    // If we are at the iFrameEnd, reset it
    // if (frame >= g_iFrameEnd) {
    //     g_iFrameEnd = -1;  // Reset iFrameEnd after we skip to it
    // }

    // Restart the audio stream after skipping


    // Optionally, unmute audio after the transition
    // atomic_store(&audio_muted, 0);
    Singe_log("Skipped to frame %d", frame);
    
    return 0;
}






static int sep_search(lua_State *L) {
    int frame = (int)luaL_checknumber(L, 1);
    
    Singe_log("[Singe] sep_search/discSearch(%d): frame=%d\n", frame);
    g_is_paused = 1;
    preload_paused = 1;
    atomic_store(&audio_muted, 1);
    atomic_store(&seek_request, frame); 
//  seek_to_frame(frame);
    return 0;
}

static int sep_pause(lua_State *L) {
    // Only pause if not already halted
        // GHalted = 1;  // Pause playback
        // g_playback_started = 0;
        // After the title FMV finishes, start the next phase (e.g., intro FMV)
        Singe_log("🎬 discPause/sep_pause.");
        g_is_paused = 1;
        preload_paused = 1;
        atomic_store(&audio_muted, 1);
        // g_iFrameEnd = -1;
        // Compute global ratios for the next phase
        compute_global_ratios();

    return 0;  // Return successfully
}

static int sep_play(lua_State *L) {
    Singe_log("[Singe] sep_play/discPlay\n");
    g_is_paused = 0;
    preload_paused = 0;
    atomic_store(&audio_muted, 0);
    compute_global_ratios();
    return 0;
}

static int sep_stop(lua_State *L) {
    // GHalted = 1;
    // snd_stream_stop(stream);
    // snd_stream_queue_enable(stream);
    return 0;
}

static int sep_set_disc_fps(lua_State *L) {
    // Stub - FPS is fixed
    return 0;
}

static int sep_audio_control(lua_State *L) {
    int n = lua_gettop(L);
    if (n != 2 || !lua_isnumber(L, 1) || !lua_isboolean(L, 2)) {
        return luaL_error(L, "discAudio(channel:int, on:boolean) expected");
    }

    int channel = (int) lua_tointeger(L, 1);   // 1 = left, 2 = right
    int onOff   = lua_toboolean(L, 2) ? 1 : 0;

    switch (channel) {
        case 1: atomic_store(&g_audio_left_on,  onOff); break;
        case 2: atomic_store(&g_audio_right_on, onOff); break;
        default: return luaL_error(L, "discAudio: invalid channel %d", channel);
    }

    Singe_log("[Singe] discAudio ch=%d -> %s (L=%d R=%d)\n",
           channel, onOff ? "ON" : "OFF",
           atomic_load(&g_audio_left_on),
           atomic_load(&g_audio_right_on));
    return 0;
}



static int sep_change_speed(lua_State *L) {
    // Stub - discChangeSpeed is fixed
    return 0;
}

static int sep_get_number_of_mice(lua_State *L) {
    int32_t r = 2;
    lua_pushinteger(L, r);
    return 1;
}

static int vldpGetHeight(lua_State *L) {
    lua_pushinteger(L, video_height);
    
    return 1;
}

static int sep_mpeg_get_width(lua_State *L) {
    lua_pushinteger(L, video_width);
    return 1;
}

static int sep_step_backward(lua_State *L) {
    Singe_log("[Singe] sep_step_backward/discStepBackward\n");
    int current_frame = atomic_load(&frame_index);
    int target_frame = current_frame - 1;
    if (target_frame < 0) target_frame = 0;
    // atomic_fetch_add(&GSeekGeneration, 1);
    // GSeeking = 1;
    // GSeekTargetFrame = target_frame;
    atomic_store(&seek_request, target_frame);
    // seek_to_frame(target_frame);
    
    return 0;
}

// Font functions
// ============================================================================
// Character-level cache for fast typewriter rendering
// ============================================================================

typedef struct {
    unsigned char ch;
    int w, h;              // Actual glyph dimensions
    int tex_w, tex_h;      // Power-of-2 texture dimensions
    pvr_ptr_t tex;
    pvr_poly_hdr_t hdr;
    int bearing_x;         // Horizontal bearing (offset from pen)
    int bearing_y;         // Vertical bearing (offset from baseline)
    int advance;           // Horizontal advance for next character
} CharCache;

static CharCache g_char_cache[128];  // ASCII 0-127


#ifndef llround
#define llround(x) ((long long)((x) + ((x) >= 0 ? 0.5 : -0.5)))
#endif



// ARGB1555 packing with threshold
static inline uint16_t pack_argb1555(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    uint16_t alpha_bit = (a >= 128) ? 0x8000 : 0;
    return alpha_bit | 
           ((uint16_t)(r >> 3) << 10) |
           ((uint16_t)(g >> 3) << 5) |
           ((uint16_t)(b >> 3));
}

// ARGB1555 packing with dithering for smoother anti-aliasing
static inline uint16_t pack_argb1555_dither(uint8_t a, uint8_t r, uint8_t g, uint8_t b, int x, int y) {
    static const uint8_t dither[4][4] = {
        {  0, 128,  32, 160 },
        { 192,  64, 224,  96 },
        {  48, 176,  16, 144 },
        { 240, 112, 208,  80 }
    };
    
    uint8_t threshold = dither[y & 3][x & 3];
    uint16_t alpha_bit = (a > threshold) ? 0x8000 : 0;
    
    return alpha_bit | 
           ((uint16_t)(r >> 3) << 10) |
           ((uint16_t)(g >> 3) << 5) |
           ((uint16_t)(b >> 3));
}

static inline uint32_t pack_argb8888_overlay(void) {
return ((uint32_t)(GFontColorA & 0xFF) << 24) |
((uint32_t)(GFontColorR & 0xFF) << 16) |
((uint32_t)(GFontColorG & 0xFF) << 8) |
((uint32_t)(GFontColorB & 0xFF));
}

static inline uint16_t pack_argb1555_overlay(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    // Dreamcast PVR expects bit15 = alpha, 14–10 = red, 9–5 = green, 4–0 = blue
    return ((a > 127) ? 0x8000 : 0x0000) |
           ((r & 0xF8) << 7) |
           ((g & 0xF8) << 2) |
           ((b) >> 3);
}

// Initialize character cache - call after font is loaded
void font_init_char_cache(void) {
    if (g_char_cache_initialized || !GCurrentFont) return;
    
    printf("Initializing character cache for font...\n");
    
    // Get font metrics
    int ascender = GCurrentFont->size->metrics.ascender >> 6;
    
    // Pre-render all printable ASCII characters (32-126)
    for (int ch = 32; ch < 128; ch++) {
        if (FT_Load_Char(GCurrentFont, ch, FT_LOAD_RENDER) != 0) continue;
        
        FT_GlyphSlot slot = GCurrentFont->glyph;
        FT_Bitmap *bmp = &slot->bitmap;
        
        // Skip empty glyphs (like space)
        if (bmp->width == 0 || bmp->rows == 0) {
            g_char_cache[ch].ch = ch;
            g_char_cache[ch].advance = slot->advance.x >> 6;
            continue;
        }
        
        // Calculate texture dimensions (power of 2)
        int tex_w = next_pow2(bmp->width);
        int tex_h = next_pow2(bmp->rows);
        size_t img_bytes = tex_w * tex_h * 2;
        
        // Allocate and clear texture buffer
        uint16_t *img = memalign(32, img_bytes);
        if (!img) continue;
        memset(img, 0, img_bytes);
        
        // Render glyph with dithering for smooth anti-aliasing
        for (int y = 0; y < bmp->rows; y++) {
            for (int x = 0; x < bmp->width; x++) {
                uint8_t a = bmp->buffer[y * bmp->pitch + x];
                if (a > 0) {
                    img[y * tex_w + x] = 
                        pack_argb1555_dither(a, GFontColorR, GFontColorG, GFontColorB, x, y);
                }
            }
        }
        
        // Upload to VRAM
        pvr_ptr_t tex = pvr_mem_malloc(img_bytes);
        if (!tex) {
            free(img);
            continue;
        }
        pvr_txr_load(img, tex, img_bytes);
        free(img);
        
        // Create PVR context
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                         tex_w, tex_h, tex, PVR_FILTER_NONE);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
        
        // Store in cache
        g_char_cache[ch].ch = ch;
        g_char_cache[ch].w = bmp->width;
        g_char_cache[ch].h = bmp->rows;
        g_char_cache[ch].tex_w = tex_w;
        g_char_cache[ch].tex_h = tex_h;
        g_char_cache[ch].tex = tex;
        g_char_cache[ch].bearing_x = slot->bitmap_left;
        g_char_cache[ch].bearing_y = slot->bitmap_top;
        g_char_cache[ch].advance = slot->advance.x >> 6;
        pvr_poly_compile(&g_char_cache[ch].hdr, &cxt);
    }
    
    g_char_cache_initialized = 1;
    printf("Character cache initialized (96 chars)\n");
}

// ----------------------------------------------------------------------------
// Pixel plot
// ----------------------------------------------------------------------------
// static inline void overlay_draw_pixel(int x, int y, uint16_t color) {
//     if ((unsigned)x < GOverlayWidth && (unsigned)y < GOverlayHeight)
//         overlay_buf[y * overlay_tex_w + x] = color;
// }

// // ----------------------------------------------------------------------------
// // Line (Bresenham)
// // ----------------------------------------------------------------------------
// static void overlay_draw_line(int x1, int y1, int x2, int y2, uint16_t color) {
//     int dx = abs(x2 - x1);
//     int sx = x1 < x2 ? 1 : -1;
//     int dy = -abs(y2 - y1);
//     int sy = y1 < y2 ? 1 : -1;
//     int err = dx + dy, e2;

//     while (true) {
//         overlay_draw_pixel(x1, y1, color);
//         if (x1 == x2 && y1 == y2) break;
//         e2 = 2 * err;
//         if (e2 >= dy) { err += dy; x1 += sx; }
//         if (e2 <= dx) { err += dx; y1 += sy; }
//     }
// }

// // ----------------------------------------------------------------------------
// // Box (outline or filled)
// // ----------------------------------------------------------------------------
// static void overlay_draw_box(int x1, int y1, int x2, int y2, uint16_t color) {
//     if (x1 > x2) { int t=x1; x1=x2; x2=t; }
//     if (y1 > y2) { int t=y1; y1=y2; y2=t; }

//     // Outline only
//     overlay_draw_line(x1, y1, x2, y1, color);
//     overlay_draw_line(x2, y1, x2, y2, color);
//     overlay_draw_line(x2, y2, x1, y2, color);
//     overlay_draw_line(x1, y2, x1, y1, color);
// }

// // ----------------------------------------------------------------------------
// // Circle (filled or outline) — integer midpoint algorithm
// // ----------------------------------------------------------------------------
// static void overlay_draw_circle(int cx, int cy, int radius, uint16_t color, bool filled) {
//     if (radius <= 0) return;
//     int x = radius;
//     int y = 0;
//     int err = 0;

//     while (x >= y) {
//         if (filled) {
//             // Draw horizontal spans for filled circle
//             for (int i = cx - x; i <= cx + x; i++) {
//                 overlay_draw_pixel(i, cy + y, color);
//                 overlay_draw_pixel(i, cy - y, color);
//             }
//             for (int i = cx - y; i <= cx + y; i++) {
//                 overlay_draw_pixel(i, cy + x, color);
//                 overlay_draw_pixel(i, cy - x, color);
//             }
//         } else {
//             // Outline points
//             overlay_draw_pixel(cx + x, cy + y, color);
//             overlay_draw_pixel(cx + y, cy + x, color);
//             overlay_draw_pixel(cx - y, cy + x, color);
//             overlay_draw_pixel(cx - x, cy + y, color);
//             overlay_draw_pixel(cx - x, cy - y, color);
//             overlay_draw_pixel(cx - y, cy - x, color);
//             overlay_draw_pixel(cx + y, cy - x, color);
//             overlay_draw_pixel(cx + x, cy - y, color);
//         }

//         y++;
//         if (err <= 0) {
//             err += 2*y + 1;
//         } else {
//             x--;
//             err -= 2*x + 1;
//         }
//     }
// }

// // ----------------------------------------------------------------------------
// // Ellipse (stub, can be implemented later if needed)
// // ----------------------------------------------------------------------------
// static void overlay_draw_ellipse(int cx, int cy, int rx, int ry, uint16_t color, bool filled) {
// #if DEBUG_STUB_LOG
//     printf("[Overlay] draw_ellipse(%d,%d,%d,%d,filled=%d) STUB\n", cx, cy, rx, ry, filled);
// #endif
//     (void)cx; (void)cy; (void)rx; (void)ry; (void)color; (void)filled;
// }

int sep_font_sprite(lua_State *L);
void overlay_draw_sprite(int x, int y, const SingeSprite *spr);
SingeSprite *make_or_get_font_sprite(const char *text, uint8_t r, uint8_t g, uint8_t b); 
// ----------------------------------------------------------------------------
// Text rendering into buffer (wraps your font cache renderer)
// ----------------------------------------------------------------------------
static void overlay_draw_text(int x, int y, const char *msg)
{
    SingeSprite *sprite = make_or_get_font_sprite(msg, GFontColorR , GFontColorG, GFontColorB);
    if (!sprite || !sprite->texture) return;
    overlay_draw_sprite(x, y, sprite);
}

    void overlay_draw_sprite(int x, int y, const SingeSprite *spr)
    {
        if (!spr || !spr->texture) return;

        int w = spr->width;
        int h = spr->height;

        // Fonts are pre-scaled, so only apply offset-based scaling
        float scaled_x = (x - g_ratio_x_offset) * g_scale_x;
        float scaled_y = (y - g_ratio_y_offset) * g_scale_y;
        float scaled_w = w;
        float scaled_h = h;

        // --- Set up PVR textured polygon ---
        pvr_vertex_t verts[4];

        // Bind the sprite’s texture and header
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &spr->hdr, 1);

        // --- Top-left ---
        verts[0].flags = PVR_CMD_VERTEX;
        verts[0].x = scaled_x;
        verts[0].y = scaled_y;
        verts[0].z = 1.0f;
        verts[0].u = 0.0f;
        verts[0].v = 0.0f;
        verts[0].argb = 0xFFFFFFFF;
        verts[0].oargb = 0;

        // --- Top-right ---
        verts[1].flags = PVR_CMD_VERTEX;
        verts[1].x = scaled_x + scaled_w;
        verts[1].y = scaled_y;
        verts[1].z = 1.0f;
        verts[1].u = 1.0f;
        verts[1].v = 0.0f;
        verts[1].argb = 0xFFFFFFFF;
        verts[1].oargb = 0;

        // --- Bottom-left ---
        verts[2].flags = PVR_CMD_VERTEX;
        verts[2].x = scaled_x;
        verts[2].y = scaled_y + scaled_h;
        verts[2].z = 1.0f;
        verts[2].u = 0.0f;
        verts[2].v = 1.0f;
        verts[2].argb = 0xFFFFFFFF;
        verts[2].oargb = 0;

        // --- Bottom-right ---
        verts[3].flags = PVR_CMD_VERTEX_EOL;
        verts[3].x = scaled_x + scaled_w;
        verts[3].y = scaled_y + scaled_h;
        verts[3].z = 1.0f;
        verts[3].u = 1.0f;
        verts[3].v = 1.0f;
        verts[3].argb = 0xFFFFFFFF;
        verts[3].oargb = 0;

        // Submit vertices to the PVR
        sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), verts, 4);

    #ifdef DEBUG_OVERLAY_SPRITE
        printf("[PVR] Draw sprite '%s' at (%d,%d) scaled=(%.1f,%.1f) size=(%dx%d)\n",
            spr->name ? spr->name : "(unnamed)",
            x, y, scaled_x, scaled_y, w, h);
    #endif
    }




// ----------------------------------------------------------------------------
// Present the overlay (upload + draw one translucent quad)
// ----------------------------------------------------------------------------
// static void overlay_present(void) {

//     int src_stride = overlay_tex_w * 2;
//     int tex_bytes  = src_stride * overlay_tex_h;

//     // Ensure VRAM texture exists
//     if (!overlay_tex) {
//         overlay_tex = pvr_mem_malloc(tex_bytes);
//         if (!overlay_tex) return;
//     }

//     // --- Fast DMA texture upload ---
//     // Kick off DMA transfer (non-blocking)
//     pvr_txr_load_dma(overlay_buf, overlay_tex, tex_bytes, -1, NULL, 0);

//     // // Wait for DMA completion before drawing (simple sync)
//     // while (!pvr_dma_ready())
//     //     thd_pass();

//     // --- Draw overlay quad ---
//     float u_max = (float)GOverlayWidth  / (float)overlay_tex_w;
//     float v_max = (float)GOverlayHeight / (float)overlay_tex_h;
//     float sx = (float)g_display_w;
//     float sy = (float)g_display_h;

//     pvr_poly_cxt_t cxt;
//     pvr_poly_hdr_t hdr;
//     pvr_vertex_t v;

//     pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
//                      PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
//                      overlay_tex_w, overlay_tex_h,
//                      overlay_tex, PVR_FILTER_NONE);
//     cxt.gen.alpha   = PVR_ALPHA_ENABLE;
//     cxt.gen.culling = PVR_CULLING_NONE;
//     pvr_poly_compile(&hdr, &cxt);

//     // (Caller already began PVR_LIST_TR_POLY)
//     pvr_prim(&hdr, sizeof(hdr));

//     v = (pvr_vertex_t){PVR_CMD_VERTEX,      0,  0, 1, 0.0f, 0.0f, 0xffffffff};
//     pvr_prim(&v, sizeof(v));
//     v = (pvr_vertex_t){PVR_CMD_VERTEX,   sx,  0, 1, u_max, 0.0f, 0xffffffff};
//     pvr_prim(&v, sizeof(v));
//     v = (pvr_vertex_t){PVR_CMD_VERTEX,      0, sy, 1, 0.0f, v_max, 0xffffffff};
//     pvr_prim(&v, sizeof(v));
//     v = (pvr_vertex_t){PVR_CMD_VERTEX_EOL, sx, sy, 1, u_max, v_max, 0xffffffff};
//     pvr_prim(&v, sizeof(v));
// }


// Free character cache
static void font_free_char_cache(void) {
    if (!g_char_cache_initialized) return;
    
    for (int i = 0; i < 128; i++) {
        if (g_char_cache[i].tex) {
            pvr_mem_free(g_char_cache[i].tex);
            g_char_cache[i].tex = NULL;
        }
    }
    
    memset(g_char_cache, 0, sizeof(g_char_cache));
    g_char_cache_initialized = 0;
}

// ============================================================================
// Lua Font Functions
// ============================================================================

static int sep_font_load(lua_State *L) {
    const char *path = lua_tostring(L, 1);
    int size = (int)lua_tonumber(L, 2);

    char *fullpath = resolve_path(path);

    // Initialize FreeType if not already
    if (!GFTLibrary) {
        if (FT_Init_FreeType(&GFTLibrary) != 0) {
            Singe_log("Failed to initialize FreeType");
            free(fullpath);
            lua_pushinteger(L, -1);
            return 1;
        }
    }

    FT_Face face;
    if (FT_New_Face(GFTLibrary, fullpath, 0, &face) != 0) {
        Singe_log("Failed to load font: %s", fullpath);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }

    // Set the font size
    if (FT_Set_Pixel_Sizes(face, 0, size + 4) != 0) {
        Singe_log("Failed to set font size: %d", size + 4);
        FT_Done_Face(face);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }

    // Add the font to the font manager
    g_font_manager.fonts = realloc(g_font_manager.fonts, sizeof(FT_Face) * (g_font_manager.font_count + 1));
    g_font_manager.fonts[g_font_manager.font_count] = face;
    g_font_manager.font_count++;

    free(fullpath);
    lua_pushinteger(L, 1);
    
    // if (!g_char_cache_initialized)
        font_init_char_cache();
    
    return 1;
}

static int sep_say_font(lua_State *L) {
    if (lua_gettop(L) < 3) return 0;

    int overlay_x = (int)lua_tonumber(L, 1);
    int overlay_y = (int)lua_tonumber(L, 2);
    const char *msg = lua_tostring(L, 3);
    if (!msg || !*msg || !GCurrentFont) return 0;
    
    // Singe_log("[Singe] sep_say_font: '%s' at (%d,%d)\n", msg, overlay_x, overlay_y);

    // Draw directly into the CPU overlay buffer
    overlay_draw_text(overlay_x, overlay_y, msg);

    lua_pushboolean(L, 1);
    return 1;
}


// Calculate text width (useful for centering)
static int sep_fontWidth(lua_State *L) {
    const char *text = lua_tostring(L, 1);
    if (!text || !GCurrentFont) {
        lua_pushinteger(L, 0);
        return 1;
    }
    
   
    int width = 0;
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (*p < 128) {
            width += g_char_cache[*p].advance;
        }
    }
    
    lua_pushinteger(L, width);
    return 1;
}

// Get font height
static int sep_fontHeight(lua_State *L) {
    if (!GCurrentFont) {
        lua_pushinteger(L, 0);
        return 1;
    }
    
    int height = (GCurrentFont->size->metrics.height >> 6);
    lua_pushinteger(L, height);
    return 1;
}
// #define DEBUG_FONTSPRITE 1
// ============================================================================
// fontToSprite - For static text (menus, titles, etc.)
// ============================================================================
SingeSprite *make_or_get_font_sprite(const char *text, uint8_t r, uint8_t g, uint8_t b) {
    if (!GCurrentFont || !text || !*text)
        return NULL;

    // --- Generate hash key (text + color) ---
    unsigned long hash_value = hash(text);
    uint8_t r5 = r & 0xF8;
    uint8_t g5 = g & 0xF8;
    uint8_t b5 = b & 0xF8;
    hash_value ^= (r5 << 16) | (g5 << 8) | b5;
    // DC_log("[FONT] Hash calc: text='%s', R=%d G=%d B=%d -> hash=%lu\n", 
    //     text, GFontColorR, GFontColorG, GFontColorB, hash_value);
    // --- Return cached font sprite if found ---
    SingeSprite *cached = get_cached_font_sprite(hash_value);
    if (cached) {
        // Singe_log("[FONT] Found cached font sprite '%s' (hash %lu)\n", text, hash_value);
        return cached;
    }

    // --- Scaling factors ---
    float scale_x = g_scale_x;
    float scale_y = g_scale_y;

    // --- Measure text ---
    int total_width = 0, max_height = 0, max_ascent = 0;
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (FT_Load_Char(GCurrentFont, *p, FT_LOAD_DEFAULT) != 0) continue;
        FT_Render_Glyph(GCurrentFont->glyph, FT_RENDER_MODE_MONO);
        FT_GlyphSlot g = GCurrentFont->glyph;
        total_width += (g->advance.x >> 6);
        if ((int)g->bitmap.rows > max_height)
            max_height = (int)g->bitmap.rows;
        if (g->bitmap_top > max_ascent)
            max_ascent = g->bitmap_top;
    }

    if (total_width <= 0 || max_height <= 0)
        return NULL;

    // --- Compute texture dimensions ---
    int scaled_width  = (int)(total_width * scale_x);
    int scaled_height = (int)(max_height * scale_y);
    int scaled_ascent = (int)(max_ascent * scale_y);
    int tex_w = next_pow2(scaled_width);
    int tex_h = next_pow2(scaled_height);
    if (tex_w > 1024) tex_w = 1024;
    if (tex_h > 1024) tex_h = 1024;

    size_t img_bytes = tex_w * tex_h * 2;
    uint16_t *img = memalign(32, img_bytes);
    if (!img) return NULL;
    memset(img, 0, img_bytes);

    // --- Render glyphs into ARGB1555 texture ---
    int pen_x = 0;
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (FT_Load_Char(GCurrentFont, *p, FT_LOAD_DEFAULT) != 0) continue;
        FT_Render_Glyph(GCurrentFont->glyph, FT_RENDER_MODE_MONO);

        FT_GlyphSlot slot = GCurrentFont->glyph;
        FT_Bitmap *bmp = &slot->bitmap;

        int glyph_x = (int)((pen_x + slot->bitmap_left) * scale_x);
        int glyph_y = (int)((scaled_ascent - slot->bitmap_top) * scale_y);
        int glyph_w = (int)(bmp->width * scale_x);
        int glyph_h = (int)(bmp->rows * scale_y);

        for (int sy = 0; sy < glyph_h; sy++) {
            int src_y = (int)(sy / scale_y);
            if (src_y >= bmp->rows) continue;
            for (int sx = 0; sx < glyph_w; sx++) {
                int src_x = (int)(sx / scale_x);
                if (src_x >= bmp->width) continue;

                uint8_t bitmask = 0;
                if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                    int byte = bmp->buffer[src_y * bmp->pitch + (src_x >> 3)];
                    bitmask = (byte & (0x80 >> (src_x & 7))) ? 1 : 0;
                } else if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
                    bitmask = bmp->buffer[src_y * bmp->pitch + src_x] > 0 ? 1 : 0;
                }

                if (bitmask) {
                    int tx = glyph_x + sx;
                    int ty = glyph_y + sy;
                    if (tx >= 0 && tx < tex_w && ty >= 0 && ty < tex_h) {
                        img[ty * tex_w + tx] =
                            (1 << 15) |                          // alpha = 1
                            ((r & 0xF8) << 7) |
                            ((g & 0xF8) << 2) |
                            (b >> 3);
                    }
                }
            }
        }

        pen_x += (slot->advance.x >> 6);
    }

    // --- Upload to VRAM ---
    pvr_ptr_t tex = pvr_mem_malloc(img_bytes);
    if (!tex) { free(img); return NULL; }
    pvr_txr_load(img, tex, img_bytes);
    free(img);

    // --- After creating the new sprite ---
    SingeSprite *sprite = Singe_xmalloc(sizeof(SingeSprite));
    sprite->hash_id = hash_value;
    sprite->name = NULL;  // Distinguish font sprites from regular ones
    sprite->width = scaled_width;
    sprite->height = scaled_height;
    sprite->texture = tex;

    // --- Add to global font sprite cache ---
    sprite->next = GSprites;
    GSprites = sprite;

    // Compile PVR header as before...
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     tex_w, tex_h, tex, PVR_FILTER_NONE);
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&sprite->hdr, &cxt);

    // DC_log("[FONT] Cached new font sprite '%s' (%dx%d) hash %lu\n",
    //        text, scaled_width, scaled_height, hash_value);
    return sprite;
}



int sep_font_sprite(lua_State *L)
{
    const char *text = lua_tostring(L, 1);
    SingeSprite *sprite = make_or_get_font_sprite(text, GFontColorR, GFontColorG, GFontColorB);
    if (!sprite) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)sprite);
    return 1;
}

static int sep_font_select(lua_State *L) {
    int index = (int)lua_tonumber(L, 1);

    // Check if index is valid
    if (index >= 0 && index < g_font_manager.font_count) {
        g_font_manager.current_font_idx = index;
        GCurrentFont = g_font_manager.fonts[index];
        lua_pushboolean(L, 1);  // Success
    } else {
        lua_pushboolean(L, 0);  // Invalid font index
    }

    return 1;
}


static int sep_font_quality(lua_State *L) {
    GFontQuality = (int)lua_tonumber(L, 1);
    return 0;
}

static int sep_font_unload(lua_State *L) {
    // font_free_char_cache();
    if (GCurrentFont) {
        FT_Done_Face(GCurrentFont);
        GCurrentFont = NULL;
    }
    return 0;
}

// by RDG2010
static int sep_get_pause_flag(lua_State *L)
{
   /*
	* This function returns g_pause_state's value to the lua script.
	*
	* Sometimes game logic pauses the game (which implies pausing video playback).
	* When implementing a pause state it is possible for the player
	* to resume playblack at moments where the game is not intended to.
	* Boolean g_pause state is an internal variable that keeps track
	* of this. It's set to true whenever sep_pre_pause is called.
	* It's set to false whenever sep_pre_play or sep_skip_to_frame is called.
	* 
	* A lua programmer can use this to prevent resuming playback accidentally.
	*/
	lua_pushboolean(L, g_is_paused);
	return 1;

}

static int sep_set_pause_flag(lua_State *L)
{
	int n = lua_gettop(L);
	bool b1 = false;
		
	if (n == 1)
	{		
		if (lua_isboolean(L, 1))
		{	
			b1 = lua_toboolean(L, 1);
			g_is_paused = b1;
			
		}
	}	
	return 0;
}


static int sep_singe_quit(lua_State *L) {
    return 0;
}

static int sep_singe_version(lua_State *L) {
    // luaTrace(L, "singeVersion", "%f", SINGE_VERSION);
    lua_pushnumber(L, SINGE_VERSION);
    return 1;
}

static int sep_singe_wants_crosshair(lua_State *L) {
    // luaTrace(L, "singeWantsCrosshairs", "%f", !_global.conf->noCrosshair);
    lua_pushboolean(L, 1); 
    return 1;
}

static int sep_set_gamename(lua_State *L) {
    // const char *name = lua_tostring(L, 1);
    // if (name) {
    //     strncpy(GGameName, name, sizeof(GGameName)-1);
    //     GGameName[sizeof(GGameName)-1] = '\0';
    // }
    return 0;
}

static int  sep_get_scriptpath(lua_State *L) {
    const char *script = (G_SCRIPT_FILE && G_SCRIPT_FILE[0]) ? G_SCRIPT_FILE : "";
    printf("[Singe] sep_get_scriptpath -> %s\n", script);
    lua_pushstring(L, script);
    return 1;
}

// Font sprite lookup by precomputed hash value
static SingeSprite *get_cached_font_sprite(unsigned long hash_value) {
    SingeSprite *sprite = NULL;

    for (sprite = GSprites; sprite != NULL; sprite = sprite->next) {
        if (sprite->hash_id == hash_value) {
            // DC_log("Font sprite found in cache with hash_id: %lu\n", hash_value);
            return sprite;
        }
    }

    // DC_log("Font sprite not found in cache with hash_id: %lu\n", hash_value);
    return NULL;
}


// Sprite functions
static SingeSprite *get_cached_sprite(const char *name_or_hash) {
    SingeSprite *sprite = NULL;
    unsigned long hash_value = 0;
    // Singe_log("name_or_hash: %s\n", name_or_hash);
    // If name_or_hash is a numeric string (hash_id)
    if (isdigit(name_or_hash[0])) {  // Check if it's a numeric string (hash_id)
        hash_value = strtoul(name_or_hash, NULL, 10);  // Convert to hash_value (hash_id)
        // DC_log("Looking up sprite by hash_id: %lu\n", hash_value);

        // Search cache based on hash_id
        for (sprite = GSprites; sprite != NULL; sprite = sprite->next) {
            if (sprite->hash_id == hash_value) {  // Compare by hash_id
                // DC_log("Sprite found in cache with hash_id: %lu\n", hash_value);
                return sprite;  // Return the cached sprite
            }
        }
        // DC_log("Sprite not found in cache with hash_id: %lu\n", hash_value);
    } else {
        // If it's not a hash_id, treat it as a file path and resolve it
        char *fullpath = resolve_path(name_or_hash);
        // DC_log("Loading sprite: %s -> %s\n", name_or_hash, fullpath);

        // Hash the sprite's content (e.g., name or text)
        hash_value = hash(name_or_hash);  // Generate hash from name or path
        // DC_log("Hashed sprite name '%s' to hash_id: %lu\n", name_or_hash, hash_value);

        // Search cache based on hash_id
        for (sprite = GSprites; sprite != NULL; sprite = sprite->next) {
            if (sprite->hash_id == hash_value) {
                // DC_log("Sprite found in cache with hash_id: %lu\n", hash_value);
                free(fullpath);
                return sprite;  // Return the cached sprite
            }
        }

        // If not found, load the texture as usual
        // DC_log("Sprite not found in cache, loading new sprite: %s\n", name_or_hash);
        int w, h;
        pvr_ptr_t tex = NULL;

        if (png_load_texture(fullpath, &tex, PNG_FULL_ALPHA, (uint32_t*)&w, (uint32_t*)&h) < 0) {
            DC_log("Failed to load sprite texture: %s\n", fullpath);
            free(fullpath);
            return NULL;
        }

        // DC_log("Loaded sprite texture with dimensions: %dx%d\n", w, h);

        // Create and initialize the new sprite
        SingeSprite *new_sprite = Singe_xmalloc(sizeof(SingeSprite));
        new_sprite->name = Singe_xstrdup(name_or_hash);  // Store original name for debugging
        new_sprite->width = w;
        new_sprite->height = h;
        new_sprite->texture = tex;  // Assign texture to the sprite
        new_sprite->next = GSprites;  // Link to the cache
                // Assign a unique hash_id
        new_sprite->hash_id = hash_value;  // Set the hash_id based on the content
        GSprites = new_sprite;  // Add to the head of the sprite list


        // DC_log("Created new sprite with hash_id: %lu\n", new_sprite->hash_id);

        // Compile PVR header
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB4444,
                         w, h, tex, is_320 ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&new_sprite->hdr, &cxt);

        free(fullpath);
        // DC_log("Sprite created with hash: %lu\n",new_sprite->hash_id);
        return new_sprite;
    }
}

static int sep_sprite_load(lua_State *L) {
    const char *path = lua_tostring(L, 1);  // Get the sprite path or hash

    // Get the sprite from the cache or create it if not found
    SingeSprite *sprite = get_cached_sprite(path);  // `path` could be a sprite name or a stringified hash_id

    // Since get_cached_sprite either finds the sprite or creates it, there's no need for "not found" check
    // DC_log("Sprite '%s' loaded with hash_id: %lu width=%d height=%d\n",
    //           path, sprite->hash_id, sprite->width, sprite->height);

    // Return the sprite pointer to Lua
    lua_pushinteger(L, (lua_Integer)sprite);

    return 1;  // Return the result to Lua
}

// #define DEBUG_SPRITEDRAW 1
static int sep_sprite_unload(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 1) return 0;

    SingeSprite *sprite = (SingeSprite *)lua_tointeger(L, 1);
    if (!sprite) return 0;

    // **CRITICAL: Don't unload cached font sprites!**
    // These have name == NULL and should persist
    if (sprite->name == NULL) {
        #ifdef DEBUG_SPRITEDRAW
        printf("[SINGE] Skipping unload of cached font sprite (hash %lu)\n", sprite->hash_id);
        #endif
        return 0;
    }

    // Free VRAM texture if allocated
    if (sprite->texture) {
        pvr_mem_free(sprite->texture);
        sprite->texture = NULL;
    }

    // Free name string
    if (sprite->name) {
        free(sprite->name);
        sprite->name = NULL;
    }

    // Unlink from global sprite list (GSprites)
    SingeSprite **prev = &GSprites;
    while (*prev) {
        if (*prev == sprite) {
            *prev = sprite->next;
            break;
        }
        prev = &((*prev)->next);
    }

    // Free the sprite structure itself
    free(sprite);

#ifdef DEBUG_SPRITEDRAW
    printf("[SINGE] Unloaded sprite at %p\n", sprite);
#endif

    return 0;
}
static int sep_vldp_getvolume(lua_State *L) {
    int volume = g_audio_movie_vol;
    lua_pushinteger(L, volume);
    return 1;
}

static int sep_vldp_setvolume(lua_State *L) {
    int volume = (int)lua_tointeger(L, 1);
    g_audio_movie_vol = volume;
    // printf("[Singe] VideoSetVolume(%d)\n", volume);
    return 0;
}

// Debug function
static int sep_debug_say(lua_State *L) {
    const char *str = lua_tostring(L, 1);
    Singe_log("%s\n", str);
    // printf("[Singe] %s\n", str);
    return 0;
}

// Lua panic handler
static int sep_panic(lua_State *L) {
    const char *errstr = lua_tostring(L, -1);
    if (errstr) {
        printf("LUA PANIC: %s\n", errstr);
    }
    exit(1);
    return 0;
}

// Custom dofile to handle Singe paths
typedef struct {
    file_t fd;
} FileIoUserdata;

static long file_read(void *userdata, void *buf, long len) {
    FileIoUserdata *ud = (FileIoUserdata *)userdata;
    mutex_lock(&io_lock);
    long size= (long)fs_read(ud->fd, buf, len);
    mutex_unlock(&io_lock);

    return size;
}

static const char *lua_reader(lua_State *L, void *data, size_t *size) {
    static uint8_t __attribute__((aligned(32))) buffer[1024];
    FileIoUserdata *ud = (FileIoUserdata *)data;
    mutex_lock(&io_lock);
    long br = fs_read(ud->fd, buffer, sizeof(buffer));
    mutex_unlock(&io_lock);
    if (br <= 0) {
        *size = 0;
        return NULL;
    }
    *size = (size_t)br;
    return buffer;
}

static int sep_doluafile(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    
    char *fullpath = resolve_path(filename);
    // DC_log("dofile: opening %s -> %s\n", filename, fullpath);
    mutex_lock(&io_lock);
    file_t fd = fs_open(fullpath, O_RDONLY);
    mutex_unlock(&io_lock);
    free(fullpath);
    
    if (fd < 0) {
        return luaL_error(L, "cannot open %s", filename);
    }
    FileIoUserdata ud;
    ud.fd = fd;
    
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", filename);
        
    int rc = lua_load(L, lua_reader, &ud, chunkname, NULL);
    mutex_lock(&io_lock);
    fs_close(fd);
    mutex_unlock(&io_lock);
    if (rc != 0) {
        return lua_error(L);
    }
    
    lua_call(L, 0, LUA_MULTRET);
    return lua_gettop(L);
}


// #define DEBUG_STUB_LOG 1
// ===========================================================================
// Hypseus Singe Stubs - Bezel / Scoreboard / UI
// ===========================================================================
// --- Bezel management ---
static int sep_bezel_load(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_load (stub)\n");
#endif
    return 0;
}

static int sep_bezel_unload(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_unload (stub)\n");
#endif
    return 0;
}

static int sep_bezel_draw(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_draw (stub)\n");
#endif
    return 0;
}

static int sep_bezel_set_alpha(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_set_alpha (stub)\n");
#endif
    return 0;
}

static int sep_bezel_get_alpha(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_get_alpha (stub)\n");
#endif
    return 0;
}

static int sep_bezel_set_visible(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_set_visible (stub)\n");
#endif
    return 0;
}

static int sep_bezel_is_visible(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_is_visible (stub)\n");
#endif
    return 0;
}

static int sep_bezel_set_overlay(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_set_overlay (stub)\n");
#endif
    return 0;
}

static int sep_bezel_enable(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_enable (stub)\n");
#endif
    return 0;
}

static int sep_bezel_clear(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_clear (stub)\n");
#endif
    return 0;
}

static int sep_bezel_is_enabled(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_is_enabled (stub)\n");
#endif
    return 0;
}

static int sep_bezel_second_score(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_second_score (stub)\n");
#endif
    return 0;
}

static int sep_bezel_player_score(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_player_score (stub)\n");
#endif
    return 0;
}

static int sep_bezel_player_lives(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_player_lives (stub)\n");
#endif
    return 0;
}

static int sep_bezel_credits(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_credits (stub)\n");
#endif
    return 0;
}

// --- Scoreboard and misc UI ---
static int sep_bezel_twin_score_on(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_bezel_twin_score_on (stub)\n");
#endif
    return 0;
}



void *Singe_realloc(void *ptr, size_t len) { return realloc(ptr, len); }
void Singe_free(void *ptr) { free(ptr); }
static void out_of_memory(void)
{
    printf("Out of memory!");
}

void *Singe_xrealloc(void *ptr, size_t len)
{
    void *retval = Singe_realloc(ptr, len);
    if (!retval && (len > 0)) {
        out_of_memory();
    }
    return retval;
}


// Allocator interface for internal Lua use.
static void *Singe_lua_allocator(void *ud, void *ptr, size_t osize, size_t nsize)
{
    if (nsize == 0) {
        Singe_free(ptr);
        return NULL;
    }
    return Singe_xrealloc(ptr, nsize);
}

// ===========================================================================
// Hypseus Singe Stubs - Ratio / Video / MPEG
// ===========================================================================


// --- Ratio Functions ---
static int sep_ratioGetX(lua_State *L)
{
    // Dreamcast: overlay 360x240 vs video 640x480 → 1.125
    double overlay_aspect = (double)GOverlayWidth / (double)GOverlayHeight;  // 1.5
    double video_aspect   = (double)g_display_w / (double)g_display_h;       // 1.333
    double ratio = overlay_aspect / video_aspect;                            // 1.125
    lua_pushnumber(L, ratio);
    Singe_log("[SINGE] ratioGetX() returning %.3f\n", ratio);

    return 1;
}

static int sep_ratioGetY(lua_State *L)
{
    lua_pushnumber(L, 1.0);
    return 1;
}


// --- MPEG / VLDP Functions ---
static int sep_mpeg_set_flash(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_set_flash (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_get_rotate(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_get_rotate (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_set_rotate(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_set_rotate (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_get_scale(lua_State *L) {
    // Return 100.0 to match PC Hypseus expected scale factor.
    lua_pushnumber(L, 100.0);
    return 1;
}
static int sep_mpeg_set_scale(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_set_scale (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_focus_area(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_focus_area (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_get_rawpixel(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_get_rawpixel (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_reset_focus(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_reset_focus (stub)\n");
#endif
    return 0;
}

static int sep_mpeg_set_grayscale(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_mpeg_set_grayscale (stub)\n");
#endif
    return 0;
}

// --- VLDP and Video helpers ---
static int sep_vldp_get_width(lua_State *L) {
    // atomic_store(&audio_muted, 0);
    // thd_destroy(worker_thread_id);
    // worker_thread_id = thd_create(0, worker_thread, NULL);
    // snd_stream_stop(stream);
    // snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    int width = g_display_w > 0 ? g_display_w : 640;
    Singe_log("[Singe] vldpGetWidth() -> %d\n", width);
    lua_pushinteger(L, width);
    return 1;
}

static int sep_vldp_get_height(lua_State *L) {
    int height = g_display_h > 0 ? g_display_h : 480;
    Singe_log("[Singe] vldpGetHeight() -> %d\n", height);
    lua_pushinteger(L, height);

    return 1;
}


static int sep_vldp_get_pixel(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_vldp_get_pixel (stub)\n");
#endif
    return 0;
}

static int sep_vldp_verbose(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_vldp_verbose (stub)\n");
#endif
    return 0;
}

static int sep_audio_suffix(lua_State *L) {
    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        const char *suffix = lua_tostring(L, 1);
        printf("[Singe] discAudioSuffix('%s') [stub]\n", suffix);
    } else {
        printf("[Singe] discAudioSuffix() [stub]\n");
    }

    lua_pushboolean(L, 0);
    return 1;
}

    // ===========================================================================
    // Hypseus Singe Stubs - Overlay / Color / Drawing
    // ===========================================================================
    // --- Color management ---
static int sep_color_set_backcolor(lua_State *L) {
    GBGColorR = (uint8_t)lua_tonumber(L, 1);
    GBGColorG = (uint8_t)lua_tonumber(L, 2);
    GBGColorB = (uint8_t)lua_tonumber(L, 3);
    if (lua_gettop(L) >= 4)
        GBGColorA = (uint8_t)lua_tonumber(L, 4);
    else
        GBGColorA = 0;
    return 0;
}

static int sep_color_set_forecolor(lua_State *L) {
    // atomic_store(&audio_muted, 0);
    // thd_destroy(worker_thread_id);
    // worker_thread_id = thd_create(0, worker_thread, NULL);
    // snd_stream_stop(stream);
    // snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    GFontColorR = (uint8_t)lua_tonumber(L, 1);
    GFontColorG = (uint8_t)lua_tonumber(L, 2);
    GFontColorB = (uint8_t)lua_tonumber(L, 3);
    if (lua_gettop(L) >= 4)
        GFontColorA = (uint8_t)lua_tonumber(L, 4);
    else
        GFontColorA = 255;
    return 0;
}


// --- Overlay control ---
static int sep_overlay_clear(lua_State *L) {
    // Singe_log("[Singe] sep_overlay_clear() begin\n");
    // Default clear color — transparent black
    uint16_t color = pack_argb1555_overlay(0, 0, 0, 0); // Transparent black color

    // Clear the overlay buffer (CPU-side)
    memset(overlay_buf, color, overlay_tex_w * overlay_tex_h * 2);  // Set all bytes to color

    return 1;
}






static int sep_get_overlay_height(lua_State *L) {
    lua_pushinteger(L, GOverlayHeight);
    return 1;
}

static int sep_get_overlay_width(lua_State *L) {
    lua_pushinteger(L, GOverlayWidth);
    return 1;
}

static int sep_overlay_set_grayscale(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_overlay_set_grayscale (stub)\n");
#endif
    return 0;
}
static int sep_set_overlaysize(lua_State *L) {
    int w = 360;
    int h = 240;

    int n = lua_gettop(L);  // Number of arguments passed to Lua function

    if (n == 1) {
        if (lua_istable(L, 1)) {
            // Table form (e.g. {640,480} or {1,640,480})
            int len = lua_rawlen(L, 1);  // Get length of the table
            if (len >= 2) {  // Ensure table has at least width and height
                lua_rawgeti(L, 1, 1);  // Get the first element (width)
                lua_rawgeti(L, 1, 2);  // Get the second element (height)
                if (lua_isnumber(L, -2)) w = (int)lua_tointeger(L, -2);  // Set width
                if (lua_isnumber(L, -1)) h = (int)lua_tointeger(L, -1);  // Set height
                lua_pop(L, 2);  // Pop the width and height values
            }
        }
    } else if (n == 3) {
        // Special case: If the first argument is 4, treat it as custom size (width, height)
        if (lua_isnumber(L, 1) && lua_tointeger(L, 1) == 4) {
            if (lua_isnumber(L, 2)) w = (int)lua_tointeger(L, 2);
            if (lua_isnumber(L, 3)) h = (int)lua_tointeger(L, 3);
        }
    } else if (n >= 2) {
        // Direct numeric args (width, height)
        if (lua_isnumber(L, 1)) w = (int)lua_tointeger(L, 1);
        if (lua_isnumber(L, 2)) h = (int)lua_tointeger(L, 2);
    }

    // Set the global overlay size
    GOverlayWidth  = w;
    GOverlayHeight = h;

    printf("[Singe] setOverlaySize(%d, %d)\n", w, h);
    return 0;
}


static int sep_set_custom_overlay(lua_State *L) {
    if (lua_gettop(L) < 2 || !lua_isnumber(L, 1) || !lua_isnumber(L, 2)) {
        return luaL_error(L, "setOverlayResolution(width, height) expected");
    }
    
    int w = (int)lua_tointeger(L, 1);
    int h = (int)lua_tointeger(L, 2);

    GOverlayWidth  = w;
    GOverlayHeight = h;

    Singe_log("[Singe] setOverlayResolution(%d, %d)\n", w, h);
    return 0;
}

static int sep_overlay_fullalpha(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_overlay_fullalpha (stub)\n");
#endif
    return 0;
}

// // --- Drawing primitives ---
// static int sep_draw_transparent(lua_State *L) {
// #if DEBUG_STUB_LOG
//     printf("[SingeStub] sep_draw_transparent (stub)\n");
// #endif
//     return 0;
// }
// #define DEBUG_HITBOX 1
static int sep_overlay_box(lua_State *L) {
    if (lua_gettop(L) < 4) { lua_pushboolean(L, 0); return 1; }

    int x1 = (int)lua_tonumber(L, 1);
    int y1 = (int)lua_tonumber(L, 2);
    int x2 = (int)lua_tonumber(L, 3);
    int y2 = (int)lua_tonumber(L, 4);

    float scaled_x1 = (x1 - g_ratio_x_offset) * g_scale_x;
    float scaled_y1 = (y1 - g_ratio_y_offset) * g_scale_y;
    float scaled_x2 = (x2 - g_ratio_x_offset) * g_scale_x;
    float scaled_y2 = (y2 - g_ratio_y_offset) * g_scale_y;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
    uint32_t color = 
        ((GFontColorA & 0xFF) << 24) |  // Alpha
        ((GFontColorR & 0xFF) << 16) |  // Red
        ((GFontColorG & 0xFF) << 8)  |  // Green
        ((GFontColorB & 0xFF));         // Blue

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x1; vert.y = scaled_y1; vert.z = 1.0f;
    vert.argb = color; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x2; vert.y = scaled_y1;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x1; vert.y = scaled_y2;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = scaled_x2; vert.y = scaled_y2;
    pvr_prim(&vert, sizeof(vert));

    lua_pushboolean(L, 1);
    return 1;
}


static int sep_overlay_circle(lua_State *L) {
    if (lua_gettop(L) < 3) { lua_pushboolean(L, 0); return 1; }

    int x = (int)lua_tonumber(L, 1);
    int y = (int)lua_tonumber(L, 2);
    int radius = (int)lua_tonumber(L, 3);
    int filled = (lua_gettop(L) >= 4) ? (int)lua_tonumber(L, 4) : 0;

    float scaled_x = (x - g_ratio_x_offset) * g_scale_x;
    float scaled_y = (y - g_ratio_y_offset) * g_scale_y;
    float scaled_radius = radius * ((g_scale_x + g_scale_y) * 0.5f);

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr;
    pvr_vertex_t vert;
    uint16_t color16 = pack_argb1555_overlay(GFontColorA, GFontColorR, GFontColorG, GFontColorB);

    pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    int segments = 32;

    if (filled) {
        for (int i = 0; i <= segments; i++) {
            float a1 = (2.0f * M_PI * i) / segments;
            float a2 = (2.0f * M_PI * (i + 1)) / segments;

            vert.flags = PVR_CMD_VERTEX;
            vert.x = scaled_x; vert.y = scaled_y; vert.z = 1.0f;
            vert.argb = color16; vert.oargb = 0; pvr_prim(&vert, sizeof(vert));

            vert.flags = PVR_CMD_VERTEX;
            vert.x = scaled_x + fcos(a1) * scaled_radius;
            vert.y = scaled_y + fsin(a1) * scaled_radius;
            vert.z = 1.0f;
            vert.argb = color16; vert.oargb = 0; pvr_prim(&vert, sizeof(vert));

            vert.flags = (i == segments - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert.x = scaled_x + fcos(a2) * scaled_radius;
            vert.y = scaled_y + fsin(a2) * scaled_radius;
            vert.z = 1.0f;
            vert.argb = color16; vert.oargb = 0; pvr_prim(&vert, sizeof(vert));
        }
    } else {
        float width = 2.0f;
        for (int i = 0; i < segments; i++) {
            float a1 = (2.0f * M_PI * i) / segments;
            float a2 = (2.0f * M_PI * (i + 1)) / segments;
            float x1 = scaled_x + fcos(a1) * scaled_radius;
            float y1 = scaled_y + fsin(a1) * scaled_radius;
            float x2 = scaled_x + fcos(a2) * scaled_radius;
            float y2 = scaled_y + fsin(a2) * scaled_radius;

            float dx = x2 - x1, dy = y2 - y1;
            float invmag = frsqrt((dx * dx) + (dy * dy)) * (width * 0.5f);
            float nx = -dy * invmag, ny = dx * invmag;

            vert.flags = PVR_CMD_VERTEX;
            vert.x = x1 + nx; vert.y = y1 + ny; vert.z = 1.0f;
            vert.argb = color16; vert.oargb = 0; pvr_prim(&vert, sizeof(vert));

            vert.flags = PVR_CMD_VERTEX;
            vert.x = x1 - nx; vert.y = y1 - ny; pvr_prim(&vert, sizeof(vert));

            vert.flags = PVR_CMD_VERTEX;
            vert.x = x2 + nx; vert.y = y2 + ny; pvr_prim(&vert, sizeof(vert));

            vert.flags = (i == segments - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert.x = x2 - nx; vert.y = y2 - ny; pvr_prim(&vert, sizeof(vert));
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}   

static int sep_overlay_ellipse(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_overlay_ellipse (stub)\n");
#endif
    return 0;
}

static int sep_overlay_line(lua_State *L) {
    int n = lua_gettop(L);
    if (n == 4 &&
        lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
        lua_isnumber(L, 3) && lua_isnumber(L, 4)) {
        
        int x1 = (int)lua_tonumber(L, 1);
        int y1 = (int)lua_tonumber(L, 2);
        int x2 = (int)lua_tonumber(L, 3);
        int y2 = (int)lua_tonumber(L, 4);

        // --- Apply global offsets before scaling ---
        float adjusted_x1 = x1 - g_ratio_x_offset;
        float adjusted_y1 = y1 - g_ratio_y_offset;
        float adjusted_x2 = x2 - g_ratio_x_offset;
        float adjusted_y2 = y2 - g_ratio_y_offset;

        // --- Scale overlay coordinates to screen coordinates ---
        float scaled_x1 = adjusted_x1 * g_scale_x;
        float scaled_y1 = adjusted_y1 * g_scale_y;
        float scaled_x2 = adjusted_x2 * g_scale_x;
        float scaled_y2 = adjusted_y2 * g_scale_y;

#ifdef DEBUG_HITBOX
        printf("[LINE] RAW:(%d,%d)->(%d,%d) "
               "offset=(%.1f,%.1f) scaled:(%.1f,%.1f)->(%.1f,%.1f) "
               "scale=(%.2f,%.2f)\n",
               x1, y1, x2, y2,
               g_ratio_x_offset, g_ratio_y_offset,
               scaled_x1, scaled_y1, scaled_x2, scaled_y2,
               g_scale_x, g_scale_y);
#endif

        // --- Calculate perpendicular normal (from KOS example) ---
        float dx = scaled_x2 - scaled_x1;
        float dy = scaled_y2 - scaled_y1;
        
        float width = 2.0f; // Line thickness in pixels
        
        // Use fast reciprocal square root to get inverse magnitude
        // Multiply by half the line width to scale the normal
        float inverse_magnitude = frsqrt((dx * dx) + (dy * dy)) * (width * 0.5f);
        float nx = -dy * inverse_magnitude;
        float ny = dx * inverse_magnitude;

        // --- Draw the line as a quad ---
        pvr_poly_cxt_t cxt;
        pvr_poly_hdr_t hdr;
        pvr_vertex_t vert;

        uint32_t color = 
                ((GFontColorA & 0xFF) << 24) |  // Alpha
                ((GFontColorR & 0xFF) << 16) |  // Red
                ((GFontColorG & 0xFF) << 8)  |  // Green
                ((GFontColorB & 0xFF));         // Blue

        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        pvr_prim(&hdr, sizeof(hdr));

        // Normal offset "down" from first endpoint
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x1 + nx;
        vert.y = scaled_y1 + ny;
        vert.z = 1.0f;
        vert.argb = color;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));

        // Normal offset "up" from first endpoint
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x1 - nx;
        vert.y = scaled_y1 - ny;
        vert.z = 1.0f;
        vert.argb = color;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));

        // Normal offset "down" from second endpoint
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x2 + nx;
        vert.y = scaled_y2 + ny;
        vert.z = 1.0f;
        vert.argb = color;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));

        // Normal offset "up" from second endpoint
        vert.flags = PVR_CMD_VERTEX_EOL;
        vert.x = scaled_x2 - nx;
        vert.y = scaled_y2 - ny;
        vert.z = 1.0f;
        vert.argb = color;
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));

        lua_pushboolean(L, 1);
        return 1;
    }

    lua_pushboolean(L, 0);
    return 1;
}


static int sep_overlay_plot(lua_State *L) {
if (lua_gettop(L) < 2) return 0;


int x = (int)lua_tonumber(L, 1);
int y = (int)lua_tonumber(L, 2);


float adjusted_x = x - g_ratio_x_offset;
float adjusted_y = y - g_ratio_y_offset;
float scaled_x = adjusted_x * g_scale_x;
float scaled_y = adjusted_y * g_scale_y;


pvr_poly_cxt_t cxt;
pvr_poly_hdr_t hdr;
pvr_vertex_t vert;


// translucent list, alpha enabled
pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
cxt.gen.alpha = PVR_ALPHA_ENABLE;
cxt.blend.src = PVR_BLEND_SRCALPHA;
cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
cxt.blend.src_enable = PVR_BLEND_ENABLE;
cxt.blend.dst_enable = PVR_BLEND_ENABLE;
pvr_poly_compile(&hdr, &cxt);
pvr_prim(&hdr, sizeof(hdr));


uint32_t color = 
        ((GFontColorA & 0xFF) << 24) |  // Alpha
        ((GFontColorR & 0xFF) << 16) |  // Red
        ((GFontColorG & 0xFF) << 8)  |  // Green
        ((GFontColorB & 0xFF));         // Blue


float pixel_size = 2.0f;


vert.flags = PVR_CMD_VERTEX;
vert.x = scaled_x;
vert.y = scaled_y;
vert.z = 1.0f;
vert.argb = color;
vert.oargb = 0;
pvr_prim(&vert, sizeof(vert));


vert.flags = PVR_CMD_VERTEX;
vert.x = scaled_x + pixel_size;
vert.y = scaled_y;
vert.z = 1.0f;
vert.argb = color;
vert.oargb = 0;
pvr_prim(&vert, sizeof(vert));


vert.flags = PVR_CMD_VERTEX;
vert.x = scaled_x;
vert.y = scaled_y + pixel_size;
vert.z = 1.0f;
vert.argb = color;
vert.oargb = 0;
pvr_prim(&vert, sizeof(vert));


vert.flags = PVR_CMD_VERTEX_EOL;
vert.x = scaled_x + pixel_size;
vert.y = scaled_y + pixel_size;
vert.z = 1.0f;
vert.argb = color;
vert.oargb = 0;
pvr_prim(&vert, sizeof(vert));


lua_pushboolean(L, 1);
return 1;
}

static int sep_say(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] overlayPrint (stub)\n");
#endif
    return 0;
}

// ===========================================================================
// Hypseus Singe Stubs - Music / Sound
// ===========================================================================
#include <mp3/sndserver.h>
#include <kos/fs.h>

#define MAX_MUSIC_TRACKS 16

// External function from your codebase
static char* resolve_path(const char* filename);

// Global state for music playback
typedef struct {
    char filepath[256];
    int handle;
    int loaded;
    int failed_to_play;  // Track if this file failed to play
} music_track_t;

static music_track_t g_music_tracks[MAX_MUSIC_TRACKS] = {0};
static int g_next_handle = 1;
static int g_current_playing_handle = -1;

// Initialize MP3 system (call this once at startup)
void sep_music_init(void) {
    printf("[Music] Initializing MP3 system...\n");
    // snd_stream_init();
    mp3_init();
    g_current_playing_handle = -1;
    for (int i = 0; i < MAX_MUSIC_TRACKS; i++) {
        g_music_tracks[i].loaded = 0;
        g_music_tracks[i].handle = -1;
        g_music_tracks[i].failed_to_play = 0;
    }
    printf("[Music] MP3 system initialized\n");
}

// Shutdown MP3 system (call this at cleanup)
void sep_music_cleanup(void) {
    printf("[Music] Shutting down MP3 system...\n");
    if (g_current_playing_handle >= 0) {
        mp3_stop();
    }
    mp3_shutdown();
    snd_stream_shutdown();
    printf("[Music] MP3 system shutdown complete\n");
}

// Find a track by handle
static music_track_t* find_track_by_handle(int handle) {
    for (int i = 0; i < MAX_MUSIC_TRACKS; i++) {
        if (g_music_tracks[i].loaded && g_music_tracks[i].handle == handle) {
            return &g_music_tracks[i];
        }
    }
    return NULL;
}

// Find an empty slot
static music_track_t* find_empty_slot(void) {
    for (int i = 0; i < MAX_MUSIC_TRACKS; i++) {
        if (!g_music_tracks[i].loaded) {
            return &g_music_tracks[i];
        }
    }
    return NULL;
}

// Check if file exists and is accessible
static int check_file_exists(const char *path) {
    file_t f = fs_open(path, O_RDONLY);
    if (f < 0) {
        return 0;
    }
    fs_close(f);
    return 1;
}

// Get file size for diagnostics
static size_t get_file_size(const char *path) {
    file_t f = fs_open(path, O_RDONLY);
    if (f < 0) {
        return 0;
    }
    size_t size = fs_total(f);
    fs_close(f);
    return size;
}

// --- Music Control ---
static int sep_music_load(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    
    printf("[Music] Loading: %s\n", filename);

    // Find an empty slot
    music_track_t *track = find_empty_slot();
    if (!track) {
        printf("[Music] Error: No free slots available (max %d tracks)\n", MAX_MUSIC_TRACKS);
        lua_pushnumber(L, -1);
        return 1;
    }

    // Use the existing resolve_path function
    char *resolved_path = resolve_path(filename);
    if (!resolved_path) {
        printf("[Music] Error: Failed to resolve path for: %s\n", filename);
        lua_pushnumber(L, -1);
        return 1;
    }
    
    printf("[Music] Resolved path: %s\n", resolved_path);

    // Check if file exists
    if (!check_file_exists(resolved_path)) {
        printf("[Music] Error: File not found: %s\n", resolved_path);
        free(resolved_path);
        lua_pushnumber(L, -1);
        return 1;
    }

    // Get file size for diagnostics
    size_t file_size = get_file_size(resolved_path);
    printf("[Music] File size: %zu bytes\n", file_size);

    // Store the resolved filepath
    strncpy(track->filepath, resolved_path, sizeof(track->filepath) - 1);
    track->filepath[sizeof(track->filepath) - 1] = '\0';
    free(resolved_path);
    
    // Generate a new handle
    track->handle = g_next_handle++;
    track->loaded = 1;
    track->failed_to_play = 0;
    
    printf("[Music] Loaded successfully, handle: %d\n", track->handle);
    
    lua_pushnumber(L, track->handle);
    return 1;
}

static int sep_music_play(lua_State *L) {
    int handle = (int)luaL_checknumber(L, 1);

    music_track_t *track = find_track_by_handle(handle);
    if (!track) {
        // Silently fail for invalid handles
        lua_pushboolean(L, 0);
        return 1;
    }

    // If this track has already failed to play, don't spam the logs
    if (track->failed_to_play) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // Only log the first attempt
    printf("[Music] Play requested for handle: %d\n", handle);

    // Stop current playback if any
    if (g_current_playing_handle >= 0) {
        printf("[Music] Stopping current playback (handle: %d)\n", g_current_playing_handle);
        mp3_stop();
    }

    // Verify file still exists before playing
    if (!check_file_exists(track->filepath)) {
        printf("[Music] Error: File no longer accessible: %s\n", track->filepath);
        track->failed_to_play = 1;
        lua_pushboolean(L, 0);
        return 1;
    }

    // Start MP3 playback (0 = play once, 1 = loop)
    printf("[Music] Starting playback: %s\n", track->filepath);
    int result = mp3_start(track->filepath, 0);
    
    // if (result == 0) {
        g_current_playing_handle = handle;
        printf("[Music] Playback started successfully\n");
        lua_pushboolean(L, 1);
    // } else {
    //     g_current_playing_handle = -1;
    //     printf("[Music] ERROR: MP3 file format not supported by KOS libmp3\n");
    //     printf("[Music] KOS libmp3 only supports basic MPEG Layer 3 (MP3) files:\n");
    //     printf("[Music]   - Sample rates: 32000, 44100, 48000 Hz\n");
    //     printf("[Music]   - Bitrates: 32-320 kbps\n");
    //     printf("[Music]   - Channels: Mono or Stereo\n");
    //     printf("[Music]   - No ID3v2 tags at the beginning\n");
    //     printf("[Music] Consider re-encoding your MP3 files with these settings:\n");
    //     printf("[Music]   ffmpeg -i input.mp3 -ar 44100 -ab 128k -ac 2 output.mp3\n");
    //     printf("[Music] Further playback attempts for this track will be silent.\n");
    //     track->failed_to_play = 1;
    //     lua_pushboolean(L, 0);
    // }
    
    return 1;
}

static int sep_music_pause(lua_State *L) {
    if (g_current_playing_handle >= 0) {
        mp3_stop();
    }
    return 0;
}

static int sep_music_resume(lua_State *L) {
    if (g_current_playing_handle >= 0) {
        music_track_t *track = find_track_by_handle(g_current_playing_handle);
        if (track && !track->failed_to_play) {
            mp3_start(track->filepath, 0);
        }
    }
    return 0;
}

static int sep_music_stop(lua_State *L) {
    if (g_current_playing_handle >= 0) {
        mp3_stop();
        g_current_playing_handle = -1;
    }
    return 0;
}

static int sep_music_playing(lua_State *L) {
    int is_playing = (g_current_playing_handle >= 0);
    lua_pushboolean(L, is_playing);
    return 1;
}

static int sep_music_volume(lua_State *L) {
    int volume = (int)luaL_checknumber(L, 1);
    // libmp3 in KOS doesn't have direct volume control in the basic API
    return 0;
}

static int sep_music_unload(lua_State *L) {
    int handle = (int)luaL_checknumber(L, 1);

    music_track_t *track = find_track_by_handle(handle);
    if (track) {
        if (g_current_playing_handle == handle) {
            mp3_stop();
            g_current_playing_handle = -1;
        }
        track->loaded = 0;
        track->handle = -1;
        track->failed_to_play = 0;
        track->filepath[0] = '\0';
    }
    
    return 0;
}

// --- Sound Control ---
static int sep_sound_load(lua_State *L) {
    const char *path = lua_tostring(L, 1);
    char *fullpath = resolve_path(path);
    // DC_log("Loading sound: %s -> %s\n", path, fullpath);
    
    // Check cache (use original path for cache key)
    for (SingeSound *sound = GSounds; sound != NULL; sound = sound->next) {
        if (strcmp(sound->name, path) == 0) {
            free(fullpath);
            lua_pushinteger(L, (lua_Integer)sound);
            return 1;
        }
    }
    
    // Load new sound (use full path for loading)

    sfxhnd_t sfx = snd_sfx_load(fullpath);

    if (sfx < 0) {
        DC_log("Failed to load sound: %s", fullpath);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }
    
    SingeSound *sound = Singe_xmalloc(sizeof(SingeSound));
    sound->name = Singe_xstrdup(path);  // Store original path for cache
    sound->handle = sfx;
    sound->next = GSounds;
    GSounds = sound;
    
    free(fullpath);
    lua_pushinteger(L, (lua_Integer)sound);
    return 1;
}
// static int sep_sound_load(lua_State *L) {
//     const char *path = lua_tostring(L, 1);
//     char *fullpath = resolve_path(path);
//     Singe_log("Loading sound: %s -> %s\n", path, fullpath);

//     // ---- Cache check ----
//     for (SingeSound *sound = GSounds; sound; sound = sound->next) {
//         if (strcmp(sound->name, path) == 0) {
//             free(fullpath);
//             lua_pushinteger(L, (lua_Integer)sound);
//             return 1;
//         }
//     }

//     // ---- Open and read sound file ----
//     int fd = fs_open(fullpath, O_RDONLY);
//     if (fd < 0) {
//         Singe_log("Failed to open sound: %s", fullpath);
//         free(fullpath);
//         lua_pushinteger(L, -1);
//         return 1;
//     }

//     size_t size = fs_total(fd);
//     // 🧩 Round up to 32 and 4-byte boundaries for G1-DMA safety
//     size_t aligned_size = (size + 31) & ~31;

//     // 🧱 Allocate 32-byte aligned buffer
//     void *aligned_buf = memalign(32, aligned_size);
//     if (!aligned_buf) {
//         Singe_log("Out of memory loading sound: %s", fullpath);
//         fs_close(fd);
//         free(fullpath);
//         lua_pushinteger(L, -1);
//         return 1;
//     }

//     size_t bytes_read = fs_read(fd, aligned_buf, aligned_size);
//     fs_close(fd);

//     if (bytes_read == 0) {
//         Singe_log("Read failed for sound: %s", fullpath);
//         free(aligned_buf);
//         free(fullpath);
//         lua_pushinteger(L, -1);
//         return 1;
//     }

    // ---- Register sound with AICA ----
    // (Assuming 44.1 kHz, 16-bit stereo for standard SFX; adjust if you use mono/22 kHz.)
//     sfxhnd_t sfx = snd_sfx_load_raw_buf(aligned_buf, size, 44100, 8, 2);
//     free(aligned_buf);

//     if (sfx < 0) {
//         Singe_log("Failed to register sound: %s", fullpath);
//         free(fullpath);
//         lua_pushinteger(L, -1);
//         return 1;
//     }

//     // ---- Cache new sound ----
//     SingeSound *sound = Singe_xmalloc(sizeof(SingeSound));
//     sound->name = Singe_xstrdup(path);
//     sound->handle = sfx;
//     sound->next = GSounds;
//     GSounds = sound;

//     free(fullpath);
//     lua_pushinteger(L, (lua_Integer)sound);
//     return 1;
// }


static int sep_sound_play(lua_State *L) {
    lua_Integer sound_id = lua_tointeger(L, 1);
    SingeSound *sound = (SingeSound *)sound_id;

    if (sound && sound->handle >= 0) {
        // Convert global volume (0–255) to sfx API scale
        int vol = atomic_load(&g_audio_movie_vol);
        if (vol < 0) vol = 0;
        if (vol > 255) vol = 255;

        // snd_sfx_play(handle, volume, pan)
        snd_sfx_play(sound->handle, vol, 128);

        // printf("[Singe] soundPlay(id=%ld, vol=%d)\n", (long)sound_id, vol);
    } else {
        printf("[Singe] soundPlay(%ld) -> invalid handle\n", (long)sound_id);
    }

    return 1;
}

static int  sep_sound_getvolume(lua_State *L) {
    // Convert from 0–255 Dreamcast volume scale to 0–63 Hypseus scale
    int raw_vol = atomic_load(&g_audio_movie_vol);
    if (raw_vol < 0) raw_vol = 0;
    if (raw_vol > 255) raw_vol = 255;

    int vol63 = (raw_vol * 63) / 255;
    printf("[Singe] soundGetVolume() -> %d (raw=%d)\n", vol63, raw_vol);

    lua_pushinteger(L, vol63);
    return 1;
}
static int sep_sound_pause(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_pause (stub)\n");
#endif
    return 0;
}

static int sep_sound_resume(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_resume (stub)\n");
#endif
    return 0;
}

static int sep_sound_stop(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_stop (stub)\n");
#endif
    return 1;
}

static int sep_sound_is_playing(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_is_playing (stub)\n");
#endif
    return 0;
}

static int sep_sound_volume(lua_State *L) {
    // Convert from 0–255 Dreamcast volume scale to 0–63 Hypseus scale
    int raw_vol = atomic_load(&g_audio_movie_vol);
    if (raw_vol < 0) raw_vol = 0;
    if (raw_vol > 255) raw_vol = 255;

    int vol63 = (raw_vol * 63) / 255;
    Singe_log("[Singe] sep_sound_volume() -> %d (raw=%d)\n", vol63, raw_vol);

    lua_pushinteger(L, vol63);
    return 1;
}

static int sep_sound_fullstop(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_fullstop (stub)\n");
#endif
    return 0;
}

static int sep_sound_unload(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sound_unload (stub)\n");
#endif
    return 0;
}
// ===========================================================================
// Hypseus Singe Stubs – Controller / Keyboard / Input
// ===========================================================================
// --- Controller support ---
static int sep_controller_valid(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_controller_valid (stub)\n");
#endif
    return 0;
}

static int sep_controller_rumble(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_controller_rumble (stub)\n");
#endif
    return 0;
}

static int sep_controller_button(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_controller_button (stub)\n");
#endif
    return 0;
}

static int sep_controller_setwad(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_controller_setwad (stub)\n");
#endif
    return 0;
}

static int sep_controller_getwad(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_controller_getwad (stub)\n");
#endif
    return 0;
}

// --- JoyMouse support ---
static int sep_joymouse_enable(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_joymouse_enable (stub)\n");
#endif
    return 0;
}

static int sep_joymouse_disable(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_joymouse_disable (stub)\n");
#endif
    return 0;
}

// --- Keyboard support ---
static int sep_keyboard_get_mode(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_keyboard_get_mode (stub)\n");
#endif
    return 0;
}

static int sep_keyboard_set_mode(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_keyboard_set_mode (stub)\n");
#endif
    return 0;
}

static int sep_keyboard_block_quit(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_keyboard_block_quit (stub)\n");
#endif
    return 0;
}
// ===========================================================================
// Hypseus Singe Stubs – Sprite / Drawing
// ===========================================================================
// #define DEBUG_SPRITEDRAW 1
int sep_sprite_draw(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 3) return 0;

    int x = 0, y = 0, x2 = 0, y2 = 0;
    bool center = false;
    unsigned long sprite_hash_id = 0;
    SingeSprite *sprite = NULL;

    // Parse parameters based on mode
    if (n == 3) {  // spriteDraw(x, y, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            sprite = (SingeSprite *)lua_tointeger(L, 3);
            sprite_hash_id = sprite->hash_id;
        }
    } else if (n == 4) {  // spriteDraw(x, y, c, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
            lua_isboolean(L, 3) && lua_isnumber(L, 4)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            center = lua_toboolean(L, 3);
            sprite = (SingeSprite *)lua_tointeger(L, 4);
            sprite_hash_id = sprite->hash_id;
        }
    } else if (n == 5) {  // spriteDraw(x, y, x2, y2, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
            lua_isnumber(L, 3) && lua_isnumber(L, 4) && lua_isnumber(L, 5)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            x2 = (int)lua_tonumber(L, 3);
            y2 = (int)lua_tonumber(L, 4);
            sprite = (SingeSprite *)lua_tointeger(L, 5);
            sprite_hash_id = sprite->hash_id;
        }
    } else if (n == 6) {  // spriteDraw(x, y, x2, y2, c, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
            lua_isnumber(L, 3) && lua_isnumber(L, 4) &&
            lua_isboolean(L, 5) && lua_isnumber(L, 6)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            x2 = (int)lua_tonumber(L, 3);
            y2 = (int)lua_tonumber(L, 4);
            center = lua_toboolean(L, 5);
            sprite = (SingeSprite *)lua_tointeger(L, 6);
            sprite_hash_id = sprite->hash_id;
        }
    }

    // Resolve hash_id to cached sprite
    char sprite_hash_str[64];
    snprintf(sprite_hash_str, sizeof(sprite_hash_str), "%lu", sprite_hash_id);
    sprite = get_cached_sprite(sprite_hash_str);

    if (!sprite || !sprite->texture) {
        DC_log("Sprite with hash_id %lu not found or has no texture\n", sprite_hash_id);
        return 0;
    }

    // --- No screen scaling or ratio offsets ---
    int scaled_x  = x;
    int scaled_y  = y;
    int scaled_x2 = x2;
    int scaled_y2 = y2;

    int w, h;
    if (n == 3 || n == 4) {
        w = sprite->width;
        h = sprite->height;
    } else {
        w = scaled_x2 - scaled_x + 1;
        h = scaled_y2 - scaled_y + 1;
    }

// --- Match coordinate transform used by fonts and overlays ---
float adjusted_x = x - g_ratio_x_offset;
float adjusted_y = y - g_ratio_y_offset;

int screen_x = (int)roundf(adjusted_x * g_scale_x);
int screen_y = (int)roundf(adjusted_y * g_scale_y);

scaled_x = screen_x;
scaled_y = screen_y;

// Center adjustment (tweak alignment for text vs digits)
if (center)
    scaled_x -= w / 2;
else
    scaled_x -= w / 4;

// Clamp to display bounds (not overlay bounds)
if (scaled_x < 0) scaled_x = 0;
if (scaled_y < 0) scaled_y = 0;
if (scaled_x + w > g_display_w)  scaled_x = g_display_w  - w;
if (scaled_y + h > g_display_h)  scaled_y = g_display_h - h;

#ifdef DEBUG_SPRITEDRAW
    const char *mode_str = "";
    if (n == 3) mode_str = "simple";
    else if (n == 4) mode_str = "centered";
    else if (n == 5) mode_str = "stretched";
    else if (n == 6) mode_str = "centered_stretched";

    Singe_log("Draw sprite '%s' mode=%s raw=(%d,%d) overlay=(%d,%d) size=%dx%d center=%d\n",
           sprite->name ? sprite->name : "(unnamed)", mode_str,
           x, y, scaled_x, scaled_y, w, h, center);
#endif

    // --- Issue PVR draw ---
    pvr_vertex_t verts[4] = {
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,     .y = scaled_y,     .z = 1.0f, .u = 0.0f, .v = 0.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x + w, .y = scaled_y,     .z = 1.0f, .u = 1.0f, .v = 0.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,     .y = scaled_y + h, .z = 1.0f, .u = 0.0f, .v = 1.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x + w, .y = scaled_y + h, .z = 1.0f, .u = 1.0f, .v = 1.0f, .argb = 0xFFFFFFFF }
    };

    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &sprite->hdr, 1);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), verts, 4);

    return 0;
}



static int sep_draw_transparent(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_draw_transparent (stub)\n");
#endif
    return 0;
}

static int sep_sprite_animate(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_animate (stub)\n");
#endif
    return 0;
}

static int sep_sprite_animate_rotated(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_animate_rotated (stub)\n");
#endif
    return 0;
}

// --- Sprite Geometry ---
static int sep_sprite_width(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 1) { lua_pushinteger(L, 0); return 1; }

    SingeSprite *sprite = (SingeSprite *)lua_tointeger(L, 1);
    if (!sprite) { lua_pushinteger(L, 0); return 1; }

    unsigned long sprite_hash_id = sprite->hash_id;

    char sprite_hash_str[64];
    snprintf(sprite_hash_str, sizeof(sprite_hash_str), "%lu", sprite_hash_id);
    sprite = get_cached_sprite(sprite_hash_str);

    if (!sprite) {
        printf("[SINGE] spriteGetWidth: not found (hash %lu)\n", sprite_hash_id);
        lua_pushinteger(L, 0);
        return 1;
    }

    lua_pushinteger(L, sprite->width);
    // DC_log("[SINGE] spriteGetWidth('%s') = %d\n",
    //        sprite->name ? sprite->name : "(unnamed)", sprite->width);
    return 1;
}

static int sep_sprite_height(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 1) { lua_pushinteger(L, 0); return 1; }

    SingeSprite *sprite = (SingeSprite *)lua_tointeger(L, 1);
    if (!sprite) { lua_pushinteger(L, 0); return 1; }

    unsigned long sprite_hash_id = sprite->hash_id;

    char sprite_hash_str[64];
    snprintf(sprite_hash_str, sizeof(sprite_hash_str), "%lu", sprite_hash_id);
    sprite = get_cached_sprite(sprite_hash_str);

    if (!sprite) {
        Singe_log("[SINGE] spriteGetHeight: not found (hash %lu)\n", sprite_hash_id);
        lua_pushinteger(L, 0);
        return 1;
    }

    lua_pushinteger(L, sprite->height);
    // DC_log("[SINGE] spriteGetHeight('%s') = %d\n",
    //        sprite->name ? sprite->name : "(unnamed)", sprite->height);
    return 1;
}




static int sep_sprite_frames(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_frames (stub)\n");
#endif
    return 0;
}

// --- Loading / Unloading ---
static int sep_sprite_loadframes(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_loadframes (stub)\n");
#endif
    return 0;
}

static int sep_sprite_loadata(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_loadata (stub)\n");
#endif
    return 0;
}

static int sep_sprite_color_rekey(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_color_rekey (stub)\n");
#endif
    return 0;
}

// --- Rotation / Scaling / Quality ---
static int sep_sprite_rotateframe(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_rotateframe (stub)\n");
#endif
    return 0;
}

static int sep_sprite_rotate(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_rotate (stub)\n");
#endif
    return 0;
}

static int sep_sprite_rotatescale(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_rotatescale (stub)\n");
#endif
    return 0;
}

static int sep_sprite_quality(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_quality (stub)\n");
#endif
    return 0;
}

static int sep_sprite_scale(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_scale (stub)\n");
#endif
    return 0;
}

// --- Animation Control ---
static int sep_sprite_get_frame(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_get_frame (stub)\n");
#endif
    return 0;
}

static int sep_sprite_playing(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_playing (stub)\n");
#endif
    return 0;
}

static int sep_sprite_loop(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_loop (stub)\n");
#endif
    return 0;
}

static int sep_sprite_pause(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_pause (stub)\n");
#endif
    return 0;
}

static int sep_sprite_play(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_play (stub)\n");
#endif
    return 0;
}

static int sep_sprite_set_frame(lua_State *L) {
#if DEBUG_STUB_LOG
    printf("[SingeStub] sep_sprite_set_frame (stub)\n");
#endif
    return 0;
}

// Custom io.output - captures the filename for VMU
static int custom_io_output(lua_State *L) {
    const char *filename = luaL_checkstring(L, 1);
    
    printf("[Custom io.output] VMU support coming soon. Output set to: %s\n", filename);
    
    // Must return a file handle (the script expects one)
    // Return a dummy userdata pointer
    lua_pushlightuserdata(L, (void*)1);
    return 1;
}

// Custom io.write function
static int custom_io_write(lua_State *L) {
    const char *data = luaL_checkstring(L, 1);
    // DC_log("[Custom io.write] VMU support coming soon: %s\n", data);
    
    // io.write should return true on success
    lua_pushboolean(L, 1);
    return 1;
}

// Custom io.close function
static int custom_io_close(lua_State *L) {
    // The script passes the file handle from io.output
    // DC_log("[Custom io.close] VMU support coming soon. Closing file.\n");
    
    // io.close returns true on success
    lua_pushboolean(L, 1);
    return 1;
}

// Patch the standard io library with custom functions
void override_lfs_with_vmu_support(lua_State *L) {
    // Get the global 'io' table
    lua_getglobal(L, "io");
    
    if (!lua_istable(L, -1)) {
        printf("[Error] 'io' library not found!\n");
        lua_pop(L, 1);
        return;
    }
    
    // Replace io.output
    lua_pushcfunction(L, custom_io_output);
    lua_setfield(L, -2, "output");
    
    // Replace io.write
    lua_pushcfunction(L, custom_io_write);
    lua_setfield(L, -2, "write");
    
    // Replace io.close
    lua_pushcfunction(L, custom_io_close);
    lua_setfield(L, -2, "close");
    
    // Pop the io table
    lua_pop(L, 1);
    
    printf("[Lua] Standard io library patched with VMU support\n");
}   
// Setup Lua
static void setup_lua(void) {
    printf("=== setup_lua() START ===\n");
    
    printf("[1] Creating Lua state...\n");
    GLua = lua_newstate(Singe_lua_allocator, NULL);
    if (!GLua) {
        printf("PANIC: Failed to create Lua state\n");
        exit(1);
    }
    printf("[1] ✓ Lua state created\n");

    lua_atpanic(GLua, sep_panic);

    printf("[3] Opening standard libraries...\n");
    luaL_openlibs(GLua);

    // ✅ Load LuaFileSystem (liblfs.a)
    // ✅ Load LuaFileSystem globally
    luaL_requiref(GLua, "lfs", luaopen_lfs, 1);
    lua_setglobal(GLua, "lfs");


    // Override the filesystem with custom VMU handlers
    override_lfs_with_vmu_support(GLua);

    // Now Lua scripts using io.write and io.open will be patched.    
    printf("    Lua version: %s\n", LUA_VERSION);

    // Register Singe API functions
    // ============================================================================
    // Hypseus Singe (sep_*) Lua Registration  
    // Based on Hypseus Singe API
    // ============================================================================

    // ---------------------------------------------------------------------------
    // Disc / Video Control (sep_*)
    // ---------------------------------------------------------------------------
    lua_register(GLua, "discGetFrame", sep_get_current_frame);
    lua_register(GLua, "discSkipToFrame", sep_skip_to_frame);
    lua_register(GLua, "discSearch", sep_search);
    lua_register(GLua, "discPause", sep_pause);
    lua_register(GLua, "discPlay", sep_play);
    lua_register(GLua, "discStop", sep_stop);
    lua_register(GLua, "discSetFPS", sep_set_disc_fps);
    lua_register(GLua, "discAudio", sep_audio_control);
    lua_register(GLua, "discChangeSpeed", sep_change_speed);
    lua_register(GLua, "mouseHowMany", sep_get_number_of_mice);
    lua_register(GLua, "discStepBackward", sep_step_backward);
    lua_register(GLua, "overlaySetResolution", sep_set_custom_overlay);

    // ---------------------------------------------------------------------------
    // Font / Color / Singe Info (api*)
    // ---------------------------------------------------------------------------
    lua_register(GLua, "fontLoad", sep_font_load);
    lua_register(GLua, "fontSelect", sep_font_select);
    lua_register(GLua, "fontPrint", sep_say_font);
    lua_register(GLua, "fontQuality", sep_font_quality);
    lua_register(GLua, "fontToSprite", sep_font_sprite);
    lua_register(GLua, "fontUnload", sep_font_unload);
    lua_register(GLua, "singeGetPauseFlag",      sep_get_pause_flag);
    lua_register(GLua, "singeSetPauseFlag",      sep_set_pause_flag);
    lua_register(GLua, "singeVersion", sep_singe_version);
    lua_register(GLua, "singeQuit", sep_singe_quit);
    lua_register(GLua, "singeWantsCrosshairs", sep_singe_wants_crosshair);
    lua_register(GLua, "singeSetGameName", sep_set_gamename);
    lua_register(GLua, "singeGetScriptPath", sep_get_scriptpath);

    // ---------------------------------------------------------------------------
    // Sprite / Video / VLDP (api*)
    // ---------------------------------------------------------------------------
    lua_register(GLua, "spriteGetHeight", sep_sprite_height);
    lua_register(GLua, "spriteGetWidth", sep_sprite_width);
    lua_register(GLua, "spriteLoad", sep_sprite_load);
    lua_register(GLua, "spriteUnload", sep_sprite_unload);
    lua_register(GLua, "videoGetVolume", sep_vldp_getvolume);
    lua_register(GLua, "videoSetVolume", sep_vldp_setvolume);

    // ---------------------------------------------------------------------------
    // Overlay (api*)
    // ---------------------------------------------------------------------------
    lua_register(GLua, "overlayClear", sep_overlay_clear);
    lua_register(GLua, "setOverlayResolution", sep_set_custom_overlay);

    // ---------------------------------------------------------------------------
    // Sound / Music (api*)
    // ---------------------------------------------------------------------------
    // lua_register(GLua, "soundLoad", sep_soundLoad);
    // lua_register(GLua, "soundPlay", sep_soundPlay);
    lua_register(GLua, "soundGetVolume", sep_sound_getvolume);
    // lua_register(GLua, "soundSetVolume", sep_SoundSetVolume);
    // lua_register(GLua, "soundUnload", sep_soundUnload);

    // ---------------------------------------------------------------------------
    // Misc (api*)
    // ---------------------------------------------------------------------------
    lua_register(GLua, "debugPrint", sep_debug_say);
    lua_register(GLua, "dofile", sep_doluafile);
    // lua_register(GLua, "spriteLoadFrames", sep_spriteLoadFrames);

    // ============================================================================
    // Hypseus Singe Compatibility (sep_*) — only unique or extra features enabled
    // ============================================================================

    // --- Ratio / MPEG / VLDP ---
    lua_register(GLua, "ratioGetX",           sep_ratioGetX);
    lua_register(GLua, "ratioGetY",           sep_ratioGetY);
    // lua_register(GLua, "vldpFlash",           sep_mpeg_set_flash);
    // lua_register(GLua, "vldpGetRotate",       sep_mpeg_get_rotate);
    // lua_register(GLua, "vldpSetRotate",       sep_mpeg_set_rotate);
    lua_register(GLua, "vldpGetScale",        sep_mpeg_get_scale);
    // lua_register(GLua, "vldpSetScale",        sep_mpeg_set_scale);
    // lua_register(GLua, "vldpFocusArea",       sep_mpeg_focus_area);
    // lua_register(GLua, "vldpGetYUVPixel",     sep_mpeg_get_rawpixel);
    // lua_register(GLua, "vldpResetFocus",      sep_mpeg_reset_focus);
    // lua_register(GLua, "vldpSetMonochrome",   sep_mpeg_set_grayscale);
    lua_register(GLua, "vldpGetWidth",     sep_vldp_get_width); 
    lua_register(GLua, "vldpGetHeight",    sep_vldp_get_height); 
    // lua_register(GLua, "vldpGetPixel",        sep_vldp_get_pixel);
    // lua_register(GLua, "vldpSetVerbose",      sep_vldp_verbose);
    lua_register(GLua, "discAudioSuffix",     sep_audio_suffix);

    // --- Overlay / Drawing ---
    lua_register(GLua, "colorBackground",     sep_color_set_backcolor);  
    lua_register(GLua, "colorForeground",     sep_color_set_forecolor);  
    lua_register(GLua, "overlayGetHeight", sep_get_overlay_height);
    lua_register(GLua, "overlayGetWidth",  sep_get_overlay_width); 
    // lua_register(GLua, "overlaySetMonochrome",sep_overlay_set_grayscale);
    lua_register(GLua, "setOverlaySize",   sep_set_overlaysize);      
    // lua_register(GLua, "setOverlayResolution", sep_set_custom_overlay);
    lua_register(GLua, "setOverlayFullAlpha", sep_overlay_fullalpha);
    lua_register(GLua, "overlayBox",          sep_overlay_box);
    lua_register(GLua, "overlayCircle",       sep_overlay_circle);
    // lua_register(GLua, "overlayEllipse",      sep_overlay_ellipse);
    lua_register(GLua, "overlayLine",         sep_overlay_line);
    lua_register(GLua, "overlayPlot",         sep_overlay_plot);
    // lua_register(GLua, "overlayPrint",        sep_say);

    // --- Bezel / Scoreboard / UI ---
    lua_register(GLua, "bezelLoad",           sep_bezel_load);
    lua_register(GLua, "bezelUnload",         sep_bezel_unload);
    lua_register(GLua, "bezelDraw",           sep_bezel_draw);
    lua_register(GLua, "bezelSetAlpha",       sep_bezel_set_alpha);
    lua_register(GLua, "bezelGetAlpha",       sep_bezel_get_alpha);
    lua_register(GLua, "bezelSetVisible",     sep_bezel_set_visible);
    lua_register(GLua, "bezelIsVisible",      sep_bezel_is_visible);
    lua_register(GLua, "bezelSetOverlay",     sep_bezel_set_overlay);
    lua_register(GLua, "scoreBezelEnable",    sep_bezel_enable);
    lua_register(GLua, "scoreBezelClear",     sep_bezel_clear);
    lua_register(GLua, "scoreBezelGetState",  sep_bezel_is_enabled);
    lua_register(GLua, "scoreBezelTwinScoreOn",sep_bezel_second_score);
    lua_register(GLua, "scoreBezelScore",     sep_bezel_player_score);
    lua_register(GLua, "scoreBezelLives",     sep_bezel_player_lives);
    lua_register(GLua, "scoreBezelCredits",   sep_bezel_credits);

    // --- Music / Sound ---
    lua_register(GLua, "musicLoad",           sep_music_load);
    lua_register(GLua, "musicPlay",           sep_music_play);
    // lua_register(GLua, "musicPause",          sep_music_pause);
    // lua_register(GLua, "musicResume",         sep_music_resume);
    lua_register(GLua, "musicStop",           sep_music_stop);
    lua_register(GLua, "musicIsPlaying",      sep_music_playing);
    lua_register(GLua, "musicSetVolume",      sep_music_volume);
    lua_register(GLua, "musicUnload",         sep_music_unload);
    // lua_register(GLua, "soundLoadData",       sep_sound_loadata);
    lua_register(GLua, "soundLoad",        sep_sound_load);   
    lua_register(GLua, "soundPlay",        sep_sound_play);       
    // lua_register(GLua, "soundPause",          sep_sound_pause);
    // lua_register(GLua, "soundResume",         sep_sound_resume);
    lua_register(GLua, "soundStop",           sep_sound_stop);
    lua_register(GLua, "soundIsPlaying",      sep_sound_is_playing);
    lua_register(GLua, "soundSetVolume",   sep_sound_volume);    
    // lua_register(GLua, "soundFullStop",       sep_sound_fullstop);
    lua_register(GLua, "soundUnload",      sep_sound_unload);      

    // --- Controller / Keyboard ---
    lua_register(GLua, "controllerIsValid",   sep_controller_valid);
    lua_register(GLua, "controllerDoRumble",  sep_controller_rumble);
    lua_register(GLua, "controllerGetButton", sep_controller_button);
    lua_register(GLua, "controllerSetPadding",sep_controller_setwad);
    lua_register(GLua, "controllerGetPadding",sep_controller_getwad);
    lua_register(GLua, "JoyMouseEnable",      sep_joymouse_enable);
    lua_register(GLua, "JoyMouseDisable",     sep_joymouse_disable);
    // // lua_register(GLua, "keyboardGetMode",     sep_keyboard_get_mode);  // [Using apiKeyboardGetMode]
    // // lua_register(GLua, "keyboardSetMode",     sep_keyboard_set_mode);  // [Using apiKeyboardSetMode]
    // lua_register(GLua, "keyboardCatchQuit",   sep_keyboard_block_quit);

    // --- Sprite / Drawing ---
    lua_register(GLua, "spriteDraw",          sep_sprite_draw);     
    // lua_register(GLua, "drawTransparent",     sep_draw_transparent);
    lua_register(GLua, "spriteDrawFrame",     sep_sprite_animate);
    // lua_register(GLua, "spriteDrawRotatedFrame", sep_sprite_animate_rotated);
    // lua_register(GLua, "spriteFrameHeight",   sep_sprite_height);
    // lua_register(GLua, "spriteFrameWidth",    sep_sprite_width);
    // lua_register(GLua, "spriteGetFrames",     sep_sprite_frames);
    lua_register(GLua, "spriteLoadFrames",    sep_sprite_loadframes);  
    // lua_register(GLua, "spriteLoadData",      sep_sprite_loadata);
    lua_register(GLua, "spriteResetColorKey", sep_sprite_color_rekey);
    // lua_register(GLua, "spriteRotateFrame",   sep_sprite_rotateframe);
    lua_register(GLua, "spriteRotate",        sep_sprite_rotate);
    // lua_register(GLua, "spriteRotateAndScale",sep_sprite_rotatescale);
    // lua_register(GLua, "spriteQuality",       sep_sprite_quality);
    lua_register(GLua, "spriteScale",         sep_sprite_scale);
    // lua_register(GLua, "spriteAnimGetFrame",  sep_sprite_get_frame);
    // lua_register(GLua, "spriteAnimIsPlaying", sep_sprite_playing);
    // lua_register(GLua, "spriteAnimLoop",      sep_sprite_loop);
    // lua_register(GLua, "spriteAnimPause",     sep_sprite_pause);
    // lua_register(GLua, "spriteAnimPlay",      sep_sprite_play);
    // lua_register(GLua, "spriteSetAnimFrame",  sep_sprite_set_frame);


    printf("[5] Setting constants...\n");
    // // Set constants
    // lua_pushinteger(GLua, 0); lua_setglobal(GLua, "flow_VLDPStart");
    // lua_pushinteger(GLua, 1); lua_setglobal(GLua, "flow_GameInit");
    // lua_pushinteger(GLua, 2); lua_setglobal(GLua, "flow_GameRunning");

    // // Initialize gameflow to start at VLDP init:
    // lua_pushinteger(GLua, 0); lua_setglobal(GLua, "gameflow");
    // lua_pushinteger(GLua, 0); lua_setglobal(GLua, "bDebug");
    
    lua_pushinteger(GLua, SWITCH_UP); lua_setglobal(GLua, "SWITCH_UP");
    lua_pushinteger(GLua, SWITCH_DOWN); lua_setglobal(GLua, "SWITCH_DOWN");
    lua_pushinteger(GLua, SWITCH_LEFT); lua_setglobal(GLua, "SWITCH_LEFT");
    lua_pushinteger(GLua, SWITCH_RIGHT); lua_setglobal(GLua, "SWITCH_RIGHT");
    lua_pushinteger(GLua, SWITCH_BUTTON1); lua_setglobal(GLua, "SWITCH_BUTTON1");
    lua_pushinteger(GLua, SWITCH_BUTTON2); lua_setglobal(GLua, "SWITCH_BUTTON2");
    lua_pushinteger(GLua, SWITCH_BUTTON3); lua_setglobal(GLua, "SWITCH_BUTTON3");
    lua_pushinteger(GLua, SWITCH_START1); lua_setglobal(GLua, "SWITCH_START1");
    lua_pushinteger(GLua, SWITCH_START2); lua_setglobal(GLua, "SWITCH_START2");
    lua_pushinteger(GLua, SWITCH_COIN1); lua_setglobal(GLua, "SWITCH_COIN1");
    lua_pushinteger(GLua, SWITCH_COIN2); lua_setglobal(GLua, "SWITCH_COIN2");
    lua_pushinteger(GLua, SWITCH_SERVICE); lua_setglobal(GLua, "SWITCH_SERVICE");
    lua_pushinteger(GLua, SWITCH_PAUSE); lua_setglobal(GLua, "SWITCH_PAUSE");
    
    printf("[6] Loading main script...\n");
    char script_path[256];
    snprintf(script_path, sizeof(script_path), "%s%s%s",
            G_BASE_PATH, G_GAME_DIR, G_SCRIPT_FILE);
    printf("    Script path: %s\n", script_path);
    mutex_lock(&io_lock);
    file_t fd = fs_open(script_path, O_RDONLY);
    mutex_unlock(&io_lock);
    if (fd < 0) {
        printf("PANIC: Failed to open %s\n", script_path);
        arch_exit();
    }
    printf("    ✓ Script file opened\n");

    printf("[7] Creating FileIoUserdata...\n");
    FileIoUserdata ud = { .fd = fd };
    printf("    ✓ FileIoUserdata created\n");
    
    printf("[8] Loading Lua script...\n");

    int rc = lua_load(GLua, lua_reader, &ud, G_CHUNK_NAME, NULL);
            mutex_lock(&io_lock);
    fs_close(fd);
    mutex_unlock(&io_lock);
    if (rc != 0) {
        printf("Error loading script: %s\n", lua_tostring(GLua, -1));
        exit(1);
    }
    printf("    ✓ Lua script loaded\n");

    // printf("[8.5] Injecting debug hook patch...\n");
    // const char *debug_hook_patch = 
    //     "print('Installing debug hook...')\n"
    //     "do\n"
    //     "    -- Set a hook that runs very frequently\n"
    //     "    debug.sethook(function(event, line)\n"
    //     "        -- Continuously force bShowLCD to false\n"
    //     "        bShowLCD = false\n"
    //     "    end, '', 100)  -- Run every 100 instructions\n"
    //     "    \n"
    //     "    -- Also set it false initially\n"
    //     "    bShowLCD = false\n"
    //     "end\n";

    // if (luaL_dostring(GLua, debug_hook_patch) != 0) {
    //     printf("Error injecting debug hook: %s\n", lua_tostring(GLua, -1));
    //     lua_pop(GLua, 1);
    // } else {
    //     printf("    ✓ Debug hook installed\n");
    // }
    printf("[8.6] Injecting full math.random Lua 5.3 compatibility patch...\n");
    const char *random_fix_patch =
        "print('Patching math.random to restore Lua 5.3 behavior...')\n"
        "local old_random = math.random\n"
        "local old_randomseed = math.randomseed\n"
        "\n"
        "-- Store original functions before patching\n"
        "math._random_54 = old_random\n"
        "math._randomseed_54 = old_randomseed\n"
        "\n"
        "-- Patch math.randomseed to handle float inputs\n"
        "math.randomseed = function(x)\n"
        "    if x == nil then\n"
        "        x = os.time()\n"
        "    end\n"
        "    if type(x) == 'number' then\n"
        "        x = math.floor(x)\n"
        "    end\n"
        "    return old_randomseed(x)\n"
        "end\n"
        "\n"
        "-- Patch math.random to fully emulate Lua 5.3 behavior\n"
        "math.random = function(a, b)\n"
        "    if a == nil and b == nil then\n"
        "        -- math.random() - in 5.3 this was integer 1-2^31, but we'll return 1-100000 as common fallback\n"
        "        return old_random(1, 100000)\n"
        "    elseif b == nil then\n"
        "        -- math.random(n) - in 5.3 this was integer 1-n\n"
        "        local n = a\n"
        "        if type(n) == 'number' then\n"
        "            n = math.floor(n)\n"
        "            if n < 1 then n = 1 end\n"
        "        end\n"
        "        return old_random(1, n)\n"
        "    else\n"
        "        -- math.random(m, n) - in 5.3 this was integer m-n  \n"
        "        local m, n = a, b\n"
        "        if type(m) == 'number' then m = math.floor(m) end\n"
        "        if type(n) == 'number' then n = math.floor(n) end\n"
        "        return old_random(m, n)\n"
        "    end\n"
        "end\n"
        "\n"
        "print('math.random patch applied - Lua 5.3 compatibility restored')\n";

    if (luaL_dostring(GLua, random_fix_patch) != 0) {
        printf("Error injecting random/randomseed fix: %s\n", lua_tostring(GLua, -1));
        lua_pop(GLua, 1);
    } else {
        printf("    ✓ randomseed fix installed\n");
    }
    snd_mem_init(256000);  // 256 KB sound memory pool
    printf("[9] Executing script...\n");
    if (lua_pcall(GLua, 0, 0, 0) != 0) {
        printf("Error executing script: %s\n", lua_tostring(GLua, -1));
        exit(1);
    }

    printf("    ✓ Script executed successfully\n");
    // lua_pushinteger(GLua, 0); lua_setglobal(GLua, "bDebug");
    printf("=== setup_lua() COMPLETE ===\n");
}

// #define DEBUG_INPUT_LOG 1

// // --- Global variables for config ---
// char G_BASE_PATH[256]  = "";
// char G_GAME_DIR[256]   = "";
// char G_VIDEO_FILE[256] = "";
// char G_SCRIPT_FILE[256]= "";
// char G_CHUNK_NAME[256] = "";
// char G_GAME_NAME[256]  = "";

// --- Default button mappings (safe defaults) ---
int MAP_A       = SWITCH_BUTTON1;
int MAP_B       = SWITCH_COIN1;
int MAP_X       = SWITCH_BUTTON3;
int MAP_Y       = SWITCH_BUTTON2;
int MAP_LTRIG   = SWITCH_BUTTON3;
int MAP_RTRIG   = SWITCH_BUTTON1;
int MAP_START   = SWITCH_START1;

// --- Default button mappings (safe defaults) ---
int MAP2_A       = SWITCH_BUTTON1;
int MAP2_B       = SWITCH_BUTTON2;
int MAP2_X       = SWITCH_BUTTON3;
int MAP2_Y       = SWITCH_COIN2;
int MAP2_LTRIG   = SWITCH_BUTTON2;
int MAP2_RTRIG   = SWITCH_BUTTON3;
int MAP2_START   = SWITCH_START2;

static int parse_button(const char *name) {
    if (!strcasecmp(name, "BUTTON1"))  return SWITCH_BUTTON1;
    if (!strcasecmp(name, "BUTTON2"))  return SWITCH_BUTTON2;
    if (!strcasecmp(name, "BUTTON3"))  return SWITCH_BUTTON3;

    if (!strcasecmp(name, "COIN1"))   return SWITCH_COIN1;
    if (!strcasecmp(name, "COIN2"))   return SWITCH_COIN2;
    if (!strcasecmp(name, "START1"))  return SWITCH_START1;
    if (!strcasecmp(name, "START2"))  return SWITCH_START2;

    if (!strcasecmp(name, "UP"))      return SWITCH_UP;
    if (!strcasecmp(name, "DOWN"))    return SWITCH_DOWN;
    if (!strcasecmp(name, "LEFT"))    return SWITCH_LEFT;
    if (!strcasecmp(name, "RIGHT"))   return SWITCH_RIGHT;
    if (!strcasecmp(name, "START"))   return SWITCH_START1;

    return SWITCH_BUTTON1;
}

void singe_tick(uint64_t monotonic_ms) {
    // --- Draw FMV and overlay ---
    pvr_scene_begin();

    // 1️⃣ Draw FMV first (opaque list)
    pvr_list_begin(PVR_LIST_OP_POLY);
    render_current_video();   // your FMV frame
    pvr_list_finish();

    // 2️⃣ Draw overlay next, on top (translucent list)
    pvr_list_begin(PVR_LIST_TR_POLY);

    // Force overlay z-depth to front (closer to camera)
    // We'll set z=0.0001f in each vertex in sep_overlay_*()
    lua_getglobal(GLua, "onOverlayUpdate");
    if (lua_isfunction(GLua, -1)) {
        if (lua_pcall(GLua, 0, 1, 0) != 0) {
            printf("Lua error in onOverlayUpdate: %s\n", lua_tostring(GLua, -1));
            lua_pop(GLua, 1);
        } else {
            lua_pop(GLua, 1);
            g_overlay_ran_once = 1;
        }
    } else {
        lua_pop(GLua, 1);
    }

    pvr_list_finish();

    pvr_scene_finish();

    // 3️⃣ Update FMV logic (tick after drawing)
    fmv_tick(monotonic_ms);
}


// Initialization
void singe_startup(const char *gamedir, const char *videopath) {
    GGameDir = Singe_xstrdup(gamedir);
    GGamePath = Singe_xstrdup(videopath);
    
    atomic_store(&audio_muted, 1); 
    preload_paused =1;

    
    // Open video file
    video_fd = fs_open(videopath, O_RDONLY);
    if (video_fd < 0) {
        printf("PANIC: Failed to open video file\n");
        exit(1);
    }
    
    // Read header (same as Singe)
    char magic[4];
    fs_read(video_fd, magic, 4);
    if (memcmp(magic, DCMV_MAGIC, 4) != 0) {
        printf("PANIC: Bad DCMV magic\n");
        exit(1);
    }
    
    uint32_t version;
    fs_read(video_fd, &version, 4);
    
    fs_read(video_fd, &frame_type, 1);
    fs_read(video_fd, &video_width, 2);
    fs_read(video_fd, &video_height, 2);
    fs_read(video_fd, &content_width, 2);
    fs_read(video_fd, &content_height, 2);
    fs_read(video_fd, &fps, sizeof(float));
    fs_read(video_fd, &sample_rate, 2);
    fs_read(video_fd, &audio_channels, 2);
    fs_read(video_fd, &num_unique_frames, 4);
    fs_read(video_fd, &num_total_frames, 4);
    fs_read(video_fd, &video_frame_size, 4);
    fs_read(video_fd, &max_compressed_size, 4);
    fs_read(video_fd, &audio_offset, 4);

    uint8_t compression_type = 0;
    fs_read(video_fd, &compression_type, 1);
    const char *compression_str = (compression_type == 1) ? "Zstandard" : "LZ4";
    use_zstd = (compression_type == 1);
    
        printf("📦 Header v%lu: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d\n",
        (unsigned long)version,
        frame_type == 1 ? "YUV422" : "RGB565",
        video_width, video_height, content_width, content_height,
        fps, sample_rate, audio_channels,
        num_unique_frames, num_total_frames);

    printf("   Frame size: %d, Max compressed: %d, Audio offset: 0x%X, Compression: %s\n",
        video_frame_size, max_compressed_size, audio_offset, compression_str);
        
    init_timebase_from_fps(fps);
    frame_duration = 1000.0f / fps;
    
    // Allocate buffers
    if (use_zstd) {
        g_zstd_dctx = ZSTD_createDCtx();
        ZSTD_DCtx_setParameter(g_zstd_dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
    }
    
    compressed_buffer = memalign(32, max_compressed_size);
    
    // Load frame tables
    fs_seek(video_fd, 50, SEEK_SET);
    
    frame_offsets = Singe_xmalloc((num_unique_frames + 1) * sizeof(uint32_t));
    frame_durations = Singe_xmalloc(num_unique_frames * sizeof(uint16_t));
    
    fs_read(video_fd, frame_offsets, (num_unique_frames + 1) * sizeof(uint32_t));
    fs_read(video_fd, frame_durations, num_unique_frames * sizeof(uint16_t));
    
    // Build total-to-unique mapping
    GTotalToUnique = Singe_xmalloc(num_total_frames * sizeof(int));
    int t = 0;
    for (int u = 0; u < num_unique_frames; u++) {
        for (int i = 0; i < frame_durations[u] && t < num_total_frames; i++) {
            GTotalToUnique[t++] = u;
        }
    }
    
    // Calculate audio size
    long curpos = fs_tell(video_fd);
    fs_seek(video_fd, 0, SEEK_END);
    long total_size = fs_tell(video_fd);
    fs_seek(video_fd, curpos, SEEK_SET);
    
    long audio_bytes_total = total_size - audio_offset;
    left_channel_size = (audio_channels == 2) ? (audio_bytes_total / 2) : audio_bytes_total;
    
    // Open audio streams
    audio_fd_left = fs_open(videopath, O_RDONLY);
    fs_seek(audio_fd_left, audio_offset, SEEK_SET);
    
    if (audio_channels == 2) {
        audio_fd_right = fs_open(videopath, O_RDONLY);
        fs_seek(audio_fd_right, audio_offset + left_channel_size, SEEK_SET);
    }
    
    // Initialize video/audio
    is_320 = 0;//(video_width == 320);
    vid_set_mode(DM_640x480_VGA, PM_RGB565);
    
    g_display_w = 640;
    g_display_h = 480;
    
    // Scale 640x480 overlay to current display size (320x240 or 640x480)
    UI_SCALE_X = 1.0f;
    UI_SCALE_Y = 1.0f;
    UI_OFFSET_X = 0;
    UI_OFFSET_Y = 0;
    
    for (int i = 0; i < NUM_BUFFERS; i++) {
        frame_buffer[i] = memalign(32, video_frame_size);
        atomic_store(&buf_state[i], BUF_EMPTY);
    }
    // printf("   Allocated %d buffers of %d bytes each\n", NUM_BUFFERS, video_frame_size);
    // Initialize PVR
    pvr_init_defaults();
    
    int use_strided = (!is_pow2(video_width) || !is_pow2(video_height));
    int pot_w = 1, pot_h = 1;
    while (pot_w < video_width) pot_w <<= 1;
    while (pot_h < video_height) pot_h <<= 1;
    
    pvr_txr = pvr_mem_malloc(pot_w * pot_h * 2);
    
    pvr_poly_cxt_t cxt;
    uint32_t fmt = (frame_type == 1) ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE;
    if (use_strided) fmt |= PVR_TXRFMT_NONTWIDDLED | (1 << 25) | PVR_TXRFMT_VQ_ENABLE;
    else fmt |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
    
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt, pot_w, pot_h, pvr_txr, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr, &cxt);
    // hdr.mode3 &= ~(0x3f<<21);
    // if (use_strided) pvr_txr_set_stride(video_width);
    if (use_strided) PVR_SET(PVR_TEXTURE_MODULO, (video_width / 32));
    
    float u1 = (float)content_width / (float)pot_w;
    float v1 = (float)content_height / (float)pot_h;
    
    vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xFFFFFFFF};
    vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=g_display_w, .y=0, .z=1, .u=u1, .v=0, .argb=0xFFFFFFFF};
    vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=g_display_h, .z=1, .u=0, .v=v1, .argb=0xFFFFFFFF};
    vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL, .x=g_display_w, .y=g_display_h, .z=1, .u=u1, .v=v1, .argb=0xFFFFFFFF};
    
    sep_music_init();
    // Initialize audio
    snd_stream_init_ex(audio_channels, soundbufferalloc);
    stream = snd_stream_alloc(NULL, soundbufferalloc);
    snd_stream_set_callback_direct(stream, audio_cb);
    snd_stream_start_adpcm(stream, sample_rate, audio_channels == 2 ? 1 : 0);
    atomic_store(&audio_muted, 1);
    worker_thread_id = thd_create(0, worker_thread, NULL);


    // ✅ Initialize timing but don't start clocks
    frame_timer_anchor = 0.0;  // Will be set when playback actually starts
    atomic_store(&audio_start_time_ms, 0.0);
    atomic_store(&audio_muted, 1);
        
   
    // // Pre-load initial frames
    // for (int uf = 0; uf < MIN(4, num_unique_frames); uf++) {
    //     int buf = uf % NUM_BUFFERS;
    //     atomic_store(&buf_state[buf], BUF_LOADING);
    //     if (load_frame(uf, buf) == 0)
    //         atomic_store(&buf_state[buf], BUF_READY);
    //     else
    //         atomic_store(&buf_state[buf], BUF_EMPTY);
    // }

    last_unique_frame_drawn = 0;

    // GDecoderActive = 1;


    // Setup Lua
    setup_lua();
    Singe_log("Singe startup complete - %d total frames at %.2f fps", num_total_frames, fps);
    // int retries = 0;
    // while (atomic_load(&frame_index) == 0 && retries < 50) {  // ~1 second wait
    //     thd_sleep(20);
    //     retries++;
    // }

    printf("   Decoder thread started\n");
    g_is_paused = 1;
    preload_paused = 1;
    atomic_store(&audio_muted, 1);       
    Singe_log("Initial frame ready, starting playback...");


}

// ---------------------------------------------------------------------------
// Load singe.cfg (tries /pc/data first, then /cd/data)
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Load singe.cfg (tries /pc/data first, then /cd/data)
// ---------------------------------------------------------------------------
static void load_config(void) {
    file_t fd = fs_open("/pc/data/singe.cfg", O_RDONLY);
    const char *base_try = "/pc/data/";
    if (fd < 0) {
        fd = fs_open("/cd/data/singe.cfg", O_RDONLY);
        base_try = "/cd/data/";
    }

    strncpy(G_BASE_PATH, base_try, sizeof(G_BASE_PATH));
    G_BASE_PATH[sizeof(G_BASE_PATH) - 1] = '\0';

    if (fd < 0) {
        printf("⚠️ singe.cfg not found on either /pc/data or /cd/data. Using defaults.\n");
        return;
    }

    printf("📄 Reading singe.cfg from %s\n", base_try);
    char line[256];
    int pos = 0;
    char c;
    while (fs_read(fd, &c, 1) == 1) {
        if (c == '\r') continue;
        if (c == '\n' || pos >= sizeof(line) - 1) {
            line[pos] = '\0';
            pos = 0;

            if (line[0] == '#' || line[0] == '\0') continue;

            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq++ = '\0';

            while (*eq == ' ' || *eq == '\t') eq++;
            while (line[strlen(line) - 1] == ' ') line[strlen(line) - 1] = '\0';

            if (strcmp(line, "game_dir") == 0)
                strncpy(G_GAME_DIR, eq, sizeof(G_GAME_DIR));
            else if (strcmp(line, "game_name") == 0)
                strncpy(G_GAME_NAME, eq, sizeof(G_GAME_NAME));
            else if (strcmp(line, "video_file") == 0)
                strncpy(G_VIDEO_FILE, eq, sizeof(G_VIDEO_FILE));
            else if (strcmp(line, "script_file") == 0)
                strncpy(G_SCRIPT_FILE, eq, sizeof(G_SCRIPT_FILE));
            else if (strcmp(line, "chunk_name") == 0)
                strncpy(G_CHUNK_NAME, eq, sizeof(G_CHUNK_NAME));
            else if (strcmp(line, "btn_a") == 0)
                MAP_A = parse_button(eq);
            else if (strcmp(line, "btn_b") == 0)
                MAP_B = parse_button(eq);
            else if (strcmp(line, "btn_x") == 0)
                MAP_X = parse_button(eq);
            else if (strcmp(line, "btn_y") == 0)
                MAP_Y = parse_button(eq);
            else if (strcmp(line, "btn_ltrigger") == 0)
                MAP_LTRIG = parse_button(eq);
            else if (strcmp(line, "btn_rtrigger") == 0)
                MAP_RTRIG = parse_button(eq);
            else if (strcmp(line, "btn_start") == 0)
                MAP_START = parse_button(eq);
            else if (strcmp(line, "btn2_a") == 0)
                MAP2_A = parse_button(eq);
            else if (strcmp(line, "btn2_b") == 0)
                MAP2_B = parse_button(eq);
            else if (strcmp(line, "btn2_x") == 0)
                MAP2_X = parse_button(eq);
            else if (strcmp(line, "btn2_y") == 0)
                MAP2_Y = parse_button(eq);
            else if (strcmp(line, "btn2_ltrigger") == 0)
                MAP2_LTRIG = parse_button(eq);
            else if (strcmp(line, "btn2_rtrigger") == 0)
                MAP2_RTRIG = parse_button(eq);
            else if (strcmp(line, "btn2_start") == 0)
                MAP2_START = parse_button(eq);
        } else {
            line[pos++] = c;
        }
    }
    fs_close(fd);

// -----------------------------------------------------------------------
    // Decide working directory based on script_file (Classic vs Hypseus)
    // -----------------------------------------------------------------------
    if (strlen(G_SCRIPT_FILE) > 0) {
        char lower_script[256];
        strncpy(lower_script, G_SCRIPT_FILE, sizeof(lower_script));
        lower_script[sizeof(lower_script) - 1] = '\0';

        // Convert to lowercase for case-insensitive search
        for (char *p = lower_script; *p; ++p)
            *p = tolower((unsigned char)*p);

        if (strstr(lower_script, "singe/")) {
            // Hypseus Singe 2.x layout
            fs_chdir(G_BASE_PATH);
            fs_chdir(G_GAME_DIR);
            printf("📂 Detected Hypseus-style game. Working dir: %s%s\n",
                G_BASE_PATH, G_GAME_DIR);
        } else {
            // Classic Singe 1.x layout or other
            fs_chdir(G_BASE_PATH);
            printf("📂 Working dir: %s\n", G_BASE_PATH);
        }
    }

    // -----------------------------------------------------------------------
    // Debug printout
    // -----------------------------------------------------------------------
    printf("✅ Loaded config:\n");
    printf("  Base:   %s\n", G_BASE_PATH);
    printf("  Dir:    %s\n", G_GAME_DIR);
    printf("  Video:  %s\n", G_VIDEO_FILE);
    printf("  Script: %s\n", G_SCRIPT_FILE);
    printf("  Chunk:  %s\n", G_CHUNK_NAME);
    printf("  Name:   %s\n", G_GAME_NAME);
    printf("  Mappings (Player 1):\n");
    printf("    A -> %d\n", MAP_A);
    printf("    B -> %d\n", MAP_B);
    printf("    X -> %d\n", MAP_X);
    printf("    Y -> %d\n", MAP_Y);
    printf("    L -> %d\n", MAP_LTRIG);
    printf("    R -> %d\n", MAP_RTRIG);
    printf("  START -> %d\n", MAP_START);
    printf("  Mappings (Player 2):\n");
    printf("    A -> %d\n", MAP2_A);
    printf("    B -> %d\n", MAP2_B);
    printf("    X -> %d\n", MAP2_X);
    printf("    Y -> %d\n", MAP2_Y);
    printf("    L -> %d\n", MAP2_LTRIG);
    printf("    R -> %d\n", MAP2_RTRIG);
    printf("  START -> %d\n", MAP2_START);
}



static void poll_and_handle_input(void) {
    static uint64_t prevbits[2] = {0, 0};    // Previous state for both players
    static float mouse_vx[2] = {0.0f, 0.0f};  // Mouse X velocity per player
    static float mouse_vy[2] = {0.0f, 0.0f};  // Mouse Y velocity per player
    static int prev_joyx[2] = {0, 0};         // Previous analog X for both players
    static int prev_joyy[2] = {0, 0};         // Previous analog Y for both players
    const int PLAYER2_OFFSET = 32;    // Offset for Player 2 input

    for (int port = 0; port < 2; port++) {
        maple_device_t *dev = maple_enum_type(port, MAPLE_FUNC_CONTROLLER);
        if (!dev || !dev->status_valid)
            continue;

        cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
        if (!state)
            continue;

        uint64_t curbits = 0;  // Clear curbits for each player
        int buttons = state->buttons;

        // --- Read from config mappings (per-player) ---
        if (port == 0) {  // Player 1
            if (buttons & CONT_START)  curbits |= (1ULL << MAP_START);
            if (buttons & CONT_A)    {  curbits |= (1ULL << MAP_A); }//printf("player 1 A, curbits: 0x%llx\n", curbits);}
            if (buttons & CONT_B)    {  curbits |= (1ULL << MAP_B); vid_screen_shot("/pc/screenshot.ppm");}
            if (buttons & CONT_X)      curbits |= (1ULL << MAP_X);
            if (buttons & CONT_Y)      curbits |= (1ULL << MAP_Y);
            if (state->ltrig > 32)     curbits |= (1ULL << MAP_LTRIG);
            if (state->rtrig > 32)     curbits |= (1ULL << MAP_RTRIG);
        } else if (port == 1) {  // Player 2
            if (buttons & CONT_START)  curbits |= (1ULL << (MAP2_START + PLAYER2_OFFSET));
            if (buttons & CONT_A)     { curbits |= (1ULL << (MAP2_A + PLAYER2_OFFSET)); }//printf("player 2 A, curbits: 0x%llx\n", curbits);}
            if (buttons & CONT_B)      curbits |= (1ULL << (MAP2_B + PLAYER2_OFFSET));
            if (buttons & CONT_X)      curbits |= (1ULL << (MAP2_X + PLAYER2_OFFSET));
            if (buttons & CONT_Y)      curbits |= (1ULL << (MAP2_Y + PLAYER2_OFFSET));
            if (state->ltrig > 32)     curbits |= (1ULL << (MAP2_LTRIG + PLAYER2_OFFSET));
            if (state->rtrig > 32)     curbits |= (1ULL << (MAP2_RTRIG + PLAYER2_OFFSET));
        }

        // --- D-pad ---
        if (port == 0) {  // Player 1
            if (buttons & CONT_DPAD_UP)    curbits |= (1ULL << SWITCH_UP);
            if (buttons & CONT_DPAD_DOWN)  curbits |= (1ULL << SWITCH_DOWN);
            if (buttons & CONT_DPAD_LEFT)  curbits |= (1ULL << SWITCH_LEFT);
            if (buttons & CONT_DPAD_RIGHT) curbits |= (1ULL << SWITCH_RIGHT);
        } else if (port == 1) {  // Player 2
            if (buttons & CONT_DPAD_UP)    curbits |= (1ULL << (SWITCH_UP + PLAYER2_OFFSET));
            if (buttons & CONT_DPAD_DOWN)  curbits |= (1ULL << (SWITCH_DOWN + PLAYER2_OFFSET));
            if (buttons & CONT_DPAD_LEFT)  curbits |= (1ULL << (SWITCH_LEFT + PLAYER2_OFFSET));
            if (buttons & CONT_DPAD_RIGHT) curbits |= (1ULL << (SWITCH_RIGHT + PLAYER2_OFFSET));
        }

        // --- Detect changed bits and call Lua ---
        uint64_t changed = curbits ^ prevbits[port];  // Detect the changes
        if (changed) {
            while (changed) {
                int switch_num = __builtin_ctzll(changed);  // Find the first set bit
                uint64_t flag = 1ULL << switch_num;
                bool pressed = (curbits & flag);

                // Adjust for Player 2 by subtracting PLAYER2_OFFSET
                if (switch_num >= PLAYER2_OFFSET) {
                    switch_num -= PLAYER2_OFFSET;
                }

                const char *event = pressed ? "onInputPressed" : "onInputReleased";
                lua_getglobal(GLua, event);
                if (lua_isfunction(GLua, -1)) {
                    DC_log("DEBUG: Sending event '%s' for Player %d, switch_num %d\n", event, port + 1, switch_num);  // Debugging line
                    lua_pushinteger(GLua, switch_num);  // Corrected switch_num
                    lua_pushinteger(GLua, port);    // Player ID
                    if (lua_pcall(GLua, 2, 0, 0) != 0) {
                        printf("Lua error in %s: %s\n", event, lua_tostring(GLua, -1));
                        lua_pop(GLua, 1);
                    }
                } else lua_pop(GLua, 1);
                changed &= ~flag;  // Clear the processed bit
            }
        }

            // --- Virtual mouse (analog stick emulation per player) ---
            int lx = state->joyx;
            int ly = state->joyy;

            if (lx != prev_joyx[port] || ly != prev_joyy[port]) {
                prev_joyx[port] = lx;
                prev_joyy[port] = ly;

                const float deadzone = 15.0f;
                float nx = (fabsf(lx) < deadzone) ? 0.0f : lx / 128.0f;
                float ny = (fabsf(ly) < deadzone) ? 0.0f : ly / 128.0f;

                const float response = 1.5f;
                nx = copysignf(powf(fabsf(nx), response), nx);
                ny = copysignf(powf(fabsf(ny), response), ny);

                const float smooth = 0.3f;
                mouse_vx[port] = mouse_vx[port] * (1.0f - smooth) + nx * smooth;
                mouse_vy[port] = mouse_vy[port] * (1.0f - smooth) + ny * smooth;

                const float speed = 14.0f;
                int relX = (int)(mouse_vx[port] * speed);
                int relY = (int)(mouse_vy[port] * speed);

  if (relX || relY) {
        static int GMouseX[2] = {320, 320};
        static int GMouseY[2] = {240, 240};

        GMouseX[port] += relX;
        GMouseY[port] += relY;

        // Constrain the mouse position within the screen boundaries
        if (GMouseX[port] < 0) GMouseX[port] = 0;
        else if (GMouseX[port] > g_display_w) GMouseX[port] = g_display_w;
        if (GMouseY[port] < 0) GMouseY[port] = 0;
        else if (GMouseY[port] > g_display_h) GMouseY[port] = g_display_h;

        // Apply proper offset and scaling
        float adjusted_x = GMouseX[port] - g_ratio_x_offset;
        float adjusted_y = GMouseY[port] - g_ratio_y_offset;

        // Correct the scaling for both axes
        int scaled_x = (int)(adjusted_x * g_scale_x);
        int scaled_y = (int)(adjusted_y * g_scale_y);

        // Send raw screen coordinates to Lua
        int relScaledX = scaled_x;
        int relScaledY = scaled_y;

        Singe_log("[MOUSE] Screen:(%d,%d) "
            "offset=(%.1f,%.1f) scale=(%.2f,%.2f)\n",
            scaled_x, scaled_y,
            g_ratio_x_offset, g_ratio_y_offset,
            g_scale_x, g_scale_y);

        lua_getglobal(GLua, "onMouseMoved");
        if (lua_isfunction(GLua, -1)) {
            lua_pushinteger(GLua, scaled_x);  // Send adjusted screen coords
            lua_pushinteger(GLua, scaled_y);
            lua_pushinteger(GLua, relScaledX);
            lua_pushinteger(GLua, relScaledY);
            lua_pushinteger(GLua, port);
            if (lua_pcall(GLua, 5, 0, 0) != 0) {
                printf("Lua error in onMouseMoved: %s\n", lua_tostring(GLua, -1));
                lua_pop(GLua, 1);
            }
        } else lua_pop(GLua, 1);
    }          }
    
            
            prevbits[port] = curbits;
        }
    }




// Main function
int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("Singe 2 for Dreamcast\n");

    cont_btn_callback(0,
        CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y,
        (cont_btn_callback_t)arch_exit);

    load_config();

    // Adjust the layout detection logic for both `/pc` and `/cd` environments

    // Hypseus Test (CDI / CD Setup)
    // char hypseus_test[512];
    // snprintf(hypseus_test, sizeof(hypseus_test),
    //         "%s%s/singe/%s", G_BASE_PATH, G_GAME_DIR, G_GAME_DIR);

    // char classic_test[512];
    // snprintf(classic_test, sizeof(classic_test),
    //         "%s%s/Script", G_BASE_PATH, G_GAME_DIR);  // classic games always have /Script/

    // // Check for Hypseus layout (as before)
    // file_t testdir = fs_open(hypseus_test, O_RDONLY);
    // bool is_hypseus = (testdir >= 0);
    // if (testdir >= 0) fs_close(testdir);

    // // Check for Classic layout (also as before)
    // file_t testdir2 = fs_open(classic_test, O_RDONLY);
    // bool is_classic = (testdir2 >= 0);
    // if (testdir2 >= 0) fs_close(testdir2);

    // char full_path[512];
    //     snprintf(full_path, sizeof(full_path), "%s%s", G_BASE_PATH, G_GAME_DIR);
    //     printf("[Singe] Detected Hypseus layout: %s\n", full_path);

// if (is_hypseus) {
//         snprintf(full_path, sizeof(full_path), "%s%s", G_BASE_PATH, G_GAME_DIR);
//         printf("[Singe] Detected Hypseus layout: %s\n", full_path);
//     }
//     else if (is_classic) {
//         snprintf(full_path, sizeof(full_path), "%s", G_BASE_PATH);
//         printf("[Singe] Detected Classic layout: %s\n", full_path);
//     }
//     else {
//         snprintf(full_path, sizeof(full_path), "%s", G_BASE_PATH);
//         printf("[Singe] ⚠️ Unknown layout, defaulting to base path: %s\n", full_path);
//     }


    // Log the working directory we're setting
    // printf("[Singe] Setting working directory to: %s\n", full_path);

    // // Set working directory
    // if (fs_chdir(full_path) < 0) {
    //     printf("[Singe] ⚠️ Warning: Failed to chdir to %s\n", full_path);
    // } else {
    //     printf("[Singe] ✓ Current working dir set to %s\n", full_path);
    // }


    char game_dir[256], video_path[256];
    snprintf(game_dir, sizeof(game_dir), "%s%s", G_BASE_PATH, G_GAME_DIR);
    snprintf(video_path, sizeof(video_path), "%s%s", G_BASE_PATH, G_VIDEO_FILE);

    printf("Starting %s...\n", G_GAME_NAME);
    singe_startup(game_dir, video_path);

    printf("Singe initialized, entering main loop...\n");

    // --- Initialize timing anchors for first playback ---
    // frame_timer_anchor = psTimer();
    // atomic_store(&audio_start_time_ms,
    //             (double)atomic_load(&frame_index) * (1000.0 / (double)fps));
    // Singe_log("[Sync] Initialized frame_timer_anchor=%.3fms, audio_start_time_ms=%.3f",
    //         (double)psTimer(), atomic_load(&audio_start_time_ms));    
    // dbgio_dev_select("fb");
    while (1) {
        uint64_t now_ms = (uint64_t)psTimer();
        // uint64_t inputbits = poll_controller_input();
        // singe_tick(now_ms, inputbits);
        poll_and_handle_input(); 
        singe_tick(now_ms);
        thd_sleep(16);
    }

    return 0;
}

