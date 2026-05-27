// singe_dreamcast.c - Hypseus Singe 2 API port/simulation for Dreamcast
// Based on Hypseus Singe - https://github.com/DirtBagXon/hypseus-singe
// and Singe 2 - https://forge.duensing.digital/Public_Skunkworks/singe.git
// uses my dreamcast-fmv encoder for media creation - https://github.com/GPF/dreamcast-fmv

#ifndef DCSINGE_ENABLE_KOSFAT_STORAGE
#define DCSINGE_ENABLE_KOSFAT_STORAGE 0
#endif

#include <kos.h>
#include <dc/sound/stream.h>
#include <dc/sound/sound.h>
#include <dc/pvr.h>
#include <dc/video.h>
#include <stdatomic.h>
#include <ctype.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/lualib.h"
#include "lua/lfs.h"
#include <png/png.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/vmu_fb.h>
#include <arch/gdb.h>
#include <shz_sh4zam.h>
#if DCSINGE_ENABLE_KOSFAT_STORAGE
#include <dc/g1ata.h>
#include <dc/sd.h>
#include <fat/fs_fat.h>
#endif
#include <ft2build.h>
#include FT_FREETYPE_H
#include <dc/fs_vmu.h>
#include <dc/vmu_pkg.h>
#include "dcfmv.h"

#ifndef SINGE_DEBUG_LOGS
#define SINGE_DEBUG_LOGS 1
#endif

#ifndef SINGE_DEBUG_LOG_GENERAL
#define SINGE_DEBUG_LOG_GENERAL 1
#endif

#ifndef SINGE_DEBUG_LOG_FRAMEFILE
#define SINGE_DEBUG_LOG_FRAMEFILE 0
#endif

#ifndef SINGE_DEBUG_LOG_FRAMEFILE_SEGMENT
#define SINGE_DEBUG_LOG_FRAMEFILE_SEGMENT 1
#endif

#ifndef SINGE_DEBUG_LOG_VMU
#define SINGE_DEBUG_LOG_VMU 1
#endif

#ifndef SINGE_DEBUG_LOG_MEMORY
#define SINGE_DEBUG_LOG_MEMORY 1
#endif

#ifndef SINGE_DEBUG_LOG_INPUT
#define SINGE_DEBUG_LOG_INPUT 1
#endif

#ifndef SINGE_DEBUG_LOG_SFX
#define SINGE_DEBUG_LOG_SFX 0
#endif

#ifndef SINGE_DEBUG_LOG_OVERLAY
#define SINGE_DEBUG_LOG_OVERLAY 1
#endif

#ifndef DCSINGE_USE_PVR_VERTBUF_BATCH
#define DCSINGE_USE_PVR_VERTBUF_BATCH 1
#endif

#ifndef DCSINGE_ENABLE_LUA53_COMPAT_PATCHES
#define DCSINGE_ENABLE_LUA53_COMPAT_PATCHES 0
#endif

#ifndef DCSINGE_DEBUG_PVR_BATCH
#define DCSINGE_DEBUG_PVR_BATCH 1
#endif

#ifndef DCSINGE_PVR_VERTBUF_TR_BUDGET_BYTES
#define DCSINGE_PVR_VERTBUF_TR_BUDGET_BYTES (192 * 1024)
#endif

#ifndef DCSINGE_PVR_QUAD_BATCH_MAX
#define DCSINGE_PVR_QUAD_BATCH_MAX 64
#endif

#ifndef DCSINGE_PVR_TR_VERTBUF_BYTES
#define DCSINGE_PVR_TR_VERTBUF_BYTES (DCSINGE_PVR_VERTBUF_TR_BUDGET_BYTES * 2)
#endif

/*
 * Mouse X feed mode into Lua onMouseMoved:
 *   0 = legacy offset (overlay + ratio_x_offset)
 *   1 = direct overlay x
 *   2 = inverse-ratio ((overlay + ratio_x_offset) / ratio_x)
 */
#ifndef SINGE_MOUSE_SEND_MODE_DEFAULT
#define SINGE_MOUSE_SEND_MODE_DEFAULT 0
#endif

enum {
    SINGE_LOG_GENERAL           = 1 << 0,
    SINGE_LOG_FRAMEFILE         = 1 << 1,
    SINGE_LOG_FRAMEFILE_SEGMENT = 1 << 2,
    SINGE_LOG_VMU               = 1 << 3,
    SINGE_LOG_MEMORY            = 1 << 4,
    SINGE_LOG_INPUT             = 1 << 5,
    SINGE_LOG_SFX               = 1 << 6,
    SINGE_LOG_OVERLAY           = 1 << 7
};

#define SINGE_DEBUG_LOG_MASK_DEFAULT ( \
    ((SINGE_DEBUG_LOG_GENERAL           ? SINGE_LOG_GENERAL           : 0) | \
     (SINGE_DEBUG_LOG_FRAMEFILE         ? SINGE_LOG_FRAMEFILE         : 0) | \
     (SINGE_DEBUG_LOG_FRAMEFILE_SEGMENT ? SINGE_LOG_FRAMEFILE_SEGMENT : 0) | \
     (SINGE_DEBUG_LOG_VMU               ? SINGE_LOG_VMU               : 0) | \
     (SINGE_DEBUG_LOG_MEMORY            ? SINGE_LOG_MEMORY            : 0) | \
     (SINGE_DEBUG_LOG_INPUT             ? SINGE_LOG_INPUT             : 0) | \
     (SINGE_DEBUG_LOG_SFX               ? SINGE_LOG_SFX               : 0) | \
     (SINGE_DEBUG_LOG_OVERLAY           ? SINGE_LOG_OVERLAY           : 0)))

#define USE_50HZ 0
#define USE_60HZ 1

#ifndef SINGE_USE_IO_MUTEX
#define SINGE_USE_IO_MUTEX 1
#endif

#if SINGE_USE_IO_MUTEX
static mutex_t singe_io_lock = MUTEX_INITIALIZER;
#define SINGE_IO_LOCK() mutex_lock(&singe_io_lock)
#define SINGE_IO_UNLOCK() mutex_unlock(&singe_io_lock)
#else
#define SINGE_IO_LOCK() do { } while (0)
#define SINGE_IO_UNLOCK() do { } while (0)
#endif

// ---------------------------------------------------------------------------
// 🎮 Singe Dreamcast runtime configuration (auto-loaded from singe.cfg)
// ---------------------------------------------------------------------------
#define SINGE_VERSION       2.10
char G_BASE_PATH[128]   = "/pc/data/";   // Auto-set from /pc, /cd, /sd/DCSinge, or /ide/DCSinge
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
static int g_cfg_chunk_cache_slots = DCFMV_CHUNK_CACHE_SLOTS_DEFAULT;
static int g_cfg_chunk_initial_preload_chunks = DCFMV_CHUNK_INITIAL_PRELOAD_DEFAULT;
static int g_cfg_chunk_audio_ring_slots = DCFMV_CHUNK_AUDIO_RING_SLOTS_DEFAULT;
static int g_cfg_crosshair_offset_x = 0;
static int g_cfg_crosshair_offset_y = 0;
static int g_cfg_hitbox_draw = 1;
static int g_cfg_mouse_send_mode = SINGE_MOUSE_SEND_MODE_DEFAULT;
static float g_cfg_joymouse_deadzone = 15.0f;
static float g_cfg_joymouse_response = 1.5f;
static float g_cfg_joymouse_smooth = 0.3f;
static float g_cfg_joymouse_speed = 14.0f;
static int g_cfg_aim_assist = 0;
static int g_cfg_aim_assist_when_firing = 1;
static float g_cfg_aim_assist_strength = 0.35f;
static float g_cfg_aim_assist_max_step = 18.0f;
static float g_cfg_aim_assist_radius = 48.0f;
static int g_cfg_aim_assist_hitbox_timeout_ms = 150;
static int g_cfg_aim_assist_red_only = 0;
static int g_aim_assist_capture_active = 0;
static int g_disc_cfg_crosshair_offset_x = 0;
static int g_disc_cfg_crosshair_offset_y = 0;
static int g_disc_cfg_hitbox_draw = 1;
static int g_disc_cfg_mouse_send_mode = SINGE_MOUSE_SEND_MODE_DEFAULT;
static float g_disc_cfg_joymouse_deadzone = 15.0f;
static float g_disc_cfg_joymouse_response = 1.5f;
static float g_disc_cfg_joymouse_smooth = 0.3f;
static float g_disc_cfg_joymouse_speed = 14.0f;
static int g_disc_cfg_aim_assist = 0;
static int g_disc_cfg_aim_assist_when_firing = 1;
static float g_disc_cfg_aim_assist_strength = 0.35f;
static float g_disc_cfg_aim_assist_max_step = 18.0f;
static float g_disc_cfg_aim_assist_radius = 48.0f;
static int g_disc_cfg_aim_assist_hitbox_timeout_ms = 150;
static int g_disc_cfg_aim_assist_red_only = 0;
static int g_mp3_stream_inited = 0;
static int g_mp3_init_failed = 0;
static atomic_int g_exit_requested = 0;
static int g_logged_first_clip_start = 0;
static int g_vmu_ready = 0;
static int g_vmu_available = 0;
static atomic_int g_vmu_flush_pending = 0;
static atomic_int g_vmu_flush_defer_until_frame = -1;
static _Atomic uint64_t g_vmu_flush_not_before_ms = 0;
static _Atomic uint64_t g_vmu_transition_flush_until_ms = 0;
static mutex_t g_vmu_flush_lock = MUTEX_INITIALIZER;
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

typedef enum {
    DCSINGE_LDP_STOPPED = 0,
    DCSINGE_LDP_SEARCHING,
    DCSINGE_LDP_PLAYING,
    DCSINGE_LDP_PAUSED,
    DCSINGE_LDP_CLIP_HOLD
} dcsinge_ldp_state_t;

static atomic_int g_ldp_state = DCSINGE_LDP_PAUSED;
static atomic_int g_ldp_post_search_state = DCSINGE_LDP_PAUSED;
static int g_skip_pause_advance_pending = 0;
static int g_skip_pause_advance_frame = -1;

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
static void flush_vmu_archive_before_transition(const char *op, int frame);
static void arm_vmu_transition_flush_window(void);
static int vmu_transition_flush_window_active(void);
static void log_memory_stats(const char *tag);

float  g_ratio_x = 1.0f;
float  g_ratio_y = 1.0f;
float  g_ratio_x_offset = 0.0f;
float  g_ratio_y_offset = 0.0f;
float  g_scale_x = 1.0f;
float  g_scale_y = 1.0f;
static int g_mouse_trace_budget = 1200;
static int g_shot_trace_budget = 240;
static int g_last_hitbox_valid = 0;
static float g_last_hitbox_x1 = 0.0f;
static float g_last_hitbox_y1 = 0.0f;
static float g_last_hitbox_x2 = 0.0f;
static float g_last_hitbox_y2 = 0.0f;
static uint64_t g_last_hitbox_ms = 0;
static uint8_t g_last_hitbox_r = 0;
static uint8_t g_last_hitbox_g = 0;
static uint8_t g_last_hitbox_b = 0;

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
 * Singe scripts request font sizes in overlay coordinates. The draw path scales
 * those overlay coordinates to the Dreamcast screen, so applying another size
 * multiplier here makes menus wider than the scripts expect.
 */
#define SINGE_FONT_SCALE_NUM 1
#define SINGE_FONT_SCALE_DEN 1
#define SINGE_FONT_BIAS_PX 0
#define SINGE_FONT_MIN_PX 8
#define SINGE_FONT_MAX_PX 32
#define SINGE_FONT_PVR_RESERVE_BYTES (256 * 1024)

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
    int is_font_sprite;
    int font_index;
    int frame_count;
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
    uint16_t channels;
    struct SingeSound *next;
} SingeSound;

typedef struct SingeActiveSound {
    int channel;
    uint16_t channels;
} SingeActiveSound;

static SingeSprite *GSprites = NULL;
static SingeSound *GSounds = NULL;
static SingeActiveSound GActiveSounds[64];
static int GActiveSoundsInit = 0;
static lua_State *GLua = NULL;

// Video decoder state (same as Singe)
#define DCMV_MAGIC "DCMV"
// ============================================================================
// Dreamcast Singe Overlay RTT Implementation (non-twiddled ARGB1555)
// Maintains original Lua overlay coordinates (GOverlayWidth/GOverlayHeight)
// ============================================================================

static void Singe_log_mask(unsigned mask, const char *fmt, ...) {
#if !SINGE_DEBUG_LOGS
    (void)mask;
    (void)fmt;
    return;
#else
    if (!(SINGE_DEBUG_LOG_MASK_DEFAULT & mask)) {
        return;
    }

    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    uint64_t now_ms = timer_ms_gettime64();
    printf("[Singe %llu.%03llu] %s\n",
           (unsigned long long)(now_ms / 1000ULL),
           (unsigned long long)(now_ms % 1000ULL),
           buffer);
#endif
}

#define SINGE_LOG(mask, ...) Singe_log_mask((mask), __VA_ARGS__)

void DC_log(const char *fmt, ...) {
#if !SINGE_DEBUG_LOGS
    (void)fmt;
    return;
#else
    if (!(SINGE_DEBUG_LOG_MASK_DEFAULT & SINGE_LOG_GENERAL)) {
        return;
    }

    char buffer[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    uint64_t now_ms = timer_ms_gettime64();
    printf("[DC %llu.%03llu] %s\n",
           (unsigned long long)(now_ms / 1000ULL),
           (unsigned long long)(now_ms % 1000ULL),
           buffer);
#endif
}

void Singe_log(const char *fmt, ...) {
    va_list ap;
    char buffer[512];

#if !SINGE_DEBUG_LOGS
    (void)fmt;
    return;
#else
    if (!(SINGE_DEBUG_LOG_MASK_DEFAULT & SINGE_LOG_GENERAL)) {
        return;
    }

    va_start(ap, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);
    SINGE_LOG(SINGE_LOG_GENERAL, "%s", buffer);
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

typedef struct {
    int start_frame;
    char path[512];
} framefile_segment_t;

static framefile_segment_t *g_framefile_segments = NULL;
static int g_framefile_segment_count = 0;
static int g_framefile_active_segment = -1;
static char g_framefile_manifest_path[512] = "";

static void framefile_clear_segments(void) {
    free(g_framefile_segments);
    g_framefile_segments = NULL;
    g_framefile_segment_count = 0;
    g_framefile_active_segment = -1;
    g_framefile_manifest_path[0] = '\0';
}

static int framefile_path_exists(const char *path) {
    file_t fd;
    if (!path || !*path) return 0;
    fd = fs_open(path, O_RDONLY);
    if (fd < 0) return 0;
    fs_close(fd);
    return 1;
}

#if DCSINGE_ENABLE_KOSFAT_STORAGE
static kos_blockdev_t g_storage_sd_dev;
static kos_blockdev_t g_storage_ide_dev;
static int g_storage_fat_inited = 0;
static int g_storage_sd_inited = 0;
static int g_storage_ide_inited = 0;
static int g_storage_sd_mounted = 0;
static int g_storage_ide_mounted = 0;

static int dcsinge_storage_fat_init(void) {
    if (g_storage_fat_inited) return 0;
    if (fs_fat_init() != 0) {
        printf("[Storage] fs_fat_init failed\n");
        return -1;
    }
    g_storage_fat_inited = 1;
    return 0;
}

static int dcsinge_storage_mount_sd(void) {
    uint8_t partition_type = 0;

    if (g_storage_sd_mounted) return 0;
    if (dcsinge_storage_fat_init() != 0) return -1;

    if (!g_storage_sd_inited) {
        if (sd_init() != 0) {
            printf("[Storage] SD init failed\n");
            return -1;
        }
        g_storage_sd_inited = 1;
    }

    if (sd_blockdev_for_partition(0, &g_storage_sd_dev, &partition_type) != 0) {
        printf("[Storage] SD FAT partition not found\n");
        return -1;
    }

    if (fs_fat_mount("/sd", &g_storage_sd_dev, FS_FAT_MOUNT_READONLY) != 0) {
        printf("[Storage] SD mount failed\n");
        return -1;
    }

    g_storage_sd_mounted = 1;
    printf("[Storage] Mounted SD at /sd, partition type 0x%02x\n", partition_type);
    return 0;
}

static int dcsinge_storage_mount_ide(void) {
    uint8_t partition_type = 0;

    if (g_storage_ide_mounted) return 0;
    if (dcsinge_storage_fat_init() != 0) return -1;

    if (!g_storage_ide_inited) {
        if (g1_ata_init() != 0) {
            printf("[Storage] IDE init failed\n");
            return -1;
        }
        g_storage_ide_inited = 1;
    }

    if (g1_ata_blockdev_for_partition(0, 1, &g_storage_ide_dev, &partition_type) != 0) {
        printf("[Storage] IDE FAT partition not found\n");
        return -1;
    }

    if (fs_fat_mount("/ide", &g_storage_ide_dev, FS_FAT_MOUNT_READONLY) != 0) {
        printf("[Storage] IDE mount failed\n");
        return -1;
    }

    g_storage_ide_mounted = 1;
    printf("[Storage] Mounted IDE at /ide, partition type 0x%02x\n", partition_type);
    return 0;
}

static void dcsinge_storage_shutdown(void) {
    if (g_storage_ide_mounted) {
        fs_fat_unmount("/ide");
        g_storage_ide_mounted = 0;
    }
    if (g_storage_ide_inited) {
        g1_ata_shutdown();
        g_storage_ide_inited = 0;
    }
    if (g_storage_sd_mounted) {
        fs_fat_unmount("/sd");
        g_storage_sd_mounted = 0;
    }
    if (g_storage_sd_inited) {
        sd_shutdown();
        g_storage_sd_inited = 0;
    }
    if (g_storage_fat_inited) {
        fs_fat_shutdown();
        g_storage_fat_inited = 0;
    }
}
#else
static void dcsinge_storage_shutdown(void) {
}
#endif

static file_t dcsinge_try_config_root(const char *root, char *base_out, size_t base_out_sz) {
    char config_path[256];
    file_t fd;

    if (!root || !*root || !base_out || base_out_sz == 0) return -1;
    snprintf(config_path, sizeof(config_path), "%s/data/singe.cfg", root);
    fd = fs_open(config_path, O_RDONLY);
    if (fd < 0) return -1;

    snprintf(base_out, base_out_sz, "%s/data/", root);
    return fd;
}

static file_t dcsinge_try_config_roots(const char *const *roots,
                                       size_t root_count,
                                       char *base_out,
                                       size_t base_out_sz) {
    for (size_t i = 0; i < root_count; i++) {
        file_t fd = dcsinge_try_config_root(roots[i], base_out, base_out_sz);
        if (fd >= 0) return fd;
    }
    return -1;
}

static void framefile_trim(char *s) {
    char *p;
    if (!s) return;
    p = s + strlen(s);
    while (p > s && isspace((unsigned char)p[-1])) {
        *--p = '\0';
    }
    p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static int framefile_resolve_segment_path(const char *manifest_path,
                                          const char *media_name,
                                          int is_first_segment,
                                          char *out,
                                          size_t out_sz) {
    char manifest_dir[512] = "";
    char manifest_stem[256] = "";
    char media_stem[256] = "";
    char candidate[512];
    const char *slash;
    const char *base;
    size_t len;

    if (!manifest_path || !media_name || !out || out_sz == 0) return -1;

    slash = strrchr(manifest_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - manifest_path);
        if (dir_len >= sizeof(manifest_dir)) dir_len = sizeof(manifest_dir) - 1;
        memcpy(manifest_dir, manifest_path, dir_len);
        manifest_dir[dir_len] = '\0';
    }

    base = slash ? slash + 1 : manifest_path;
    len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".txt") == 0) {
        len -= 4;
    }
    if (len >= sizeof(manifest_stem)) len = sizeof(manifest_stem) - 1;
    memcpy(manifest_stem, base, len);
    manifest_stem[len] = '\0';

    base = strrchr(media_name, '/');
    base = base ? base + 1 : media_name;
    len = strlen(base);
    if (len > 4 && strcmp(base + len - 4, ".m2v") == 0) {
        len -= 4;
    } else if (len > 5 && strcmp(base + len - 5, ".dcmv") == 0) {
        len -= 5;
    }
    if (len >= sizeof(media_stem)) len = sizeof(media_stem) - 1;
    memcpy(media_stem, base, len);
    media_stem[len] = '\0';

    if (is_first_segment && manifest_stem[0]) {
        snprintf(candidate, sizeof(candidate), "%s%s.dcmv", G_BASE_PATH, manifest_stem);
        if (framefile_path_exists(candidate)) {
            strncpy(out, candidate, out_sz);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }

    snprintf(candidate, sizeof(candidate), "%s%s.dcmv", G_BASE_PATH, media_stem);
    if (framefile_path_exists(candidate)) {
        strncpy(out, candidate, out_sz);
        out[out_sz - 1] = '\0';
        return 0;
    }

    if (manifest_dir[0]) {
        snprintf(candidate, sizeof(candidate), "%s/%s.dcmv", manifest_dir, media_stem);
        if (framefile_path_exists(candidate)) {
            strncpy(out, candidate, out_sz);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }

    if (manifest_dir[0]) {
        snprintf(candidate, sizeof(candidate), "%s/%s", manifest_dir, media_name);
        len = strlen(candidate);
        if (len > 4 && strcmp(candidate + len - 4, ".m2v") == 0) {
            memcpy(candidate + len - 4, ".dcmv", 6);
            if (framefile_path_exists(candidate)) {
                strncpy(out, candidate, out_sz);
                out[out_sz - 1] = '\0';
                return 0;
            }
        }
    }

    return -1;
}

static int framefile_load_manifest(const char *manifest_path, char *initial_video_path, size_t initial_video_sz) {
    file_t fd;
    char line[512];
    int pos = 0;
    int segment_count = 0;
    framefile_segment_t *segments = NULL;
    int eof_processed = 0;

    if (!manifest_path || !initial_video_path || initial_video_sz == 0) return -1;

    framefile_clear_segments();

    if (!strstr(manifest_path, ".txt")) {
        strncpy(initial_video_path, manifest_path, initial_video_sz);
        initial_video_path[initial_video_sz - 1] = '\0';
        return 0;
    }

    fd = fs_open(manifest_path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    while (fs_read(fd, &line[pos], 1) == 1) {
        char *p, *end;
        long start_frame;
        char media[256];
        char resolved[512];

        if (line[pos] == '\r') continue;
        if (line[pos] != '\n' && pos < (int)sizeof(line) - 1) {
            pos++;
            continue;
        }

        line[pos] = '\0';
        pos = 0;

        p = line;
        framefile_trim(p);
        if (*p == '\0' || *p == '#') continue;

        start_frame = strtol(p, &end, 10);
        if (end == p) continue;
        while (*end && isspace((unsigned char)*end)) end++;
        if (*end == '\0' || *end == '#') continue;

        strncpy(media, end, sizeof(media));
        media[sizeof(media) - 1] = '\0';
        framefile_trim(media);
        if (media[0] == '\0') continue;

        if (framefile_resolve_segment_path(manifest_path, media, segment_count == 0, resolved, sizeof(resolved)) != 0) {
            SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] skipping unresolved segment %ld %s", start_frame, media);
            continue;
        }

        framefile_segment_t *tmp = realloc(segments, (size_t)(segment_count + 1) * sizeof(*segments));
        if (!tmp) {
            free(segments);
            fs_close(fd);
            return -1;
        }
        segments = tmp;
        segments[segment_count].start_frame = (int)start_frame;
        strncpy(segments[segment_count].path, resolved, sizeof(segments[segment_count].path));
        segments[segment_count].path[sizeof(segments[segment_count].path) - 1] = '\0';
        segment_count++;
    }

    if (pos > 0) {
        char *p, *end;
        long start_frame;
        char media[256];
        char resolved[512];

        line[pos] = '\0';
        p = line;
        framefile_trim(p);
        if (*p != '\0' && *p != '#') {
            start_frame = strtol(p, &end, 10);
            if (end != p) {
                while (*end && isspace((unsigned char)*end)) end++;
                if (*end != '\0' && *end != '#') {
                    strncpy(media, end, sizeof(media));
                    media[sizeof(media) - 1] = '\0';
                    framefile_trim(media);
                    if (media[0] != '\0' &&
                        framefile_resolve_segment_path(manifest_path, media, segment_count == 0, resolved, sizeof(resolved)) == 0) {
                        framefile_segment_t *tmp = realloc(segments, (size_t)(segment_count + 1) * sizeof(*segments));
                        if (!tmp) {
                            free(segments);
                            fs_close(fd);
                            return -1;
                        }
                        segments = tmp;
                        segments[segment_count].start_frame = (int)start_frame;
                        strncpy(segments[segment_count].path, resolved, sizeof(segments[segment_count].path));
                        segments[segment_count].path[sizeof(segments[segment_count].path) - 1] = '\0';
                        segment_count++;
                        eof_processed = 1;
                    } else if (media[0] != '\0') {
                        SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] skipping unresolved segment %ld %s (EOF)", start_frame, media);
                    }
                }
            }
        }
    }

    fs_close(fd);

    if (segment_count <= 0) {
        free(segments);
        return -1;
    }

    g_framefile_segments = segments;
    g_framefile_segment_count = segment_count;
    g_framefile_active_segment = 0;
    strncpy(g_framefile_manifest_path, manifest_path, sizeof(g_framefile_manifest_path));
    g_framefile_manifest_path[sizeof(g_framefile_manifest_path) - 1] = '\0';
    strncpy(initial_video_path, g_framefile_segments[0].path, initial_video_sz);
    initial_video_path[initial_video_sz - 1] = '\0';

    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] loaded %d segment(s) from %s", g_framefile_segment_count, manifest_path);
    for (int i = 0; i < g_framefile_segment_count; i++) {
        SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] %d: start=%d path=%s", i, g_framefile_segments[i].start_frame, g_framefile_segments[i].path);
    }
    if (eof_processed) {
        SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] EOF line parsed without trailing newline");
    }
    return 0;
}

