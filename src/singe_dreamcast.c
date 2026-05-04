
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
#include <ctype.h>
#include <malloc.h>
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
#include <dc/vmu_fb.h>
#include <arch/gdb.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>
#include "dcfmv.h"

#ifndef SINGE_DEBUG_LOGS
#define SINGE_DEBUG_LOGS 0
#endif

#define USE_50HZ 0
#define USE_60HZ 1
#define USE_IO_MUTEX 1  
static mutex_t io_lock = MUTEX_INITIALIZER;

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

#define PREFETCH_AHEAD (MIN(DCFMV_NUM_BUFFERS, 8))

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
static char G_VMU_ICON_FILE[128] = "resources/dcsinge_vmu_icon.ico";
static int g_cfg_disable_fmv_audio = 0;
static int g_cfg_enable_mp3 = 0;
static int g_mp3_stream_inited = 0;
static atomic_int g_exit_requested = 0;
static int g_vmu_ready = 0;
static int g_vmu_available = 0;
static atomic_int g_vmu_flush_pending = 0;
static atomic_int g_vmu_flush_defer_until_frame = -1;
static char *g_vmu_lcd_icon = NULL;
static char g_vmu_mount_path[16] = "";
static char g_vmu_save_name[16] = "";
static char g_vmu_save_path[64] = "";
static char g_vmu_icon_path[256] = "";
static vmu_pkg_t g_vmu_pkg;
static uint8_t g_vmu_icon_data[1024];
static uint64_t GPreviousInputBits = 0;
static int GMouseX = 0;
static int GMouseY = 0;
static int GMouseRelX = 0;
static int GMouseRelY = 0;
// static int GHalted = 0;
// static int GShowingSingleFrame = 0;
static uint64_t GClipStartTicks = 0;
static uint64_t GTicks = 0;
static uint64_t GTicksOffset = 0;
static int GDecoderActive = 0;
static int g_overlay_ran_once = 0;
static int g_startup_intro_drawn = 0;
static int g_disc_skip_count = 0;
static int g_display_w = 0, g_display_h = 0;
static int g_offset_x = 0, g_offset_y = 0;
static int g_iFrameEnd = -1;
static atomic_int g_clip_boundary_hold = 0;

static void request_exit_callback(void) {
    atomic_store(&g_exit_requested, 1);
}

static void clear_io_cache(void);
static int load_vmu_lcd_icon(void);
static void update_vmu_lcd(void);
static int init_vmu_context(void);
static int persist_vmu_archive_locked(void);
static void flush_vmu_archive_if_pending(void);
static int seed_vmu_archive_locked(void);
static void log_memory_stats(const char *tag);

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

/*
 * Dreamcast-side font compensation:
 * many Singe scripts were authored for larger desktop render targets, so the
 * raw requested sizes render too small on hardware unless we scale them up.
 */
#define SINGE_FONT_SCALE_NUM 3
#define SINGE_FONT_SCALE_DEN 2
#define SINGE_FONT_BIAS_PX 4
#define SINGE_FONT_MIN_PX 8

typedef struct LoadedFont LoadedFont;

typedef struct FontManager {
    LoadedFont *fonts;     // Array to store multiple fonts and their caches
    int font_count;        // Number of fonts loaded
    int current_font_idx;  // Index of the currently selected font
} FontManager;

static FontManager g_font_manager = { NULL, 0, -1 };  // Initialize font manager
static LoadedFont *g_active_loaded_font = NULL;

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
// ============================================================================
// Dreamcast Singe Overlay RTT Implementation (non-twiddled ARGB1555)
// Maintains original Lua overlay coordinates (GOverlayWidth/GOverlayHeight)
// ============================================================================

void DC_log(const char *fmt, ...) {
#if !SINGE_DEBUG_LOGS
    (void)fmt;
    return;
#else
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    printf("[DC] %s\n", buffer);  // Logs for Dreamcast C side
#endif
}


void Singe_log(const char *fmt, ...) {
#if !SINGE_DEBUG_LOGS
    (void)fmt;
    return;
#else
    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    printf("[Singe] %s\n", buffer);
    // dbglog(DBG_INFO, "%s\n\n", buffer);
#endif
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

// FMV render scratch that remains local to the Singe bridge for now.
static pvr_ptr_t pvr_txr = NULL;
static pvr_poly_hdr_t hdr;
static pvr_poly_hdr_t fallback_hdr;
static pvr_vertex_t vert[4];
static pvr_vertex_t fallback_vert[4];


int overlay_tex_w = 0;
int overlay_tex_h = 0;

void font_init_char_cache();

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

    Singe_log("ratio_x=%.3f offset_x=%.2f scale_x=%.3f\n",
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

static inline int total_to_unique_frame(int total_frame) {
    return dcfmv_total_to_unique(dcfmv_current, total_frame);
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

/* audio_cb and stream start/stop are now owned by dcfmv.c (dcfmv_audio_init /
   dcfmv_audio_stop).  The Singe bridge just calls those at the right time. */

// Frame loading
static int load_frame(int total_frame, int buf_index) {
    return dcfmv_load_frame(dcfmv_current, total_frame, buf_index);
}
// --- render_current_video(): always mark forward progress ---
static void render_current_video(void) {
    dcfmv_render_current_video(dcfmv_current);
}


bool schedule_frame_preload(int frame) {
    return dcfmv_schedule_frame_preload(dcfmv_current, frame);
}

bool schedule_frame_preload_with_generation(int frame, int generation) {
    return dcfmv_schedule_frame_preload_with_generation(dcfmv_current, frame, generation);
}



kthread_t *worker_thread_id;

// Worker thread for preloading
// Worker thread for preloading and stream maintenance
void *worker_thread(void *p) {
    (void)p;
    while (1) {
        if (atomic_load(&g_exit_requested)) {
            break;
        }
        dcfmv_worker_step(dcfmv_current);
    }
    return NULL;
}



// --- seek_to_frame(): flush ring + re-prime fresh preload jobs ---
void seek_to_frame(int new_frame) {
    dcfmv_seek_to_frame(dcfmv_current, new_frame);
}



static void fmv_tick(uint64_t now_ms) {
    (void)now_ms;
    dcfmv_tick(dcfmv_current);
}

static void log_memory_stats(const char *tag) {
    struct mallinfo mi = mallinfo();
    size_t pvr_free = pvr_mem_available();

    Singe_log("[Mem] %s heap_used=%d heap_free=%d heap_arena=%d pvr_free=%lu",
              tag ? tag : "stats",
              mi.uordblks,
              mi.fordblks,
              mi.arena,
              (unsigned long)pvr_free);
}


static void pace_main_loop(void) {
    thd_sleep(16);
    // vid_waitvbl();
 
}

//=============================================================================
// SINGE LUA API FUNCTIONS
//=============================================================================

// Disc control functions
// Disc control functions
static int sep_get_current_frame(lua_State *L) {
    int cur = dcfmv_frame_index(dcfmv_current);

    if (g_iFrameEnd > 0 && cur >= g_iFrameEnd) {
        int entering_hold = !atomic_exchange(&g_clip_boundary_hold, 1);

        if (entering_hold) {
            dcfmv_set_seek_settle_frames(dcfmv_current, 0);
            Singe_log("[Singe] clip-boundary settle set to %d frames at iFrameEnd=%d",
                      60, g_iFrameEnd);
        }

        dcfmv_log_state("clip_hold", dcfmv_current);
        dcfmv_set_audio_muted(dcfmv_current, 1);
        dcfmv_audio_stop_stream(dcfmv_current);
        dcfmv_set_paused(dcfmv_current, 1);
        dcfmv_set_preload_paused(dcfmv_current, 1);

        lua_pushinteger(L, g_iFrameEnd);
        return 1;
    }

    lua_pushinteger(L, cur);
    return 1;
}

// Handle seeking to a new frame (skip to the next FMV segment)
static int sep_skip_to_frame(lua_State *L) {
    int frame = (int)luaL_checknumber(L, 1);
    Singe_log("discSkipToFrame(%d)", frame);
    dcfmv_log_state("skip_pre", dcfmv_current);

    // Leaving a clip boundary hold: restart FMV from the requested frame.
    atomic_store(&g_clip_boundary_hold, 0);

    dcfmv_set_audio_muted(dcfmv_current, 1);

    compute_global_ratios();

    lua_getglobal(L, "iFrameEnd");
    lua_getglobal(L, "iFrameStart");

    if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        int newiFrameEnd = (int)lua_tonumber(L, -2);
        int iFrameStart  = (int)lua_tonumber(L, -1);

        if (frame == iFrameStart) {
            if (newiFrameEnd != g_iFrameEnd) {
                Singe_log("Updating g_iFrameEnd from %d to %d (clip start)",
                          g_iFrameEnd, newiFrameEnd);
                g_iFrameEnd = newiFrameEnd;
            }

            Singe_log("iFrameEnd from Lua: %d (clip start)", g_iFrameEnd);
            /* Give the next clip about one second to prime before audio starts. */
            dcfmv_set_seek_settle_frames(dcfmv_current, 0);
            dcfmv_set_paused(dcfmv_current, 1);
            Singe_log("[Singe] clip-start settle set to %d frames", 0);
        } else {
            if (g_iFrameEnd > 0 && (frame < iFrameStart || frame >= g_iFrameEnd)) {
                Singe_log("Skip to %d is outside active clip [%d, %d); clearing clip end",
                          frame, iFrameStart, g_iFrameEnd);
                g_iFrameEnd = -1;
            } else {
                Singe_log("Skip to %d is not clip start (iFrameStart=%d), keeping g_iFrameEnd=%d",
                          frame, iFrameStart, g_iFrameEnd);
            }
            dcfmv_set_seek_settle_frames(dcfmv_current, 0);
        }
    } else {
        if (lua_isnumber(L, -2)) {
            int newiFrameEnd = (int)lua_tonumber(L, -2);
            Singe_log("iFrameEnd found: %d but iFrameStart missing", newiFrameEnd);
        } else {
            Singe_log("iFrameEnd not found or not a number in Lua.");
        }
    }

    lua_pop(L, 2);

    /*
     * Keep FMV fully frozen until the transition bookkeeping is finished,
     * then let the seek path take over. Resuming too early here allows the
     * next scene to wake up while Lua is still inside the transition code.
     */
    dcfmv_set_preload_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_set_paused(dcfmv_current, 0);
    atomic_store(&g_vmu_flush_defer_until_frame, frame);
    Singe_log("[VMU] Flushing pending save at seek start for discSkipToFrame(%d)", frame);
    dcfmv_request_seek(dcfmv_current, frame);

    Singe_log("Skipped to frame %d", frame);
    return 0;
}




static int sep_search(lua_State *L) {
    int frame = (int)luaL_checknumber(L, 1);
    
    Singe_log("[Singe] sep_search/discSearch(%d)\n", frame);
    dcfmv_log_state("search_pre", dcfmv_current);
    dcfmv_set_seek_settle_frames(dcfmv_current, 0);
    /*
     * Match the PC Singe held-search behavior for menu/select screens.
     * Audio is muted and playback is paused so the requested frame is held
     * once the seek completes.
     */
    dcfmv_set_paused(dcfmv_current, 1);
    dcfmv_set_preload_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_audio_stop_stream(dcfmv_current);
    atomic_store(&g_vmu_flush_defer_until_frame, frame);
    Singe_log("[VMU] Flushing pending save at seek start for discSearch(%d)", frame);
    dcfmv_request_seek(dcfmv_current, frame);
//  seek_to_frame(frame);
    return 0;
}

static int sep_pause(lua_State *L) {
    // Only pause if not already halted
        // GHalted = 1;  // Pause playback
        // g_playback_started = 0;
        // After the title FMV finishes, start the next phase (e.g., intro FMV)
    Singe_log("🎬 discPause/sep_pause.");
    dcfmv_log_state("pause_pre", dcfmv_current);
    dcfmv_set_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_audio_stop_stream(dcfmv_current);
    dcfmv_set_preload_paused(dcfmv_current, 1);
    // Compute global ratios for the next phase
    compute_global_ratios();

    return 0;  // Return successfully
}

static int sep_play(lua_State *L) {
    Singe_log("[Singe] sep_play/discPlay\n");
    dcfmv_log_state("play_pre", dcfmv_current);
    dcfmv_set_paused(dcfmv_current, 0);
    dcfmv_set_preload_paused(dcfmv_current, 0);
    dcfmv_audio_start_stream(dcfmv_current);
    dcfmv_set_audio_muted(dcfmv_current, 0);
    dcfmv_log_state("play_post", dcfmv_current);
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
        case 1:
        case 2:
            dcfmv_set_audio_channel_enabled(dcfmv_current, channel, onOff);
            break;
        default: return luaL_error(L, "discAudio: invalid channel %d", channel);
    }

    Singe_log("[Singe] discAudio ch=%d -> %s (L=%d R=%d)\n",
           channel, onOff ? "ON" : "OFF",
           dcfmv_audio_channel_enabled(dcfmv_current, 1),
           dcfmv_audio_channel_enabled(dcfmv_current, 2));
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
    const dcfmv_media_info_t *info = dcfmv_media_info(dcfmv_current);
    lua_pushinteger(L, info ? info->tex_height : 0);
    
    return 1;
}

static int sep_mpeg_get_width(lua_State *L) {
    const dcfmv_media_info_t *info = dcfmv_media_info(dcfmv_current);
    lua_pushinteger(L, info ? info->tex_width : 0);
    return 1;
}

static int sep_step_backward(lua_State *L) {
    Singe_log("[Singe] sep_step_backward/discStepBackward\n");
    int current_frame = dcfmv_frame_index(dcfmv_current);
    int target_frame = current_frame - 1;
    if (target_frame < 0) target_frame = 0;
    // atomic_fetch_add(&GSeekGeneration, 1);
    // GSeeking = 1;
    // GSeekTargetFrame = target_frame;
    dcfmv_set_preload_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    atomic_store(&g_vmu_flush_defer_until_frame, target_frame);
    Singe_log("[VMU] Flushing pending save at seek start for discStepBackward(%d)", target_frame);
    dcfmv_request_seek(dcfmv_current, target_frame);
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

struct LoadedFont {
    FT_Face face;
    CharCache char_cache[128];
    int char_cache_initialized;
};


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
    CharCache *char_cache;

    if (!g_active_loaded_font || !GCurrentFont) return;
    if (g_active_loaded_font->char_cache_initialized) return;

    char_cache = g_active_loaded_font->char_cache;
#if SINGE_DEBUG_LOGS
    printf("Initializing character cache for font...\n");
#endif
    
    // Get font metrics
    int ascender = GCurrentFont->size->metrics.ascender >> 6;
    
    // Pre-render all printable ASCII characters (32-126)
    for (int ch = 32; ch < 128; ch++) {
        if (FT_Load_Char(GCurrentFont, ch, FT_LOAD_RENDER) != 0) continue;
        
        FT_GlyphSlot slot = GCurrentFont->glyph;
        FT_Bitmap *bmp = &slot->bitmap;
        
        // Skip empty glyphs (like space)
        if (bmp->width == 0 || bmp->rows == 0) {
            char_cache[ch].ch = ch;
            char_cache[ch].advance = slot->advance.x >> 6;
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
        char_cache[ch].ch = ch;
        char_cache[ch].w = bmp->width;
        char_cache[ch].h = bmp->rows;
        char_cache[ch].tex_w = tex_w;
        char_cache[ch].tex_h = tex_h;
        char_cache[ch].tex = tex;
        char_cache[ch].bearing_x = slot->bitmap_left;
        char_cache[ch].bearing_y = slot->bitmap_top;
        char_cache[ch].advance = slot->advance.x >> 6;
        pvr_poly_compile(&char_cache[ch].hdr, &cxt);
    }
    
    g_active_loaded_font->char_cache_initialized = 1;
#if SINGE_DEBUG_LOGS
    printf("Character cache initialized (96 chars)\n");
#endif
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

static void build_root_resource_path(const char *relative, char *out, size_t out_sz)
{
    char base[sizeof(G_BASE_PATH)];
    strncpy(base, G_BASE_PATH, sizeof(base));
    base[sizeof(base) - 1] = '\0';

    size_t len = strlen(base);
    while (len > 0 && base[len - 1] == '/') {
        base[--len] = '\0';
    }

    char *slash = strrchr(base, '/');
    if (slash && strcmp(slash + 1, "data") == 0) {
        *(slash + 1) = '\0';
    } else if (slash) {
        *(slash + 1) = '\0';
    }

    snprintf(out, out_sz, "%s%s", base, relative);
}

static void build_intro_path(char *out, size_t out_sz)
{
    build_root_resource_path("resources/dcsinge_intro.png", out, out_sz);
}

static void build_vmu_icon_path(char *out, size_t out_sz)
{
    build_root_resource_path(G_VMU_ICON_FILE, out, out_sz);
}

static void draw_startup_intro(void)
{
    if (g_startup_intro_drawn) return;
    g_startup_intro_drawn = 1;

    char intro_path[256];
    build_intro_path(intro_path, sizeof(intro_path));

    pvr_ptr_t tex = NULL;
    uint32_t w = 0, h = 0;
    if (png_load_texture(intro_path, &tex, PNG_FULL_ALPHA, &w, &h) < 0 || !tex || !w || !h) {
        printf("[Startup] Intro splash not found: %s\n", intro_path);
        return;
    }

    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, PVR_TXRFMT_ARGB4444,
                     w, h, tex, PVR_FILTER_BILINEAR);
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.gen.culling = PVR_CULLING_NONE;

    pvr_poly_hdr_t hdr;
    pvr_poly_compile(&hdr, &cxt);

    float u1 = 1.0f;
    float v1 = 1.0f;
    pvr_vertex_t verts[4];
    verts[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xFFFFFFFF};
    verts[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=g_display_w, .y=0, .z=1, .u=u1, .v=0, .argb=0xFFFFFFFF};
    verts[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=g_display_h, .z=1, .u=0, .v=v1, .argb=0xFFFFFFFF};
    verts[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL, .x=g_display_w, .y=g_display_h, .z=1, .u=u1, .v=v1, .argb=0xFFFFFFFF};

    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &hdr, 1);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), verts, 4);
    pvr_list_finish();
    pvr_scene_finish();
}

    void overlay_draw_sprite(int x, int y, const SingeSprite *spr)
    {
        if (!spr || !spr->texture) return;

        int w = spr->width;
        int h = spr->height;

        // Text sprites are authored in overlay space; only the draw position
        // gets converted to the Dreamcast screen plane here.
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
    CharCache *char_cache;

    if (!g_active_loaded_font) return;
    if (!g_active_loaded_font->char_cache_initialized) return;

    char_cache = g_active_loaded_font->char_cache;
    
    for (int i = 0; i < 128; i++) {
        if (char_cache[i].tex) {
            pvr_mem_free(char_cache[i].tex);
            char_cache[i].tex = NULL;
        }
    }
    
    memset(char_cache, 0, sizeof(g_active_loaded_font->char_cache));
    g_active_loaded_font->char_cache_initialized = 0;
}

