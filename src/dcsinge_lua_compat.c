// Compatibility hooks for Hypseus Lua/LFS sources on Dreamcast.

#include <string.h>

#include "lua/lua.h"
#include "lua/lauxlib.h"
#include "lua/luretro.h"

unsigned char get_espath(void) {
    return 0;
}

unsigned char get_zipath(void) {
    return 0;
}

void lua_set_espath(unsigned char value) {
    (void)value;
}

void lua_set_zipath(unsigned char value) {
    (void)value;
}

void lua_set_abpath(const char *value) {
    (void)value;
}

const char *get_romdir_path(void) {
    return "";
}

const char *get_ramdir_path(void) {
    return "";
}

int lua_chkdir(const char *path) {
    (void)path;
    return 0;
}

void lua_espath(const char *src, char *dst, int dstsize) {
    if (dstsize <= 0) {
        return;
    }

    size_t len = strlen(src);
    if (len >= (size_t)dstsize) {
        len = (size_t)dstsize - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
}

void lua_rampath(const char *src, char *dst, int dstsize) {
    lua_espath(src, dst, dstsize);
}

#define ROTL32(a,b) ((uint32_t)(((a) << (b)) | ((a) >> (32 - (b)))))

static uint64_t lumux_mix(uint64_t *state) {
    uint64_t x = (*state += 0x9E3779B97F4A7C15ULL);

    x ^= (x >> 30);
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= (x >> 27);
    x *= 0x94D049BB133111EBULL;

    return x ^ (x >> 31);
}

static void lumux_quarter(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b; *d ^= *a; *d = ROTL32(*d, 16);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 12);
    *a += *b; *d ^= *a; *d = ROTL32(*d, 8);
    *c += *d; *b ^= *c; *b = ROTL32(*b, 7);
}

static uint32_t lumux_load32(const uint8_t *p) {
    return (uint32_t)p[0]
        | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16)
        | ((uint32_t)p[3] << 24);
}

static void lumux_store32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void lumux_block(uint32_t out[16], const uint32_t key[8], uint32_t counter,
                        const uint32_t nonce[3]) {
    static const uint32_t constants[4] = {
        0x61707865, 0x3320646e, 0x79622d32, 0x6b206574
    };
    uint32_t state[16];

    for (int i = 0; i < 4; i++) state[i] = constants[i];
    for (int i = 0; i < 8; i++) state[4 + i] = key[i];
    state[12] = counter;
    state[13] = nonce[0];
    state[14] = nonce[1];
    state[15] = nonce[2];

    for (int i = 0; i < 16; i++) out[i] = state[i];

    for (int i = 0; i < 10; i++) {
        lumux_quarter(&out[0], &out[4], &out[8],  &out[12]);
        lumux_quarter(&out[1], &out[5], &out[9],  &out[13]);
        lumux_quarter(&out[2], &out[6], &out[10], &out[14]);
        lumux_quarter(&out[3], &out[7], &out[11], &out[15]);

        lumux_quarter(&out[0], &out[5], &out[10], &out[15]);
        lumux_quarter(&out[1], &out[6], &out[11], &out[12]);
        lumux_quarter(&out[2], &out[7], &out[8],  &out[13]);
        lumux_quarter(&out[3], &out[4], &out[9],  &out[14]);
    }

    for (int i = 0; i < 16; i++) out[i] += state[i];
}

void lua_settab(uint64_t value, uint8_t *k, uint8_t *n) {
    uint64_t state = value;

    for (int i = 0; i < 32; i += 8) {
        uint64_t x = lumux_mix(&state);
        for (int j = 0; j < 8; j++) {
            k[i + j] = (uint8_t)(x >> (8 * j));
        }
    }

    for (int i = 0; i < 12; i += 8) {
        uint64_t x = lumux_mix(&state);
        for (int j = 0; j < 8 && (i + j) < 12; j++) {
            n[i + j] = (uint8_t)(x >> (8 * j));
        }
    }
}

void lua_push(void *data, size_t size, const uint8_t *k, const uint8_t *n, uint32_t flags) {
    (void)flags;

    uint8_t *p = (uint8_t *)data;
    uint32_t key[8];
    uint32_t nonce[3];
    uint32_t block[16];
    uint8_t stream[64];
    uint32_t counter = 0;

    for (int i = 0; i < 8; i++) {
        key[i] = lumux_load32(k + 4 * i);
    }
    for (int i = 0; i < 3; i++) {
        nonce[i] = lumux_load32(n + 4 * i);
    }

    while (size) {
        lumux_block(block, key, counter, nonce);
        for (int i = 0; i < 16; i++) {
            lumux_store32(&stream[i * 4], block[i]);
        }

        size_t chunk = size < sizeof(stream) ? size : sizeof(stream);
        for (size_t i = 0; i < chunk; i++) {
            p[i] ^= stream[i];
        }

        p += chunk;
        size -= chunk;
        counter++;
    }
}

void lua_setmeta(void *data, size_t size, long long source) {
    uint8_t key[32] = {0};
    uint8_t nonce[12] = {0};

    lua_settab((uint64_t)source, key, nonce);
    lua_push(data, size, key, nonce, 0);
}

#undef ROTL32

void zip_noentry(lua_State *L) {
    luaL_error(L, "zip-backed lfs is not available on Dreamcast");
}

int zip_iter_factory(lua_State *L) {
    return luaL_error(L, "zip-backed lfs.dir is not available on Dreamcast");
}

int zip_file_info(lua_State *L) {
    return luaL_error(L, "zip-backed lfs.attributes is not available on Dreamcast");
}