static int framefile_find_segment_index(int absolute_frame) {
    int lo, hi, best;

    if (!g_framefile_segments || g_framefile_segment_count <= 0) return -1;

    lo = 0;
    hi = g_framefile_segment_count - 1;
    best = 0;
    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (g_framefile_segments[mid].start_frame <= absolute_frame) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return best;
}

static int framefile_active_absolute_frame(void) {
    if (!dcfmv_current) return 0;
    if (!g_framefile_segments || g_framefile_segment_count <= 0 || g_framefile_active_segment < 0 || g_framefile_active_segment >= g_framefile_segment_count) {
        return dcfmv_frame_index(dcfmv_current);
    }
    return g_framefile_segments[g_framefile_active_segment].start_frame + dcfmv_frame_index(dcfmv_current);
}

static void framefile_quiesce_segment_switch(dcfmv_t *fmv) {
    if (!fmv) {
        return;
    }

    dcfmv_set_preload_paused(fmv, 1);
    atomic_fetch_add(&fmv->GSeekGeneration, 1);
    atomic_store(&fmv->seek_request, -1);
    atomic_store(&fmv->seek_in_progress, 0);
    thd_sleep(20);
}

static int framefile_prepare_segment_for_frame(int absolute_frame) {
    int idx;
    int local_frame;

    if (!dcfmv_current || !g_framefile_segments || g_framefile_segment_count <= 0) {
        return absolute_frame;
    }

    idx = framefile_find_segment_index(absolute_frame);
    if (idx < 0) return absolute_frame;

    local_frame = absolute_frame - g_framefile_segments[idx].start_frame;
    if (local_frame < 0) local_frame = 0;

    if (idx != g_framefile_active_segment) {
        int was_paused = dcfmv_is_paused(dcfmv_current);
        int was_muted = dcfmv_audio_muted(dcfmv_current);
        int was_preload_paused = atomic_load(&dcfmv_current->preload_paused);
        SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT,
                  "[FrameFile] switching segment %d -> %d for abs=%d local=%d",
                  g_framefile_active_segment, idx, absolute_frame, local_frame);

        framefile_quiesce_segment_switch(dcfmv_current);
        if (dcfmv_open(dcfmv_current, g_framefile_segments[idx].path) != 0) {
            printf("PANIC: Failed to open DCMV segment %d (%s)\n", idx, g_framefile_segments[idx].path);
            exit(1);
        }
        if (dcfmv_audio_channels(dcfmv_current) > 0) {
            if (dcfmv_audio_init(dcfmv_current) != 0) {
                printf("PANIC: dcfmv_audio_init failed for segment %d\n", idx);
                exit(1);
            }
        }
        if (g_cfg_disable_fmv_audio) {
            dcfmv_set_audio_enabled(dcfmv_current, 0);
        }
        dcfmv_set_audio_clock_mode(dcfmv_current, dcfmv_audio_channels(dcfmv_current) > 0);
        dcfmv_set_audio_muted(dcfmv_current, was_muted);
        dcfmv_set_paused(dcfmv_current, was_paused);
        dcfmv_set_preload_paused(dcfmv_current, was_preload_paused);
        g_framefile_active_segment = idx;
    } else {
        SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] seeking segment %d abs=%d local=%d", idx, absolute_frame, local_frame);
    }

    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] request seek abs=%d local=%d audio_started=%d muted=%d paused=%d",
              absolute_frame,
              local_frame,
              dcfmv_playback_started(dcfmv_current),
              dcfmv_audio_muted(dcfmv_current),
              dcfmv_is_paused(dcfmv_current));
    dcfmv_request_seek(dcfmv_current, local_frame);
    return absolute_frame;
}

static const char *dcfmv_compression_name(uint8_t c) {
    switch (c) {
        case 0: return "LZ4";
        case 1: return "Zstd";
        default: return "Unknown";
    }
}

static const char *dcfmv_frame_type_name(uint8_t t) {
    switch (t) {
        case 0: return "RGB565";
        case 1: return "YUV422";
        default: return "Unknown";
    }
}

static void framefile_log_segment_opened(int old_idx, int new_idx,
                                         int abs_frame, int local_frame,
                                         const char *path) {
    const dcfmv_media_info_t *info = dcfmv_media_info(dcfmv_current);
    if (!info) return;

    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT, "📦 [FrameFile] Segment %d -> %d opened", old_idx, new_idx);
    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT, "   Path: %s", path);
    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT, "   Seek: abs=%d local=%d", abs_frame, local_frame);
    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT,
              "   Header v%lu: %s %ux%u (content: %ux%u) @ %.2ffps, %uHz, %uch, unique=%lu, total=%lu",
              (unsigned long)info->version,
              dcfmv_frame_type_name(info->frame_type),
              info->tex_width,
              info->tex_height,
              info->content_width,
              info->content_height,
              info->fps,
              info->sample_rate,
              info->channels,
              (unsigned long)info->num_unique_frames,
              (unsigned long)info->num_total_frames);
    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT,
              "   Frame size: %lu, Max compressed: %lu, Audio offset: 0x%lX, Compression: %s",
              (unsigned long)info->uncompressed_frame_size,
              (unsigned long)info->max_compressed_frame_size,
              (unsigned long)dcfmv_audio_offset(dcfmv_current),
              dcfmv_compression_name(info->compression_type));
}

static void framefile_activate_segment(int idx, int absolute_frame, int local_frame, int request_seek) {
    int was_paused;
    int was_muted;

    if (!dcfmv_current || !g_framefile_segments || idx < 0 || idx >= g_framefile_segment_count) {
        return;
    }

    was_paused = dcfmv_is_paused(dcfmv_current);
    was_muted = dcfmv_audio_muted(dcfmv_current);
    int was_preload_paused = atomic_load(&dcfmv_current->preload_paused);

    int old_segment = g_framefile_active_segment;

    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT,
              "[FrameFile] switching segment %d -> %d for abs=%d local=%d path=%s",
              old_segment, idx, absolute_frame, local_frame,
              g_framefile_segments[idx].path);

    framefile_quiesce_segment_switch(dcfmv_current);

    if (dcfmv_open(dcfmv_current, g_framefile_segments[idx].path) != 0) {
        printf("PANIC: Failed to open DCMV segment %d (%s)\n",
            idx, g_framefile_segments[idx].path);
        exit(1);
    }

    g_framefile_active_segment = idx;

    framefile_log_segment_opened(old_segment, idx,
                                absolute_frame, local_frame,
                                g_framefile_segments[idx].path);    

    if (dcfmv_audio_channels(dcfmv_current) > 0) {
        if (dcfmv_audio_init(dcfmv_current) != 0) {
            printf("PANIC: dcfmv_audio_init failed for segment %d\n", idx);
            exit(1);
        }
    }
    if (g_cfg_disable_fmv_audio) {
        dcfmv_set_audio_enabled(dcfmv_current, 0);
    }
    dcfmv_set_audio_clock_mode(dcfmv_current, dcfmv_audio_channels(dcfmv_current) > 0);
    dcfmv_set_audio_muted(dcfmv_current, was_muted);
    dcfmv_set_paused(dcfmv_current, was_paused);
    dcfmv_set_preload_paused(dcfmv_current, was_preload_paused);
    dcfmv_set_seek_settle_frames(dcfmv_current, 10);
    SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT, "[FrameFile] segment switch settle set to %d frames", 10);
    atomic_store(&g_clip_boundary_hold, 0);
    g_framefile_active_segment = idx;

    if (request_seek) {
        SINGE_LOG(SINGE_LOG_FRAMEFILE_SEGMENT,
                  "[FrameFile] boundary switch request seek abs=%d local=%d path=%s",
                  absolute_frame, local_frame, g_framefile_segments[idx].path);
        dcfmv_request_seek(dcfmv_current, local_frame);
    }
}

static void framefile_ensure_segment_for_frame(int absolute_frame) {
    int idx;
    int local_frame;
    int next_idx;
    int next_start;

    if (!dcfmv_current || !g_framefile_segments || g_framefile_segment_count <= 0) {
        return;
    }

    idx = framefile_find_segment_index(absolute_frame);
    if (idx < 0) return;
    local_frame = absolute_frame - g_framefile_segments[idx].start_frame;
    if (local_frame < 0) local_frame = 0;

    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] ensure abs=%d active=%d idx=%d local=%d segments=%d hold=%d",
              absolute_frame,
              g_framefile_active_segment,
              idx,
              local_frame,
              g_framefile_segment_count,
              atomic_load(&g_clip_boundary_hold));

    if (idx == g_framefile_active_segment) {
        next_idx = idx + 1;
        if (next_idx < g_framefile_segment_count) {
            next_start = g_framefile_segments[next_idx].start_frame;
            if (absolute_frame + 1 >= next_start) {
                SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] boundary reached abs=%d current_segment=%d next_segment=%d next_start=%d",
                          absolute_frame, idx, next_idx, next_start);
                framefile_activate_segment(next_idx, next_start, 0, 1);
            }
        }
        return;
    }

    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] boundary switch %d -> %d abs=%d local=%d path=%s",
              g_framefile_active_segment, idx, absolute_frame, local_frame,
              g_framefile_segments[idx].path);
    framefile_activate_segment(idx, absolute_frame, local_frame, 1);
}

static void framefile_seek_absolute_frame(int absolute_frame) {
    int idx;
    int local_frame;

    if (!dcfmv_current || !g_framefile_segments || g_framefile_segment_count <= 0) {
        dcfmv_request_seek(dcfmv_current, absolute_frame);
        return;
    }

    idx = framefile_find_segment_index(absolute_frame);
    if (idx < 0) {
        dcfmv_request_seek(dcfmv_current, absolute_frame);
        return;
    }

    local_frame = absolute_frame - g_framefile_segments[idx].start_frame;
    if (local_frame < 0) local_frame = 0;

    if (idx != g_framefile_active_segment) {
        framefile_activate_segment(idx, absolute_frame, local_frame, 0);
    }
    g_framefile_active_segment = idx;

    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] request seek abs=%d local=%d segment=%d path=%s",
              absolute_frame, local_frame, idx, g_framefile_segments[idx].path);
    dcfmv_request_seek(dcfmv_current, local_frame);
}