// ============================================================================
// Lua Font Functions
// ============================================================================

static int sep_font_load(lua_State *L) {
    const char *path = lua_tostring(L, 1);
    int size = (int)lua_tonumber(L, 2);
    int font_index;
    int requested_size;
    int pixel_size;
    LoadedFont *new_fonts;

    requested_size = (size < 0) ? -size : size;
    pixel_size = (requested_size * SINGE_FONT_SCALE_NUM + (SINGE_FONT_SCALE_DEN - 1)) / SINGE_FONT_SCALE_DEN;
    pixel_size += SINGE_FONT_BIAS_PX;
    if (pixel_size < SINGE_FONT_MIN_PX) {
        pixel_size = SINGE_FONT_MIN_PX;
    }

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

    if (FT_Set_Pixel_Sizes(face, 0, pixel_size) != 0) {
        Singe_log("Failed to set font size: %d", pixel_size);
        FT_Done_Face(face);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }

    // Add the font to the font manager
    new_fonts = realloc(g_font_manager.fonts, sizeof(LoadedFont) * (g_font_manager.font_count + 1));
    if (!new_fonts) {
        FT_Done_Face(face);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }
    g_font_manager.fonts = new_fonts;
    memset(&g_font_manager.fonts[g_font_manager.font_count], 0, sizeof(LoadedFont));
    g_font_manager.fonts[g_font_manager.font_count].face = face;
    font_index = g_font_manager.font_count;
    g_font_manager.font_count++;

    free(fullpath);
    lua_pushinteger(L, font_index);
    
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
    CharCache *char_cache;

    if (!g_active_loaded_font || !g_active_loaded_font->char_cache_initialized) {
        lua_pushinteger(L, 0);
        return 1;
    }
    char_cache = g_active_loaded_font->char_cache;
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (*p < 128) {
            width += char_cache[*p].advance;
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
//-------------------------------------------------------------
// Clean, correct FreeType → Dreamcast font sprite generator
//-------------------------------------------------------------
SingeSprite *make_or_get_font_sprite(const char *text, uint8_t r, uint8_t g, uint8_t b)
{
    if (!GCurrentFont || !text || !*text)
        return NULL;

    // ---------------------------------------------------------
    // Compute hash key (text + RGB)
    // ---------------------------------------------------------
    unsigned long hash_value = hash(text);
    uint8_t r5 = r & 0xF8;
    uint8_t g5 = g & 0xF8;
    uint8_t b5 = b & 0xF8;
    uintptr_t font_key = (uintptr_t)GCurrentFont;
    hash_value ^= (r5 << 16) | (g5 << 8) | b5;
    hash_value ^= (unsigned long)(font_key >> 4);

    // Return cached version if present
    SingeSprite *cached = get_cached_font_sprite(hash_value);
    if (cached)
        return cached;

    // ---------------------------------------------------------
    // Retrieve correct FreeType ascent/descent
    // ---------------------------------------------------------
    FT_Size_Metrics m = GCurrentFont->size->metrics;

    int ascent  = m.ascender  >> 6;     // pixels above baseline
    int descent = -(m.descender >> 6);  // pixels below baseline
    int line_height = ascent + descent; // full vertical line height

    /*
     * Font sizes are already authored in overlay space by Lua. Do not apply
     * the Dreamcast screen scale again here or menu text grows a second time.
     */
    float scale_x = 1.0f;
    float scale_y = 1.0f;

    // ---------------------------------------------------------
    // Measure total width only
    // ---------------------------------------------------------
    int total_width = 0;
    for (const unsigned char *p = (const unsigned char*)text; *p; p++) {
        if (FT_Load_Char(GCurrentFont, *p, FT_LOAD_DEFAULT) != 0)
            continue;
        total_width += (GCurrentFont->glyph->advance.x >> 6);
    }

    if (total_width <= 0 || line_height <= 0)
        return NULL;

    // ---------------------------------------------------------
    // Compute scaled text size
    // ---------------------------------------------------------
    int scaled_width  = (int)(total_width * scale_x);
    int scaled_height = (int)(line_height * scale_y);

    // Round to next power of two
    int tex_w = next_pow2(scaled_width);
    int tex_h = next_pow2(scaled_height);
    if (tex_w > 1024) tex_w = 1024;
    if (tex_h > 1024) tex_h = 1024;

    // ---------------------------------------------------------
    // Allocate Dreamcast ARGB1555 texture buffer
    // ---------------------------------------------------------
    size_t img_bytes = tex_w * tex_h * 2;
    uint16_t *img = memalign(32, img_bytes);
    if (!img)
        return NULL;
    memset(img, 0, img_bytes);

    // Prepack our Dreamcast color
    uint16_t dc_color =
        (1 << 15) |    // alpha
        ((r & 0xF8) << 7) |
        ((g & 0xF8) << 2) |
        (b >> 3);

    // ---------------------------------------------------------
    // Render glyphs into the texture
    // ---------------------------------------------------------
    int pen_x = 0;
    int scaled_ascent = (int)(ascent * scale_y);

    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (FT_Load_Char(GCurrentFont, *p, FT_LOAD_DEFAULT) != 0)
            continue;

        FT_Render_Glyph(GCurrentFont->glyph, FT_RENDER_MODE_MONO);
        FT_GlyphSlot slot = GCurrentFont->glyph;
        FT_Bitmap *bmp = &slot->bitmap;

        int glyph_w = (int)(bmp->width * scale_x);
        int glyph_h = (int)(bmp->rows * scale_y);

        // Correct baseline alignment:
        int glyph_x = (int)(pen_x * scale_x) + (int)(slot->bitmap_left * scale_x);
        int glyph_y = scaled_ascent - (int)(slot->bitmap_top * scale_y);

        for (int sy = 0; sy < glyph_h; sy++) {
            int src_y = (int)(sy / scale_y);
            if (src_y >= bmp->rows)
                continue;

            for (int sx = 0; sx < glyph_w; sx++) {
                int src_x = (int)(sx / scale_x);
                if (src_x >= bmp->width)
                    continue;

                // Read 1-bit or 8-bit alpha depending on glyph mode
                uint8_t bitmask = 0;
                if (bmp->pixel_mode == FT_PIXEL_MODE_MONO) {
                    int byte = bmp->buffer[src_y * bmp->pitch + (src_x >> 3)];
                    bitmask = (byte & (0x80 >> (src_x & 7))) ? 1 : 0;
                } else if (bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
                    bitmask = (bmp->buffer[src_y * bmp->pitch + src_x] > 0);
                }

                if (!bitmask)
                    continue;

                int tx = glyph_x + sx;
                int ty = glyph_y + sy;
                if (tx >= 0 && tx < tex_w && ty >= 0 && ty < tex_h)
                    img[ty * tex_w + tx] = dc_color;
            }
        }

        // Advance pen along baseline
        pen_x += (slot->advance.x >> 6);
    }

    // ---------------------------------------------------------
    // Upload to VRAM and create sprite
    // ---------------------------------------------------------
    pvr_ptr_t tex = pvr_mem_malloc(img_bytes);
    if (!tex) { free(img); return NULL; }
    pvr_txr_load(img, tex, img_bytes);
    free(img);

    SingeSprite *sprite = Singe_xmalloc(sizeof(SingeSprite));
    sprite->hash_id = hash_value;
    sprite->name = NULL;
    sprite->width = scaled_width;
    sprite->height = scaled_height;
    sprite->texture = tex;

    // Cache into linked list
    sprite->next = GSprites;
    GSprites = sprite;

    // Build PVR context
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
                     tex_w, tex_h, tex, PVR_FILTER_NONE);
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&sprite->hdr, &cxt);

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
        g_active_loaded_font = &g_font_manager.fonts[index];
        GCurrentFont = g_active_loaded_font->face;
        font_init_char_cache();
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
    (void)L;
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
	lua_pushboolean(L, dcfmv_is_paused(dcfmv_current));
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
			dcfmv_set_paused(dcfmv_current, b1);
			
		}
	}	
	return 0;
}


