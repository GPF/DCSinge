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

void lua_settab(uint64_t value, uint8_t *k, uint8_t *n) {
    (void)value;
    (void)k;
    (void)n;
}

void lua_setmeta(void *data, size_t size, long long source) {
    (void)data;
    (void)size;
    (void)source;
}

void lua_push(void *data, size_t size, const uint8_t *k, const uint8_t *n, uint32_t flags) {
    (void)data;
    (void)size;
    (void)k;
    (void)n;
    (void)flags;
}

void zip_noentry(lua_State *L) {
    luaL_error(L, "zip-backed lfs is not available on Dreamcast");
}

int zip_iter_factory(lua_State *L) {
    return luaL_error(L, "zip-backed lfs.dir is not available on Dreamcast");
}

int zip_file_info(lua_State *L) {
    return luaL_error(L, "zip-backed lfs.attributes is not available on Dreamcast");
}