static int resolve_framefile_media_path(const char *framefile_path, char *out, size_t out_sz) {
    file_t fd;
    char line[512];
    char dir[512];
    char base_root_candidate[512];
    char framefile_stem[256];
    int pos = 0;
    const char *slash;

    if (!framefile_path || !out || out_sz == 0) return -1;

    if (!strstr(framefile_path, ".txt")) {
        strncpy(out, framefile_path, out_sz);
        out[out_sz - 1] = '\0';
        return 0;
    }

    slash = strrchr(framefile_path, '/');
    if (slash) {
        size_t dir_len = (size_t)(slash - framefile_path);
        if (dir_len >= sizeof(dir)) dir_len = sizeof(dir) - 1;
        memcpy(dir, framefile_path, dir_len);
        dir[dir_len] = '\0';
    } else {
        dir[0] = '\0';
    }

    {
        const char *base = strrchr(framefile_path, '/');
        const char *name = base ? base + 1 : framefile_path;
        size_t name_len = strlen(name);
        if (name_len > 4 && strcmp(name + name_len - 4, ".txt") == 0) {
            name_len -= 4;
        }
        if (name_len >= sizeof(framefile_stem)) name_len = sizeof(framefile_stem) - 1;
        memcpy(framefile_stem, name, name_len);
        framefile_stem[name_len] = '\0';
    }

    if (framefile_stem[0] != '\0') {
        snprintf(base_root_candidate, sizeof(base_root_candidate), "%s%s.dcmv", G_BASE_PATH, framefile_stem);
        fd = fs_open(base_root_candidate, O_RDONLY);
        if (fd >= 0) {
            fs_close(fd);
            strncpy(out, base_root_candidate, out_sz);
            out[out_sz - 1] = '\0';
            return 0;
        }
    }

    fd = fs_open(framefile_path, O_RDONLY);
    if (fd < 0) return -1;

    while (fs_read(fd, &line[pos], 1) == 1) {
        char *p, *media;
        size_t media_len;

        if (line[pos] == '\r') continue;
        if (line[pos] != '\n' && pos < (int)sizeof(line) - 1) {
            pos++;
            continue;
        }

        line[pos] = '\0';
        pos = 0;

        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        while (*p && !isdigit((unsigned char)*p)) p++;
        if (!isdigit((unsigned char)*p)) continue;

        while (*p && !isspace((unsigned char)*p)) p++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        media = p;
        media_len = strlen(media);
        while (media_len > 0 && isspace((unsigned char)media[media_len - 1])) media[--media_len] = '\0';

        if (media_len >= 4 && strcmp(media + media_len - 4, ".m2v") == 0) {
            media_len -= 4;
            if (dir[0] != '\0')
                snprintf(out, out_sz, "%s/%.*s.dcmv", dir, (int)media_len, media);
            else
                snprintf(out, out_sz, "%.*s.dcmv", (int)media_len, media);
            fs_close(fd);
            return 0;
        }
    }

    fs_close(fd);
    return -1;
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

static inline int aim_assist_color_is_red(int r, int g, int b) {
    return (r >= 140) && (r > g + 24) && (r > b + 24);
}

static inline int aim_assist_hitbox_is_red(void) {
    return aim_assist_color_is_red(g_last_hitbox_r, g_last_hitbox_g, g_last_hitbox_b);
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
static void render_current_video() {
    dcfmv_render_current_video(dcfmv_current);
}


bool schedule_frame_preload(int frame) {
    return dcfmv_schedule_frame_preload(dcfmv_current, frame);
}

bool schedule_frame_preload_with_generation(int frame, int generation) {
    return dcfmv_schedule_frame_preload_with_generation(dcfmv_current, frame, generation);
}



kthread_t *worker_thread_id;
kthread_t *vmu_flush_thread_id;

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

static void *vmu_flush_thread(void *p) {
    (void)p;
    while (!atomic_load(&g_exit_requested)) {
        flush_vmu_archive_if_pending();
        thd_sleep(50);
    }
    return NULL;
}



// --- seek_to_frame(): flush ring + re-prime fresh preload jobs ---
void seek_to_frame(int new_frame) {
    framefile_seek_absolute_frame(new_frame);
}

static const char *dcsinge_ldp_state_name(dcsinge_ldp_state_t state) {
    switch (state) {
        case DCSINGE_LDP_STOPPED:   return "STOPPED";
        case DCSINGE_LDP_SEARCHING: return "SEARCHING";
        case DCSINGE_LDP_PLAYING:   return "PLAYING";
        case DCSINGE_LDP_PAUSED:    return "PAUSED";
        case DCSINGE_LDP_CLIP_HOLD: return "CLIP_HOLD";
        default:                    return "UNKNOWN";
    }
}

static dcsinge_ldp_state_t dcsinge_ldp_get_state(void) {
    return (dcsinge_ldp_state_t)atomic_load(&g_ldp_state);
}

static void dcsinge_ldp_set_state(dcsinge_ldp_state_t state, const char *reason) {
    dcsinge_ldp_state_t old = (dcsinge_ldp_state_t)atomic_exchange(&g_ldp_state, state);
    if (old != state) {
        SINGE_LOG(SINGE_LOG_GENERAL, "[LDP] %s -> %s (%s)",
                  dcsinge_ldp_state_name(old),
                  dcsinge_ldp_state_name(state),
                  reason ? reason : "state");
    }
}

static void dcsinge_ldp_begin_search(dcsinge_ldp_state_t post_search_state,
                                     const char *reason) {
    atomic_store(&g_ldp_post_search_state, post_search_state);
    dcsinge_ldp_set_state(DCSINGE_LDP_SEARCHING, reason);
}

static void dcsinge_ldp_request_after_search(dcsinge_ldp_state_t state,
                                             const char *reason) {
    if (dcsinge_ldp_get_state() == DCSINGE_LDP_SEARCHING) {
        atomic_store(&g_ldp_post_search_state, state);
        SINGE_LOG(SINGE_LOG_GENERAL, "[LDP] SEARCHING post-state -> %s (%s)",
                  dcsinge_ldp_state_name(state),
                  reason ? reason : "state");
        return;
    }

    dcsinge_ldp_set_state(state, reason);
}

static void dcsinge_ldp_after_tick(void) {
    if (dcsinge_ldp_get_state() == DCSINGE_LDP_SEARCHING &&
        dcfmv_current &&
        !dcfmv_seek_active(dcfmv_current) &&
        dcfmv_seek_settle_frames(dcfmv_current) <= 0) {
        dcsinge_ldp_set_state((dcsinge_ldp_state_t)atomic_load(&g_ldp_post_search_state),
                              "search complete");
    }
}

static int dcsinge_ldp_should_tick(void) {
    dcsinge_ldp_state_t state = dcsinge_ldp_get_state();
    return state == DCSINGE_LDP_PLAYING || state == DCSINGE_LDP_SEARCHING;
}

static int dcsinge_ldp_should_present_video(void) {
    dcsinge_ldp_state_t state = dcsinge_ldp_get_state();
    return state == DCSINGE_LDP_PLAYING ||
           state == DCSINGE_LDP_PAUSED ||
           state == DCSINGE_LDP_CLIP_HOLD;
}

static int dcsinge_enter_clip_hold_if_needed(int cur_frame) {
    if (dcsinge_ldp_get_state() == DCSINGE_LDP_SEARCHING) {
        return 0;
    }

    if (g_iFrameEnd <= 0 || cur_frame < g_iFrameEnd) {
        return 0;
    }

    int entering_hold = !atomic_exchange(&g_clip_boundary_hold, 1);
    if (entering_hold) {
        dcfmv_set_seek_settle_frames(dcfmv_current, 0);
        Singe_log("[Singe] clip-boundary hold at iFrameEnd=%d", g_iFrameEnd);
    }

    dcsinge_ldp_set_state(DCSINGE_LDP_CLIP_HOLD, "clip boundary");
    dcfmv_log_state("clip_hold", dcfmv_current);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_audio_stop_stream(dcfmv_current);
    dcfmv_set_paused(dcfmv_current, 1);
    dcfmv_set_preload_paused(dcfmv_current, 1);
    return 1;
}

static void dcsinge_sync_lua_clip_end(int cur_frame) {
    if (!GLua) {
        return;
    }

    if (dcsinge_ldp_get_state() == DCSINGE_LDP_SEARCHING) {
        return;
    }

    lua_getglobal(GLua, "iFrameEnd");
    lua_getglobal(GLua, "iFrameStart");
    if (lua_isnumber(GLua, -2) && lua_isnumber(GLua, -1)) {
        int lua_iFrameEnd = (int)lua_tonumber(GLua, -2);
        int lua_iFrameStart = (int)lua_tonumber(GLua, -1);
        int old_iFrameEnd = g_iFrameEnd;
        if (lua_iFrameEnd != g_iFrameEnd &&
            cur_frame >= lua_iFrameStart &&
            cur_frame <= lua_iFrameEnd) {
            Singe_log("Updating g_iFrameEnd from %d to %d (Lua sync)",
                      g_iFrameEnd, lua_iFrameEnd);
            g_iFrameEnd = lua_iFrameEnd;
            if (dcsinge_ldp_get_state() == DCSINGE_LDP_CLIP_HOLD &&
                old_iFrameEnd > 0 &&
                lua_iFrameEnd > old_iFrameEnd) {
                Singe_log("[Singe] Lua extended clip end while held; resuming to iFrameEnd=%d",
                          lua_iFrameEnd);
                atomic_store(&g_clip_boundary_hold, 0);
                dcsinge_ldp_set_state(DCSINGE_LDP_PLAYING, "Lua clip extension");
                dcfmv_set_paused(dcfmv_current, 0);
                dcfmv_set_preload_paused(dcfmv_current, 0);
                dcfmv_audio_start_stream(dcfmv_current);
                dcfmv_set_audio_muted(dcfmv_current, 0);
            }
        }
    }
    lua_pop(GLua, 2);
}



static void fmv_tick(uint64_t now_ms) {
    (void)now_ms;
    dcfmv_tick(dcfmv_current);
    if (g_skip_pause_advance_pending &&
        (!dcfmv_current || !dcfmv_seek_active(dcfmv_current))) {
        g_skip_pause_advance_pending = 0;
        g_skip_pause_advance_frame = -1;
    }
}

static int should_advance_skip_pause_hold(int frame) {
    /*
     * Maddog-HD's entrance menu asks to pause on 41088, but the extracted frame
     * is black and the visible menu begins at 41089. Other paused seeks are
     * intentional still frames and must remain exact.
     */
    return frame == 41088;
}

static void log_memory_stats(const char *tag) {
    struct mallinfo mi = mallinfo();
    size_t pvr_free = pvr_mem_available();

    SINGE_LOG(SINGE_LOG_MEMORY, "[Mem] %s heap_used=%d heap_free=%d heap_arena=%d pvr_free=%lu",
              tag ? tag : "stats",
              mi.uordblks,
              mi.fordblks,
              mi.arena,
              (unsigned long)pvr_free);
}


static void pace_main_loop(void) {
    // vid_waitvbl();
    //  thd_sleep(16);
}

//=============================================================================
// SINGE LUA API FUNCTIONS
//=============================================================================

// Disc control functions
// Disc control functions
static int sep_get_current_frame(lua_State *L) {
    int cur = framefile_active_absolute_frame();
    if (dcsinge_ldp_get_state() == DCSINGE_LDP_CLIP_HOLD && g_iFrameEnd > 0) {
        cur = g_iFrameEnd;
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
    dcsinge_ldp_begin_search(DCSINGE_LDP_PLAYING, "discSkipToFrame");
    if (g_iFrameEnd != -1) {
        Singe_log("Invalidating g_iFrameEnd from %d for discSkipToFrame(%d)",
                  g_iFrameEnd, frame);
    }
    g_iFrameEnd = -1;

    dcfmv_set_audio_muted(dcfmv_current, 1);

    compute_global_ratios();

    lua_getglobal(L, "iFrameEnd");
    lua_getglobal(L, "iFrameStart");

    if (lua_isnumber(L, -2) && lua_isnumber(L, -1)) {
        int newiFrameEnd = (int)lua_tonumber(L, -2);
        int iFrameStart  = (int)lua_tonumber(L, -1);

        if (frame == iFrameStart) {
            Singe_log("Updating g_iFrameEnd from %d to %d (clip start)",
                      g_iFrameEnd, newiFrameEnd);
            g_iFrameEnd = newiFrameEnd;

            Singe_log("iFrameEnd from Lua: %d (clip start)", g_iFrameEnd);
            /*
             * LDP SEARCHING now owns the seek transition. Do not add the old
             * one-second clip-start settle here; dcfmv_seek_to_frame() still
             * applies its short chunk-audio warmup when the backend needs it.
             */
            dcfmv_set_seek_settle_frames(dcfmv_current, 0);
            dcfmv_set_paused(dcfmv_current, 1);
            Singe_log("[Singe] clip-start settle handled by LDP search");
        } else {
            Singe_log("Skip to %d is not clip start (iFrameStart=%d); clip end remains invalid",
                      frame, iFrameStart);
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
    arm_vmu_transition_flush_window();
    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Flushing pending save at seek start for discSkipToFrame(%d)", frame);
    flush_vmu_archive_before_transition("discSkipToFrame", frame);
    framefile_seek_absolute_frame(frame);
    g_skip_pause_advance_pending = should_advance_skip_pause_hold(frame);
    g_skip_pause_advance_frame = g_skip_pause_advance_pending ? frame : -1;

    Singe_log("Skipped to frame %d", frame);
    return 0;
}




static int sep_search(lua_State *L) {
    int frame = (int)luaL_checknumber(L, 1);
    
    Singe_log("[Singe] sep_search/discSearch(%d)\n", frame);
    dcfmv_log_state("search_pre", dcfmv_current);
    g_skip_pause_advance_pending = 0;
    g_skip_pause_advance_frame = -1;
    dcfmv_set_seek_settle_frames(dcfmv_current, 0);
    atomic_store(&g_clip_boundary_hold, 0);
    dcsinge_ldp_begin_search(DCSINGE_LDP_PAUSED, "discSearch");
    /*
     * Match the PC Singe held-search behavior for menu/select screens.
     * Audio is muted and playback is paused so the requested frame is held
     * once the seek completes.
     */
    dcfmv_set_paused(dcfmv_current, 1);
    dcfmv_set_preload_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_audio_stop_stream(dcfmv_current);
    arm_vmu_transition_flush_window();
    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Flushing pending save at seek start for discSearch(%d)", frame);
    flush_vmu_archive_before_transition("discSearch", frame);
    framefile_seek_absolute_frame(frame);
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
    if (g_skip_pause_advance_pending &&
        g_skip_pause_advance_frame >= 0 &&
        dcsinge_ldp_get_state() == DCSINGE_LDP_SEARCHING &&
        dcfmv_current &&
        atomic_load(&dcfmv_current->seek_request) >= 0) {
        int hold_frame = g_skip_pause_advance_frame + 1;
        Singe_log("[Singe] discSkipToFrame+discPause hold advance: %d -> %d",
                  g_skip_pause_advance_frame, hold_frame);
        framefile_seek_absolute_frame(hold_frame);
    }
    g_skip_pause_advance_pending = 0;
    g_skip_pause_advance_frame = -1;
    dcsinge_ldp_request_after_search(DCSINGE_LDP_PAUSED, "discPause");
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
    g_skip_pause_advance_pending = 0;
    g_skip_pause_advance_frame = -1;
    atomic_store(&g_clip_boundary_hold, 0);
    dcsinge_ldp_request_after_search(DCSINGE_LDP_PLAYING, "discPlay");
    dcfmv_set_paused(dcfmv_current, 0);
    dcfmv_set_preload_paused(dcfmv_current, 0);
    dcfmv_audio_start_stream(dcfmv_current);
    dcfmv_set_audio_muted(dcfmv_current, 0);
    dcfmv_log_state("play_post", dcfmv_current);
    if (!g_logged_first_clip_start) {
        log_memory_stats("after_first_clip_starts");
        g_logged_first_clip_start = 1;
    }
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
    int current_frame = framefile_active_absolute_frame();
    int target_frame = current_frame - 1;
    if (target_frame < 0) target_frame = 0;
    // atomic_fetch_add(&GSeekGeneration, 1);
    // GSeeking = 1;
    // GSeekTargetFrame = target_frame;
    g_skip_pause_advance_pending = 0;
    g_skip_pause_advance_frame = -1;
    atomic_store(&g_clip_boundary_hold, 0);
    dcsinge_ldp_begin_search(DCSINGE_LDP_PAUSED, "discStepBackward");
    dcfmv_set_paused(dcfmv_current, 1);
    dcfmv_set_preload_paused(dcfmv_current, 1);
    dcfmv_set_audio_muted(dcfmv_current, 1);
    dcfmv_audio_stop_stream(dcfmv_current);
    arm_vmu_transition_flush_window();
    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Flushing pending save at seek start for discStepBackward(%d)", target_frame);
    flush_vmu_archive_before_transition("discStepBackward", target_frame);
    framefile_seek_absolute_frame(target_frame);
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
    float u_max, v_max;    // Used glyph area inside the power-of-2 texture
    pvr_ptr_t tex;
    pvr_poly_hdr_t hdr;
    int bearing_x;         // Horizontal bearing (offset from pen)
    int bearing_y;         // Vertical bearing (offset from baseline)
    int advance;           // Horizontal advance for next character
} CharCache;

struct LoadedFont {
    FT_Face face;
    char *path;
    int requested_size;
    int pixel_size;
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
    int logged_pvr_limit = 0;

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
        if (tex_w < 16) tex_w = 16;
        if (tex_h < 16) tex_h = 16;
        size_t img_bytes = tex_w * tex_h * 2;

        if (pvr_mem_available() < img_bytes + SINGE_FONT_PVR_RESERVE_BYTES) {
            if (!logged_pvr_limit) {
                SINGE_LOG(SINGE_LOG_MEMORY,
                          "[Font] stopping glyph prewarm at char=%d: pvr_free=%lu reserve=%u need=%lu",
                          ch,
                          (unsigned long)pvr_mem_available(),
                          (unsigned)SINGE_FONT_PVR_RESERVE_BYTES,
                          (unsigned long)img_bytes);
                logged_pvr_limit = 1;
            }
            break;
        }
        
        // Allocate and clear texture buffer
        uint16_t *img = memalign(32, img_bytes);
        if (!img) continue;
        memset(img, 0, img_bytes);
        
        // Render glyph with dithering for smooth anti-aliasing
        for (int y = 0; y < bmp->rows; y++) {
            for (int x = 0; x < bmp->width; x++) {
                uint8_t a = bmp->buffer[y * bmp->pitch + x];
                if (a > 0) {
                    img[y * tex_w + x] = 0xFFFF;
                }
            }
        }
        
        // Upload to VRAM
        pvr_ptr_t tex = pvr_mem_malloc(img_bytes);
        if (!tex) {
            free(img);
            continue;
        }
        dcache_flush_range((uint32)img, img_bytes);
        pvr_txr_load_ex(img, tex, tex_w, tex_h, PVR_TXRLOAD_16BPP);
        free(img);
        
        // Create PVR context
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                         PVR_TXRFMT_ARGB1555,
                         tex_w, tex_h, tex, PVR_FILTER_NONE);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        
        // Store in cache
        char_cache[ch].ch = ch;
        char_cache[ch].w = bmp->width;
        char_cache[ch].h = bmp->rows;
        char_cache[ch].tex_w = tex_w;
        char_cache[ch].tex_h = tex_h;
        char_cache[ch].u_max = (float)bmp->width / (float)tex_w;
        char_cache[ch].v_max = (float)bmp->rows / (float)tex_h;
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
static void dc_pvr_emit_tr_poly_batch(const pvr_poly_hdr_t *hdr,
                                      const pvr_vertex_t *vertices,
                                      size_t vertex_count);
static void overlay_draw_glyph(int x, int y, const CharCache *glyph, uint32_t color);
static float g_overlay_submit_z = 1.0f;
// ----------------------------------------------------------------------------
// Text rendering into buffer (wraps your font cache renderer)
// ----------------------------------------------------------------------------
static void overlay_draw_text(int x, int y, const char *msg)
{
    if (!msg || !*msg || !g_active_loaded_font || !g_active_loaded_font->char_cache_initialized || !GCurrentFont) {
        return;
    }

    const CharCache *char_cache = g_active_loaded_font->char_cache;
    const FT_Size_Metrics m = GCurrentFont->size->metrics;
    const int ascent = m.ascender >> 6;
    const int line_height = (m.height >> 6);
    const uint32_t color = pack_argb8888_overlay();
    int pen_x = 0;
    int pen_y = 0;

    for (const unsigned char *p = (const unsigned char *)msg; *p; p++) {
        if (*p == '\n') {
            pen_x = 0;
            pen_y += line_height;
            continue;
        }
        if (*p >= 128) {
            continue;
        }

        const CharCache *glyph = &char_cache[*p];
        if (glyph->tex) {
            const int gx = x + pen_x + glyph->bearing_x;
            const int gy = y + pen_y + (ascent - glyph->bearing_y);
            overlay_draw_glyph(gx, gy, glyph, color);
        }
        pen_x += glyph->advance;
    }
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

    char intro_path[256];
    build_intro_path(intro_path, sizeof(intro_path));

    log_memory_stats("before_startup_intro");
    file_t fd = fs_open(intro_path, O_RDONLY);
    if (fd < 0) {
        printf("[Startup] Intro splash open failed: %s\n", intro_path);
        log_memory_stats("after_startup_intro_open_failed");
        return;
    }
    fs_close(fd);

    pvr_ptr_t tex = NULL;
    uint32_t w = 0, h = 0;
    if (png_load_texture(intro_path, &tex, PNG_FULL_ALPHA, &w, &h) < 0 || !tex || !w || !h) {
        printf("[Startup] Intro splash PNG load failed: %s tex=%p size=%lux%lu\n",
               intro_path, tex, (unsigned long)w, (unsigned long)h);
        if (tex) pvr_mem_free(tex);
        log_memory_stats("after_startup_intro_png_failed");
        return;
    }
    g_startup_intro_drawn = 1;
    log_memory_stats("after_startup_intro_png_load");

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
    pvr_mem_free(tex);
    log_memory_stats("after_startup_intro_draw");
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
        float scaled_w = w * g_scale_x;
        float scaled_h = h * g_scale_y;

        // --- Set up PVR textured polygon ---
        pvr_vertex_t verts[4];

        // --- Top-left ---
        verts[0].flags = PVR_CMD_VERTEX;
        verts[0].x = scaled_x;
        verts[0].y = scaled_y;
        verts[0].z = g_overlay_submit_z;
        verts[0].u = 0.0f;
        verts[0].v = 0.0f;
        verts[0].argb = 0xFFFFFFFF;
        verts[0].oargb = 0;

        // --- Top-right ---
        verts[1].flags = PVR_CMD_VERTEX;
        verts[1].x = scaled_x + scaled_w;
        verts[1].y = scaled_y;
        verts[1].z = g_overlay_submit_z;
        verts[1].u = 1.0f;
        verts[1].v = 0.0f;
        verts[1].argb = 0xFFFFFFFF;
        verts[1].oargb = 0;

        // --- Bottom-left ---
        verts[2].flags = PVR_CMD_VERTEX;
        verts[2].x = scaled_x;
        verts[2].y = scaled_y + scaled_h;
        verts[2].z = g_overlay_submit_z;
        verts[2].u = 0.0f;
        verts[2].v = 1.0f;
        verts[2].argb = 0xFFFFFFFF;
        verts[2].oargb = 0;

        // --- Bottom-right ---
        verts[3].flags = PVR_CMD_VERTEX_EOL;
        verts[3].x = scaled_x + scaled_w;
        verts[3].y = scaled_y + scaled_h;
        verts[3].z = g_overlay_submit_z;
        verts[3].u = 1.0f;
        verts[3].v = 1.0f;
        verts[3].argb = 0xFFFFFFFF;
        verts[3].oargb = 0;

        // Submit through the same ordered path as overlay primitive batches.
        dc_pvr_emit_tr_poly_batch(&spr->hdr, verts, 4);

    #ifdef DEBUG_OVERLAY_SPRITE
        printf("[PVR] Draw sprite '%s' at (%d,%d) scaled=(%.1f,%.1f) size=(%dx%d)\n",
            spr->name ? spr->name : "(unnamed)",
            x, y, scaled_x, scaled_y, w, h);
    #endif
    }




static void overlay_draw_glyph(int x, int y, const CharCache *glyph, uint32_t color)
{
    if (!glyph || !glyph->tex || glyph->w <= 0 || glyph->h <= 0) {
        return;
    }

    float scaled_x = (x - g_ratio_x_offset) * g_scale_x;
    float scaled_y = (y - g_ratio_y_offset) * g_scale_y;
    float scaled_w = glyph->w * g_scale_x;
    float scaled_h = glyph->h * g_scale_y;

    pvr_vertex_t verts[4] = {
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,            .y = scaled_y,            .z = g_overlay_submit_z, .u = 0.0f,         .v = 0.0f,         .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x + scaled_w, .y = scaled_y,            .z = g_overlay_submit_z, .u = glyph->u_max, .v = 0.0f,         .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,            .y = scaled_y + scaled_h, .z = g_overlay_submit_z, .u = 0.0f,         .v = glyph->v_max, .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x + scaled_w, .y = scaled_y + scaled_h, .z = g_overlay_submit_z, .u = glyph->u_max, .v = glyph->v_max, .argb = color, .oargb = 0 }
    };

    dc_pvr_emit_tr_poly_batch(&glyph->hdr, verts, 4);
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

static void font_prewarm_loaded_font(int font_index, int requested_size, int pixel_size, const char *path) {
    int saved_font_idx;
    FT_Face saved_font;

    if (font_index < 0 || font_index >= g_font_manager.font_count) {
        return;
    }

    saved_font_idx = g_font_manager.current_font_idx;
    saved_font = GCurrentFont;

    SINGE_LOG(SINGE_LOG_MEMORY,
              "[Font] prewarm begin index=%d requested_size=%d pixel_size=%d path=%s",
              font_index, requested_size, pixel_size, path ? path : "(null)");
    log_memory_stats("before_font_prewarm");

    g_font_manager.current_font_idx = font_index;
    g_active_loaded_font = &g_font_manager.fonts[font_index];
    GCurrentFont = g_active_loaded_font->face;
    font_init_char_cache();

    log_memory_stats("after_font_prewarm");
    SINGE_LOG(SINGE_LOG_MEMORY, "[Font] prewarm end index=%d initialized=%d",
              font_index, g_active_loaded_font->char_cache_initialized);

    g_font_manager.current_font_idx = saved_font_idx;
    if (saved_font_idx >= 0 && saved_font_idx < g_font_manager.font_count) {
        g_active_loaded_font = &g_font_manager.fonts[saved_font_idx];
        GCurrentFont = g_active_loaded_font->face;
    } else {
        g_active_loaded_font = NULL;
        GCurrentFont = saved_font;
    }
}

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
    if (pixel_size > SINGE_FONT_MAX_PX) {
        SINGE_LOG(SINGE_LOG_MEMORY,
                  "[Font] clamping requested size %d to Dreamcast max %d",
                  pixel_size, SINGE_FONT_MAX_PX);
        pixel_size = SINGE_FONT_MAX_PX;
    }

    char *fullpath = resolve_path(path);
    if (!fullpath) {
        lua_pushinteger(L, -1);
        return 1;
    }

    for (int i = 0; i < g_font_manager.font_count; i++) {
        LoadedFont *loaded = &g_font_manager.fonts[i];
        if (loaded->path &&
            loaded->pixel_size == pixel_size &&
            strcmp(loaded->path, fullpath) == 0) {
            SINGE_LOG(SINGE_LOG_MEMORY,
                      "[Font] reusing index=%d requested_size=%d pixel_size=%d path=%s",
                      i, requested_size, pixel_size, fullpath);
            free(fullpath);
            lua_pushinteger(L, i);
            return 1;
        }
    }

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
    if (g_font_manager.current_font_idx >= 0 &&
        g_font_manager.current_font_idx < g_font_manager.font_count) {
        g_active_loaded_font = &g_font_manager.fonts[g_font_manager.current_font_idx];
    }
    memset(&g_font_manager.fonts[g_font_manager.font_count], 0, sizeof(LoadedFont));
    g_font_manager.fonts[g_font_manager.font_count].face = face;
    g_font_manager.fonts[g_font_manager.font_count].path = fullpath;
    g_font_manager.fonts[g_font_manager.font_count].requested_size = requested_size;
    g_font_manager.fonts[g_font_manager.font_count].pixel_size = pixel_size;
    font_index = g_font_manager.font_count;
    g_font_manager.font_count++;

    font_prewarm_loaded_font(font_index, requested_size, pixel_size, fullpath);

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
    hash_value ^= 0x53464f4e544c554cUL; /* Keep font sprites out of file-sprite hash space. */
    hash_value ^= (r5 << 16) | (g5 << 8) | b5;
    hash_value ^= (unsigned long)(font_key >> 4);
    hash_value ^= ((unsigned long)(g_font_manager.current_font_idx + 1) << 24);

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
    dcache_flush_range((uint32)img, img_bytes);
    pvr_txr_load_ex(img, tex, tex_w, tex_h, PVR_TXRLOAD_16BPP);
    free(img);

    SingeSprite *sprite = Singe_xmalloc(sizeof(SingeSprite));
    sprite->hash_id = hash_value;
    sprite->name = NULL;
    sprite->is_font_sprite = 1;
    sprite->font_index = g_font_manager.current_font_idx;
    sprite->frame_count = 1;
    sprite->width = scaled_width;
    sprite->height = scaled_height;
    sprite->texture = tex;

    // Cache into linked list
    sprite->next = GSprites;
    GSprites = sprite;

    // Build PVR context
    pvr_poly_cxt_t cxt;
    pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY,
                     PVR_TXRFMT_ARGB1555,
                     tex_w, tex_h, tex, PVR_FILTER_NONE);
    cxt.gen.alpha = PVR_ALPHA_ENABLE;
    cxt.gen.culling = PVR_CULLING_NONE;
    cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
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
			if (b1) {
				dcsinge_ldp_request_after_search(DCSINGE_LDP_PAUSED, "singeSetPauseFlag");
			} else {
				atomic_store(&g_clip_boundary_hold, 0);
				dcsinge_ldp_request_after_search(DCSINGE_LDP_PLAYING, "singeSetPauseFlag");
			}
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
        if (sprite->hash_id == hash_value &&
            sprite->is_font_sprite &&
            sprite->font_index == g_font_manager.current_font_idx) {
            // DC_log("Font sprite found in cache with hash_id: %lu\n", hash_value);
            return sprite;
        }
    }

    // DC_log("Font sprite not found in cache with hash_id: %lu\n", hash_value);
    return NULL;
}

typedef struct __attribute__((packed)) {
    char fourcc[4];
    uint32_t total_size;
    uint8_t version;
    uint8_t padding0[3];
    uint16_t width_pixels;
    uint16_t height_pixels;
    uint32_t pvr_type;
    uint32_t pad1;
    uint32_t pad2;
    uint32_t pad3;
} SingeDtHeader;

#define SINGE_DT_HEADER_SIZE 32
#define SINGE_DT_PVR_FORMAT_MASK 0xFE000000u

static char *singe_make_sibling_path_with_ext(const char *path, const char *ext) {
    const char *dot;
    size_t base_len;
    size_t ext_len;
    char *out;

    if (!path || !ext) return NULL;
    dot = strrchr(path, '.');
    base_len = dot ? (size_t)(dot - path) : strlen(path);
    ext_len = strlen(ext);
    out = malloc(base_len + ext_len + 1);
    if (!out) return NULL;
    memcpy(out, path, base_len);
    memcpy(out + base_len, ext, ext_len + 1);
    return out;
}

static int singe_load_dt_texture(const char *path, pvr_ptr_t *out_tex,
                                 uint32_t *out_w, uint32_t *out_h,
                                 uint32_t *out_pvr_format) {
    file_t fd;
    SingeDtHeader hdr;
    size_t tex_size;
    pvr_ptr_t tex;
    uint8_t scratch[4096];
    size_t copied = 0;

    if (!path || !out_tex || !out_w || !out_h || !out_pvr_format)
        return -1;

    fd = fs_open(path, O_RDONLY);
    if (fd < 0)
        return -1;

    if (fs_read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        SINGE_LOG(SINGE_LOG_OVERLAY, "[Sprite] DT reject read failed: %s", path);
        fs_close(fd);
        return -1;
    }

    if (memcmp(hdr.fourcc, "DcTx", 4) != 0 ||
        hdr.total_size <= SINGE_DT_HEADER_SIZE ||
        hdr.width_pixels == 0 ||
        hdr.height_pixels == 0) {
        SINGE_LOG(SINGE_LOG_OVERLAY,
                  "[Sprite] DT reject header: %s fourcc=%c%c%c%c total=%lu version=%u size=%ux%u pvr=0x%08lx",
                  path,
                  hdr.fourcc[0], hdr.fourcc[1], hdr.fourcc[2], hdr.fourcc[3],
                  (unsigned long)hdr.total_size,
                  (unsigned)hdr.version,
                  (unsigned)hdr.width_pixels,
                  (unsigned)hdr.height_pixels,
                  (unsigned long)hdr.pvr_type);
        fs_close(fd);
        return -1;
    }

    tex_size = hdr.total_size - SINGE_DT_HEADER_SIZE;
    tex = pvr_mem_malloc(tex_size);
    if (!tex) {
        SINGE_LOG(SINGE_LOG_OVERLAY, "[Sprite] DT reject pvr_mem_malloc failed: %s bytes=%lu",
                  path, (unsigned long)tex_size);
        fs_close(fd);
        return -1;
    }

    while (copied < tex_size) {
        size_t want = tex_size - copied;
        ssize_t got;
        if (want > sizeof(scratch))
            want = sizeof(scratch);
        got = fs_read(fd, scratch, want);
        if (got <= 0) {
            SINGE_LOG(SINGE_LOG_OVERLAY, "[Sprite] DT reject data read failed: %s copied=%lu total=%lu",
                      path, (unsigned long)copied, (unsigned long)tex_size);
            pvr_mem_free(tex);
            fs_close(fd);
            return -1;
        }
        memcpy((uint8_t *)tex + copied, scratch, (size_t)got);
        copied += (size_t)got;
    }

    fs_close(fd);
    *out_tex = tex;
    *out_w = hdr.width_pixels;
    *out_h = hdr.height_pixels;
    *out_pvr_format = hdr.pvr_type & SINGE_DT_PVR_FORMAT_MASK;
    return 0;
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
            if (sprite->hash_id == hash_value && !sprite->is_font_sprite) {
                // DC_log("Sprite found in cache with hash_id: %lu\n", hash_value);
                free(fullpath);
                return sprite;  // Return the cached sprite
            }
        }

        // If not found, load the texture as usual
        // DC_log("Sprite not found in cache, loading new sprite: %s\n", name_or_hash);
        int w = 0, h = 0;
        pvr_ptr_t tex = NULL;
        uint32_t pvr_format = PVR_TXRFMT_ARGB4444;
        char *dt_path = singe_make_sibling_path_with_ext(fullpath, ".dt");

        log_memory_stats("before_sprite_texture_load");
        if (dt_path &&
            singe_load_dt_texture(dt_path, &tex, (uint32_t *)&w, (uint32_t *)&h, &pvr_format) == 0) {
            SINGE_LOG(SINGE_LOG_OVERLAY, "[Sprite] Loaded DT texture: %s %dx%d fmt=0x%08lx",
                      dt_path, w, h, (unsigned long)pvr_format);
        } else if (png_load_texture(fullpath, &tex, PNG_FULL_ALPHA, (uint32_t*)&w, (uint32_t*)&h) < 0) {
            DC_log("Failed to load sprite texture: %s\n", fullpath);
            free(dt_path);
            free(fullpath);
            return NULL;
        } else {
            pvr_format = PVR_TXRFMT_ARGB4444;
            SINGE_LOG(SINGE_LOG_OVERLAY, "[Sprite] Loaded PNG texture fallback: %s %dx%d",
                      fullpath, w, h);
        }
        log_memory_stats("after_sprite_texture_load");

        // DC_log("Loaded sprite texture with dimensions: %dx%d\n", w, h);

        // Create and initialize the new sprite
        SingeSprite *new_sprite = Singe_xmalloc(sizeof(SingeSprite));
        new_sprite->name = Singe_xstrdup(name_or_hash);  // Store original name for debugging
        new_sprite->is_font_sprite = 0;
        new_sprite->font_index = -1;
        new_sprite->frame_count = 1;
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
        pvr_poly_cxt_txr(&cxt, PVR_LIST_TR_POLY, (int)pvr_format,
                         w, h, tex, is_320 ? PVR_FILTER_BILINEAR : PVR_FILTER_NONE);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
        pvr_poly_compile(&new_sprite->hdr, &cxt);

        free(dt_path);
        free(fullpath);
        // DC_log("Sprite created with hash: %lu\n",new_sprite->hash_id);
        return new_sprite;
    }
    return NULL;
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
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
#endif
    long size= (long)fs_read(ud->fd, buf, len);
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
#endif

    return size;
}