static int sep_singe_quit(lua_State *L) {
    (void)L;
    atomic_store(&g_exit_requested, 1);
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
        DC_log("Loading sprite: %s -> %s\n", name_or_hash, fullpath);

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
    int volume = dcfmv_audio_volume(dcfmv_current);
    lua_pushinteger(L, volume);
    return 1;
}

static int sep_vldp_setvolume(lua_State *L) {
    int volume = (int)lua_tointeger(L, 1);
    dcfmv_set_audio_volume(dcfmv_current, volume);
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
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    long size= (long)fs_read(ud->fd, buf, len);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif

    return size;
}

static const char *lua_reader(lua_State *L, void *data, size_t *size) {
    static uint8_t __attribute__((aligned(32))) buffer[1024];
    FileIoUserdata *ud = (FileIoUserdata *)data;
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    long br = fs_read(ud->fd, buffer, sizeof(buffer));
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif
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
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    file_t fd = fs_open(fullpath, O_RDONLY);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif
    free(fullpath);
    
    if (fd < 0) {
        return luaL_error(L, "cannot open %s", filename);
    }
    FileIoUserdata ud;
    ud.fd = fd;
    
    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", filename);
        
    int rc = lua_load(L, lua_reader, &ud, chunkname, NULL);
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    fs_close(fd);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif
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
    const char *suffix = "";

    if (lua_gettop(L) >= 1 && lua_isstring(L, 1)) {
        suffix = lua_tostring(L, 1);
    }
    dcfmv_set_audio_muted(dcfmv_current, 1);
    printf("[Singe] discAudioSuffix('%s') [DC unified audio]\n", suffix);

    /*
     * IMPORTANT:
     * Pretend audio switch succeeded so Lua does not
     * alter playback state or mute audio incorrectly.
     */
    lua_pushboolean(L, 1);
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
#define DEBUG_HITBOX 1
static int sep_overlay_box(lua_State *L) {
    if (lua_gettop(L) < 4) { lua_pushboolean(L, 0); return 1; }

    int x1 = (int)lua_tonumber(L, 1);
    int y1 = (int)lua_tonumber(L, 2);
    int x2 = (int)lua_tonumber(L, 3);
    int y2 = (int)lua_tonumber(L, 4);

    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;

    /*
     * Singe gun games commonly author hitboxes in 320-wide video space, then
     * Lua expands them through ratioGetX(). Convert that ratio-expanded X back
     * to the Dreamcast video plane here so every gun script can keep its PC
     * hitbox formulas.
     */
    float scaled_x1 = (x1 / g_ratio_x) * g_scale_x;
    float scaled_y1 = y1 * g_scale_y;
    float scaled_x2 = (x2 / g_ratio_x) * g_scale_x;
    float scaled_y2 = y2 * g_scale_y;

    static int overlay_box_log_count = 0;
    if (overlay_box_log_count < 80) {
        Singe_log("[OVERLAY_BOX] in=(%d,%d)-(%d,%d) screen=(%.1f,%.1f)-(%.1f,%.1f) scale=(%.3f,%.3f) ratio_offset=(%.1f,%.1f)",
                  x1, y1, x2, y2,
                  scaled_x1, scaled_y1, scaled_x2, scaled_y2,
                  g_scale_x, g_scale_y,
                  g_ratio_x_offset, g_ratio_y_offset);
        overlay_box_log_count++;
    }

    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;

    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }

    pvr_prim(&hdr, sizeof(hdr));

    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));

    pvr_vertex_t vert;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x1; vert.y = scaled_y1; vert.z = 1.0f;
    vert.argb = color; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.x = scaled_x2; vert.y = scaled_y1;
    pvr_prim(&vert, sizeof(vert));

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

    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;

    float scaled_x = ((x / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
    float scaled_y = ((y / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
    float scaled_r = (radius / (float)GOverlayWidth) * 640.0f * ((g_scale_x + g_scale_y) * 0.5f);

    static pvr_poly_hdr_t hdr_fill, hdr_line;
    static bool hdr_fill_ok = false, hdr_line_ok = false;

    if (filled && !hdr_fill_ok) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr_fill, &cxt);
        hdr_fill_ok = true;
    }
    if (!filled && !hdr_line_ok) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr_line, &cxt);
        hdr_line_ok = true;
    }

    pvr_prim(filled ? &hdr_fill : &hdr_line,
             sizeof(pvr_poly_hdr_t));

    uint16_t color = pack_argb1555_overlay(
        GFontColorA, GFontColorR, GFontColorG, GFontColorB);

    pvr_vertex_t vert;
    int segments = 32;

    if (filled) {
        for (int i = 0; i < segments; i++) {
            float a1 = (2.0f * M_PI * i) / segments;
            float a2 = (2.0f * M_PI * (i + 1)) / segments;

            vert.flags = PVR_CMD_VERTEX;
            vert.x = scaled_x; vert.y = scaled_y; vert.z = 1.0f;
            vert.argb = color; vert.oargb = 0;
            pvr_prim(&vert, sizeof(vert));

            vert.x = scaled_x + fcos(a1) * scaled_r;
            vert.y = scaled_y + fsin(a1) * scaled_r;
            pvr_prim(&vert, sizeof(vert));

            vert.flags = (i == segments - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert.x = scaled_x + fcos(a2) * scaled_r;
            vert.y = scaled_y + fsin(a2) * scaled_r;
            pvr_prim(&vert, sizeof(vert));
        }
    } else {
        float width = 2.0f;
        for (int i = 0; i < segments; i++) {
            float a1 = (2.0f * M_PI * i) / segments;
            float a2 = (2.0f * M_PI * (i + 1)) / segments;

            float x1 = scaled_x + fcos(a1) * scaled_r;
            float y1 = scaled_y + fsin(a1) * scaled_r;
            float x2 = scaled_x + fcos(a2) * scaled_r;
            float y2 = scaled_y + fsin(a2) * scaled_r;

            float dx = x2 - x1, dy = y2 - y1;
            float inv = frsqrt(dx*dx + dy*dy) * (width * 0.5f);
            float nx = -dy * inv, ny = dx * inv;

            vert.flags = PVR_CMD_VERTEX;
            vert.x = x1 + nx; vert.y = y1 + ny; vert.z = 1.0f;
            vert.argb = color; vert.oargb = 0;
            pvr_prim(&vert, sizeof(vert));

            vert.x = x1 - nx; vert.y = y1 - ny;
            pvr_prim(&vert, sizeof(vert));

            vert.x = x2 + nx; vert.y = y2 + ny;
            pvr_prim(&vert, sizeof(vert));

            vert.flags = (i == segments - 1) ? PVR_CMD_VERTEX_EOL : PVR_CMD_VERTEX;
            vert.x = x2 - nx; vert.y = y2 - ny;
            pvr_prim(&vert, sizeof(vert));
        }
    }

    lua_pushboolean(L, 1);
    return 1;
}

static int sep_overlay_line(lua_State *L) {
    if (lua_gettop(L) < 4) { lua_pushboolean(L, 0); return 1; }

    int x1 = (int)lua_tonumber(L, 1);
    int y1 = (int)lua_tonumber(L, 2);
    int x2 = (int)lua_tonumber(L, 3);
    int y2 = (int)lua_tonumber(L, 4);

    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;

    float sx1 = (x1 / (float)GOverlayWidth) * 640.0f;
    float sy1 = (y1 / (float)GOverlayHeight) * 480.0f;
    float sx2 = (x2 / (float)GOverlayWidth) * 640.0f;
    float sy2 = (y2 / (float)GOverlayHeight) * 480.0f;

    float scaled_x1 = (sx1 - g_ratio_x_offset) * g_scale_x;
    float scaled_y1 = (sy1 - g_ratio_y_offset) * g_scale_y;
    float scaled_x2 = (sx2 - g_ratio_x_offset) * g_scale_x;
    float scaled_y2 = (sy2 - g_ratio_y_offset) * g_scale_y;

    // --- Line normal ---
    float dx = scaled_x2 - scaled_x1;
    float dy = scaled_y2 - scaled_y1;
    float width = 2.0f;
    float invmag = frsqrt((dx * dx) + (dy * dy)) * (width * 0.5f);
    float nx = -dy * invmag;
    float ny =  dx * invmag;

    // --- Cached polygon header ---
    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;

    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }

    pvr_prim(&hdr, sizeof(hdr));

    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));

    pvr_vertex_t vert;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x1 + nx; vert.y = scaled_y1 + ny; vert.z = 1.0f;
    vert.argb = color; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.x = scaled_x1 - nx; vert.y = scaled_y1 - ny;
    pvr_prim(&vert, sizeof(vert));

    vert.x = scaled_x2 + nx; vert.y = scaled_y2 + ny;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = scaled_x2 - nx; vert.y = scaled_y2 - ny;
    pvr_prim(&vert, sizeof(vert));

    lua_pushboolean(L, 1);
    return 1;
}



static int sep_overlay_plot(lua_State *L) {
    if (lua_gettop(L) < 2) return 0;

    int x = (int)lua_tonumber(L, 1);
    int y = (int)lua_tonumber(L, 2);

    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;

    float scaled_x = ((x / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
    float scaled_y = ((y / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;

    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;

    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = PVR_BLEND_ENABLE;
        cxt.blend.dst_enable = PVR_BLEND_ENABLE;
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }

    pvr_prim(&hdr, sizeof(hdr));

    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));

    float pixel = 2.0f;
    pvr_vertex_t vert;

    vert.flags = PVR_CMD_VERTEX;
    vert.x = scaled_x; vert.y = scaled_y; vert.z = 1.0f;
    vert.argb = color; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.x = scaled_x + pixel; vert.y = scaled_y;
    pvr_prim(&vert, sizeof(vert));

    vert.x = scaled_x; vert.y = scaled_y + pixel;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = scaled_x + pixel; vert.y = scaled_y + pixel;
    pvr_prim(&vert, sizeof(vert));

    lua_pushboolean(L, 1);
    return 1;
}


// Batch rendering functions for DCSinge
// Add these to your Singe C implementation

static int sep_overlay_lines_batch(lua_State *L) {
    if (!lua_istable(L, 1)) { 
        lua_pushboolean(L, 0); 
        return 1; 
    }
    
    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;
    
    // Compile header once
    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;
    
    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }
    
    pvr_prim(&hdr, sizeof(hdr));
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    // Get table length
    int num_lines = lua_rawlen(L, 1);
    
    for (int i = 1; i <= num_lines; i++) {
        lua_rawgeti(L, 1, i); // Get line table {x1, y1, x2, y2}
        
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        
        // Extract x1, y1, x2, y2
        lua_rawgeti(L, -1, 1); 
        int x1 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 2); 
        int y1 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 3); 
        int x2 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 4); 
        int y2 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_pop(L, 1); // Pop line table
        
        // Scale coordinates
        float sx1 = (x1 / (float)GOverlayWidth) * 640.0f;
        float sy1 = (y1 / (float)GOverlayHeight) * 480.0f;
        float sx2 = (x2 / (float)GOverlayWidth) * 640.0f;
        float sy2 = (y2 / (float)GOverlayHeight) * 480.0f;
        
        float scaled_x1 = (sx1 - g_ratio_x_offset) * g_scale_x;
        float scaled_y1 = (sy1 - g_ratio_y_offset) * g_scale_y;
        float scaled_x2 = (sx2 - g_ratio_x_offset) * g_scale_x;
        float scaled_y2 = (sy2 - g_ratio_y_offset) * g_scale_y;
        
        // Calculate normal for thick line
        float dx = scaled_x2 - scaled_x1;
        float dy = scaled_y2 - scaled_y1;
        float width = 2.0f;
        float invmag = frsqrt((dx * dx) + (dy * dy)) * (width * 0.5f);
        float nx = -dy * invmag;
        float ny = dx * invmag;
        
        // Submit quad (4 vertices per line)
        pvr_vertex_t vert;
        
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x1 + nx; 
        vert.y = scaled_y1 + ny; 
        vert.z = 1.0f;
        vert.argb = color; 
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x1 - nx; 
        vert.y = scaled_y1 - ny;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x2 + nx; 
        vert.y = scaled_y2 + ny;
        pvr_prim(&vert, sizeof(vert));
        
        // IMPORTANT: Mark EVERY quad's last vertex as EOL
        vert.flags = PVR_CMD_VERTEX_EOL;
        vert.x = scaled_x2 - nx; 
        vert.y = scaled_y2 - ny;
        pvr_prim(&vert, sizeof(vert));
    }
    
    lua_pushboolean(L, 1);
    return 1;
}