static const char *lua_reader(lua_State *L, void *data, size_t *size) {
    static uint8_t __attribute__((aligned(32))) buffer[1024];
    FileIoUserdata *ud = (FileIoUserdata *)data;
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
#endif
    long br = fs_read(ud->fd, buffer, sizeof(buffer));
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
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

    printf("[Lua] dofile open: %s -> %s\n", filename, fullpath);

    SINGE_IO_LOCK();
    file_t fd = fs_open(fullpath, O_RDONLY);
    SINGE_IO_UNLOCK();

    if (fd < 0) {
        free(fullpath);
        return luaL_error(L, "cannot open %s", filename);
    }

    SINGE_IO_LOCK();
    int size = fs_total(fd);
    SINGE_IO_UNLOCK();

    if (size <= 0) {
        SINGE_IO_LOCK();
        fs_close(fd);
        SINGE_IO_UNLOCK();
        free(fullpath);
        return luaL_error(L, "cannot get size for %s", filename);
    }

    char *buf = malloc(size + 1);
    if (!buf) {
        SINGE_IO_LOCK();
        fs_close(fd);
        SINGE_IO_UNLOCK();
        free(fullpath);
        return luaL_error(L, "out of memory loading %s (%d bytes)", filename, size);
    }

    int total = 0;
    while (total < size) {
        SINGE_IO_LOCK();
        int br = fs_read(fd, buf + total, size - total);
        SINGE_IO_UNLOCK();

        if (br <= 0) break;
        total += br;
    }

    SINGE_IO_LOCK();
    fs_close(fd);
    SINGE_IO_UNLOCK();

    buf[total] = '\0';

    printf("[Lua] dofile read: %s bytes=%d expected=%d\n", filename, total, size);

    if (total != size) {
        free(buf);
        free(fullpath);
        return luaL_error(L, "short read loading %s: got %d expected %d", filename, total, size);
    }

    char chunkname[256];
    snprintf(chunkname, sizeof(chunkname), "@%s", filename);

    int rc = luaL_loadbuffer(L, buf, total, chunkname);
    free(buf);
    free(fullpath);

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
    int x = (int)luaL_optnumber(L, 1, 0);
    int y = (int)luaL_optnumber(L, 2, 0);
    int luma = 0;

    if (dcfmv_current && dcfmv_current->frame_type == 1 &&
        x >= 0 && y >= 0 &&
        x < dcfmv_current->content_width &&
        y < dcfmv_current->content_height) {
        int total = atomic_load(&dcfmv_current->displayed_total_frame);
        int unique = dcfmv_total_to_unique(dcfmv_current, total);
        int buf = unique % DCFMV_NUM_BUFFERS;

        if (buf >= 0 && atomic_load(&dcfmv_current->buf_state[buf]) == DCFMV_BUF_READY) {
            const uint8_t *frame = dcfmv_current->frame_buffer[buf];
            int stride = dcfmv_current->video_width * 2;
            int pair_x = x & ~1;
            int off = y * stride + pair_x * 2;

            if (frame && off >= 0 && off + 3 < dcfmv_current->video_frame_size) {
                int pair_index = x & 1;
                int yuyv_luma = frame[off + (pair_index ? 2 : 0)];
                int uyvy_luma = frame[off + (pair_index ? 3 : 1)];
                int yuyv_delta = abs(yuyv_luma - 128);
                int uyvy_delta = abs(uyvy_luma - 128);

                /*
                 * DCMV/PVR YUV422 data may be authored as YUYV or UYVY.
                 * ActionMax only needs black/white state, so choose the byte
                 * that looks like luma instead of neutral chroma.
                 */
                luma = (yuyv_delta >= uyvy_delta) ? yuyv_luma : uyvy_luma;
            }
        }
    }

    lua_pushinteger(L, luma);
    lua_pushinteger(L, luma);
    lua_pushinteger(L, luma);
    return 3;
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
    return sep_mpeg_get_rawpixel(L);
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
#ifndef LUA_OK
#define LUA_OK 0
#endif

#define lua_rawlen lua_objlen
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

    overlay_ready = true;
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
    overlay_ready = true;
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
    if (!g_aim_assist_capture_active ||
        !g_cfg_aim_assist_red_only ||
        aim_assist_color_is_red(GFontColorR, GFontColorG, GFontColorB)) {
        g_last_hitbox_x1 = x1;
        g_last_hitbox_y1 = y1;
        g_last_hitbox_x2 = x2;
        g_last_hitbox_y2 = y2;
        g_last_hitbox_valid = 1;
        g_last_hitbox_ms = timer_ms_gettime64();
        g_last_hitbox_r = GFontColorR;
        g_last_hitbox_g = GFontColorG;
        g_last_hitbox_b = GFontColorB;
    }

    static int overlay_box_log_count = 0;
    if (overlay_box_log_count < 200) {
        Singe_log("[OVERLAY_BOX] in=(%d,%d)-(%d,%d) center=(%.1f,%.1f) color=(%d,%d,%d) screen=(%.1f,%.1f)-(%.1f,%.1f) scale=(%.3f,%.3f) ratio_offset=(%.1f,%.1f)",
                  x1, y1, x2, y2,
                  (x1 + x2) * 0.5f, (y1 + y2) * 0.5f,
                  GFontColorR, GFontColorG, GFontColorB,
                  scaled_x1, scaled_y1, scaled_x2, scaled_y2,
                  g_scale_x, g_scale_y,
                  g_ratio_x_offset, g_ratio_y_offset);
        overlay_box_log_count++;
    }

    if (!g_cfg_hitbox_draw) {
        lua_pushboolean(L, 1);
        return 1;
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
            float mag2 = dx * dx + dy * dy;
            if (!(mag2 > 0.0001f) || !isfinite(mag2))
                continue;

            float inv = frsqrt(mag2) * (width * 0.5f);
            float nx = -dy * inv, ny = dx * inv;
            if (!isfinite(nx) || !isfinite(ny))
                continue;

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

static int overlay_compute_line_normal(float x1, float y1,
                                       float x2, float y2,
                                       float width,
                                       float *nx_out, float *ny_out) {
    float dx = x2 - x1;
    float dy = y2 - y1;
    float mag2 = dx * dx + dy * dy;

    if (!(mag2 > 0.0001f) || !isfinite(mag2)) {
        return 0;
    }

    float invmag = frsqrt(mag2) * (width * 0.5f);
    float nx = -dy * invmag;
    float ny =  dx * invmag;

    if (!isfinite(nx) || !isfinite(ny)) {
        return 0;
    }

    *nx_out = nx;
    *ny_out = ny;
    return 1;
}

typedef struct {
    unsigned batches;
    unsigned vertices;
    unsigned fallbacks;
} DcPvrBatchStats;

static DcPvrBatchStats g_pvr_batch_stats;
static size_t g_pvr_tr_vertbuf_bytes_this_frame = 0;
static uint8_t g_pvr_tr_vertbuf[DCSINGE_PVR_TR_VERTBUF_BYTES] __attribute__((aligned(32)));
static int g_pvr_tr_vertbuf_ready = 0;

static void dc_pvr_batch_frame_begin(void) {
    g_pvr_tr_vertbuf_bytes_this_frame = 0;
}

static void dc_pvr_batch_frame_end(void) {
#if DCSINGE_DEBUG_PVR_BATCH
    static unsigned frame_counter = 0;
    if ((++frame_counter & 63) == 0) {
        printf("[PVR_BATCH] dma=%d batches=%u vertices=%u fallbacks=%u frame_bytes=%lu\n",
               pvr_vertex_dma_enabled() ? 1 : 0,
               g_pvr_batch_stats.batches,
               g_pvr_batch_stats.vertices,
               g_pvr_batch_stats.fallbacks,
               (unsigned long)g_pvr_tr_vertbuf_bytes_this_frame);
    }
#endif
}

/*
 * Direct PVR vertex-buffer submission follows the pattern JNMARTIN documented
 * in pvr_dma_rendering/main_dma.c: pvr_vertbuf_tail(), direct header/vertex
 * writes using sh4zam's aligned shz_memcpy32(), then pvr_vertbuf_written(). DCSinge
 * only uses that 2D submission pattern here; none of the demo's transform,
 * clipping, camera, or near-Z pipeline is used.
 */
static void dc_pvr_emit_tr_poly_fallback(const pvr_poly_hdr_t *hdr,
                                         const pvr_vertex_t *vertices,
                                         size_t vertex_count) {
    pvr_prim(hdr, sizeof(*hdr));
    for (size_t i = 0; i < vertex_count; i++) {
        pvr_prim(&vertices[i], sizeof(vertices[i]));
    }
    g_pvr_batch_stats.fallbacks++;
}

static int dc_pvr_emit_tr_poly_direct(const pvr_poly_hdr_t *hdr,
                                      const pvr_vertex_t *vertices,
                                      size_t vertex_count) {
#if DCSINGE_USE_PVR_VERTBUF_BATCH
    const size_t bytes = sizeof(*hdr) + vertex_count * sizeof(vertices[0]);

    if (!pvr_vertex_dma_enabled() || !g_pvr_tr_vertbuf_ready || !vertices || vertex_count == 0 ||
        (bytes & 31) != 0 ||
        g_pvr_tr_vertbuf_bytes_this_frame + bytes > DCSINGE_PVR_VERTBUF_TR_BUDGET_BYTES) {
        return 0;
    }

    uint8_t *tail = (uint8_t *)pvr_vertbuf_tail(PVR_LIST_TR_POLY);
    if (!tail) return 0;

    if ((((uintptr_t)tail | (uintptr_t)hdr | (uintptr_t)vertices) & 7) == 0 &&
        (((uintptr_t)tail) & 31) == 0) {
        shz_memcpy32(tail, hdr, sizeof(*hdr));
        shz_memcpy32(tail + sizeof(*hdr), vertices, vertex_count * sizeof(vertices[0]));
    } else {
        shz_memcpy(tail, hdr, sizeof(*hdr));
        shz_memcpy(tail + sizeof(*hdr), vertices, vertex_count * sizeof(vertices[0]));
    }
    pvr_vertbuf_written(PVR_LIST_TR_POLY, bytes);

    g_pvr_tr_vertbuf_bytes_this_frame += bytes;
    g_pvr_batch_stats.batches++;
    g_pvr_batch_stats.vertices += (unsigned)vertex_count;
    return 1;
#else
    (void)hdr;
    (void)vertices;
    (void)vertex_count;
    return 0;
#endif
}

static void dc_pvr_emit_tr_poly_batch(const pvr_poly_hdr_t *hdr,
                                      const pvr_vertex_t *vertices,
                                      size_t vertex_count) {
    if (!dc_pvr_emit_tr_poly_direct(hdr, vertices, vertex_count)) {
        dc_pvr_emit_tr_poly_fallback(hdr, vertices, vertex_count);
    }
}

static void dc_pvr_append_colored_quad(const pvr_poly_hdr_t *hdr,
                                       pvr_vertex_t *vertices,
                                       size_t *vertex_count,
                                       const pvr_vertex_t quad[4]) {
    const size_t max_vertices = DCSINGE_PVR_QUAD_BATCH_MAX * 4;
    if (*vertex_count + 4 > max_vertices) {
        dc_pvr_emit_tr_poly_batch(hdr, vertices, *vertex_count);
        *vertex_count = 0;
    }

    memcpy(&vertices[*vertex_count], quad, sizeof(pvr_vertex_t) * 4);
    *vertex_count += 4;
}

static void dc_pvr_flush_colored_quads(const pvr_poly_hdr_t *hdr,
                                       pvr_vertex_t *vertices,
                                       size_t *vertex_count) {
    if (*vertex_count == 0) return;
    dc_pvr_emit_tr_poly_batch(hdr, vertices, *vertex_count);
    *vertex_count = 0;
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
    float width = 2.0f;
    float nx, ny;
    if (!overlay_compute_line_normal(scaled_x1, scaled_y1,
                                     scaled_x2, scaled_y2,
                                     width, &nx, &ny)) {
        lua_pushboolean(L, 1);
        return 1;
    }

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
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    // Get table length
    int num_lines = lua_rawlen(L, 1);
    pvr_vertex_t batch[DCSINGE_PVR_QUAD_BATCH_MAX * 4];
    size_t batch_vertices = 0;
    
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
        float width = 2.0f;
        float nx, ny;
        if (!overlay_compute_line_normal(scaled_x1, scaled_y1,
                                         scaled_x2, scaled_y2,
                                         width, &nx, &ny)) {
            continue;
        }
        
        pvr_vertex_t quad[4] = {
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x1 + nx, .y = scaled_y1 + ny, .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x1 - nx, .y = scaled_y1 - ny, .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x2 + nx, .y = scaled_y2 + ny, .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x2 - nx, .y = scaled_y2 - ny, .z = 1.0f, .argb = color, .oargb = 0 }
        };
        dc_pvr_append_colored_quad(&hdr, batch, &batch_vertices, quad);
    }
    dc_pvr_flush_colored_quads(&hdr, batch, &batch_vertices);
    
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
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    float pixel = 2.0f;
    
    // Get table length
    int num_plots = lua_rawlen(L, 1);
    pvr_vertex_t batch[DCSINGE_PVR_QUAD_BATCH_MAX * 4];
    size_t batch_vertices = 0;
    
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
        
        pvr_vertex_t quad[4] = {
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x,         .y = scaled_y,         .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x + pixel, .y = scaled_y,         .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX,     .x = scaled_x,         .y = scaled_y + pixel, .z = 1.0f, .argb = color, .oargb = 0 },
            { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x + pixel, .y = scaled_y + pixel, .z = 1.0f, .argb = color, .oargb = 0 }
        };
        dc_pvr_append_colored_quad(&hdr, batch, &batch_vertices, quad);
    }
    dc_pvr_flush_colored_quads(&hdr, batch, &batch_vertices);
    
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
    const int draw_hitbox = g_cfg_hitbox_draw;
    
    // Compile header once
    static pvr_poly_hdr_t hdr;
    static bool header_compiled = false;
    
    if (draw_hitbox && !header_compiled) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        pvr_poly_compile(&hdr, &cxt);
        header_compiled = true;
    }
    
    uint32_t color =
        ((GFontColorA & 0xFF) << 24) |
        ((GFontColorR & 0xFF) << 16) |
        ((GFontColorG & 0xFF) << 8)  |
        ((GFontColorB & 0xFF));
    
    // Get table length
    int num_boxes = lua_rawlen(L, 1);
    pvr_vertex_t batch[DCSINGE_PVR_QUAD_BATCH_MAX * 4];
    size_t batch_vertices = 0;
    
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
        if (!g_aim_assist_capture_active ||
            !g_cfg_aim_assist_red_only ||
            aim_assist_color_is_red(GFontColorR, GFontColorG, GFontColorB)) {
            g_last_hitbox_x1 = x1;
            g_last_hitbox_y1 = y1;
            g_last_hitbox_x2 = x2;
            g_last_hitbox_y2 = y2;
            g_last_hitbox_valid = 1;
            g_last_hitbox_ms = timer_ms_gettime64();
            g_last_hitbox_r = GFontColorR;
            g_last_hitbox_g = GFontColorG;
            g_last_hitbox_b = GFontColorB;
        }
        
        if (draw_hitbox) {
            // Scale coordinates
            float scaled_x1 = ((x1 / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
            float scaled_y1 = ((y1 / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
            float scaled_x2 = ((x2 / (float)GOverlayWidth)  * 640.0f - g_ratio_x_offset) * g_scale_x;
            float scaled_y2 = ((y2 / (float)GOverlayHeight) * 480.0f - g_ratio_y_offset) * g_scale_y;
            
            pvr_vertex_t quad[4] = {
                { .flags = PVR_CMD_VERTEX,     .x = scaled_x1, .y = scaled_y1, .z = 1.0f, .argb = color, .oargb = 0 },
                { .flags = PVR_CMD_VERTEX,     .x = scaled_x2, .y = scaled_y1, .z = 1.0f, .argb = color, .oargb = 0 },
                { .flags = PVR_CMD_VERTEX,     .x = scaled_x1, .y = scaled_y2, .z = 1.0f, .argb = color, .oargb = 0 },
                { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x2, .y = scaled_y2, .z = 1.0f, .argb = color, .oargb = 0 }
            };
            dc_pvr_append_colored_quad(&hdr, batch, &batch_vertices, quad);
        }
    }
    if (draw_hitbox) {
        dc_pvr_flush_colored_quads(&hdr, batch, &batch_vertices);
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
        g_mp3_init_failed = 0;
        return;
    }

    if (g_mp3_stream_inited) {
        return;
    }

    printf("[Music] Initializing MP3 system...\n");
    if (mp3_init() < 0) {
        printf("[Music] ERROR: mp3_init failed\n");
        g_mp3_init_failed = 1;
        g_current_playing_handle = -1;
        return;
    }
    g_mp3_stream_inited = 1;
    g_mp3_init_failed = 0;
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
        g_mp3_init_failed = 0;
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
    dcsinge_storage_shutdown();
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

    if (!g_mp3_stream_inited) {
        if (g_mp3_init_failed) {
            printf("[Music] ERROR: MP3 playback unavailable for %s because mp3_init failed\n",
                   track->filepath);
            printf("[Music]        This title enables both FMV audio and MP3; set disable_fmv_audio=1 to prefer MP3\n");
        } else {
            printf("[Music] ERROR: MP3 playback requested before MP3 system initialized for %s\n",
                   track->filepath);
        }
        g_current_playing_handle = -1;
        track->failed_to_play = 1;
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
static uint16_t read_le16_buf(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32_buf(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t singe_probe_wav_channels(const char *path) {
    uint8_t hdr[12];
    file_t fd = fs_open(path, O_RDONLY);
    uint16_t ch = 1;

    if (fd < 0) return 1;

    if (fs_read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fs_close(fd);
        return 1;
    }

    while (1) {
        uint8_t chunk[8];
        uint32_t chunk_size;
        long next;

        if (fs_read(fd, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk)) break;
        chunk_size = read_le32_buf(chunk + 4);
        next = fs_tell(fd) + (long)((chunk_size + 1u) & ~1u);

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint8_t fmt[16];
            if (fs_read(fd, fmt, sizeof(fmt)) != (ssize_t)sizeof(fmt)) break;
            ch = read_le16_buf(fmt + 2);
            break;
        }

        fs_seek(fd, next, SEEK_SET);
    }

    fs_close(fd);
    return ch ? ch : 1;
}

static uint32_t singe_probe_wav_rate(const char *path) {
    uint8_t hdr[12];
    file_t fd = fs_open(path, O_RDONLY);
    uint32_t rate = 44100;

    if (fd < 0) return rate;

    if (fs_read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fs_close(fd);
        return rate;
    }

    while (1) {
        uint8_t chunk[8];
        uint32_t chunk_size;
        long next;

        if (fs_read(fd, chunk, sizeof(chunk)) != (ssize_t)sizeof(chunk)) break;
        chunk_size = read_le32_buf(chunk + 4);
        next = fs_tell(fd) + (long)((chunk_size + 1u) & ~1u);

        if (memcmp(chunk, "fmt ", 4) == 0 && chunk_size >= 16) {
            uint8_t fmt[16];
            if (fs_read(fd, fmt, sizeof(fmt)) != (ssize_t)sizeof(fmt)) break;
            rate = read_le32_buf(fmt + 4);
            break;
        }

        fs_seek(fd, next, SEEK_SET);
    }

    fs_close(fd);
    return rate ? rate : 44100;
}

static uint32_t singe_dca_rate_hz(uint16_t aica_rate) {
    int freq_hi = (aica_rate >> 11) & 0x0f;
    unsigned int freq_lo = aica_rate & 0x03ff;
    double rate;

    if (freq_hi & 0x08)
        freq_hi |= ~0x0f;

    rate = 44100.0 * pow(2.0, freq_hi) * (1.0 + ((double)freq_lo / 1024.0));
    if (rate < 172.0)
        rate = 172.0;
    if (rate > 88200.0)
        rate = 88200.0;
    return (uint32_t)(rate + 0.5);
}

static sfxhnd_t singe_load_dca_sfx(const char *dca_path, const char *wav_path,
                                   uint16_t *out_channels) {
    uint8_t hdr[32];
    file_t fd;
    uint32_t total_size;
    uint16_t flags;
    uint16_t aica_rate;
    uint16_t channels;
    uint32_t rate;
    size_t data_size;
    char *data;
    sfxhnd_t sfx;

    if (out_channels)
        *out_channels = 1;

    fd = fs_open(dca_path, O_RDONLY);
    if (fd < 0)
        return SFXHND_INVALID;

    if (fs_read(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        memcmp(hdr, "DcAF", 4) != 0) {
        fs_close(fd);
        return SFXHND_INVALID;
    }

    total_size = read_le32_buf(hdr + 4);
    flags = read_le16_buf(hdr + 12);
    aica_rate = read_le16_buf(hdr + 14);
    if (((flags >> 7) & 0x03) != 2) {
        fs_close(fd);
        return SFXHND_INVALID;
    }
    channels = flags & 0x07;
    if (channels < 1 || channels > 2)
        channels = singe_probe_wav_channels(wav_path);
    rate = aica_rate ? singe_dca_rate_hz(aica_rate) : singe_probe_wav_rate(wav_path);

    if (total_size <= sizeof(hdr))
        total_size = (uint32_t)fs_total(fd);
    if (total_size <= sizeof(hdr)) {
        fs_close(fd);
        return SFXHND_INVALID;
    }

    data_size = total_size - sizeof(hdr);
    data = malloc(data_size);
    if (!data) {
        fs_close(fd);
        return SFXHND_INVALID;
    }

    if (fs_read(fd, data, data_size) != (ssize_t)data_size) {
        free(data);
        fs_close(fd);
        return SFXHND_INVALID;
    }

    fs_close(fd);
    sfx = snd_sfx_load_raw_buf(data, data_size, rate, 4, channels);
    free(data);
    if (sfx >= 0 && out_channels)
        *out_channels = channels;
    return sfx;
}

static SingeActiveSound *singe_find_active_sound(int channel) {
    if (!GActiveSoundsInit) {
        for (size_t i = 0; i < sizeof(GActiveSounds) / sizeof(GActiveSounds[0]); ++i) {
            GActiveSounds[i].channel = -1;
        }
        GActiveSoundsInit = 1;
    }

    if (channel < 0) return NULL;
    for (size_t i = 0; i < sizeof(GActiveSounds) / sizeof(GActiveSounds[0]); ++i) {
        if (GActiveSounds[i].channel == channel) {
            return &GActiveSounds[i];
        }
    }
    return NULL;
}

static void singe_track_active_sound(int channel, const SingeSound *sound) {
    if (channel < 0 || !sound) return;
    SingeActiveSound *slot = singe_find_active_sound(channel);
    if (!slot) {
        for (size_t i = 0; i < sizeof(GActiveSounds) / sizeof(GActiveSounds[0]); ++i) {
            if (GActiveSounds[i].channel < 0) {
                slot = &GActiveSounds[i];
                break;
            }
        }
    }
    if (!slot) return;

    slot->channel = channel;
    slot->channels = sound->channels ? sound->channels : 1;
}

static int singe_sound_channel_active(int channel) {
    SingeActiveSound *slot = singe_find_active_sound(channel);
    int active;

    if (channel < 0 || channel >= 64) return 0;

    active = snd_is_playing((unsigned)channel) ? 1 : 0;
    if (!active && slot && slot->channels > 1 && channel < 63) {
        active = snd_is_playing((unsigned)(channel + 1)) ? 1 : 0;
    }

    if (!active && slot) {
        slot->channel = -1;
        slot->channels = 0;
    }
    return active;
}

static int sep_sound_load(lua_State *L) {
    const char *path = lua_tostring(L, 1);
    char *fullpath = resolve_path(path);
    // DC_log("Loading sound: %s -> %s\n", path, fullpath);
    SINGE_LOG(SINGE_LOG_SFX, "[SFX] Loading: %s -> %s", path, fullpath ? fullpath : "(null)");
    
    // Check cache (use original path for cache key)
    for (SingeSound *sound = GSounds; sound != NULL; sound = sound->next) {
        if (strcmp(sound->name, path) == 0) {
            free(fullpath);
            lua_pushinteger(L, (lua_Integer)sound);
            return 1;
        }
    }
    
    // Load new sound (prefer preconverted Dreamcast ADPCM when present)
    char *dca_path = singe_make_sibling_path_with_ext(fullpath, ".dca");
    char *adpcm_path = singe_make_sibling_path_with_ext(fullpath, ".adpcm.wav");
    const char *load_path = fullpath;
    int load_dca = 0;
    uint16_t loaded_channels = 1;
    if (dca_path && check_file_exists(dca_path)) {
        load_path = dca_path;
        load_dca = 1;
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] Using DCA ADPCM: %s", load_path);
    } else if (adpcm_path && check_file_exists(adpcm_path)) {
        load_path = adpcm_path;
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] Using ADPCM WAV: %s", load_path);
    }

    log_memory_stats("before_sfx_load");
    sfxhnd_t sfx = load_dca ?
        singe_load_dca_sfx(load_path, fullpath, &loaded_channels) :
        snd_sfx_load(load_path);
    log_memory_stats("after_sfx_load");
    if (sfx < 0) {
        DC_log("Failed to load sound: %s", load_path);
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] Failed to load: %s", load_path);
        free(dca_path);
        free(adpcm_path);
        free(fullpath);
        lua_pushinteger(L, -1);
        return 1;
    }
    
    SingeSound *sound = Singe_xmalloc(sizeof(SingeSound));
    sound->name = Singe_xstrdup(path);  // Store original path for cache
    sound->handle = sfx;
    sound->channels = load_dca ? loaded_channels : singe_probe_wav_channels(load_path);
    sound->next = GSounds;
    GSounds = sound;
    SINGE_LOG(SINGE_LOG_SFX, "[SFX] Loaded successfully: %s handle=%lu ptr=%p channels=%u",
              load_path,
              (unsigned long)sfx,
              (void *)(uintptr_t)sfx,
              (unsigned)sound->channels);
    
    free(dca_path);
    free(adpcm_path);
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
    SingeSound *sound = NULL;

    if (sound_id == 0 || sound_id == -1) {
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] soundPlay(%ld) skipped invalid id", (long)sound_id);
        lua_pushinteger(L, -1);
        return 1;
    }

    uintptr_t sound_ptr = (sound_id < 0)
        ? (uintptr_t)(uint32_t)sound_id
        : (uintptr_t)sound_id;

    for (SingeSound *it = GSounds; it != NULL; it = it->next) {
        if ((uintptr_t)it == sound_ptr) {
            sound = it;
            break;
        }
    }

    if (sound && sound->handle >= 0) {
        // Convert global volume (0–255) to sfx API scale
        int vol = dcfmv_audio_volume(dcfmv_current);
        if (vol < 0) vol = 0;
        if (vol > 255) vol = 255;

        SINGE_LOG(SINGE_LOG_SFX, "[SFX] Play requested: sound=%p handle=%lu ptr=%p vol=%d",
                  (void *)sound,
                  (unsigned long)sound->handle,
                  (void *)(uintptr_t)sound->handle,
                  vol);
        // SFX playback shares KOS' AICA/G2 transfer path with FMV audio.
        dcfmv_audio_transfer_lock();
        int chn = snd_sfx_play(sound->handle, vol, 128);
        dcfmv_audio_transfer_unlock();
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] snd_sfx_play returned chn=%d", chn);
        singe_track_active_sound(chn, sound);

        // printf("[Singe] soundPlay(id=%ld, vol=%d)\n", (long)sound_id, vol);
        lua_pushinteger(L, chn);
    } else {
        SINGE_LOG(SINGE_LOG_SFX, "[Singe] soundPlay(%ld) -> invalid handle", (long)sound_id);
        lua_pushinteger(L, -1);
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
    int channel = (int)lua_tointeger(L, 1);
    SingeActiveSound *slot = singe_find_active_sound(channel);
    uint16_t channels = slot && slot->channels ? slot->channels : 1;

    if (channel >= 0) {
        dcfmv_audio_transfer_lock();
        snd_sfx_stop(channel);
        if (channels > 1 && channel < 63) {
            snd_sfx_stop(channel + 1);
        }
        dcfmv_audio_transfer_unlock();
        if (slot) {
            slot->channel = -1;
            slot->channels = 0;
        }
        SINGE_LOG(SINGE_LOG_SFX, "[SFX] soundStop(%d)", channel);
    }

    return 0;
}

static int sep_sound_is_playing(lua_State *L) {
    if (!lua_isnumber(L, 1)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    int channel = (int)lua_tointeger(L, 1);
    int active = singe_sound_channel_active(channel);
    lua_pushboolean(L, active);
    return 1;
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
static void log_invalid_sprite_draw(lua_State *L, int n, const char *reason) {
    static int invalid_sprite_draw_logs = 0;
    if (invalid_sprite_draw_logs >= 128) return;
    invalid_sprite_draw_logs++;

    lua_Debug ar;
    const char *src = "(unknown)";
    int line = 0;
    if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "Sl", &ar)) {
        src = ar.short_src;
        line = ar.currentline;
    }

    char args[192];
    size_t used = 0;
    args[0] = '\0';
    for (int i = 1; i <= n && used < sizeof(args); i++) {
        int type = lua_type(L, i);
        int written = 0;
        if (type == LUA_TNUMBER) {
            written = snprintf(args + used, sizeof(args) - used, "%s%d:number=%ld",
                               (i == 1) ? "" : " ", i, (long)lua_tointeger(L, i));
        } else if (type == LUA_TBOOLEAN) {
            written = snprintf(args + used, sizeof(args) - used, "%s%d:boolean=%d",
                               (i == 1) ? "" : " ", i, lua_toboolean(L, i));
        } else {
            written = snprintf(args + used, sizeof(args) - used, "%s%d:%s",
                               (i == 1) ? "" : " ", i, lua_typename(L, type));
        }
        if (written < 0) break;
        if ((size_t)written >= sizeof(args) - used) {
            used = sizeof(args) - 1;
            args[used] = '\0';
            break;
        }
        used += (size_t)written;
    }

    DC_log("Invalid spriteDraw at %s:%d (%s, argc=%d args=[%s])\n",
           src, line, reason, n, args);
}

int sep_sprite_draw(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 3) return 0;

    int x = 0, y = 0, x2 = 0, y2 = 0;
    bool center = false;
    unsigned long sprite_hash_id = 0;
    SingeSprite *sprite = NULL;
    lua_Integer sprite_arg = 0;

    // Parse parameters based on mode
    if (n == 3) {  // spriteDraw(x, y, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) && lua_isnumber(L, 3)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            sprite_arg = lua_tointeger(L, 3);
            sprite = (SingeSprite *)sprite_arg;
            if (sprite) sprite_hash_id = sprite->hash_id;
        }
    } else if (n == 4) {  // spriteDraw(x, y, c, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
            lua_isboolean(L, 3) && lua_isnumber(L, 4)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            center = lua_toboolean(L, 3);
            sprite_arg = lua_tointeger(L, 4);
            sprite = (SingeSprite *)sprite_arg;
            if (sprite) sprite_hash_id = sprite->hash_id;
        }
    } else if (n == 5) {  // spriteDraw(x, y, x2, y2, id)
        if (lua_isnumber(L, 1) && lua_isnumber(L, 2) &&
            lua_isnumber(L, 3) && lua_isnumber(L, 4) && lua_isnumber(L, 5)) {
            x = (int)lua_tonumber(L, 1);
            y = (int)lua_tonumber(L, 2);
            x2 = (int)lua_tonumber(L, 3);
            y2 = (int)lua_tonumber(L, 4);
            sprite_arg = lua_tointeger(L, 5);
            sprite = (SingeSprite *)sprite_arg;
            if (sprite) sprite_hash_id = sprite->hash_id;
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
            sprite_arg = lua_tointeger(L, 6);
            sprite = (SingeSprite *)sprite_arg;
            if (sprite) sprite_hash_id = sprite->hash_id;
        }
    }

    if (sprite_hash_id == 0) {
        log_invalid_sprite_draw(L, n, sprite_arg == 0 ? "nil-or-zero sprite" : "invalid arguments");
        return 0;
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
    int draw_w = w;
    int draw_h = h;

// --- Match coordinate transform used by fonts and overlays ---
	float scaled_xf = (x * g_scale_x) + g_ratio_x_offset;
	float scaled_yf = (y * g_scale_y) + g_ratio_y_offset;

int screen_x = (int)roundf(scaled_xf);
int screen_y = (int)roundf(scaled_yf);

scaled_x = screen_x;
scaled_y = screen_y;

/*
 * Crosshair sprites are authored around their visual center in Lua
 * (cursorX/Y already subtract half sprite size). Do not apply the legacy
 * non-centered X tweak to those assets, or the rendered crosshair appears
 * right of the actual hit-test point.
 */
	const int is_crosshair_sprite = (sprite->name && strstr(sprite->name, "crosshair") != NULL);
    if (sprite->is_font_sprite && (n == 3 || n == 4)) {
        draw_w = (int)roundf((float)w * (float)g_scale_x);
        draw_h = (int)roundf((float)h * (float)g_scale_y);
        if (draw_w < 1) draw_w = 1;
        if (draw_h < 1) draw_h = 1;
    }
	if (center) {
	    scaled_x -= draw_w / 2;
	} else if (!is_crosshair_sprite) {
	    // Legacy offset kept for non-crosshair sprites.
	    scaled_x -= draw_w / 4;
	}
if (is_crosshair_sprite) {
    scaled_x += g_cfg_crosshair_offset_x;
    scaled_y += g_cfg_crosshair_offset_y;
}

	// Clamp to display bounds (not overlay bounds)
	if (scaled_x < 0) scaled_x = 0;
	if (scaled_y < 0) scaled_y = 0;
	if (scaled_x + draw_w > g_display_w)  scaled_x = g_display_w  - draw_w;
	if (scaled_y + draw_h > g_display_h)  scaled_y = g_display_h - draw_h;

	#ifdef DEBUG_SPRITEDRAW
	    const char *mode_str = "";
	    if (n == 3) mode_str = "simple";
	    else if (n == 4) mode_str = "centered";
	    else if (n == 5) mode_str = "stretched";
    else if (n == 6) mode_str = "centered_stretched";

	    Singe_log("Draw sprite '%s' mode=%s raw=(%d,%d) overlay=(%d,%d) size=%dx%d center=%d\n",
	           sprite->name ? sprite->name : "(unnamed)", mode_str,
		           x, y, scaled_x, scaled_y, draw_w, draw_h, center);
	#endif

    /*
     * TODO experimental: optionally mirror recognized hint/action sprites to
     * the VMU LCD. This should be convention/data driven from sprite->name
     * basenames such as arrowup/arrowdown/action/hold, with filtering for menu
     * selectors, and must not require game-specific Lua changes.
     */
	
		    // --- Issue PVR draw ---
		    pvr_vertex_t verts[4] = {
	        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,     .y = scaled_y,     .z = 1.0f, .u = 0.0f, .v = 0.0f, .argb = 0xFFFFFFFF },
	        { .flags = PVR_CMD_VERTEX,     .x = scaled_x + draw_w, .y = scaled_y,     .z = 1.0f, .u = 1.0f, .v = 0.0f, .argb = 0xFFFFFFFF },
	        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,     .y = scaled_y + draw_h, .z = 1.0f, .u = 0.0f, .v = 1.0f, .argb = 0xFFFFFFFF },
	        { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x + draw_w, .y = scaled_y + draw_h, .z = 1.0f, .u = 1.0f, .v = 1.0f, .argb = 0xFFFFFFFF }
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