static int sep_overlay_plots_batch(lua_State *L) {
    if (!lua_istable(L, 1)) { 
        lua_pushboolean(L, 0); 
        return 1; 
    }
    
    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;
    
    // Compile header once
    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;
    
    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = PVR_BLEND_ENABLE;
        cxt.blend.dst_enable = PVR_BLEND_ENABLE;
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }
    
    pvr_prim(&hdr, sizeof(hdr));
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    float pixel = 2.0f;
    
    // Get table length
    int num_plots = lua_rawlen(L, 1);
    
    for (int i = 1; i <= num_plots; i++) {
        lua_rawgeti(L, 1, i); // Get plot table {x, y}
        
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        
        // Extract x, y
        lua_rawgeti(L, -1, 1); 
        int x = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 2); 
        int y = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_pop(L, 1); // Pop plot table
        
        // Scale coordinates
        float scaled_x = ((x / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
        float scaled_y = ((y / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
        
        // Submit quad (small pixel)
        pvr_vertex_t vert;
        
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x; 
        vert.y = scaled_y; 
        vert.z = 1.0f;
        vert.argb = color; 
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x + pixel; 
        vert.y = scaled_y;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x; 
        vert.y = scaled_y + pixel;
        pvr_prim(&vert, sizeof(vert));
        
        // IMPORTANT: Mark EVERY quad's last vertex as EOL
        vert.flags = PVR_CMD_VERTEX_EOL;
        vert.x = scaled_x + pixel; 
        vert.y = scaled_y + pixel;
        pvr_prim(&vert, sizeof(vert));
    }
    
    lua_pushboolean(L, 1);
    return 1;
}


static int sep_overlay_boxes_batch(lua_State *L) {
    if (!lua_istable(L, 1)) { 
        lua_pushboolean(L, 0); 
        return 1; 
    }
    
    if (GOverlayWidth <= 0)  GOverlayWidth  = 360;
    if (GOverlayHeight <= 0) GOverlayHeight = 240;
    
    // Compile header once
    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;
    
    if (!header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }
    
    pvr_prim(&hdr, sizeof(hdr));
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    // Get table length
    int num_boxes = lua_rawlen(L, 1);
    
    for (int i = 1; i <= num_boxes; i++) {
        lua_rawgeti(L, 1, i); // Get box table {x1, y1, x2, y2}
        
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        
        // Extract x1, y1, x2, y2
        lua_rawgeti(L, -1, 1); 
        int x1 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 2); 
        int y1 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 3); 
        int x2 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_rawgeti(L, -1, 4); 
        int y2 = (int)lua_tonumber(L, -1); 
        lua_pop(L, 1);
        
        lua_pop(L, 1); // Pop box table
        
        // Scale coordinates
        float scaled_x1 = ((x1 / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
        float scaled_y1 = ((y1 / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
        float scaled_x2 = ((x2 / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
        float scaled_y2 = ((y2 / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
        
        // Submit quad
        pvr_vertex_t vert;
        
        vert.flags = PVR_CMD_VERTEX;
        vert.x = scaled_x1; 
        vert.y = scaled_y1; 
        vert.z = 1.0f;
        vert.argb = color; 
        vert.oargb = 0;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x2; 
        vert.y = scaled_y1;
        pvr_prim(&vert, sizeof(vert));
        
        vert.x = scaled_x1; 
        vert.y = scaled_y2;
        pvr_prim(&vert, sizeof(vert));
        
        // IMPORTANT: Mark EVERY quad's last vertex as EOL
        vert.flags = PVR_CMD_VERTEX_EOL;
        vert.x = scaled_x2; 
        vert.y = scaled_y2;
        pvr_prim(&vert, sizeof(vert));
    }
    
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
    if (!g_cfg_enable_mp3) {
        printf("[Music] MP3 disabled by config\n");
        g_current_playing_handle = -1;
        return;
    }

    printf("[Music] Initializing MP3 system...\n");
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
    if (g_cfg_enable_mp3) {
        printf("[Music] Shutting down MP3 system...\n");
        if (g_current_playing_handle >= 0) {
            mp3_stop();
        }
        mp3_shutdown();
        printf("[Music] MP3 system shutdown complete\n");
    }

    if (g_mp3_stream_inited || (dcfmv_current && dcfmv_audio_channels(dcfmv_current) > 0)) {
        snd_stream_shutdown();
        g_mp3_stream_inited = 0;
    }
}

static void singe_shutdown(void) {
    atomic_store(&g_exit_requested, 1);

    /*
     * Give the worker thread a moment to observe the exit flag before we
     * start tearing down shared FMV state and sound resources.
     */
    thd_sleep(20);

    sep_music_cleanup();

    flush_vmu_archive_if_pending();

    if (dcfmv_current) {
        dcfmv_close(dcfmv_current);
        dcfmv_destroy(dcfmv_current);
    }

    if (GLua) {
        lua_close(GLua);
        GLua = NULL;
    }

    clear_io_cache();
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
    if (!g_cfg_enable_mp3) {
        lua_pushnumber(L, -1);
        return 1;
    }

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
    if (!g_cfg_enable_mp3) {
        lua_pushboolean(L, 0);
        return 1;
    }

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
    if (result == 0) {
        g_current_playing_handle = handle;
        printf("[Music] Playback started successfully\n");
        lua_pushboolean(L, 1);
    } else {
        g_current_playing_handle = -1;
        printf("[Music] ERROR: mp3_start failed for %s (code=%d)\n",
               track->filepath, result);
        track->failed_to_play = 1;
        lua_pushboolean(L, 0);
    }
    
    return 1;
}

static int sep_music_pause(lua_State *L) {
    if (!g_cfg_enable_mp3) return 0;

    if (g_current_playing_handle >= 0) {
        mp3_stop();
    }
    return 0;
}

static int sep_music_resume(lua_State *L) {
    if (!g_cfg_enable_mp3) return 0;

    if (g_current_playing_handle >= 0) {
        music_track_t *track = find_track_by_handle(g_current_playing_handle);
        if (track && !track->failed_to_play) {
            mp3_start(track->filepath, 0);
        }
    }
    return 0;
}

static int sep_music_stop(lua_State *L) {
    if (!g_cfg_enable_mp3) return 0;

    if (g_current_playing_handle >= 0) {
        mp3_stop();
        g_current_playing_handle = -1;
    }
    return 0;
}

static int sep_music_playing(lua_State *L) {
    if (!g_cfg_enable_mp3) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int is_playing = (g_current_playing_handle >= 0);
    lua_pushboolean(L, is_playing);
    return 1;
}

static int sep_music_volume(lua_State *L) {
    if (!g_cfg_enable_mp3) return 0;

    int volume = (int)luaL_checknumber(L, 1);
    // libmp3 in KOS doesn't have direct volume control in the basic API
    return 0;
}

static int sep_music_unload(lua_State *L) {
    if (!g_cfg_enable_mp3) return 0;

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
    printf("[SFX] Loading: %s -> %s\n", path, fullpath ? fullpath : "(null)");
    
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
        printf("[SFX] Failed to load: %s\n", fullpath);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }
    
    SingeSound *sound = Singe_xmalloc(sizeof(SingeSound));
    sound->name = Singe_xstrdup(path);  // Store original path for cache
    sound->handle = sfx;
    sound->next = GSounds;
    GSounds = sound;
    printf("[SFX] Loaded successfully: %s handle=%lu ptr=%p\n",
           fullpath,
           (unsigned long)sfx,
           (void *)(uintptr_t)sfx);
    
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
        int vol = dcfmv_audio_volume(dcfmv_current);
        if (vol < 0) vol = 0;
        if (vol > 255) vol = 255;

        printf("[SFX] Play requested: sound=%p handle=%lu ptr=%p vol=%d\n",
               (void *)sound,
               (unsigned long)sound->handle,
               (void *)(uintptr_t)sound->handle,
               vol);
        // snd_sfx_play(handle, volume, pan)
        int chn = snd_sfx_play(sound->handle, vol, 128);
        printf("[SFX] snd_sfx_play returned chn=%d\n", chn);

        // printf("[Singe] soundPlay(id=%ld, vol=%d)\n", (long)sound_id, vol);
    } else {
        printf("[Singe] soundPlay(%ld) -> invalid handle\n", (long)sound_id);
    }

    return 1;
}

static int  sep_sound_getvolume(lua_State *L) {
    // Convert from 0–255 Dreamcast volume scale to 0–63 Hypseus scale
    int raw_vol = dcfmv_audio_volume(dcfmv_current);
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
    int raw_vol = dcfmv_audio_volume(dcfmv_current);
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
float scaled_xf = (x * g_scale_x) + g_ratio_x_offset;
float scaled_yf = (y * g_scale_y) + g_ratio_y_offset;

int screen_x = (int)roundf(scaled_xf);
int screen_y = (int)roundf(scaled_yf);

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

typedef struct SingeLuaFileCache {
    char *path;
    char *data;
    size_t len;
    int dirty;
    struct SingeLuaFileCache *next;
} SingeLuaFileCache;

typedef struct {
    SingeLuaFileCache *entry;
    size_t pos;
    int active;
} SingeLuaInputState;

typedef struct {
    char *path;
    char *data;
    size_t len;
    size_t cap;
    int active;
} SingeLuaOutputState;

static SingeLuaFileCache *g_io_cache = NULL;
static SingeLuaInputState g_io_input = {0};
static SingeLuaOutputState g_io_output = {0};
static int g_orig_io_input_ref = LUA_NOREF;
static int g_orig_io_read_ref = LUA_NOREF;
static int g_orig_io_output_ref = LUA_NOREF;
static int g_orig_io_write_ref = LUA_NOREF;
static int g_orig_io_close_ref = LUA_NOREF;
static char g_io_input_token;
static char g_io_output_token;

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t payload_len;
    uint32_t entry_count;
} DCSingeVmuArchiveHeader;

typedef struct {
    uint32_t key_len;
    uint32_t data_len;
} DCSingeVmuArchiveEntryHeader;

static uint32_t fnv1a32(const char *text) {
    uint32_t hash = 2166136261u;
    while (text && *text) {
        hash ^= (uint8_t)*text++;
        hash *= 16777619u;
    }
    return hash;
}

static int add_size_checked(size_t *total, size_t add) {
    if (add > SIZE_MAX - *total) {
        return 0;
    }
    *total += add;
    return 1;
}

static int build_vmu_mount_path(char *out, size_t out_sz) {
    static const char ports[] = { 'a', 'b', 'c', 'd' };
    for (int port = 0; port < 4; port++) {
        if (maple_enum_type(port, MAPLE_FUNC_MEMCARD)) {
            snprintf(out, out_sz, "/vmu/%c1", ports[port]);
            return 1;
        }
    }
    if (out_sz > 0) {
        out[0] = '\0';
    }
    return 0;
}

static maple_device_t *get_vmu_mount_device(void) {
    if (g_vmu_mount_path[0] != '/' || strncmp(g_vmu_mount_path, "/vmu/", 5) != 0) {
        return NULL;
    }

    int port = tolower((unsigned char)g_vmu_mount_path[5]) - 'a';
    int unit = g_vmu_mount_path[6] - '0';
    if (port < 0 || port >= 4 || unit < 0 || unit >= 6) {
        return NULL;
    }

    return maple_enum_dev(port, unit);
}

static void wait_for_vmu_device_idle(void) {
    maple_device_t *dev = get_vmu_mount_device();
    if (!dev) return;

    int waited = 0;
    while (dev->frame.queued) {
        maple_queue_flush();
        thd_sleep(5);
        if (++waited >= 100) {
            printf("[VMU] VMU unresponsive, skipping save\n");
            g_vmu_available = 0;  // disable VMU until next init
            return;
        }
        dev = get_vmu_mount_device();
        if (!dev) return;
    }
}
static void build_vmu_save_name(char *out, size_t out_sz) {
    uint32_t hash = fnv1a32(G_GAME_DIR);
    hash ^= fnv1a32(G_GAME_NAME) << 1;
    snprintf(out, out_sz, "DS%08lX", (unsigned long)hash);
}

static int load_vmu_archive_locked(void);

static SingeLuaFileCache *find_io_cache_entry(const char *path) {
    for (SingeLuaFileCache *entry = g_io_cache; entry; entry = entry->next) {
        if (strcmp(entry->path, path) == 0) {
            return entry;
        }
    }
    return NULL;
}

static int load_text_file(const char *path, char **data_out, size_t *len_out) {
    file_t fd = fs_open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    size_t size = fs_total(fd);
    char *buffer = malloc(size + 1);
    if (!buffer) {
        fs_close(fd);
        return 0;
    }

    size_t br = fs_read(fd, buffer, size);
    fs_close(fd);

    buffer[br] = '\0';
    *data_out = buffer;
    *len_out = br;
    return 1;
}

static void canonicalize_io_key(const char *fullpath, char *out, size_t out_sz) {
    const char *rel = fullpath;
    size_t base_len = strlen(G_BASE_PATH);
    size_t game_len = strlen(G_GAME_DIR);

    if (strncmp(rel, G_BASE_PATH, base_len) == 0) {
        rel += base_len;
    }
    if (strncmp(rel, G_GAME_DIR, game_len) == 0) {
        rel += game_len;
    }

    snprintf(out, out_sz, "%s", rel);
}

static SingeLuaFileCache *upsert_io_cache_entry(const char *path, const char *data, size_t len, int dirty) {
    SingeLuaFileCache *entry = find_io_cache_entry(path);
    if (entry) {
        char *new_data = malloc(len + 1);
        if (!new_data) {
            return NULL;
        }
        if (len > 0 && data) {
            memcpy(new_data, data, len);
        }
        new_data[len] = '\0';

        free(entry->data);
        entry->data = new_data;
        entry->len = len;
        entry->dirty = dirty;
        return entry;
    }

    entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }

    entry->path = strdup(path);
    if (!entry->path) {
        free(entry);
        return NULL;
    }

    entry->data = malloc(len + 1);
    if (!entry->data) {
        free(entry->path);
        free(entry);
        return NULL;
    }

    if (len > 0 && data) {
        memcpy(entry->data, data, len);
    }
    entry->data[len] = '\0';
    entry->len = len;
    entry->dirty = dirty;
    entry->next = g_io_cache;
    g_io_cache = entry;
    return entry;
}

static SingeLuaFileCache *load_io_cache_entry_from_source(const char *fullpath) {
    char *file_data = NULL;
    size_t file_len = 0;
    char key[512];

    if (!load_text_file(fullpath, &file_data, &file_len)) {
        return NULL;
    }

    canonicalize_io_key(fullpath, key, sizeof(key));
    SingeLuaFileCache *entry = upsert_io_cache_entry(key, file_data, file_len, 0);
    free(file_data);
    return entry;
}

static int read_exact(file_t fd, void *buf, size_t len) {
    uint8_t *ptr = buf;
    size_t total = 0;
    while (total < len) {
        size_t br = fs_read(fd, ptr + total, len - total);
        if (br == 0) {
            break;
        }
        total += br;
    }
    return total == len;
}

static int load_vmu_icon_package(void) {
    char icon_path[256];
    build_vmu_icon_path(icon_path, sizeof(icon_path));
    strncpy(g_vmu_icon_path, icon_path, sizeof(g_vmu_icon_path));
    g_vmu_icon_path[sizeof(g_vmu_icon_path) - 1] = '\0';

    memset(&g_vmu_pkg, 0, sizeof(g_vmu_pkg));
    strncpy(g_vmu_pkg.desc_short, G_GAME_NAME, sizeof(g_vmu_pkg.desc_short) - 1);
    snprintf(g_vmu_pkg.desc_long, sizeof(g_vmu_pkg.desc_long), "%s Save", G_GAME_NAME);
    strncpy(g_vmu_pkg.app_id, "DCSinge", sizeof(g_vmu_pkg.app_id) - 1);
    /*
     * KOS's ICO loader iterates every frame in the file before it clamps to
     * the preallocated frame count, so keep two icon frames available. That
     * covers the common two-frame Dreamcast icons and still works for our
     * single-frame default icon.
     */
    g_vmu_pkg.icon_cnt = 2;
    g_vmu_pkg.icon_anim_speed = 0;
    g_vmu_pkg.eyecatch_type = VMUPKG_EC_NONE;
    g_vmu_pkg.data_len = 0;
    g_vmu_pkg.icon_data = g_vmu_icon_data;
    g_vmu_pkg.eyecatch_data = NULL;
    g_vmu_pkg.data = NULL;

    if (vmu_pkg_load_icon(&g_vmu_pkg, g_vmu_icon_path) < 0) {
        char fallback_icon[256];
        strncpy(fallback_icon, g_vmu_icon_path, sizeof(fallback_icon));
        fallback_icon[sizeof(fallback_icon) - 1] = '\0';
        char *ext = strrchr(fallback_icon, '.');
        if (ext && strcmp(ext, ".png") == 0) {
            strcpy(ext, ".ico");
            if (vmu_pkg_load_icon(&g_vmu_pkg, fallback_icon) >= 0) {
                strncpy(g_vmu_icon_path, fallback_icon, sizeof(g_vmu_icon_path));
                g_vmu_icon_path[sizeof(g_vmu_icon_path) - 1] = '\0';
                g_vmu_pkg.icon_data = g_vmu_icon_data;
                return 1;
            }
        }
        g_vmu_pkg.icon_data = NULL;
        printf("[VMU] Failed to load icon package: %s\n", g_vmu_icon_path);
        return 0;
    }

    g_vmu_pkg.icon_data = g_vmu_icon_data;
    return 1;
}

static int init_vmu_context(void) {
    if (g_vmu_ready) {
        return g_vmu_available;
    }

    g_vmu_ready = 1;
    g_vmu_available = build_vmu_mount_path(g_vmu_mount_path, sizeof(g_vmu_mount_path));
    build_vmu_save_name(g_vmu_save_name, sizeof(g_vmu_save_name));

    if (!g_vmu_available) {
        printf("[VMU] No memory card detected; VMU persistence disabled\n");
        return 0;
    }

    snprintf(g_vmu_save_path, sizeof(g_vmu_save_path), "%s/%s", g_vmu_mount_path, g_vmu_save_name);
    if (!load_vmu_icon_package()) {
        printf("[VMU] Icon package unavailable; continuing without VMU header\n");
    }
    if (!load_vmu_archive_locked()) {
        seed_vmu_archive_locked();
    }
    return 1;
}

static int parse_vmu_archive(const uint8_t *data, size_t len) {
    if (len < sizeof(DCSingeVmuArchiveHeader)) {
        return 0;
    }

    DCSingeVmuArchiveHeader hdr;
    memcpy(&hdr, data, sizeof(hdr));
    if (memcmp(hdr.magic, "DCSV", 4) != 0 || hdr.version != 1) {
        return 0;
    }

    size_t pos = sizeof(DCSingeVmuArchiveHeader);
    uint32_t count = hdr.entry_count;
    if (hdr.payload_len > len) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (pos + sizeof(DCSingeVmuArchiveEntryHeader) > hdr.payload_len) {
            return 0;
        }

        DCSingeVmuArchiveEntryHeader ehdr;
        memcpy(&ehdr, data + pos, sizeof(ehdr));
        pos += sizeof(DCSingeVmuArchiveEntryHeader);

        if (pos + ehdr.key_len + ehdr.data_len > hdr.payload_len) {
            return 0;
        }

        char *key = malloc(ehdr.key_len + 1);
        if (!key) {
            return 0;
        }
        memcpy(key, data + pos, ehdr.key_len);
        key[ehdr.key_len] = '\0';
        pos += ehdr.key_len;

        const char *entry_data = (const char *)(data + pos);
        SingeLuaFileCache *entry = upsert_io_cache_entry(key, entry_data, ehdr.data_len, 0);
        free(key);
        if (!entry) {
            return 0;
        }
        pos += ehdr.data_len;
    }

    return 1;
}

static int load_vmu_archive_locked(void) {
    if (!g_vmu_available) {
        return 0;
    }

    int was_buttons_enabled = vmu_get_buttons_enabled();
    if (was_buttons_enabled) {
        vmu_set_buttons_enabled(0);
        maple_queue_flush();  // flush any in-flight button frame AFTER disabling
        thd_sleep(10);        // give it time to complete
    }
    wait_for_vmu_device_idle();

    file_t fd = fs_open(g_vmu_save_path, O_RDONLY);
    if (fd < 0) {
        printf("[VMU] No existing save archive at %s\n", g_vmu_save_path);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }

    size_t size = fs_total(fd);
    uint8_t *buffer = malloc(size + 1);
    if (!buffer) {
        fs_close(fd);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }

    if (!read_exact(fd, buffer, size)) {
        free(buffer);
        fs_close(fd);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }
    fs_close(fd);

    int ok = parse_vmu_archive(buffer, size);
    free(buffer);
    if (ok) {
        printf("[VMU] Loaded save archive %s (%zu bytes)\n", g_vmu_save_path, size);
    } else {
        printf("[VMU] Save archive %s was invalid; starting fresh\n", g_vmu_save_path);
    }
    if (was_buttons_enabled) {
        vmu_set_buttons_enabled(1);
    }
    return ok;
}

static int serialize_vmu_archive(uint8_t **out_data, size_t *out_len) {
    size_t total = sizeof(DCSingeVmuArchiveHeader);
    uint32_t count = 0;

    for (SingeLuaFileCache *entry = g_io_cache; entry; entry = entry->next) {
        if (!entry->path || !entry->data) {
            printf("[VMU] Skipping invalid cache entry during serialize\n");
            continue;
        }

        size_t key_len = strlen(entry->path);
        if (key_len == 0) {
            printf("[VMU] Skipping empty cache key during serialize\n");
            continue;
        }

        if (!add_size_checked(&total, sizeof(DCSingeVmuArchiveEntryHeader)) ||
            !add_size_checked(&total, key_len) ||
            !add_size_checked(&total, entry->len)) {
            printf("[VMU] VMU archive size overflow while serializing\n");
            return 0;
        }
        count++;
    }

    uint8_t *buffer = calloc(1, total);
    if (!buffer) {
        return 0;
    }

    DCSingeVmuArchiveHeader hdr;
    memcpy(hdr.magic, "DCSV", 4);
    hdr.version = 1;
    hdr.payload_len = (uint32_t)total;
    hdr.entry_count = count;
    memcpy(buffer, &hdr, sizeof(hdr));

    size_t pos = sizeof(DCSingeVmuArchiveHeader);
    for (SingeLuaFileCache *entry = g_io_cache; entry; entry = entry->next) {
        if (!entry->path || !entry->data) {
            continue;
        }

        size_t key_len = strlen(entry->path);
        if (key_len == 0) {
            continue;
        }

        if (pos + sizeof(DCSingeVmuArchiveEntryHeader) + key_len + entry->len > total) {
            free(buffer);
            return 0;
        }

        DCSingeVmuArchiveEntryHeader ehdr;
        ehdr.key_len = (uint32_t)key_len;
        ehdr.data_len = (uint32_t)entry->len;
        memcpy(buffer + pos, &ehdr, sizeof(ehdr));
        pos += sizeof(DCSingeVmuArchiveEntryHeader);

        memcpy(buffer + pos, entry->path, key_len);
        pos += key_len;

        if (entry->len > 0) {
            memcpy(buffer + pos, entry->data, entry->len);
        }
        pos += entry->len;
    }

    *out_data = buffer;
    *out_len = total;
    return 1;
}

static int persist_vmu_archive_locked(void) {
    if (!g_vmu_available) {
        return 1;
    }
    Singe_log("[VMU] Persisting VMU archive to %s...\n", g_vmu_save_path);
    uint8_t *archive = NULL;
    size_t archive_len = 0;
    if (!serialize_vmu_archive(&archive, &archive_len)) {
        return 0;
    }

    int was_buttons_enabled = vmu_get_buttons_enabled();
    if (was_buttons_enabled) {
        vmu_set_buttons_enabled(0);
    }
    wait_for_vmu_device_idle();

    size_t padded_len = (archive_len + 511u) & ~511u;
    uint8_t *padded = calloc(1, padded_len);
    if (!padded) {
        free(archive);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }
    memcpy(padded, archive, archive_len);

    fs_unlink(g_vmu_save_path);
    file_t fd = fs_open(g_vmu_save_path, O_WRONLY);
    if (fd < 0) {
        printf("[VMU] Failed to open %s for writing\n", g_vmu_save_path);
        free(archive);
        free(padded);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }

    if (fs_write(fd, padded, padded_len) != padded_len) {
        printf("[VMU] Short write while persisting archive to %s\n", g_vmu_save_path);
        fs_close(fd);
        free(archive);
        free(padded);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }
        return 0;
    }

    if (g_vmu_pkg.icon_data) {
        g_vmu_pkg.data_len = (int)archive_len;
        g_vmu_pkg.data = archive;
        if (fs_vmu_set_header(fd, &g_vmu_pkg) < 0) {
            printf("[VMU] Failed to set VMU header on %s\n", g_vmu_save_path);
        }
    }

    fs_close(fd);
    free(archive);
    free(padded);
    if (was_buttons_enabled) {
        vmu_set_buttons_enabled(1);
    }
    printf("[VMU] Saved archive %s (%zu bytes)\n", g_vmu_save_path, archive_len);
    return 1;
}

static int seed_vmu_archive_locked(void) {
    if (!g_vmu_available) {
        return 0;
    }

    Singe_log("[VMU] Seeding empty VMU archive at %s\n", g_vmu_save_path);
    return persist_vmu_archive_locked();
}

static void flush_vmu_archive_if_pending(void) {
    if (!atomic_load(&g_vmu_flush_pending)) {
        return;
    }

    int defer_until = atomic_load(&g_vmu_flush_defer_until_frame);
    if (defer_until >= 0 && dcfmv_current) {
        int cur = dcfmv_frame_index(dcfmv_current);
        int seek_active = dcfmv_seek_active(dcfmv_current);
        if (seek_active || cur <= defer_until) {
            return;
        }
    }

    if (!atomic_exchange(&g_vmu_flush_pending, 0)) {
        return;
    }

    atomic_store(&g_vmu_flush_defer_until_frame, -1);
    int was_muted = dcfmv_current ? dcfmv_audio_muted(dcfmv_current) : 1;
    if (dcfmv_current) {
        dcfmv_set_audio_muted(dcfmv_current, 1);
        dcfmv_reanchor_clock_to_current_frame(dcfmv_current);
    }

    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
    #endif
    if (g_vmu_ready && g_vmu_available) {
        persist_vmu_archive_locked();
    }
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
    #endif

    if (dcfmv_current) {
        dcfmv_reanchor_clock_to_current_frame(dcfmv_current);
        if (!was_muted && !dcfmv_is_paused(dcfmv_current)) {
            dcfmv_set_audio_muted(dcfmv_current, 0);
        }
    }
}

static void update_vmu_lcd(void) {
    if (g_vmu_lcd_icon) {
        vmu_set_icon(g_vmu_lcd_icon);
        return;
    }

    vmufb_t fb;
    maple_device_t *dev;
    unsigned int idx = 0;
    const vmufb_font_t *font = vmu_get_font();
    char title[48];

    if (!font) {
        return;
    }

    vmufb_clear(&fb);
    vmufb_print_string_into(&fb, font,
                            0, 0, VMU_SCREEN_WIDTH, 12, 0,
                            "DCSinge");

    if (GGameName && GGameName[0]) {
        snprintf(title, sizeof(title), "%s", GGameName);
        vmufb_print_string_into(&fb, font,
                                0, 12, VMU_SCREEN_WIDTH, 14, 0,
                                title);
    } else {
        vmufb_print_string_into(&fb, font,
                                0, 12, VMU_SCREEN_WIDTH, 14, 0,
                                "Singe 2");
    }

    vmufb_print_string_into(&fb, font,
                            0, 24, VMU_SCREEN_WIDTH, 8, 0,
                            "VMU");

    while ((dev = maple_enum_type(idx++, MAPLE_FUNC_LCD))) {
        vmufb_present(&fb, dev);
    }
}

static int load_vmu_lcd_icon(void) {
    char lcd_path[256];
    build_root_resource_path("resources/dcsinge_vmu_lcd.txt", lcd_path, sizeof(lcd_path));

    file_t fd = fs_open(lcd_path, O_RDONLY);
    if (fd < 0) {
        printf("[VMU] No LCD icon asset found at %s; using text fallback\n", lcd_path);
        return 0;
    }

    size_t size = fs_total(fd);
    char *raw = malloc(size + 1);
    char *compact = malloc(size + 1);
    if (!raw || !compact) {
        free(raw);
        free(compact);
        fs_close(fd);
        return 0;
    }

    if (!read_exact(fd, raw, size)) {
        free(raw);
        free(compact);
        fs_close(fd);
        return 0;
    }
    fs_close(fd);
    raw[size] = '\0';

    size_t out = 0;
    for (size_t i = 0; i < size; i++) {
        unsigned char c = (unsigned char)raw[i];
        if (!isspace(c)) {
            compact[out++] = (char)c;
        }
    }
    compact[out] = '\0';

    free(g_vmu_lcd_icon);
    g_vmu_lcd_icon = compact;
    free(raw);

    printf("[VMU] Loaded LCD icon asset (%zu chars)\n", out);
    return 1;
}

static void clear_io_cache(void) {
    SingeLuaFileCache *entry = g_io_cache;
    while (entry) {
        SingeLuaFileCache *next = entry->next;
        free(entry->path);
        free(entry->data);
        free(entry);
        entry = next;
    }
    g_io_cache = NULL;

    free(g_io_output.path);
    g_io_output.path = NULL;
    free(g_io_output.data);
    g_io_output.data = NULL;
    g_io_output.len = 0;
    g_io_output.cap = 0;
    g_io_output.active = 0;

    g_io_input.entry = NULL;
    g_io_input.pos = 0;
    g_io_input.active = 0;

    g_vmu_ready = 0;
    g_vmu_available = 0;
    g_vmu_mount_path[0] = '\0';
    g_vmu_save_name[0] = '\0';
    g_vmu_save_path[0] = '\0';
    g_vmu_icon_path[0] = '\0';
    free(g_vmu_lcd_icon);
    g_vmu_lcd_icon = NULL;
    memset(&g_vmu_pkg, 0, sizeof(g_vmu_pkg));
    memset(g_vmu_icon_data, 0, sizeof(g_vmu_icon_data));
}

static int call_original_io_n(lua_State *L, int ref, int nargs) {
    if (ref == LUA_NOREF) {
        lua_pushnil(L);
        lua_pushliteral(L, "original io function unavailable");
        return 2;
    }

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    if (nargs > 0) {
        lua_insert(L, -(nargs + 1));
    }
    if (lua_pcall(L, nargs, LUA_MULTRET, 0) != LUA_OK) {
        return lua_error(L);
    }
    return lua_gettop(L);
}

static int ensure_output_capacity(size_t extra) {
    size_t needed = g_io_output.len + extra + 1;
    if (needed <= g_io_output.cap) {
        return 1;
    }

    size_t new_cap = g_io_output.cap ? g_io_output.cap : 256;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    char *new_data = realloc(g_io_output.data, new_cap);
    if (!new_data) {
        return 0;
    }

    g_io_output.data = new_data;
    g_io_output.cap = new_cap;
    return 1;
}

static int commit_output_buffer_locked(void) {
    if (!g_io_output.active || !g_io_output.path) {
        return 1;
    }

    SingeLuaFileCache *entry = upsert_io_cache_entry(
        g_io_output.path,
        g_io_output.data ? g_io_output.data : "",
        g_io_output.len,
        1
    );
    if (!entry) {
        return 0;
    }

    free(g_io_output.path);
    g_io_output.path = NULL;
    free(g_io_output.data);
    g_io_output.data = NULL;
    g_io_output.len = 0;
    g_io_output.cap = 0;
    g_io_output.active = 0;
    return 1;
}

static int read_shadow_line(lua_State *L) {
    if (!g_io_input.active || !g_io_input.entry) {
        return 0;
    }

    const char *data = g_io_input.entry->data;
    size_t len = g_io_input.entry->len;
    size_t pos = g_io_input.pos;

    if (pos >= len) {
        lua_pushnil(L);
        return 1;
    }

    size_t start = pos;
    while (pos < len && data[pos] != '\n' && data[pos] != '\r') {
        pos++;
    }

    lua_pushlstring(L, data + start, pos - start);

    if (pos < len) {
        if (data[pos] == '\r' && (pos + 1) < len && data[pos + 1] == '\n') {
            pos += 2;
        } else {
            pos++;
        }
    }

    g_io_input.pos = pos;
    return 1;
}

static int read_shadow_all(lua_State *L) {
    if (!g_io_input.active || !g_io_input.entry) {
        return 0;
    }

    const char *data = g_io_input.entry->data;
    size_t len = g_io_input.entry->len;
    size_t pos = g_io_input.pos;

    if (pos >= len) {
        lua_pushliteral(L, "");
        return 1;
    }

    lua_pushlstring(L, data + pos, len - pos);
    g_io_input.pos = len;
    return 1;
}

static int read_shadow_bytes(lua_State *L, size_t count) {
    if (!g_io_input.active || !g_io_input.entry) {
        return 0;
    }

    const char *data = g_io_input.entry->data;
    size_t len = g_io_input.entry->len;
    size_t pos = g_io_input.pos;

    if (pos >= len) {
        lua_pushnil(L);
        return 1;
    }

    size_t avail = len - pos;
    size_t take = count < avail ? count : avail;
    lua_pushlstring(L, data + pos, take);
    g_io_input.pos += take;
    return 1;
}

static int custom_io_input(lua_State *L) {
    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        if (g_io_input.active) {
            lua_pushlightuserdata(L, &g_io_input_token);
            return 1;
        }
        return call_original_io_n(L, g_orig_io_input_ref, 0);
    }

    const char *filename = luaL_checkstring(L, 1);
    char *fullpath = resolve_path(filename);
    if (!fullpath) {
        return luaL_error(L, "failed to resolve %s", filename);
    }
    char key[512];
    canonicalize_io_key(fullpath, key, sizeof(key));

    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
    #endif

    SingeLuaFileCache *entry = find_io_cache_entry(key);
    if (!entry) {
        entry = load_io_cache_entry_from_source(fullpath);
    }

    if (entry) {
        g_io_input.entry = entry;
        g_io_input.pos = 0;
        g_io_input.active = 1;
        printf("[Custom io.input] Shadow read enabled for: %s\n", filename);
        free(fullpath);
        #if USE_IO_MUTEX
            mutex_unlock(&io_lock);
        #endif
        lua_pushlightuserdata(L, &g_io_input_token);
        return 1;
    }

    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
    #endif

    printf("[Custom io.input] Falling back to standard io.input for: %s\n", filename);
    free(fullpath);
    return call_original_io_n(L, g_orig_io_input_ref, 1);
}

static int custom_io_read(lua_State *L) {
    if (!g_io_input.active) {
        int nargs = lua_gettop(L);
        return call_original_io_n(L, g_orig_io_read_ref, nargs);
    }

    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        return read_shadow_line(L);
    }

    if (lua_isnumber(L, 1)) {
        lua_Integer count = lua_tointeger(L, 1);
        if (count < 0) {
            return luaL_error(L, "invalid read length");
        }
        return read_shadow_bytes(L, (size_t)count);
    }

    const char *fmt = luaL_checkstring(L, 1);
    if (strcmp(fmt, "*line") == 0 || strcmp(fmt, "*l") == 0) {
        return read_shadow_line(L);
    }
    if (strcmp(fmt, "*a") == 0 || strcmp(fmt, "*all") == 0) {
        return read_shadow_all(L);
    }

    return luaL_error(L, "unsupported shadow io.read format: %s", fmt);
}

static int custom_io_output(lua_State *L) {
    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        if (g_io_output.active) {
            lua_pushlightuserdata(L, &g_io_output_token);
            return 1;
        }
        return call_original_io_n(L, g_orig_io_output_ref, 0);
    }

    const char *filename = luaL_checkstring(L, 1);
    char *fullpath = resolve_path(filename);
    if (!fullpath) {
        return luaL_error(L, "failed to resolve %s", filename);
    }
    char key[512];
    canonicalize_io_key(fullpath, key, sizeof(key));

    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
    #endif

    if (g_io_output.active) {
        if (!commit_output_buffer_locked()) {
            #if USE_IO_MUTEX
                mutex_unlock(&io_lock);
            #endif
            free(fullpath);
            return luaL_error(L, "failed to switch output to %s", filename);
        }
    }

    g_io_output.path = strdup(key);
    free(fullpath);
    if (!g_io_output.path) {
        #if USE_IO_MUTEX
            mutex_unlock(&io_lock);
        #endif
        return luaL_error(L, "out of memory while opening %s", filename);
    }
    g_io_output.active = 1;
    g_io_output.len = 0;
    g_io_output.cap = 0;
    g_io_output.data = NULL;

    printf("[Custom io.output] Shadow write enabled for: %s\n", filename);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
    #endif

    lua_pushlightuserdata(L, &g_io_output_token);
    return 1;
}

static int custom_io_write(lua_State *L) {
    if (!g_io_output.active) {
        int nargs = lua_gettop(L);
        return call_original_io_n(L, g_orig_io_write_ref, nargs);
    }

    int nargs = lua_gettop(L);
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
    #endif

    for (int i = 1; i <= nargs; i++) {
        size_t len = 0;
        const char *chunk = luaL_checklstring(L, i, &len);
        if (!ensure_output_capacity(len)) {
            #if USE_IO_MUTEX
                mutex_unlock(&io_lock);
            #endif
            return luaL_error(L, "out of memory while buffering io.write");
        }
        memcpy(g_io_output.data + g_io_output.len, chunk, len);
        g_io_output.len += len;
        g_io_output.data[g_io_output.len] = '\0';
    }

    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
    #endif

    lua_pushboolean(L, 1);
    return 1;
}

static int custom_io_close(lua_State *L) {
    void *handle = lua_isnoneornil(L, 1) ? NULL : lua_touserdata(L, 1);

    if (handle == &g_io_output_token || (!handle && g_io_output.active)) {
        #if USE_IO_MUTEX
            mutex_lock(&io_lock);
        #endif
        int ok = commit_output_buffer_locked();
        if (ok) {
            atomic_store(&g_vmu_flush_pending, 1);
        }
        #if USE_IO_MUTEX
            mutex_unlock(&io_lock);
        #endif
        if (!ok) {
            return luaL_error(L, "failed to close shadow output");
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    if (handle == &g_io_input_token || (!handle && g_io_input.active)) {
        g_io_input.entry = NULL;
        g_io_input.pos = 0;
        g_io_input.active = 0;
        lua_pushboolean(L, 1);
        return 1;
    }

    int nargs = lua_gettop(L);
    return call_original_io_n(L, g_orig_io_close_ref, nargs);
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

    lua_getfield(L, -1, "input");
    g_orig_io_input_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "read");
    g_orig_io_read_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "output");
    g_orig_io_output_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "write");
    g_orig_io_write_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, -1, "close");
    g_orig_io_close_ref = luaL_ref(L, LUA_REGISTRYINDEX);

    // Replace io.input
    lua_pushcfunction(L, custom_io_input);
    lua_setfield(L, -2, "input");

    // Replace io.read
    lua_pushcfunction(L, custom_io_read);
    lua_setfield(L, -2, "read");

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
    GLua = lua_newstate(Singe_lua_allocator, NULL, 0);
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

    // Now Lua scripts using io.input/io.read/io.write/io.close will be patched.
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

    //custom dreamcast api extensions below
    lua_register(GLua, "overlayLinesBatch", sep_overlay_lines_batch);
    lua_register(GLua, "overlayPlotsBatch", sep_overlay_plots_batch);
    lua_register(GLua, "overlayBoxesBatch", sep_overlay_boxes_batch);

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
    #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    file_t fd = fs_open(script_path, O_RDONLY);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif
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
            #if USE_IO_MUTEX
        mutex_lock(&io_lock);