static SingeSprite *resolve_lua_sprite(lua_State *L, int idx) {
    if (!lua_isnumber(L, idx)) return NULL;

    SingeSprite *sprite = (SingeSprite *)lua_tointeger(L, idx);
    if (!sprite || sprite->hash_id == 0) return NULL;

    char sprite_hash_str[64];
    snprintf(sprite_hash_str, sizeof(sprite_hash_str), "%lu", sprite->hash_id);
    return get_cached_sprite(sprite_hash_str);
}

static int sprite_frame_width(const SingeSprite *sprite) {
    int frames = (sprite && sprite->frame_count > 0) ? sprite->frame_count : 1;
    int width = sprite ? sprite->width / frames : 0;
    return width > 0 ? width : (sprite ? sprite->width : 0);
}

static int sprite_frame_height(const SingeSprite *sprite) {
    return sprite ? sprite->height : 0;
}

static int sep_sprite_animate(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 4) return 0;

    int x = (int)lua_tonumber(L, 1);
    int y = (int)lua_tonumber(L, 2);
    int frame = (int)lua_tonumber(L, 3);
    SingeSprite *sprite = resolve_lua_sprite(L, 4);
    if (!sprite || !sprite->texture) return 0;

    int frames = sprite->frame_count > 0 ? sprite->frame_count : 1;
    if (frame < 1) frame = 1;
    if (frame > frames) frame = frames;

    int frame_w = sprite_frame_width(sprite);
    int frame_h = sprite_frame_height(sprite);
    if (frame_w <= 0 || frame_h <= 0) return 0;

    float u0 = (float)(frame - 1) / (float)frames;
    float u1 = (float)frame / (float)frames;

    int scaled_x = (int)roundf((x * g_scale_x) + g_ratio_x_offset);
    int scaled_y = (int)roundf((y * g_scale_y) + g_ratio_y_offset);

    if (scaled_x < 0) scaled_x = 0;
    if (scaled_y < 0) scaled_y = 0;
    if (scaled_x + frame_w > g_display_w) scaled_x = g_display_w - frame_w;
    if (scaled_y + frame_h > g_display_h) scaled_y = g_display_h - frame_h;

    pvr_vertex_t verts[4] = {
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,           .y = scaled_y,           .z = 1.0f, .u = u0, .v = 0.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x + frame_w, .y = scaled_y,           .z = 1.0f, .u = u1, .v = 0.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX,     .x = scaled_x,           .y = scaled_y + frame_h, .z = 1.0f, .u = u0, .v = 1.0f, .argb = 0xFFFFFFFF },
        { .flags = PVR_CMD_VERTEX_EOL, .x = scaled_x + frame_w, .y = scaled_y + frame_h, .z = 1.0f, .u = u1, .v = 1.0f, .argb = 0xFFFFFFFF }
    };

    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), &sprite->hdr, 1);
    sq_fast_cpy((void *)SQ_MASK_DEST(PVR_TA_INPUT), verts, 4);

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

    SingeSprite *sprite = resolve_lua_sprite(L, 1);

    if (!sprite) {
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

    SingeSprite *sprite = resolve_lua_sprite(L, 1);

    if (!sprite) {
        lua_pushinteger(L, 0);
        return 1;
    }

    lua_pushinteger(L, sprite->height);
    // DC_log("[SINGE] spriteGetHeight('%s') = %d\n",
    //        sprite->name ? sprite->name : "(unnamed)", sprite->height);
    return 1;
}

static int sep_sprite_frame_width(lua_State *L) {
    SingeSprite *sprite = resolve_lua_sprite(L, 1);
    lua_pushinteger(L, sprite_frame_width(sprite));
    return 1;
}

static int sep_sprite_frame_height(lua_State *L) {
    SingeSprite *sprite = resolve_lua_sprite(L, 1);
    lua_pushinteger(L, sprite_frame_height(sprite));
    return 1;
}



static int sep_sprite_frames(lua_State *L) {
    SingeSprite *sprite = resolve_lua_sprite(L, 1);
    lua_pushinteger(L, sprite && sprite->frame_count > 0 ? sprite->frame_count : 0);
    return 1;
}

// --- Loading / Unloading ---
static int sep_sprite_loadframes(lua_State *L) {
    int n = lua_gettop(L);
    if (n < 2 || !lua_isnumber(L, 1) || !lua_isstring(L, 2)) {
        lua_pushnil(L);
        return 1;
    }

    int frames = (int)lua_tointeger(L, 1);
    const char *path = lua_tostring(L, 2);
    if (frames < 1) frames = 1;

    SingeSprite *sprite = get_cached_sprite(path);
    if (!sprite) {
        lua_pushnil(L);
        return 1;
    }

    sprite->frame_count = frames;
    lua_pushinteger(L, (lua_Integer)sprite);
    return 1;
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
    int logged_first_line;
    char ram_path[64];
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

static void reset_vmu_device_frame(void) {
    maple_device_t *dev = get_vmu_mount_device();
    if (!dev) {
        return;
    }

    if (dev->frame.queued) {
        maple_queue_remove(&dev->frame);
    }

    dev->frame.state = MAPLE_FRAME_VACANT;
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

static void free_io_cache_entry(SingeLuaFileCache *entry) {
    if (!entry) {
        return;
    }

    free(entry->path);
    free(entry->data);
    free(entry);
}

static void remove_io_cache_entry(const char *path) {
    SingeLuaFileCache **link = &g_io_cache;
    while (*link) {
        SingeLuaFileCache *entry = *link;
        if (strcmp(entry->path, path) == 0) {
            *link = entry->next;
            free_io_cache_entry(entry);
            return;
        }
        link = &entry->next;
    }
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

static int text_first_line_has_char(const char *data, size_t len, char ch) {
    if (!data) {
        return 0;
    }

    for (size_t i = 0; i < len && data[i] != '\n' && data[i] != '\r'; i++) {
        if (data[i] == ch) {
            return 1;
        }
    }

    return 0;
}

static int text_first_line_has_all_chars(const char *data, size_t len, const char *chars) {
    if (!data || !chars) {
        return 0;
    }

    for (const char *needle = chars; *needle; needle++) {
        if (!text_first_line_has_char(data, len, *needle)) {
            return 0;
        }
    }

    return 1;
}

static int is_save_slot_cfg_path(const char *path) {
    const char *slot = path ? strstr(path, "/Cfg/s") : NULL;
    if (!slot) {
        return 0;
    }

    slot += strlen("/Cfg/s");
    return slot[0] >= '1' && slot[0] <= '6' && strcmp(slot + 1, ".cfg") == 0;
}

static int io_cache_entry_valid_for_path(const char *path, const SingeLuaFileCache *entry) {
    if (!path || !entry) {
        return 0;
    }

    if (strstr(path, "/Cfg/game") && strstr(path, ".cfg")) {
        return text_first_line_has_char(entry->data, entry->len, '=');
    }

    if (strstr(path, "/Cfg/hscore") && strstr(path, ".cfg")) {
        return text_first_line_has_char(entry->data, entry->len, ',');
    }

    if (is_save_slot_cfg_path(path)) {
        return text_first_line_has_all_chars(entry->data, entry->len, ",!?;:ABCDEF");
    }

    return 1;
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

    /*
     * This runtime never consumes VMU button input, and the KOS periodic
     * polling path reuses the same maple frame as VMU filesystem I/O.
     * Keep it disabled so save/load traffic doesn't race the poller.
     */
    vmu_set_buttons_enabled(0);

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
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t *buffer = NULL;
        file_t fd = -1;
        size_t size = 0;
        int ok = 0;

        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(0);
            maple_queue_flush();
            thd_sleep(10);
        }
        reset_vmu_device_frame();
        wait_for_vmu_device_idle();

        fd = fs_open(g_vmu_save_path, O_RDONLY);
        if (fd < 0) {
            printf("[VMU] No existing save archive at %s\n", g_vmu_save_path);
            if (was_buttons_enabled) {
                vmu_set_buttons_enabled(1);
            }
            return 0;
        }

        size = fs_total(fd);
        buffer = malloc(size + 1);
        if (!buffer) {
            goto load_cleanup;
        }

        if (!read_exact(fd, buffer, size)) {
            goto load_cleanup;
        }

        ok = parse_vmu_archive(buffer, size);
        if (ok) {
            printf("[VMU] Loaded save archive %s (%zu bytes)\n", g_vmu_save_path, size);
        } else {
            printf("[VMU] Save archive %s was invalid; starting fresh\n", g_vmu_save_path);
        }

load_cleanup:
        if (fd >= 0) {
            fs_close(fd);
        }
        free(buffer);

        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }

        if (ok) {
            return 1;
        }

        reset_vmu_device_frame();
        maple_queue_flush();
        thd_sleep(25 + attempt * 25);
        printf("[VMU] Retry load archive attempt %d/3\n", attempt + 1);
    }

    return 0;
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
    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Persisting VMU archive to %s...\n", g_vmu_save_path);

    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t *archive = NULL;
        uint8_t *padded = NULL;
        size_t archive_len = 0;
        size_t padded_len = 0;
        file_t fd = -1;
        int was_buttons_enabled = vmu_get_buttons_enabled();
        int ok = 0;

        if (!serialize_vmu_archive(&archive, &archive_len)) {
            return 0;
        }

        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(0);
        }
        maple_queue_flush();
        thd_sleep(10);
        reset_vmu_device_frame();
        wait_for_vmu_device_idle();

        padded_len = (archive_len + 511u) & ~511u;
        padded = calloc(1, padded_len);
        if (!padded) {
            goto persist_cleanup;
        }
        memcpy(padded, archive, archive_len);

        fd = fs_open(g_vmu_save_path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) {
            printf("[VMU] Failed to open %s for writing\n", g_vmu_save_path);
            goto persist_cleanup;
        }

        if (fs_write(fd, padded, padded_len) != padded_len) {
            printf("[VMU] Short write while persisting archive to %s\n", g_vmu_save_path);
            goto persist_cleanup;
        }

        if (g_vmu_pkg.icon_data) {
            g_vmu_pkg.data_len = (int)archive_len;
            g_vmu_pkg.data = archive;
            if (fs_vmu_set_header(fd, &g_vmu_pkg) < 0) {
                printf("[VMU] Failed to set VMU header on %s\n", g_vmu_save_path);
            }
        }

        ok = 1;

persist_cleanup:
        if (fd >= 0) {
            fs_close(fd);
        }
        free(archive);
        free(padded);
        if (was_buttons_enabled) {
            vmu_set_buttons_enabled(1);
        }

        if (ok) {
            printf("[VMU] Saved archive %s (%zu bytes)\n", g_vmu_save_path, archive_len);
            return 1;
        }

        reset_vmu_device_frame();
        maple_queue_flush();
        thd_sleep(25 + attempt * 25);
        printf("[VMU] Retry persist archive attempt %d/3\n", attempt + 1);
    }

    return 0;
}

static int seed_vmu_archive_locked(void) {
    if (!g_vmu_available) {
        return 0;
    }

    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Seeding empty VMU archive at %s\n", g_vmu_save_path);
    return persist_vmu_archive_locked();
}

static void flush_vmu_archive_before_transition(const char *op, int frame) {
    if (!atomic_load(&g_vmu_flush_pending)) {
        return;
    }

    atomic_store(&g_vmu_flush_not_before_ms, 0);
    atomic_store(&g_vmu_flush_defer_until_frame, -1);
    SINGE_LOG(SINGE_LOG_VMU, "[VMU] Forcing pending save before %s(%d)", op ? op : "transition", frame);
    flush_vmu_archive_if_pending();
}

static void arm_vmu_transition_flush_window(void) {
    atomic_store(&g_vmu_transition_flush_until_ms, (uint64_t)dcfmv_ps_ms() + 2000);
}

static int vmu_transition_flush_window_active(void) {
    uint64_t until_ms = atomic_load(&g_vmu_transition_flush_until_ms);
    if (!until_ms) {
        return 0;
    }

    if ((uint64_t)dcfmv_ps_ms() > until_ms) {
        atomic_store(&g_vmu_transition_flush_until_ms, 0);
        return 0;
    }

    return 1;
}

static void flush_vmu_archive_if_pending(void) {
    mutex_lock(&g_vmu_flush_lock);

    if (!atomic_load(&g_vmu_flush_pending)) {
        mutex_unlock(&g_vmu_flush_lock);
        return;
    }

    uint64_t now_ms = (uint64_t)dcfmv_ps_ms();
    uint64_t not_before_ms = atomic_load(&g_vmu_flush_not_before_ms);
    if (not_before_ms > 0 && now_ms < not_before_ms) {
        mutex_unlock(&g_vmu_flush_lock);
        return;
    }

    int defer_until = atomic_load(&g_vmu_flush_defer_until_frame);
    if (defer_until >= 0 && dcfmv_current) {
        int cur = framefile_active_absolute_frame();
        int seek_active = dcfmv_seek_active(dcfmv_current);
        if (seek_active || cur <= defer_until) {
            mutex_unlock(&g_vmu_flush_lock);
            return;
        }
    }

    if (!atomic_exchange(&g_vmu_flush_pending, 0)) {
        mutex_unlock(&g_vmu_flush_lock);
        return;
    }

    atomic_store(&g_vmu_flush_defer_until_frame, -1);

    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
    #endif
    int persisted = 0;
    if (g_vmu_ready && g_vmu_available) {
        persisted = persist_vmu_archive_locked();
    }
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
    #endif

    if (!persisted && g_vmu_ready && g_vmu_available) {
        atomic_store(&g_vmu_flush_pending, 1);
        atomic_store(&g_vmu_flush_not_before_ms, now_ms + 2000);
    } else {
        atomic_store(&g_vmu_flush_not_before_ms, 0);
    }

    atomic_store(&g_vmu_transition_flush_until_ms, 0);
    mutex_unlock(&g_vmu_flush_lock);
}

static void update_vmu_lcd(void) {
    /*
     * TODO: Add an optional VMU HUD for score/lives/credits. Keep this separate
     * from scoreBezelGetState() so Lua continues drawing the normal on-screen
     * overlay; poll stable Lua globals such as iScore/iLives/iCredits at a low
     * rate and redraw only when values change.
     */
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
    g_io_input.logged_first_line = 0;
    g_io_input.ram_path[0] = '\0';

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

static int write_shadow_entry_to_ram_file(const char *key, const SingeLuaFileCache *entry,
                                          char *out_path, size_t out_path_sz) {
    if (!key || !entry || !entry->data || !out_path || out_path_sz == 0) {
        return 0;
    }

    snprintf(out_path, out_path_sz, "/ram/dcsvmu%08lX.tmp", (unsigned long)fnv1a32(key));

    file_t fd = fs_open(out_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        printf("[Custom io.input] Failed to open VMU shadow RAM file: %s\n", out_path);
        out_path[0] = '\0';
        return 0;
    }

    size_t written = 0;
    while (written < entry->len) {
        ssize_t wr = fs_write(fd, entry->data + written, entry->len - written);
        if (wr <= 0) {
            break;
        }
        written += (size_t)wr;
    }

    fs_close(fd);

    if (written != entry->len) {
        printf("[Custom io.input] Short write for VMU shadow RAM file %s (%zu/%zu)\n",
               out_path, written, entry->len);
        fs_unlink(out_path);
        out_path[0] = '\0';
        return 0;
    }

    return 1;
}

static int commit_output_buffer_locked(void) {
    if (!g_io_output.active || !g_io_output.path) {
        return 1;
    }

    SingeLuaFileCache pending = {
        .path = g_io_output.path,
        .data = g_io_output.data ? g_io_output.data : "",
        .len = g_io_output.len,
        .dirty = 1,
        .next = NULL,
    };
    if (!io_cache_entry_valid_for_path(g_io_output.path, &pending)) {
        size_t preview_len = g_io_output.len;
        if (preview_len > 96) {
            preview_len = 96;
        }
        printf("[Custom io.output] Rejecting invalid shadow write for %s first='%.*s'\n",
               g_io_output.path,
               (int)preview_len,
               g_io_output.data ? g_io_output.data : "");
        remove_io_cache_entry(g_io_output.path);
        goto clear_output;
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

    size_t preview_len = g_io_output.len;
    if (preview_len > 96) {
        preview_len = 96;
    }
    printf("[Custom io.output] Committed shadow write for %s (%zu bytes) first='%.*s'\n",
           g_io_output.path,
           g_io_output.len,
           (int)preview_len,
           g_io_output.data ? g_io_output.data : "");

clear_output:
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

    if (!g_io_input.logged_first_line) {
        size_t preview_len = pos - start;
        if (preview_len > 96) {
            preview_len = 96;
        }
        printf("[Custom io.read] first line from %s: '%.*s'\n",
               g_io_input.entry->path ? g_io_input.entry->path : "(unknown)",
               (int)preview_len,
               data + start);
        g_io_input.logged_first_line = 1;
    }

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

    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
    #endif

    SingeLuaFileCache *entry = find_io_cache_entry(key);
    if (entry && !io_cache_entry_valid_for_path(key, entry)) {
        printf("[Custom io.input] Ignoring invalid VMU shadow for: %s\n", filename);
        remove_io_cache_entry(key);
        entry = NULL;
    }

    if (entry) {
        char ram_path[sizeof(g_io_input.ram_path)];
        if (!write_shadow_entry_to_ram_file(key, entry, ram_path, sizeof(ram_path))) {
            free(fullpath);
            #if SINGE_USE_IO_MUTEX
                SINGE_IO_UNLOCK();
            #endif
            return luaL_error(L, "failed to stage VMU shadow read for %s", filename);
        }

        g_io_input.entry = NULL;
        g_io_input.pos = 0;
        g_io_input.active = 0;
        g_io_input.logged_first_line = 0;
        strncpy(g_io_input.ram_path, ram_path, sizeof(g_io_input.ram_path));
        g_io_input.ram_path[sizeof(g_io_input.ram_path) - 1] = '\0';
        printf("[Custom io.input] VMU shadow staged for: %s -> %s\n", filename, g_io_input.ram_path);
        free(fullpath);
        #if SINGE_USE_IO_MUTEX
            SINGE_IO_UNLOCK();
        #endif
        lua_settop(L, 0);
        lua_pushstring(L, g_io_input.ram_path);
        return call_original_io_n(L, g_orig_io_input_ref, 1);
    }

    g_io_input.entry = NULL;
    g_io_input.pos = 0;
    g_io_input.active = 0;
    g_io_input.logged_first_line = 0;
    g_io_input.ram_path[0] = '\0';

    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
    #endif

    printf("[Custom io.input] Using source file for: %s -> %s\n", filename, fullpath);
    lua_settop(L, 0);
    lua_pushstring(L, fullpath);
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

    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
    #endif

    if (g_io_output.active) {
        if (!commit_output_buffer_locked()) {
            #if SINGE_USE_IO_MUTEX
                SINGE_IO_UNLOCK();
            #endif
            free(fullpath);
            return luaL_error(L, "failed to switch output to %s", filename);
        }
    }

    g_io_output.path = strdup(key);
    free(fullpath);
    if (!g_io_output.path) {
        #if SINGE_USE_IO_MUTEX
            SINGE_IO_UNLOCK();
        #endif
        return luaL_error(L, "out of memory while opening %s", filename);
    }
    g_io_output.active = 1;
    g_io_output.len = 0;
    g_io_output.cap = 0;
    g_io_output.data = NULL;

    printf("[Custom io.output] Shadow write enabled for: %s\n", filename);
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
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
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
    #endif

    for (int i = 1; i <= nargs; i++) {
        size_t len = 0;
        const char *chunk = luaL_checklstring(L, i, &len);
        if (!ensure_output_capacity(len)) {
            #if SINGE_USE_IO_MUTEX
                SINGE_IO_UNLOCK();
            #endif
            return luaL_error(L, "out of memory while buffering io.write");
        }
        memcpy(g_io_output.data + g_io_output.len, chunk, len);
        g_io_output.len += len;
        g_io_output.data[g_io_output.len] = '\0';
    }

    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
    #endif

    lua_pushboolean(L, 1);
    return 1;
}

static int custom_io_close(lua_State *L) {
    void *handle = lua_isnoneornil(L, 1) ? NULL : lua_touserdata(L, 1);

    if (handle == &g_io_output_token || (!handle && g_io_output.active)) {
        int flush_during_transition = 0;
        #if SINGE_USE_IO_MUTEX
            SINGE_IO_LOCK();
        #endif
        int ok = commit_output_buffer_locked();
        if (ok) {
            atomic_store(&g_vmu_flush_pending, 1);
            atomic_store(&g_vmu_flush_not_before_ms, (uint64_t)dcfmv_ps_ms() + 1500);
            if (vmu_transition_flush_window_active()) {
                atomic_store(&g_vmu_flush_not_before_ms, 0);
                flush_during_transition = 1;
            }
        }
        #if SINGE_USE_IO_MUTEX
            SINGE_IO_UNLOCK();
        #endif
        if (!ok) {
            return luaL_error(L, "failed to close shadow output");
        }
        if (flush_during_transition) {
            SINGE_LOG(SINGE_LOG_VMU, "[VMU] Flushing save immediately from io.close during transition");
            flush_vmu_archive_if_pending();
        }
        lua_pushboolean(L, 1);
        return 1;
    }

    if (handle == &g_io_input_token || (!handle && g_io_input.active)) {
        g_io_input.entry = NULL;
        g_io_input.pos = 0;
        g_io_input.active = 0;
        g_io_input.logged_first_line = 0;
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

#if LUA_VERSION_NUM < 502
#define lua_rawlen      lua_objlen
#define LUA_OK          0

/* Lua 5.1 shim for luaL_requiref. It must populate package.loaded so require() works. */
static void luaL_requiref(lua_State *L, const char *modname, lua_CFunction openf, int glb) {
    lua_pushcfunction(L, openf);
    lua_pushstring(L, modname);
    lua_call(L, 1, 1);

    lua_getglobal(L, "package");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "loaded");
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, -3);
            lua_setfield(L, -2, modname);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    if (glb) {
        lua_pushvalue(L, -1);
        lua_setglobal(L, modname);
    }
}
#endif

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

    luaL_requiref(GLua, "lfs", luaopen_lfs, 1);
    lua_pop(GLua, 1);

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
    lua_register(GLua, "vldpGetYUVPixel",     sep_mpeg_get_rawpixel);
    // lua_register(GLua, "vldpResetFocus",      sep_mpeg_reset_focus);
    // lua_register(GLua, "vldpSetMonochrome",   sep_mpeg_set_grayscale);
    lua_register(GLua, "vldpGetWidth",     sep_vldp_get_width); 
    lua_register(GLua, "vldpGetHeight",    sep_vldp_get_height); 
    lua_register(GLua, "vldpGetPixel",        sep_vldp_get_pixel);
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
    lua_register(GLua, "spriteFrameHeight",   sep_sprite_frame_height);
    lua_register(GLua, "spriteFrameWidth",    sep_sprite_frame_width);
    lua_register(GLua, "spriteGetFrames",     sep_sprite_frames);
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



    /* -----------------------------------------------------------------------
     * Singe 'random' object  (random.new(seed):value(min, max))
     * This is a genuine Singe API feature - not a Lua version shim.
     * Scripts use it to create independent, seeded RNG instances.
     * --------------------------------------------------------------------- */
    {
        const char *random_lib =
            "random = {}\n"
            "random.__index = random\n"
            "function random.new(seed)\n"
            "    local r = setmetatable({}, random)\n"
            "    r._seed = math.floor(seed or os.clock() * 100000)\n"
            "    math.randomseed(r._seed)\n"
            "    return r\n"
            "end\n"
            "function random:value(lo, hi)\n"
            "    return math.random(math.floor(lo), math.floor(hi))\n"
            "end\n";
        if (luaL_dostring(GLua, random_lib) != 0) {
            printf("Error injecting random lib: %s\n", lua_tostring(GLua, -1));
            lua_pop(GLua, 1);
        } else {
            printf("    [OK] Singe 'random' library registered\n");
        }
    }
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
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
#endif
    file_t fd = fs_open(script_path, O_RDONLY);
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
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

    int rc = lua_load(GLua, lua_reader, &ud, G_CHUNK_NAME);
            #if SINGE_USE_IO_MUTEX
        SINGE_IO_LOCK();
#endif
    fs_close(fd);
    #if SINGE_USE_IO_MUTEX
        SINGE_IO_UNLOCK();
#endif
    if (rc != 0) {
        printf("Error loading script: %s\n", lua_tostring(GLua, -1));
        exit(1);
    }
    printf("    ✓ Lua script loaded\n");

#if DCSINGE_ENABLE_LUA53_COMPAT_PATCHES
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
#else
    printf("[8.6] Lua 5.3 compatibility patches disabled\n");
#endif
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

static int g_disc_map_a = SWITCH_BUTTON1;
static int g_disc_map_b = SWITCH_COIN1;
static int g_disc_map_x = SWITCH_BUTTON3;
static int g_disc_map_y = SWITCH_BUTTON2;
static int g_disc_map_ltrig = SWITCH_BUTTON3;
static int g_disc_map_rtrig = SWITCH_BUTTON1;
static int g_disc_map_start = SWITCH_START1;

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

#define DCSINGE_SYSTEM_CFG_KEY "__dcsinge/system.cfg"

typedef enum {
    MENU_AIM_ASSIST = 0,
    MENU_AIM_WHEN_FIRING,
    MENU_AIM_STRENGTH,
    MENU_AIM_RADIUS,
    MENU_AIM_MAX_STEP,
    MENU_AIM_HITBOX_TIMEOUT,
    MENU_AIM_RED_ONLY,
    MENU_MOUSE_MODE,
    MENU_CROSSHAIR_X,
    MENU_CROSSHAIR_Y,
    MENU_JOYMOUSE_SPEED,
    MENU_JOYMOUSE_DEADZONE,
    MENU_JOYMOUSE_RESPONSE,
    MENU_JOYMOUSE_SMOOTH,
    MENU_HITBOX_DRAW,
    MENU_MAP_A,
    MENU_MAP_B,
    MENU_MAP_X,
    MENU_MAP_Y,
    MENU_MAP_L,
    MENU_MAP_R,
    MENU_MAP_START,
    MENU_SAVE,
    MENU_RESET,
    MENU_CLOSE,
    MENU_COUNT
} SystemMenuItem;

static int g_system_menu_active = 0;
static int g_system_menu_selected = 0;
static int g_system_menu_page = 0;
static uint64_t g_system_menu_combo_started_ms = 0;
static int g_system_menu_combo_latched = 0;
static dcsinge_ldp_state_t g_system_menu_saved_ldp_state = DCSINGE_LDP_PAUSED;
static int g_system_menu_saved_paused = 1;
static int g_system_menu_saved_muted = 1;
static int g_system_menu_saved_preload_paused = 1;
static char g_system_menu_status[64] = "";

static const int g_menu_switch_values[] = {
    SWITCH_BUTTON1, SWITCH_BUTTON2, SWITCH_BUTTON3,
    SWITCH_COIN1, SWITCH_START1, SWITCH_SERVICE, SWITCH_TEST, SWITCH_PAUSE
};

#define SYSTEM_MENU_MAX_ITEMS 10

static const SystemMenuItem g_system_menu_pages[][SYSTEM_MENU_MAX_ITEMS] = {
    {
        MENU_AIM_ASSIST,
        MENU_AIM_WHEN_FIRING,
        MENU_AIM_STRENGTH,
        MENU_AIM_RADIUS,
        MENU_AIM_MAX_STEP,
        MENU_AIM_HITBOX_TIMEOUT,
        MENU_AIM_RED_ONLY,
        MENU_HITBOX_DRAW,
        MENU_SAVE,
        MENU_CLOSE
    },
    {
        MENU_MOUSE_MODE,
        MENU_CROSSHAIR_X,
        MENU_CROSSHAIR_Y,
        MENU_JOYMOUSE_SPEED,
        MENU_JOYMOUSE_DEADZONE,
        MENU_JOYMOUSE_RESPONSE,
        MENU_JOYMOUSE_SMOOTH,
        MENU_SAVE
    },
    {
        MENU_MAP_A,
        MENU_MAP_B,
        MENU_MAP_X,
        MENU_MAP_Y,
        MENU_MAP_L,
        MENU_MAP_R,
        MENU_MAP_START,
        MENU_SAVE
    },
    {
        MENU_SAVE,
        MENU_RESET,
        MENU_CLOSE
    }
};

static const int g_system_menu_page_counts[] = { 10, 8, 8, 3 };
static const char *g_system_menu_page_names[] = { "Aim", "Mouse", "Buttons", "System" };
#define SYSTEM_MENU_PAGE_COUNT ((int)(sizeof(g_system_menu_page_counts) / sizeof(g_system_menu_page_counts[0])))

static const char *singe_switch_name(int sw) {
    switch (sw) {
        case SWITCH_UP: return "UP";
        case SWITCH_LEFT: return "LEFT";
        case SWITCH_DOWN: return "DOWN";
        case SWITCH_RIGHT: return "RIGHT";
        case SWITCH_START1: return "START1";
        case SWITCH_START2: return "START2";
        case SWITCH_BUTTON1: return "BUTTON1";
        case SWITCH_BUTTON2: return "BUTTON2";
        case SWITCH_BUTTON3: return "BUTTON3";
        case SWITCH_COIN1: return "COIN1";
        case SWITCH_COIN2: return "COIN2";
        case SWITCH_SERVICE: return "SERVICE";
        case SWITCH_TEST: return "TEST";
        case SWITCH_PAUSE: return "PAUSE";
        default: return "BUTTON1";
    }
}

static const char *mouse_mode_name(int mode) {
    switch (mode) {
        case 1: return "Direct";
        case 2: return "Inverse";
        default: return "Offset";
    }
}

static int system_menu_current_page_count(void) {
    if (g_system_menu_page < 0 || g_system_menu_page >= SYSTEM_MENU_PAGE_COUNT) {
        g_system_menu_page = 0;
    }
    return g_system_menu_page_counts[g_system_menu_page];
}

static SystemMenuItem system_menu_current_item(void) {
    int count = system_menu_current_page_count();
    if (g_system_menu_selected < 0) g_system_menu_selected = 0;
    if (g_system_menu_selected >= count) g_system_menu_selected = count - 1;
    return g_system_menu_pages[g_system_menu_page][g_system_menu_selected];
}

static void system_menu_change_page(int dir) {
    g_system_menu_page = (g_system_menu_page + dir + SYSTEM_MENU_PAGE_COUNT) % SYSTEM_MENU_PAGE_COUNT;
    if (g_system_menu_selected >= system_menu_current_page_count()) {
        g_system_menu_selected = system_menu_current_page_count() - 1;
    }
}

static void system_cfg_clamp_user_values(void) {
    if (g_cfg_mouse_send_mode < 0) g_cfg_mouse_send_mode = 0;
    if (g_cfg_mouse_send_mode > 2) g_cfg_mouse_send_mode = 2;
    if (g_cfg_aim_assist_strength < 0.0f) g_cfg_aim_assist_strength = 0.0f;
    if (g_cfg_aim_assist_strength > 1.0f) g_cfg_aim_assist_strength = 1.0f;
    if (g_cfg_aim_assist_max_step < 0.0f) g_cfg_aim_assist_max_step = 0.0f;
    if (g_cfg_aim_assist_max_step > 120.0f) g_cfg_aim_assist_max_step = 120.0f;
    if (g_cfg_aim_assist_radius < 0.0f) g_cfg_aim_assist_radius = 0.0f;
    if (g_cfg_aim_assist_radius > 400.0f) g_cfg_aim_assist_radius = 400.0f;
    if (g_cfg_aim_assist_hitbox_timeout_ms < 0) g_cfg_aim_assist_hitbox_timeout_ms = 0;
    if (g_cfg_aim_assist_hitbox_timeout_ms > 2000) g_cfg_aim_assist_hitbox_timeout_ms = 2000;
    if (g_cfg_joymouse_deadzone < 0.0f) g_cfg_joymouse_deadzone = 0.0f;
    if (g_cfg_joymouse_deadzone > 127.0f) g_cfg_joymouse_deadzone = 127.0f;
    if (g_cfg_joymouse_response < 0.1f) g_cfg_joymouse_response = 0.1f;
    if (g_cfg_joymouse_response > 4.0f) g_cfg_joymouse_response = 4.0f;
    if (g_cfg_joymouse_smooth < 0.0f) g_cfg_joymouse_smooth = 0.0f;
    if (g_cfg_joymouse_smooth > 1.0f) g_cfg_joymouse_smooth = 1.0f;
    if (g_cfg_joymouse_speed < 0.0f) g_cfg_joymouse_speed = 0.0f;
    if (g_cfg_joymouse_speed > 60.0f) g_cfg_joymouse_speed = 60.0f;
}

static void capture_disc_system_defaults(void) {
    g_disc_cfg_crosshair_offset_x = g_cfg_crosshair_offset_x;
    g_disc_cfg_crosshair_offset_y = g_cfg_crosshair_offset_y;
    g_disc_cfg_hitbox_draw = g_cfg_hitbox_draw;
    g_disc_cfg_mouse_send_mode = g_cfg_mouse_send_mode;
    g_disc_cfg_joymouse_deadzone = g_cfg_joymouse_deadzone;
    g_disc_cfg_joymouse_response = g_cfg_joymouse_response;
    g_disc_cfg_joymouse_smooth = g_cfg_joymouse_smooth;
    g_disc_cfg_joymouse_speed = g_cfg_joymouse_speed;
    g_disc_cfg_aim_assist = g_cfg_aim_assist;
    g_disc_cfg_aim_assist_when_firing = g_cfg_aim_assist_when_firing;
    g_disc_cfg_aim_assist_strength = g_cfg_aim_assist_strength;
    g_disc_cfg_aim_assist_max_step = g_cfg_aim_assist_max_step;
    g_disc_cfg_aim_assist_radius = g_cfg_aim_assist_radius;
    g_disc_cfg_aim_assist_hitbox_timeout_ms = g_cfg_aim_assist_hitbox_timeout_ms;
    g_disc_cfg_aim_assist_red_only = g_cfg_aim_assist_red_only;
    g_disc_map_a = MAP_A;
    g_disc_map_b = MAP_B;
    g_disc_map_x = MAP_X;
    g_disc_map_y = MAP_Y;
    g_disc_map_ltrig = MAP_LTRIG;
    g_disc_map_rtrig = MAP_RTRIG;
    g_disc_map_start = MAP_START;
}

static void reset_system_settings_to_disc_defaults(void) {
    g_cfg_crosshair_offset_x = g_disc_cfg_crosshair_offset_x;
    g_cfg_crosshair_offset_y = g_disc_cfg_crosshair_offset_y;
    g_cfg_hitbox_draw = g_disc_cfg_hitbox_draw;
    g_cfg_mouse_send_mode = g_disc_cfg_mouse_send_mode;
    g_cfg_joymouse_deadzone = g_disc_cfg_joymouse_deadzone;
    g_cfg_joymouse_response = g_disc_cfg_joymouse_response;
    g_cfg_joymouse_smooth = g_disc_cfg_joymouse_smooth;
    g_cfg_joymouse_speed = g_disc_cfg_joymouse_speed;
    g_cfg_aim_assist = g_disc_cfg_aim_assist;
    g_cfg_aim_assist_when_firing = g_disc_cfg_aim_assist_when_firing;
    g_cfg_aim_assist_strength = g_disc_cfg_aim_assist_strength;
    g_cfg_aim_assist_max_step = g_disc_cfg_aim_assist_max_step;
    g_cfg_aim_assist_radius = g_disc_cfg_aim_assist_radius;
    g_cfg_aim_assist_hitbox_timeout_ms = g_disc_cfg_aim_assist_hitbox_timeout_ms;
    g_cfg_aim_assist_red_only = g_disc_cfg_aim_assist_red_only;
    MAP_A = g_disc_map_a;
    MAP_B = g_disc_map_b;
    MAP_X = g_disc_map_x;
    MAP_Y = g_disc_map_y;
    MAP_LTRIG = g_disc_map_ltrig;
    MAP_RTRIG = g_disc_map_rtrig;
    MAP_START = g_disc_map_start;
    system_cfg_clamp_user_values();
}

static int apply_system_cfg_kv(const char *key, const char *value) {
    if (!key || !value) return 0;
    if (strcmp(key, "btn_a") == 0) MAP_A = parse_button(value);
    else if (strcmp(key, "btn_b") == 0) MAP_B = parse_button(value);
    else if (strcmp(key, "btn_x") == 0) MAP_X = parse_button(value);
    else if (strcmp(key, "btn_y") == 0) MAP_Y = parse_button(value);
    else if (strcmp(key, "btn_ltrigger") == 0) MAP_LTRIG = parse_button(value);
    else if (strcmp(key, "btn_rtrigger") == 0) MAP_RTRIG = parse_button(value);
    else if (strcmp(key, "btn_start") == 0) MAP_START = parse_button(value);
    else if (strcmp(key, "crosshair_offset") == 0) g_cfg_crosshair_offset_x = atoi(value);
    else if (strcmp(key, "crosshair_offset_x") == 0) g_cfg_crosshair_offset_x = atoi(value);
    else if (strcmp(key, "crosshair_offset_y") == 0) g_cfg_crosshair_offset_y = atoi(value);
    else if (strcmp(key, "hitbox_draw") == 0) g_cfg_hitbox_draw = atoi(value) != 0;
    else if (strcmp(key, "mouse_send_mode") == 0) g_cfg_mouse_send_mode = atoi(value);
    else if (strcmp(key, "aim_assist") == 0) g_cfg_aim_assist = atoi(value) != 0;
    else if (strcmp(key, "aim_assist_when_firing") == 0) g_cfg_aim_assist_when_firing = atoi(value) != 0;
    else if (strcmp(key, "aim_assist_strength") == 0) g_cfg_aim_assist_strength = (float)atof(value);
    else if (strcmp(key, "aim_assist_max_step") == 0) g_cfg_aim_assist_max_step = (float)atof(value);
    else if (strcmp(key, "aim_assist_radius") == 0) g_cfg_aim_assist_radius = (float)atof(value);
    else if (strcmp(key, "aim_assist_hitbox_timeout_ms") == 0) g_cfg_aim_assist_hitbox_timeout_ms = atoi(value);
    else if (strcmp(key, "aim_assist_red_only") == 0) g_cfg_aim_assist_red_only = atoi(value) != 0;
    else if (strcmp(key, "joymouse_deadzone") == 0) g_cfg_joymouse_deadzone = (float)atof(value);
    else if (strcmp(key, "joymouse_response") == 0) g_cfg_joymouse_response = (float)atof(value);
    else if (strcmp(key, "joymouse_smooth") == 0) g_cfg_joymouse_smooth = (float)atof(value);
    else if (strcmp(key, "joymouse_speed") == 0) g_cfg_joymouse_speed = (float)atof(value);
    else return 0;
    return 1;
}

static void load_system_cfg_override(void) {
    SingeLuaFileCache *entry = find_io_cache_entry(DCSINGE_SYSTEM_CFG_KEY);
    if (!entry || !entry->data) {
        printf("[SystemMenu] No VMU system override\n");
        return;
    }

    char line[160];
    size_t pos = 0;
    for (size_t i = 0; i <= entry->len; i++) {
        char c = (i < entry->len) ? entry->data[i] : '\n';
        if (c == '\r') continue;
        if (c == '\n' || pos >= sizeof(line) - 1) {
            line[pos] = '\0';
            pos = 0;
            if (line[0] == '#' || line[0] == '\0') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq++ = '\0';
            while (*eq == ' ' || *eq == '\t') eq++;
            size_t key_len = strlen(line);
            while (key_len > 0 && (line[key_len - 1] == ' ' || line[key_len - 1] == '\t')) {
                line[--key_len] = '\0';
            }
            apply_system_cfg_kv(line, eq);
        } else {
            line[pos++] = c;
        }
    }
    system_cfg_clamp_user_values();
    printf("[SystemMenu] Applied VMU system override\n");
}

static int save_system_cfg_override(void) {
    char cfg[1024];
    int n = snprintf(cfg, sizeof(cfg),
        "# DCSinge VMU system settings override\n"
        "aim_assist=%d\n"
        "aim_assist_when_firing=%d\n"
        "aim_assist_strength=%.2f\n"
        "aim_assist_max_step=%.2f\n"
        "aim_assist_radius=%.2f\n"
        "aim_assist_hitbox_timeout_ms=%d\n"
        "aim_assist_red_only=%d\n"
        "hitbox_draw=%d\n"
        "mouse_send_mode=%d\n"
        "crosshair_offset_x=%d\n"
        "crosshair_offset_y=%d\n"
        "joymouse_deadzone=%.2f\n"
        "joymouse_response=%.2f\n"
        "joymouse_smooth=%.2f\n"
        "joymouse_speed=%.2f\n"
        "btn_a=%s\n"
        "btn_b=%s\n"
        "btn_x=%s\n"
        "btn_y=%s\n"
        "btn_ltrigger=%s\n"
        "btn_rtrigger=%s\n"
        "btn_start=%s\n",
        g_cfg_aim_assist,
        g_cfg_aim_assist_when_firing,
        g_cfg_aim_assist_strength,
        g_cfg_aim_assist_max_step,
        g_cfg_aim_assist_radius,
        g_cfg_aim_assist_hitbox_timeout_ms,
        g_cfg_aim_assist_red_only,
        g_cfg_hitbox_draw,
        g_cfg_mouse_send_mode,
        g_cfg_crosshair_offset_x,
        g_cfg_crosshair_offset_y,
        g_cfg_joymouse_deadzone,
        g_cfg_joymouse_response,
        g_cfg_joymouse_smooth,
        g_cfg_joymouse_speed,
        singe_switch_name(MAP_A),
        singe_switch_name(MAP_B),
        singe_switch_name(MAP_X),
        singe_switch_name(MAP_Y),
        singe_switch_name(MAP_LTRIG),
        singe_switch_name(MAP_RTRIG),
        singe_switch_name(MAP_START));

    if (n <= 0 || n >= (int)sizeof(cfg)) {
        snprintf(g_system_menu_status, sizeof(g_system_menu_status), "Save failed: cfg too large");
        return 0;
    }

    if (!upsert_io_cache_entry(DCSINGE_SYSTEM_CFG_KEY, cfg, (size_t)n, 1)) {
        snprintf(g_system_menu_status, sizeof(g_system_menu_status), "Save failed: memory");
        return 0;
    }

    atomic_store(&g_vmu_flush_pending, 1);
    atomic_store(&g_vmu_flush_not_before_ms, 0);
    flush_vmu_archive_if_pending();
    snprintf(g_system_menu_status, sizeof(g_system_menu_status),
             g_vmu_available ? "Saved to VMU" : "No VMU: live only");
    return g_vmu_available;
}

static void system_menu_open(void) {
    if (g_system_menu_active) return;
    g_system_menu_active = 1;
    g_system_menu_saved_ldp_state = dcsinge_ldp_get_state();
    if (dcfmv_current) {
        g_system_menu_saved_paused = dcfmv_is_paused(dcfmv_current);
        g_system_menu_saved_muted = dcfmv_audio_muted(dcfmv_current);
        g_system_menu_saved_preload_paused = atomic_load(&dcfmv_current->preload_paused);
        dcfmv_set_audio_muted(dcfmv_current, 1);
        dcfmv_set_paused(dcfmv_current, 1);
        dcfmv_set_preload_paused(dcfmv_current, 1);
    }
    g_system_menu_status[0] = '\0';
    printf("[SystemMenu] opened\n");
}

static void system_menu_close(void) {
    if (!g_system_menu_active) return;
    g_system_menu_active = 0;
    if (dcfmv_current) {
        dcfmv_set_paused(dcfmv_current, g_system_menu_saved_paused);
        dcfmv_set_audio_muted(dcfmv_current, g_system_menu_saved_muted);
        dcfmv_set_preload_paused(dcfmv_current, g_system_menu_saved_preload_paused);
    }
    dcsinge_ldp_set_state(g_system_menu_saved_ldp_state, "system menu close");
    printf("[SystemMenu] closed\n");
}

static void cycle_menu_switch(int *mapping, int dir) {
    int count = (int)(sizeof(g_menu_switch_values) / sizeof(g_menu_switch_values[0]));
    int idx = 0;
    for (int i = 0; i < count; i++) {
        if (g_menu_switch_values[i] == *mapping) {
            idx = i;
            break;
        }
    }
    idx = (idx + dir + count) % count;
    *mapping = g_menu_switch_values[idx];
}

static void system_menu_adjust_selected(int dir) {
    switch (system_menu_current_item()) {
        case MENU_AIM_ASSIST: g_cfg_aim_assist = !g_cfg_aim_assist; break;
        case MENU_AIM_WHEN_FIRING: g_cfg_aim_assist_when_firing = !g_cfg_aim_assist_when_firing; break;
        case MENU_AIM_STRENGTH: g_cfg_aim_assist_strength += 0.05f * dir; break;
        case MENU_AIM_RADIUS: g_cfg_aim_assist_radius += 8.0f * dir; break;
        case MENU_AIM_MAX_STEP: g_cfg_aim_assist_max_step += 2.0f * dir; break;
        case MENU_AIM_HITBOX_TIMEOUT: g_cfg_aim_assist_hitbox_timeout_ms += 25 * dir; break;
        case MENU_AIM_RED_ONLY: g_cfg_aim_assist_red_only = !g_cfg_aim_assist_red_only; break;
        case MENU_MOUSE_MODE: g_cfg_mouse_send_mode = (g_cfg_mouse_send_mode + dir + 3) % 3; break;
        case MENU_CROSSHAIR_X: g_cfg_crosshair_offset_x += dir; break;
        case MENU_CROSSHAIR_Y: g_cfg_crosshair_offset_y += dir; break;
        case MENU_JOYMOUSE_SPEED: g_cfg_joymouse_speed += 1.0f * dir; break;
        case MENU_JOYMOUSE_DEADZONE: g_cfg_joymouse_deadzone += 1.0f * dir; break;
        case MENU_JOYMOUSE_RESPONSE: g_cfg_joymouse_response += 0.1f * dir; break;
        case MENU_JOYMOUSE_SMOOTH: g_cfg_joymouse_smooth += 0.05f * dir; break;
        case MENU_HITBOX_DRAW: g_cfg_hitbox_draw = !g_cfg_hitbox_draw; break;
        case MENU_MAP_A: cycle_menu_switch(&MAP_A, dir); break;
        case MENU_MAP_B: cycle_menu_switch(&MAP_B, dir); break;
        case MENU_MAP_X: cycle_menu_switch(&MAP_X, dir); break;
        case MENU_MAP_Y: cycle_menu_switch(&MAP_Y, dir); break;
        case MENU_MAP_L: cycle_menu_switch(&MAP_LTRIG, dir); break;
        case MENU_MAP_R: cycle_menu_switch(&MAP_RTRIG, dir); break;
        case MENU_MAP_START: cycle_menu_switch(&MAP_START, dir); break;
        case MENU_SAVE: save_system_cfg_override(); break;
        case MENU_RESET:
            reset_system_settings_to_disc_defaults();
            snprintf(g_system_menu_status, sizeof(g_system_menu_status), "Reset to disc defaults");
            break;
        case MENU_CLOSE: system_menu_close(); break;
        default: break;
    }
    system_cfg_clamp_user_values();
}

static int system_menu_handle_input(const cont_state_t *state, uint64_t now_ms) {
    static uint32_t prev_buttons = 0;
    static int prev_l = 0;
    static int prev_r = 0;

    if (!state) return g_system_menu_active;

    uint32_t buttons = state->buttons;
    int l_down = state->ltrig > 32;
    int r_down = state->rtrig > 32;
    int combo_down = (buttons & CONT_START) && l_down && r_down;

    if (combo_down && !g_system_menu_combo_latched) {
        if (!g_system_menu_combo_started_ms) {
            g_system_menu_combo_started_ms = now_ms;
        } else if (now_ms - g_system_menu_combo_started_ms >= 500) {
            if (g_system_menu_active) system_menu_close();
            else system_menu_open();
            g_system_menu_combo_latched = 1;
        }
    } else if (!combo_down) {
        g_system_menu_combo_started_ms = 0;
        g_system_menu_combo_latched = 0;
    }

    if (!g_system_menu_active) {
        prev_buttons = buttons;
        prev_l = l_down;
        prev_r = r_down;
        return 0;
    }

    uint32_t pressed = buttons & ~prev_buttons;
    int l_pressed = l_down && !prev_l;
    int r_pressed = r_down && !prev_r;

    int menu_item_count = system_menu_current_page_count();

    if (l_pressed) {
        system_menu_change_page(-1);
        menu_item_count = system_menu_current_page_count();
    }
    if (r_pressed) {
        system_menu_change_page(1);
        menu_item_count = system_menu_current_page_count();
    }
    if (pressed & CONT_DPAD_UP) {
        if (g_system_menu_selected < 0) {
            g_system_menu_selected = menu_item_count - 1;
        } else {
            g_system_menu_selected--;
            if (g_system_menu_selected < -1) {
                g_system_menu_selected = menu_item_count - 1;
            }
        }
    }
    if (pressed & CONT_DPAD_DOWN) {
        if (g_system_menu_selected < 0) {
            g_system_menu_selected = 0;
        } else {
            g_system_menu_selected++;
            if (g_system_menu_selected >= menu_item_count) {
                g_system_menu_selected = -1;
            }
        }
    }
    if (pressed & CONT_DPAD_LEFT) {
        if (g_system_menu_selected < 0) system_menu_change_page(-1);
        else system_menu_adjust_selected(-1);
    }
    if ((pressed & CONT_DPAD_RIGHT) || (pressed & CONT_A)) {
        if (g_system_menu_selected < 0) system_menu_change_page(1);
        else system_menu_adjust_selected(1);
    }
    if (pressed & CONT_B) {
        system_menu_close();
    }
    if (pressed & CONT_X) {
        save_system_cfg_override();
    }
    if (pressed & CONT_Y) {
        reset_system_settings_to_disc_defaults();
        snprintf(g_system_menu_status, sizeof(g_system_menu_status), "Reset to disc defaults");
    }

    prev_buttons = buttons;
    prev_l = l_down;
    prev_r = r_down;
    return 1;
}

static void system_menu_draw_quad(float x1, float y1, float x2, float y2, uint32_t color) {
    static pvr_poly_hdr_t hdr;
    static int hdr_ok = 0;
    if (!hdr_ok) {
        pvr_poly_cxt_t cxt;
        pvr_poly_cxt_col(&cxt, PVR_LIST_TR_POLY);
        cxt.gen.alpha = PVR_ALPHA_ENABLE;
        cxt.gen.culling = PVR_CULLING_NONE;
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        cxt.blend.src_enable = PVR_BLEND_ENABLE;
        cxt.blend.dst_enable = PVR_BLEND_ENABLE;
        cxt.depth.comparison = PVR_DEPTHCMP_ALWAYS;
        cxt.depth.write = PVR_DEPTHWRITE_DISABLE;
        pvr_poly_compile(&hdr, &cxt);
        hdr_ok = 1;
    }

    pvr_vertex_t verts[4] = {
        { .flags = PVR_CMD_VERTEX,     .x = x1, .y = y1, .z = g_overlay_submit_z, .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX,     .x = x2, .y = y1, .z = g_overlay_submit_z, .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX,     .x = x1, .y = y2, .z = g_overlay_submit_z, .argb = color, .oargb = 0 },
        { .flags = PVR_CMD_VERTEX_EOL, .x = x2, .y = y2, .z = g_overlay_submit_z, .argb = color, .oargb = 0 }
    };
    dc_pvr_emit_tr_poly_batch(&hdr, verts, 4);
}

static void system_menu_draw_text(int x, int y, uint8_t r, uint8_t g, uint8_t b, const char *text) {
    uint8_t old_r = GFontColorR, old_g = GFontColorG, old_b = GFontColorB, old_a = GFontColorA;
    GFontColorR = r;
    GFontColorG = g;
    GFontColorB = b;
    GFontColorA = 255;
    overlay_draw_text(x, y, text);
    GFontColorR = old_r;
    GFontColorG = old_g;
    GFontColorB = old_b;
    GFontColorA = old_a;
}

static void system_menu_item_text(SystemMenuItem item, char *out, size_t out_sz) {
    switch (item) {
        case MENU_AIM_ASSIST: snprintf(out, out_sz, "Aim Assist              %s", g_cfg_aim_assist ? "On" : "Off"); break;
        case MENU_AIM_WHEN_FIRING: snprintf(out, out_sz, "Aim Only While Firing   %s", g_cfg_aim_assist_when_firing ? "On" : "Off"); break;
        case MENU_AIM_STRENGTH: snprintf(out, out_sz, "Aim Strength            %d%%", (int)roundf(g_cfg_aim_assist_strength * 100.0f)); break;
        case MENU_AIM_RADIUS: snprintf(out, out_sz, "Aim Radius              %.0f", g_cfg_aim_assist_radius); break;
        case MENU_AIM_MAX_STEP: snprintf(out, out_sz, "Aim Max Step            %.0f", g_cfg_aim_assist_max_step); break;
        case MENU_AIM_HITBOX_TIMEOUT: snprintf(out, out_sz, "Hitbox Timeout          %dms", g_cfg_aim_assist_hitbox_timeout_ms); break;
        case MENU_AIM_RED_ONLY: snprintf(out, out_sz, "Red Hitboxes Only       %s", g_cfg_aim_assist_red_only ? "On" : "Off"); break;
        case MENU_MOUSE_MODE: snprintf(out, out_sz, "Mouse Send Mode         %s", mouse_mode_name(g_cfg_mouse_send_mode)); break;
        case MENU_CROSSHAIR_X: snprintf(out, out_sz, "Crosshair X Offset      %d", g_cfg_crosshair_offset_x); break;
        case MENU_CROSSHAIR_Y: snprintf(out, out_sz, "Crosshair Y Offset      %d", g_cfg_crosshair_offset_y); break;
        case MENU_JOYMOUSE_SPEED: snprintf(out, out_sz, "Joymouse Speed          %.0f", g_cfg_joymouse_speed); break;
        case MENU_JOYMOUSE_DEADZONE: snprintf(out, out_sz, "Joymouse Deadzone       %.0f", g_cfg_joymouse_deadzone); break;
        case MENU_JOYMOUSE_RESPONSE: snprintf(out, out_sz, "Joymouse Response       %.1f", g_cfg_joymouse_response); break;
        case MENU_JOYMOUSE_SMOOTH: snprintf(out, out_sz, "Joymouse Smooth         %.2f", g_cfg_joymouse_smooth); break;
        case MENU_HITBOX_DRAW: snprintf(out, out_sz, "Debug Hitbox Draw       %s", g_cfg_hitbox_draw ? "On" : "Off"); break;
        case MENU_MAP_A: snprintf(out, out_sz, "Dreamcast A             %s", singe_switch_name(MAP_A)); break;
        case MENU_MAP_B: snprintf(out, out_sz, "Dreamcast B             %s", singe_switch_name(MAP_B)); break;
        case MENU_MAP_X: snprintf(out, out_sz, "Dreamcast X             %s", singe_switch_name(MAP_X)); break;
        case MENU_MAP_Y: snprintf(out, out_sz, "Dreamcast Y             %s", singe_switch_name(MAP_Y)); break;
        case MENU_MAP_L: snprintf(out, out_sz, "Left Trigger            %s", singe_switch_name(MAP_LTRIG)); break;
        case MENU_MAP_R: snprintf(out, out_sz, "Right Trigger           %s", singe_switch_name(MAP_RTRIG)); break;
        case MENU_MAP_START: snprintf(out, out_sz, "Start Button            %s", singe_switch_name(MAP_START)); break;
        case MENU_SAVE: snprintf(out, out_sz, "Save Settings To VMU"); break;
        case MENU_RESET: snprintf(out, out_sz, "Reset To Disc Defaults"); break;
        case MENU_CLOSE: snprintf(out, out_sz, "Close Menu"); break;
        default: out[0] = '\0'; break;
    }
}

static void system_menu_draw(void) {
    if (!g_system_menu_active) return;

    system_menu_draw_quad(56.0f, 34.0f, 584.0f, 446.0f, 0xD0101218);
    system_menu_draw_quad(56.0f, 34.0f, 584.0f, 70.0f, 0xE0202830);
    char title[96];
    snprintf(title, sizeof(title), "<  DCSinge Settings: %s  >",
             g_system_menu_page_names[g_system_menu_page]);
    if (g_system_menu_selected < 0) {
        system_menu_draw_quad(70.0f, 42.0f, 570.0f, 64.0f, 0xC0506070);
        system_menu_draw_text(78, 44, 255, 236, 150, title);
    } else {
        system_menu_draw_text(78, 44, 255, 255, 255, title);
    }

    int menu_item_count = system_menu_current_page_count();

    char line[96];
    for (int row = 0; row < menu_item_count; row++) {
        SystemMenuItem item = g_system_menu_pages[g_system_menu_page][row];
        int y = 96 + row * 30;
        if (row == g_system_menu_selected) {
            system_menu_draw_quad(70.0f, (float)y - 4.0f, 570.0f, (float)y + 19.0f, 0xC0506070);
        }
        system_menu_item_text(item, line, sizeof(line));
        if (row == g_system_menu_selected) {
            char selected[104];
            snprintf(selected, sizeof(selected), "> %s", line);
            system_menu_draw_text(82, y, 255, 236, 150, selected);
        } else {
            system_menu_draw_text(100, y, 220, 226, 232, line);
        }
    }

    system_menu_draw_quad(56.0f, 408.0f, 584.0f, 446.0f, 0xE0202830);
    system_menu_draw_text(78, 416, 180, 205, 230, "Up: Title   Left/Right: Page/Edit   A: Edit   B: Close   X: Save");
    if (g_system_menu_status[0]) {
        system_menu_draw_text(78, 392, 160, 240, 170, g_system_menu_status);
    }
}

static void aim_assist_capture_lua_hitboxes(void) {
    if (!g_cfg_aim_assist || !GLua) {
        return;
    }

    lua_getglobal(GLua, "drawHitboxes");
    if (!lua_isfunction(GLua, -1)) {
        lua_pop(GLua, 1);
        return;
    }

    const int saved_hitbox_draw = g_cfg_hitbox_draw;
    g_cfg_hitbox_draw = 0;
    g_aim_assist_capture_active = 1;

    if (lua_pcall(GLua, 0, 0, 0) != 0) {
        printf("Lua error in drawHitboxes for aim assist: %s\n", lua_tostring(GLua, -1));
        lua_pop(GLua, 1);
    }

    g_aim_assist_capture_active = 0;
    g_cfg_hitbox_draw = saved_hitbox_draw;
}

void singe_tick(uint64_t monotonic_ms) {
    int absolute_frame = framefile_active_absolute_frame();
    if (dcsinge_ldp_get_state() == DCSINGE_LDP_PLAYING) {
        framefile_ensure_segment_for_frame(absolute_frame);
    }

    if (dcsinge_ldp_should_tick()) {
        fmv_tick(monotonic_ms);
        dcsinge_ldp_after_tick();
    }

    pvr_wait_ready();
    pvr_wait_render_done();

    if (dcsinge_ldp_should_present_video()) {
        dcfmv_upload_current_video(dcfmv_current);
    }

    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    if (dcsinge_ldp_should_present_video()) {
        dcfmv_submit_current_video(dcfmv_current);
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    dc_pvr_batch_frame_begin();

    if (!g_system_menu_active) {
        lua_getglobal(GLua, "onOverlayUpdate");
        if (lua_isfunction(GLua, -1)) {
            if (lua_pcall(GLua, 0, 1, 0) != 0) {
                printf("Lua error in onOverlayUpdate: %s\n", lua_tostring(GLua, -1));
                lua_pop(GLua, 1);
            } else {
                lua_pop(GLua, 1);
                g_overlay_ran_once = 1;
                aim_assist_capture_lua_hitboxes();
            }
        } else {
            lua_pop(GLua, 1);
        }
    }

    system_menu_draw();

    int post_overlay_frame = framefile_active_absolute_frame();
    if (!g_system_menu_active) {
        dcsinge_sync_lua_clip_end(post_overlay_frame);
        dcsinge_enter_clip_hold_if_needed(post_overlay_frame);
    }

    dc_pvr_batch_frame_end();
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
    g_logged_first_clip_start = 0;

    // Initialize video output before opening DCMV so startup assets load before
    // the chunk cache consumes heap.
    is_320 = 0;
    g_display_w = 640;
    g_display_h = 480;
    UI_SCALE_X = 1.0f;
    UI_SCALE_Y = 1.0f;
    UI_OFFSET_X = 0;
    UI_OFFSET_Y = 0;

    const pvr_init_params_t pvr_params = {
        { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_0 },
        512 * 1024,
        1,
        0,
        0,
        3,
        0
    };
    pvr_init(&pvr_params);
    pvr_set_vertbuf(PVR_LIST_TR_POLY, g_pvr_tr_vertbuf, sizeof(g_pvr_tr_vertbuf));
    g_pvr_tr_vertbuf_ready = 1;
    printf("[PVR_BATCH] vertex DMA %s, direct overlay batches %s\n",
           pvr_vertex_dma_enabled() ? "enabled" : "disabled",
           DCSINGE_USE_PVR_VERTBUF_BATCH ? "enabled" : "disabled");
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    draw_startup_intro();

    if (dcfmv_open(dcfmv_current, videopath) != 0) {
        printf("PANIC: Failed to open DCMV file\n");
        exit(1);
    }
    log_memory_stats("after_dcmv_open");
    dcfmv_t *fmv = dcfmv_current;
    const dcfmv_media_info_t *info = dcfmv_media_info(fmv);
    dcfmv_set_audio_muted(fmv, 1);
    dcfmv_set_preload_paused(fmv, 1);

    if (g_cfg_disable_fmv_audio) {
        printf("   FMV audio disabled by config; KOS streaming will not start.\n");
        dcfmv_set_audio_enabled(fmv, 0);
    } else if (g_cfg_enable_mp3 && dcfmv_audio_channels(fmv) > 0) {
        printf("   Title enables both FMV audio and MP3; initializing MP3 stream system first.\n");
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

    fallback_vert[0] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=0, .z=1, .argb=0xFF000000};
    fallback_vert[1] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=g_display_w, .y=0, .z=1, .argb=0xFF000000};
    fallback_vert[2] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX, .x=0, .y=g_display_h, .z=1, .argb=0xFF000000};
    fallback_vert[3] = (pvr_vertex_t){.flags=PVR_CMD_VERTEX_EOL, .x=g_display_w, .y=g_display_h, .z=1, .argb=0xFF000000};
    dcfmv_set_render_resources(fmv, pvr_txr, &hdr, &fallback_hdr, vert, fallback_vert);
    dcfmv_reset_render_tracking(fmv);
    log_memory_stats("after_fmv_alloc");

    // GDecoderActive = 1;

    /*
     * Base sound system must be initialized before any SFX loads.
     * If a title enables MP3, let libmp3 claim the KOS stream system first
     * with its larger buffer size so later FMV stream setup can reuse it.
     */
    snd_init();
    log_memory_stats("before_lua_setup");
    // Setup Lua
    setup_lua();
    log_memory_stats("after_lua_setup");
    if (g_cfg_enable_mp3) {
        sep_music_init();
    }

    if (dcfmv_audio_channels(fmv) > 0) {
        if (dcfmv_audio_init(fmv) != 0) {
            printf("PANIC: dcfmv_audio_init failed\n");
            exit(1);
        }
    }

        

    log_memory_stats("after_fmv_audio_init");
    Singe_log("Singe startup complete - %u total frames at %.2f fps",
              info ? info->num_total_frames : 0u,
              info ? info->fps : 0.0f);
    // int retries = 0;
    // while (atomic_load(&frame_index) == 0 && retries < 50) {  // ~1 second wait
    //     thd_sleep(20);
    //     retries++;
    // }

    if (g_cfg_enable_mp3 && !g_mp3_stream_inited && !g_mp3_init_failed) {
        sep_music_init(); // libmp3
    } else if (!g_cfg_enable_mp3) {
        printf("[Music] MP3 disabled by config\n");
        g_current_playing_handle = -1;
    }


    // Initialize audio
    /* Stream slot was already allocated and started by dcfmv_audio_init(). */
    dcfmv_set_audio_muted(fmv, 1);

    worker_thread_id = thd_create(0, worker_thread, NULL);
    vmu_flush_thread_id = thd_create(0, vmu_flush_thread, NULL);


    // ✅ Initialize timing but don't start clocks
    dcfmv_reset_timing(fmv);
    dcfmv_set_audio_muted(fmv, 1);
    printf("   Decoder thread started\n");
    dcfmv_set_paused(fmv, 1);
    dcfmv_set_preload_paused(fmv, 1);
    dcfmv_set_audio_muted(fmv, 1);
    dcsinge_ldp_set_state(DCSINGE_LDP_PAUSED, "startup");
    Singe_log("Initial frame ready, starting playback...");


}

// ---------------------------------------------------------------------------
// Load singe.cfg from /pc or /cd first, then lazily mounted /sd or /ide.
// ---------------------------------------------------------------------------
static void load_config(void) {
    char base_try[sizeof(G_BASE_PATH)] = "";
    static const char *const primary_roots[] = {
        "/pc",
        "/cd"
    };
    static const char *const sd_roots[] = {
        "/sd/DCSinge",
        "/sd/dcsinge"
    };
    static const char *const ide_roots[] = {
        "/ide/DCSinge",
        "/ide/dcsinge"
    };

    file_t fd = dcsinge_try_config_roots(primary_roots,
                                         sizeof(primary_roots) / sizeof(primary_roots[0]),
                                         base_try,
                                         sizeof(base_try));

#if DCSINGE_ENABLE_KOSFAT_STORAGE
    if (fd < 0 && dcsinge_storage_mount_sd() == 0) {
        fd = dcsinge_try_config_roots(sd_roots,
                                      sizeof(sd_roots) / sizeof(sd_roots[0]),
                                      base_try,
                                      sizeof(base_try));
    }

    if (fd < 0 && dcsinge_storage_mount_ide() == 0) {
        fd = dcsinge_try_config_roots(ide_roots,
                                      sizeof(ide_roots) / sizeof(ide_roots[0]),
                                      base_try,
                                      sizeof(base_try));
    }
#endif

    if (fd < 0) {
        printf("⚠️ singe.cfg not found on /pc, /cd, /sd/DCSinge, or /ide/DCSinge. Using defaults.\n");
        return;
    }

    strncpy(G_BASE_PATH, base_try, sizeof(G_BASE_PATH));
    G_BASE_PATH[sizeof(G_BASE_PATH) - 1] = '\0';

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
            else if (strcmp(line, "frame_file") == 0)
                strncpy(G_VIDEO_FILE, eq, sizeof(G_VIDEO_FILE));
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
            else if (strcmp(line, "dcmv_chunk_profile") == 0 || strcmp(line, "chunk_profile") == 0) {
                if (strcmp(eq, "lowmem") == 0 || strcmp(eq, "gdemu_large") == 0 ||
                    strcmp(eq, "gdemu-large") == 0 || strcmp(eq, "bba_large") == 0 ||
                    strcmp(eq, "bba-large") == 0) {
                    g_cfg_chunk_cache_slots = 3;
                    g_cfg_chunk_initial_preload_chunks = 1;
                    g_cfg_chunk_audio_ring_slots = 4;
                } else if (strcmp(eq, "cdr") == 0 || strcmp(eq, "default") == 0) {
                    g_cfg_chunk_cache_slots = DCFMV_CHUNK_CACHE_SLOTS_DEFAULT;
                    g_cfg_chunk_initial_preload_chunks = DCFMV_CHUNK_INITIAL_PRELOAD_DEFAULT;
                    g_cfg_chunk_audio_ring_slots = DCFMV_CHUNK_AUDIO_RING_SLOTS_DEFAULT;
                }
            }
            else if (strcmp(line, "chunk_cache_slots") == 0 || strcmp(line, "dcmv_chunk_cache_slots") == 0)
                g_cfg_chunk_cache_slots = atoi(eq);
            else if (strcmp(line, "initial_preload_chunks") == 0 || strcmp(line, "dcmv_initial_preload_chunks") == 0)
                g_cfg_chunk_initial_preload_chunks = atoi(eq);
            else if (strcmp(line, "audio_ring_slots") == 0 || strcmp(line, "dcmv_audio_ring_slots") == 0)
                g_cfg_chunk_audio_ring_slots = atoi(eq);
            else if (strcmp(line, "crosshair_offset") == 0)
                g_cfg_crosshair_offset_x = atoi(eq);
            else if (strcmp(line, "crosshair_offset_x") == 0)
                g_cfg_crosshair_offset_x = atoi(eq);
            else if (strcmp(line, "crosshair_offset_y") == 0)
                g_cfg_crosshair_offset_y = atoi(eq);
            else if (strcmp(line, "hitbox_draw") == 0)
                g_cfg_hitbox_draw = atoi(eq) != 0;
            else if (strcmp(line, "mouse_send_mode") == 0)
                g_cfg_mouse_send_mode = atoi(eq);
            else if (strcmp(line, "aim_assist") == 0)
                g_cfg_aim_assist = atoi(eq) != 0;
            else if (strcmp(line, "aim_assist_when_firing") == 0)
                g_cfg_aim_assist_when_firing = atoi(eq) != 0;
            else if (strcmp(line, "aim_assist_strength") == 0)
                g_cfg_aim_assist_strength = (float)atof(eq);
            else if (strcmp(line, "aim_assist_max_step") == 0)
                g_cfg_aim_assist_max_step = (float)atof(eq);
            else if (strcmp(line, "aim_assist_radius") == 0)
                g_cfg_aim_assist_radius = (float)atof(eq);
            else if (strcmp(line, "aim_assist_hitbox_timeout_ms") == 0)
                g_cfg_aim_assist_hitbox_timeout_ms = atoi(eq);
            else if (strcmp(line, "aim_assist_red_only") == 0)
                g_cfg_aim_assist_red_only = atoi(eq) != 0;
            else if (strcmp(line, "joymouse_deadzone") == 0)
                g_cfg_joymouse_deadzone = (float)atof(eq);
            else if (strcmp(line, "joymouse_response") == 0)
                g_cfg_joymouse_response = (float)atof(eq);
            else if (strcmp(line, "joymouse_smooth") == 0)
                g_cfg_joymouse_smooth = (float)atof(eq);
            else if (strcmp(line, "joymouse_speed") == 0)
                g_cfg_joymouse_speed = (float)atof(eq);
        } else {
            line[pos++] = c;
        }
    }
    fs_close(fd);

    if (g_cfg_mouse_send_mode < 0) g_cfg_mouse_send_mode = 0;
    if (g_cfg_mouse_send_mode > 2) g_cfg_mouse_send_mode = 2;
    if (g_cfg_aim_assist_strength < 0.0f) g_cfg_aim_assist_strength = 0.0f;
    if (g_cfg_aim_assist_strength > 1.0f) g_cfg_aim_assist_strength = 1.0f;
    if (g_cfg_aim_assist_max_step < 0.0f) g_cfg_aim_assist_max_step = 0.0f;
    if (g_cfg_aim_assist_max_step > 120.0f) g_cfg_aim_assist_max_step = 120.0f;
    if (g_cfg_aim_assist_radius < 0.0f) g_cfg_aim_assist_radius = 0.0f;
    if (g_cfg_aim_assist_radius > 400.0f) g_cfg_aim_assist_radius = 400.0f;
    if (g_cfg_aim_assist_hitbox_timeout_ms < 0) g_cfg_aim_assist_hitbox_timeout_ms = 0;
    if (g_cfg_aim_assist_hitbox_timeout_ms > 2000) g_cfg_aim_assist_hitbox_timeout_ms = 2000;
    if (g_cfg_joymouse_deadzone < 0.0f) g_cfg_joymouse_deadzone = 0.0f;
    if (g_cfg_joymouse_deadzone > 127.0f) g_cfg_joymouse_deadzone = 127.0f;
    if (g_cfg_joymouse_response < 0.1f) g_cfg_joymouse_response = 0.1f;
    if (g_cfg_joymouse_response > 4.0f) g_cfg_joymouse_response = 4.0f;
    if (g_cfg_joymouse_smooth < 0.0f) g_cfg_joymouse_smooth = 0.0f;
    if (g_cfg_joymouse_smooth > 1.0f) g_cfg_joymouse_smooth = 1.0f;
    if (g_cfg_joymouse_speed < 0.0f) g_cfg_joymouse_speed = 0.0f;
    if (g_cfg_joymouse_speed > 60.0f) g_cfg_joymouse_speed = 60.0f;

    dcfmv_chunk_config_t chunk_config = {
        g_cfg_chunk_cache_slots,
        g_cfg_chunk_initial_preload_chunks,
        g_cfg_chunk_audio_ring_slots
    };
    dcfmv_set_chunk_config(&chunk_config);
    chunk_config = dcfmv_get_chunk_config();
    g_cfg_chunk_cache_slots = chunk_config.chunk_cache_slots;
    g_cfg_chunk_initial_preload_chunks = chunk_config.initial_preload_chunks;
    g_cfg_chunk_audio_ring_slots = chunk_config.audio_ring_slots;

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
    printf("  DCMV chunk config: cache_slots=%d initial_preload=%d audio_ring_slots=%d\n",
           g_cfg_chunk_cache_slots,
           g_cfg_chunk_initial_preload_chunks,
           g_cfg_chunk_audio_ring_slots);
    printf("  VMU Icon: %s\n", G_VMU_ICON_FILE);
    printf("  Name:   %s\n", G_GAME_NAME);
    printf("  Crosshair offset: x=%d y=%d\n", g_cfg_crosshair_offset_x, g_cfg_crosshair_offset_y);
    printf("  Hitbox draw: %d\n", g_cfg_hitbox_draw);
    printf("  Mouse send mode: %d (0=offset,1=direct,2=inverse)\n", g_cfg_mouse_send_mode);
    printf("  Aim assist: enabled=%d when_firing=%d strength=%.2f max_step=%.2f radius=%.2f timeout_ms=%d red_only=%d\n",
           g_cfg_aim_assist, g_cfg_aim_assist_when_firing,
           g_cfg_aim_assist_strength, g_cfg_aim_assist_max_step,
           g_cfg_aim_assist_radius, g_cfg_aim_assist_hitbox_timeout_ms,
           g_cfg_aim_assist_red_only);
    printf("  JoyMouse: deadzone=%.2f response=%.2f smooth=%.2f speed=%.2f\n",
           g_cfg_joymouse_deadzone, g_cfg_joymouse_response,
           g_cfg_joymouse_smooth, g_cfg_joymouse_speed);
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

    capture_disc_system_defaults();
}