#endif
    fs_close(fd);
    #if USE_IO_MUTEX
        mutex_unlock(&io_lock);
#endif
    if (rc != 0) {
        printf("Error loading script: %s\n", lua_tostring(GLua, -1));
        exit(1);
    }
    printf("    ✓ Lua script loaded\n");

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
printf("[8.7] Injecting string.sub Lua 5.3 compatibility patch...\n");
const char *string_sub_patch =
    "local original_string_sub = string.sub\n"
    "string.sub = function(s, i, j)\n"
    "    if j == nil then\n"
    "        j = #s\n"
    "    end\n"
    "    return original_string_sub(s, i, j)\n"
    "end\n";

if (luaL_dostring(GLua, string_sub_patch) != 0) {
    printf("Error injecting string.sub fix: %s\n", lua_tostring(GLua, -1));
    lua_pop(GLua, 1);
} else {
    printf("    ✓ string.sub Lua 5.3 compatibility installed\n");
}

printf("[8.8] Injecting tonumber compatibility patch...\n");
const char *tonumber_fix_patch =
    "local original_tonumber = tonumber\n"
    "tonumber = function(s, base)\n"
    "    if type(s) == 'string' then\n"
    "        s = s:match('^%s*(.-)%s*$')\n"
    "    end\n"
    "    if base ~= nil and type(base) == 'number' then\n"
    "        if base < 2 or base > 36 then\n"
    "            base = nil\n"
    "        end\n"
    "    end\n"
    "    return original_tonumber(s, base)\n"
    "end\n";

if (luaL_dostring(GLua, tonumber_fix_patch) != 0) {
    printf("Error injecting tonumber fix: %s\n", lua_tostring(GLua, -1));
    lua_pop(GLua, 1);
} else {
    printf("    ✓ tonumber compatibility installed\n");
}
    // snd_mem_init(512000); // 5MB sound buffer for Singe audio system
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
    pvr_wait_ready();
    pvr_scene_begin();
    pvr_list_begin(PVR_LIST_OP_POLY);

    if (!atomic_load(&g_clip_boundary_hold)) {
        fmv_tick(monotonic_ms);
    }

    render_current_video();
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);

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
}

static int pal_menu(void) {
    maple_device_t *cont1;
    cont_state_t *state;

    /* Re-init to a 50Hz mode to display the menu. */
    vid_set_mode(DM_640x480_PAL_IL, PM_RGB565);

    /* Draw the "menu" on the screen. */
    bfont_draw_str(vram_s + 640 * 200 + 64, 640, 1, "Press A to run at 60Hz");
    bfont_draw_str(vram_s + 640 * 240 + 64, 640, 1, "or B to run at 50Hz");

    /* Wait for the user to press either A or B to pick which mode to use.*/
    for(;;) {
        if((cont1 = maple_enum_type(0, MAPLE_FUNC_CONTROLLER))) {
            if((state = (cont_state_t *)maple_dev_status(cont1))) {
                if(state->buttons & CONT_A)
                    return USE_60HZ;
                else if(state->buttons & CONT_B)
                    return USE_50HZ;
            }
        }

        /* Sleep for a bit. */
        thd_sleep(20);
    }
}