static void poll_and_handle_input(void) {
    static uint64_t prevbits[2] = {0, 0};    // Previous state for both players
    static float mouse_vx[2] = {0.0f, 0.0f};  // Mouse X velocity per player
    static float mouse_vy[2] = {0.0f, 0.0f};  // Mouse Y velocity per player
    static int GMouseX[2] = {180, 180};
    static int GMouseY[2] = {120, 120};
    const int PLAYER2_OFFSET = 32;    // Offset for Player 2 input

    maple_device_t *menu_dev = maple_enum_dev(0, 0);
    if (menu_dev && menu_dev->valid && (menu_dev->info.functions & MAPLE_FUNC_CONTROLLER)) {
        cont_state_t *menu_state = (cont_state_t *)maple_dev_status(menu_dev);
        if (system_menu_handle_input(menu_state, timer_ms_gettime64())) {
            prevbits[0] = 0;
            prevbits[1] = 0;
            return;
        }
    }

    for (int port = 0; port < 2; port++) {
        maple_device_t *dev = maple_enum_dev(port, 0);
        if (!dev || !dev->valid || !(dev->info.functions & MAPLE_FUNC_CONTROLLER)) {
            prevbits[port] = 0;
            continue;
        }

        cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
        if (!state)
            continue;
        const uint64_t now_ms = timer_ms_gettime64();
        if (g_last_hitbox_valid && g_cfg_aim_assist_hitbox_timeout_ms > 0) {
            if ((now_ms - g_last_hitbox_ms) > (uint64_t)g_cfg_aim_assist_hitbox_timeout_ms) {
                g_last_hitbox_valid = 0;
            }
        }

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
                    SINGE_LOG(SINGE_LOG_INPUT, "DEBUG: Sending event '%s' for Player %d, switch_num %d",
                              event, port + 1, lua_switch_num);
                    if (pressed && lua_switch_num == SWITCH_BUTTON3 && g_shot_trace_budget > 0) {
                        const float ratio_x = (g_ratio_x > 0.0001f) ? g_ratio_x : 1.0f;
                        const float hitbox_cx = g_last_hitbox_valid ? ((g_last_hitbox_x1 + g_last_hitbox_x2) * 0.5f) : -1.0f;
                        const float hitbox_cy = g_last_hitbox_valid ? ((g_last_hitbox_y1 + g_last_hitbox_y2) * 0.5f) : -1.0f;
                        const float lua_from_offset = ((float)roundf((float)GMouseX[port] + g_ratio_x_offset) * ratio_x) - g_ratio_x_offset;
                        const float lua_from_direct = ((float)GMouseX[port] * ratio_x) - g_ratio_x_offset;
                        const float lua_from_inverse = ((float)roundf((((float)GMouseX[port] + g_ratio_x_offset) / ratio_x)) * ratio_x) - g_ratio_x_offset;
                        SINGE_LOG(SINGE_LOG_INPUT,
                                  "[SHOT_TRACE] p=%d overlay=(%d,%d) last_hitbox_center=(%.1f,%.1f) hitbox_color=(%d,%d,%d) luaX{offset=%.2f,direct=%.2f,inverse=%.2f}",
                                  port + 1, GMouseX[port], GMouseY[port],
                                  hitbox_cx, hitbox_cy,
                                  g_last_hitbox_r, g_last_hitbox_g, g_last_hitbox_b,
                                  lua_from_offset, lua_from_direct, lua_from_inverse);
                        g_shot_trace_budget--;
                    }
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

            const float deadzone = g_cfg_joymouse_deadzone;
            float nx = (fabsf(lx) < deadzone) ? 0.0f : lx / 128.0f;
            float ny = (fabsf(ly) < deadzone) ? 0.0f : ly / 128.0f;

            const float response = g_cfg_joymouse_response;
            nx = copysignf(powf(fabsf(nx), response), nx);
            ny = copysignf(powf(fabsf(ny), response), ny);

            const float smooth = g_cfg_joymouse_smooth;
            mouse_vx[port] = mouse_vx[port] * (1.0f - smooth) + nx * smooth;
            mouse_vy[port] = mouse_vy[port] * (1.0f - smooth) + ny * smooth;

            const float speed = g_cfg_joymouse_speed;
            int relX = (int)roundf(mouse_vx[port] * speed);
            int relY = (int)roundf(mouse_vy[port] * speed);

            const int hitbox_recent = g_last_hitbox_valid &&
                (g_cfg_aim_assist_hitbox_timeout_ms <= 0 ||
                 (now_ms - g_last_hitbox_ms) <= (uint64_t)g_cfg_aim_assist_hitbox_timeout_ms);
            const int hitbox_color_ok = (!g_cfg_aim_assist_red_only) || aim_assist_hitbox_is_red();
            if (g_cfg_aim_assist && hitbox_recent && hitbox_color_ok) {
                const uint64_t fire_flag = (port == 0)
                    ? (1ULL << SWITCH_BUTTON3)
                    : (1ULL << (SWITCH_BUTTON3 + PLAYER2_OFFSET));
                if (!g_cfg_aim_assist_when_firing || (curbits & fire_flag)) {
                    const float ratio_x = (g_ratio_x > 0.0001f) ? g_ratio_x : 1.0f;
                    const float hitbox_cx = (g_last_hitbox_x1 + g_last_hitbox_x2) * 0.5f;
                    const float hitbox_cy = (g_last_hitbox_y1 + g_last_hitbox_y2) * 0.5f;
                    const float lua_x_offset = ((float)roundf((float)GMouseX[port] + g_ratio_x_offset) * ratio_x) - g_ratio_x_offset;
                    const float lua_x_direct = ((float)GMouseX[port] * ratio_x) - g_ratio_x_offset;
                    const float lua_x_inverse = ((float)roundf((((float)GMouseX[port] + g_ratio_x_offset) / ratio_x)) * ratio_x) - g_ratio_x_offset;
                    float lua_x_active = lua_x_offset;
                    float gain_x = ratio_x;
                    if (g_cfg_mouse_send_mode == 1) {
                        lua_x_active = lua_x_direct;
                        gain_x = ratio_x;
                    } else if (g_cfg_mouse_send_mode == 2) {
                        lua_x_active = lua_x_inverse;
                        gain_x = 1.0f;
                    }
                    if (gain_x < 0.0001f) gain_x = 1.0f;

                    const float err_x = hitbox_cx - lua_x_active;
                    const float err_y = hitbox_cy - (float)GMouseY[port];
                    if ((g_cfg_aim_assist_radius <= 0.0f) ||
                        (fabsf(err_x) <= g_cfg_aim_assist_radius && fabsf(err_y) <= g_cfg_aim_assist_radius)) {
                        float assist_x = (fabsf(err_x) < 0.75f) ? 0.0f : (err_x * g_cfg_aim_assist_strength / gain_x);
                        float assist_y = (fabsf(err_y) < 0.75f) ? 0.0f : (err_y * g_cfg_aim_assist_strength);

                        /*
                         * Do not let assist fight strong manual movement.
                         * This prevents "auto-lure" toward the wrong target
                         * while the player is actively pushing to a new one.
                         */
                        const float manual_mag = sqrtf(nx * nx + ny * ny);
                        const float oppose_axis_threshold = 0.22f;
                        const float suppress_assist_mag = 0.55f;
                        if (manual_mag >= suppress_assist_mag) {
                            assist_x = 0.0f;
                            assist_y = 0.0f;
                        } else {
                            if (fabsf(nx) >= oppose_axis_threshold && (err_x * nx) < 0.0f) {
                                assist_x = 0.0f;
                            }
                            if (fabsf(ny) >= oppose_axis_threshold && (err_y * ny) < 0.0f) {
                                assist_y = 0.0f;
                            }
                        }

                        if (assist_x > g_cfg_aim_assist_max_step) assist_x = g_cfg_aim_assist_max_step;
                        if (assist_x < -g_cfg_aim_assist_max_step) assist_x = -g_cfg_aim_assist_max_step;
                        if (assist_y > g_cfg_aim_assist_max_step) assist_y = g_cfg_aim_assist_max_step;
                        if (assist_y < -g_cfg_aim_assist_max_step) assist_y = -g_cfg_aim_assist_max_step;

                        relX += (int)roundf(assist_x);
                        relY += (int)roundf(assist_y);
                    }
                }
            }

            if (relX || relY) {
                GMouseX[port] += relX;
                GMouseY[port] += relY;

                /*
                 * Dreamcast analog mouse emulation: keep the virtual cursor
                 * in overlay bounds, then convert to the mouse space Lua
                 * expects before calling onMouseMoved.
                 */
                if (GMouseX[port] < 0) GMouseX[port] = 0;
                else if (GMouseX[port] > GOverlayWidth) GMouseX[port] = GOverlayWidth;
                if (GMouseY[port] < 0) GMouseY[port] = 0;
                else if (GMouseY[port] > GOverlayHeight) GMouseY[port] = GOverlayHeight;

                const float ratio_x = (g_ratio_x > 0.0001f) ? g_ratio_x : 1.0f;
                const int mouse_x_offset = (int)roundf((float)GMouseX[port] + g_ratio_x_offset);
                const int mouse_x_direct = GMouseX[port];
                const int mouse_x_inverse = (int)roundf((((float)GMouseX[port] + g_ratio_x_offset) / ratio_x));
                int mouse_x = mouse_x_offset;
                if (g_cfg_mouse_send_mode == 1) {
                    mouse_x = mouse_x_direct;
                } else if (g_cfg_mouse_send_mode == 2) {
                    mouse_x = mouse_x_inverse;
                }
                const int mouse_y = GMouseY[port];
                const int relMouseX = relX;
                const int relMouseY = relY;

                if (g_mouse_trace_budget > 0) {
                    const float lua_x_from_offset = ((float)mouse_x * ratio_x) - g_ratio_x_offset;
                    const float lua_x_from_direct = ((float)mouse_x_direct * ratio_x) - g_ratio_x_offset;
                    const float lua_x_from_inverse = ((float)mouse_x_inverse * ratio_x) - g_ratio_x_offset;
                    if (g_last_hitbox_valid) {
                        const float hitbox_cx = (g_last_hitbox_x1 + g_last_hitbox_x2) * 0.5f;
                        const float hitbox_cy = (g_last_hitbox_y1 + g_last_hitbox_y2) * 0.5f;
                        SINGE_LOG(SINGE_LOG_INPUT,
                            "[AIM_TRACE] p=%d mode=%d joy=(%d,%d) rel=(%d,%d) overlay=(%d,%d) sendX{offset=%d,direct=%d,inverse=%d,active=%d} luaX{offset=%.2f,direct=%.2f,inverse=%.2f} hitbox_center=(%.1f,%.1f) dx{offset=%.2f,direct=%.2f,inverse=%.2f}",
                            port + 1, g_cfg_mouse_send_mode,
                            lx, ly, relMouseX, relMouseY, GMouseX[port], GMouseY[port],
                            mouse_x_offset, mouse_x_direct, mouse_x_inverse, mouse_x,
                            lua_x_from_offset, lua_x_from_direct, lua_x_from_inverse,
                            hitbox_cx, hitbox_cy,
                            lua_x_from_offset - hitbox_cx,
                            lua_x_from_direct - hitbox_cx,
                            lua_x_from_inverse - hitbox_cx);
                    } else {
                        SINGE_LOG(SINGE_LOG_INPUT,
                            "[AIM_TRACE] p=%d mode=%d joy=(%d,%d) rel=(%d,%d) overlay=(%d,%d) sendX{offset=%d,direct=%d,inverse=%d,active=%d} luaX{offset=%.2f,direct=%.2f,inverse=%.2f} hitbox_center=(n/a)",
                            port + 1, g_cfg_mouse_send_mode, lx, ly, relMouseX, relMouseY, GMouseX[port], GMouseY[port],
                            mouse_x_offset, mouse_x_direct, mouse_x_inverse, mouse_x,
                            lua_x_from_offset, lua_x_from_direct, lua_x_from_inverse);
                    }
                    g_mouse_trace_budget--;
                }

                SINGE_LOG(SINGE_LOG_INPUT,
                    "[MOUSE] send=(%d,%d) overlay=(%d,%d) rel=(%d,%d) size=%dx%d ratio=(%.3f,%.3f) ratio_offset=(%.2f,%.2f)",
                    mouse_x, mouse_y,
                    GMouseX[port], GMouseY[port],
                    relMouseX, relMouseY,
                    GOverlayWidth, GOverlayHeight,
                    g_ratio_x, g_ratio_y,
                    g_ratio_x_offset, g_ratio_y_offset);

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
            }

            
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
    load_system_cfg_override();
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


    char game_dir[256], video_path[512], framefile_path[512];
    snprintf(game_dir, sizeof(game_dir), "%s%s", G_BASE_PATH, G_GAME_DIR);
    snprintf(framefile_path, sizeof(framefile_path), "%s%s", G_BASE_PATH, G_VIDEO_FILE);
    if (framefile_load_manifest(framefile_path, video_path, sizeof(video_path)) != 0) {
        printf("PANIC: Failed to resolve frame_file: %s\n", framefile_path);
        dcsinge_storage_shutdown();
        exit(1);
    }
    SINGE_LOG(SINGE_LOG_FRAMEFILE, "[FrameFile] manifest resolved %s -> %s (segments=%d active=%d)",
              framefile_path,
              video_path,
              g_framefile_segment_count,
              g_framefile_active_segment);
    Singe_log("Resolved frame_file %s -> %s", framefile_path, video_path);

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
        pace_main_loop();
    }

    printf("Singe shutdown requested, cleaning up...\n");
    singe_shutdown();

    return 0;
}