// Initialization
void singe_startup(const char *gamedir, const char *videopath) {
    if (!dcfmv_current) {
        dcfmv_current = dcfmv_create(DCFMV_PRESENT_CLIENT);
        if (!dcfmv_current) {
            printf("PANIC: Failed to allocate FMV module state\n");
            exit(1);
        }
    }
    GGameDir = Singe_xstrdup(gamedir);
    GGamePath = Singe_xstrdup(videopath);
    dcfmv_control_reset();
    if (dcfmv_open(dcfmv_current, videopath) != 0) {
        printf("PANIC: Failed to open DCMV file\n");
        exit(1);
    }
    dcfmv_t *fmv = dcfmv_current;
    const dcfmv_media_info_t *info = dcfmv_media_info(fmv);

    dcfmv_set_audio_muted(fmv, 1);
    dcfmv_set_preload_paused(fmv, 1);

    if (g_cfg_disable_fmv_audio) {
        printf("   FMV audio disabled by config; KOS streaming will not start.\n");
        dcfmv_set_audio_enabled(fmv, 0);
    }
    dcfmv_set_audio_clock_mode(fmv, dcfmv_audio_channels(fmv) > 0);
    Singe_log("[FMV] startup audio mode: disable_fmv_audio=%d audio_channels=%d clock=%s",
              g_cfg_disable_fmv_audio,
              dcfmv_audio_channels(fmv),
              dcfmv_audio_channels(fmv) > 0 ? "audio" : "fps");

    printf("📦 Header v%lu: %s %dx%d (content: %dx%d) @ %.2ffps, %dHz, %dch, unique=%d, total=%d\n",
        info ? (unsigned long)info->version : 0ul,
        info && info->frame_type == 1 ? "YUV422" : "RGB565",
        info ? info->tex_width : 0, info ? info->tex_height : 0,
        info ? info->content_width : 0, info ? info->content_height : 0,
        info ? info->fps : 0.0f, info ? info->sample_rate : 0, dcfmv_audio_channels(fmv),
        info ? (int)info->num_unique_frames : 0,
        info ? (int)info->num_total_frames : 0);

    printf("   Frame size: %lu, Max compressed: %lu, Audio offset: 0x%lX, Compression: %s\n",
        info ? (unsigned long)info->uncompressed_frame_size : 0ul,
        info ? (unsigned long)info->max_compressed_frame_size : 0ul,
        (unsigned long)dcfmv_audio_offset(fmv),
        info && info->compression_type == 1 ? "Zstandard" : "LZ4");


    // Initialize video/audio
    is_320 = 0;//(video_width == 320);
    
    g_display_w = 640;
    g_display_h = 480;
    
    // Scale 640x480 overlay to current display size (320x240 or 640x480)
    UI_SCALE_X = 1.0f;
    UI_SCALE_Y = 1.0f;
    UI_OFFSET_X = 0;
    UI_OFFSET_Y = 0;

    // Initialize PVR
    pvr_init_defaults();
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    draw_startup_intro();
    
    int use_strided = !(info && is_pow2(info->tex_width) && is_pow2(info->tex_height));
    int pot_w = 1, pot_h = 1;
    while (info && pot_w < info->tex_width) pot_w <<= 1;
    while (info && pot_h < info->tex_height) pot_h <<= 1;
    
    pvr_txr = pvr_mem_malloc(pot_w * pot_h * 2);
    
    pvr_poly_cxt_t cxt;
    uint32_t fmt = (info && info->frame_type == 1) ? PVR_TXRFMT_YUV422 : PVR_TXRFMT_RGB565 | PVR_TXRFMT_VQ_ENABLE;
    if (use_strided) fmt |= PVR_TXRFMT_NONTWIDDLED | (1 << 25) | PVR_TXRFMT_VQ_ENABLE;
    else fmt |= PVR_TXRFMT_TWIDDLED | PVR_TXRFMT_VQ_ENABLE;
    
    pvr_poly_cxt_txr(&cxt, PVR_LIST_OP_POLY, fmt, pot_w, pot_h, pvr_txr, PVR_FILTER_NONE);
    pvr_poly_compile(&hdr, &cxt);
    // hdr.mode3 &= ~(0x3f<<21);
    // if (use_strided) pvr_txr_set_stride(video_width);
    if (use_strided && info) PVR_SET(PVR_TEXTURE_MODULO, (info->tex_width / 32));
    printf("[PVR] frame_type=%u use_strided=%d tex=%ux%u pot=%dx%d fmt=0x%08x modulo=%u\n",
           info ? (unsigned)info->frame_type : 0u,
           use_strided,
           info ? (unsigned)info->tex_width : 0u,
           info ? (unsigned)info->tex_height : 0u,
           pot_w,
           pot_h,
           (unsigned)fmt,
           (unsigned)((use_strided && info) ? (info->tex_width / 32) : 0));

    pvr_poly_cxt_col(&cxt, PVR_LIST_OP_POLY);
    pvr_poly_compile(&fallback_hdr, &cxt);
    
    float u1 = info ? (float)info->content_width / (float)pot_w : 0.0f;
    float v1 = info ? (float)info->content_height / (float)pot_h : 0.0f;
    
    vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .u=0, .v=0, .argb=0xFFFFFFFF};
    vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=g_display_w, .y=0, .z=1, .u=u1, .v=0, .argb=0xFFFFFFFF};
    vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=g_display_h, .z=1, .u=0, .v=v1, .argb=0xFFFFFFFF};
    vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL, .x=g_display_w, .y=g_display_h, .z=1, .u=u1, .v=v1, .argb=0xFFFFFFFF};

    fallback_vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .argb=0xFFFF0000};
    fallback_vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=g_display_w, .y=0, .z=1, .argb=0xFFFF0000};
    fallback_vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=g_display_h, .z=1, .argb=0xFFFF0000};
    fallback_vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL, .x=g_display_w, .y=g_display_h, .z=1, .argb=0xFFFF0000};
    dcfmv_set_render_resources(fmv, pvr_txr, &hdr, &fallback_hdr, vert, fallback_vert);
    dcfmv_reset_render_tracking(fmv);
    log_memory_stats("after_fmv_alloc");

    // GDecoderActive = 1;

    /*
     * Base sound system must be initialized before any SFX loads, even when
     * FMV audio is present. The FMV path owns the streaming setup itself.
     */
    snd_init();

    if (dcfmv_audio_channels(fmv) > 0) {
        if (dcfmv_audio_init(fmv) != 0) {
            printf("PANIC: dcfmv_audio_init failed\n");
            exit(1);
        }
    } else {
        if (g_cfg_enable_mp3) {
            printf("[Music] Initializing KOS stream subsystem for MP3-only mode...\n");
            snd_stream_init();
            g_mp3_stream_inited = 1;
        }
    }

        
    // Setup Lua
    setup_lua();
    log_memory_stats("after_setup_lua");
    Singe_log("Singe startup complete - %u total frames at %.2f fps",
              info ? info->num_total_frames : 0u,
              info ? info->fps : 0.0f);
    // int retries = 0;
    // while (atomic_load(&frame_index) == 0 && retries < 50) {  // ~1 second wait
    //     thd_sleep(20);
    //     retries++;
    // }

    if (g_cfg_enable_mp3) {
        sep_music_init(); // libmp3
    } else {
        printf("[Music] MP3 disabled by config\n");
        g_current_playing_handle = -1;
    }


    // Initialize audio
    /* Stream slot was already allocated and started by dcfmv_audio_init(). */
    dcfmv_set_audio_muted(fmv, 1);

    worker_thread_id = thd_create(0, worker_thread, NULL);


    // ✅ Initialize timing but don't start clocks
    dcfmv_reset_timing(fmv);
    dcfmv_set_audio_muted(fmv, 1);
    printf("   Decoder thread started\n");
    dcfmv_set_paused(fmv, 1);
    dcfmv_set_preload_paused(fmv, 1);
    dcfmv_set_audio_muted(fmv, 1);
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
            else if (strcmp(line, "vmu_icon_path") == 0)
                strncpy(G_VMU_ICON_FILE, eq, sizeof(G_VMU_ICON_FILE));
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
            else if (strcmp(line, "disable_audio") == 0)
                g_cfg_disable_fmv_audio = atoi(eq) != 0;
            else if (strcmp(line, "enable_mp3") == 0)
                g_cfg_enable_mp3 = atoi(eq) != 0;
        } else {
            line[pos++] = c;
        }
    }
    fs_close(fd);

    if (G_VMU_ICON_FILE[0] == '\0') {
        strncpy(G_VMU_ICON_FILE, "resources/dcsinge_vmu_icon.ico", sizeof(G_VMU_ICON_FILE));
        G_VMU_ICON_FILE[sizeof(G_VMU_ICON_FILE) - 1] = '\0';
    }

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
    printf("  VMU Icon: %s\n", G_VMU_ICON_FILE);
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
        maple_device_t *dev = maple_enum_dev(port, 0);
        if (!dev || !dev->valid || !(dev->info.functions & MAPLE_FUNC_CONTROLLER)) {
            prevbits[port] = 0;
            continue;
        }

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

                int lua_switch_num = switch_num;
                if (switch_num >= PLAYER2_OFFSET) {
                    lua_switch_num = switch_num - PLAYER2_OFFSET;
                }

                const char *event = pressed ? "onInputPressed" : "onInputReleased";
                lua_getglobal(GLua, event);
                if (lua_isfunction(GLua, -1)) {
                    DC_log("DEBUG: Sending event '%s' for Player %d, switch_num %d\n", event, port + 1, lua_switch_num);  // Debugging line
                    lua_pushinteger(GLua, lua_switch_num);
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
        static int GMouseX[2] = {180, 180};
        static int GMouseY[2] = {120, 120};

        GMouseX[port] += relX;
        GMouseY[port] += relY;

        /*
         * Dreamcast analog mouse emulation: Singe scripts expect logical
         * overlay coordinates here. The Lua layer applies ratio/gun offsets,
         * and spriteDraw() handles the final display transform.
         */
        if (GMouseX[port] < 0) GMouseX[port] = 0;
        else if (GMouseX[port] > GOverlayWidth) GMouseX[port] = GOverlayWidth;
        if (GMouseY[port] < 0) GMouseY[port] = 0;
        else if (GMouseY[port] > GOverlayHeight) GMouseY[port] = GOverlayHeight;

        /*
         * Report X in the same ratio-expanded coordinate space used by PC
         * Singe gun scripts. Those scripts subtract ratioxOffset after
         * multiplying by ratioGetX(), so feeding physical overlay X directly
         * makes shots land left of their 320-authored hitboxes.
         */
        int mouse_x = (int)roundf(GMouseX[port] + g_ratio_x_offset);
        int mouse_y = GMouseY[port];

        int relMouseX = relX;
        int relMouseY = relY;

        Singe_log("[MOUSE] Singe:(%d,%d) overlay:(%d,%d) rel=(%d,%d) size=%dx%d\n",
            mouse_x, mouse_y,
            GMouseX[port], GMouseY[port],
            relMouseX, relMouseY,
            GOverlayWidth, GOverlayHeight);

        lua_getglobal(GLua, "onMouseMoved");
        if (lua_isfunction(GLua, -1)) {
            lua_pushinteger(GLua, mouse_x);
            lua_pushinteger(GLua, mouse_y);
            lua_pushinteger(GLua, relMouseX);
            lua_pushinteger(GLua, relMouseY);
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




int main(int argc, char **argv) {
    (void)argc; (void)argv;

    printf("Singe 2 for Dreamcast\n");

    cont_btn_callback(0,
        CONT_START | CONT_A | CONT_B | CONT_X | CONT_Y,
        (cont_btn_callback_t)request_exit_callback);
        
    /* KOS normally initializes the video hardware to run at 60Hz, so on NTSC
       consoles, or those with VGA connections, we don't have to do anything
       else here... */
    int region, cable, mode;
    region = flashrom_get_region();
    cable = vid_check_cable();

    /* So, if we detect a European console that isn't using VGA, prompt the user
       whether they want 50Hz mode or 60Hz mode. */
    if(region == FLASHROM_REGION_EUROPE && cable != CT_VGA) {
        mode = pal_menu();

        if(mode == USE_60HZ)
            vid_set_mode(DM_640x480_NTSC_IL, PM_RGB565);
        else /* if(mode == USE_50HZ) */
            vid_set_mode(DM_640x480_PAL_IL, PM_RGB565);
    }

    load_config();
    log_memory_stats("after_load_config");
    init_vmu_context();
    load_vmu_lcd_icon();
    update_vmu_lcd();
    log_memory_stats("after_vmu_init");

#ifndef DCSINGE_GDB_BREAK
#define DCSINGE_GDB_BREAK 0
#endif
#if DCSINGE_GDB_BREAK
        gdb_init();
        gdb_breakpoint();
#endif

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
    // frame_timer_anchor = dcfmv_ps_ms();
    // atomic_store(&audio_start_time_ms,
    //             (double)atomic_load(&frame_index) * (1000.0 / (double)fps));
    // Singe_log("[Sync] Initialized frame_timer_anchor=%.3fms, audio_start_time_ms=%.3f",
    //         (double)dcfmv_ps_ms(), atomic_load(&audio_start_time_ms));    
    // dbgio_dev_select("fb");
    while (!atomic_load(&g_exit_requested)) {
        uint64_t now_ms = (uint64_t)dcfmv_ps_ms();
        // uint64_t inputbits = poll_controller_input();
        // singe_tick(now_ms, inputbits);
        poll_and_handle_input(); 
        if (dcfmv_current) {
          dcfmv_audio_poll(dcfmv_current);
        }
        singe_tick(now_ms);
        flush_vmu_archive_if_pending();
        pace_main_loop();
    }

    printf("Singe shutdown requested, cleaning up...\n");
    singe_shutdown();

    return 0;
}
