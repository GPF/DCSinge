cmake_minimum_required(VERSION 3.16)
project(DCSinge C)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
option(DCSINGE_ENABLE_KOSFAT_STORAGE "Enable lazy /sd and /ide asset loading through KOS libkosfat" ON)
option(DCSINGE_USE_IO_MUTEX "Protect Singe Lua/VMU shadow filesystem I/O with a mutex" ON)
option(DCFMV_USE_IO_MUTEX "Protect DCFMV video/audio filesystem I/O with a mutex" ON)
option(DCSINGE_ENABLE_MPEG "Build the MPEG-1/MP2 movie backend (libavmpeg, vendor/dc-libavmpeg)" ON)
# ------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hypseus-singe
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hypseus-singe/lua
    ${KOS_PORTS}/include/freetype2
    ${KOS_PORTS}/include/sh4zam
)

# ------------------------------------------------------------------
# libavmpeg (MPEG-1 video + MP2 audio decoder, git submodule)
# Built by the submodule's own libavmpeg.mk with the KOS environment (environ.sh), the
# same one the toolchain file needs. Run: git submodule update --init vendor/dc-libavmpeg
# ------------------------------------------------------------------
if(DCSINGE_ENABLE_MPEG)
    set(AVMPEG_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/dc-libavmpeg)
    if(NOT EXISTS ${AVMPEG_SRC_DIR}/libavmpeg.mk)
        message(FATAL_ERROR "vendor/dc-libavmpeg is empty: run "
                "'git submodule update --init vendor/dc-libavmpeg' "
                "(or configure with -DDCSINGE_ENABLE_MPEG=OFF)")
    endif()
    find_program(AVMPEG_MAKE NAMES gmake make REQUIRED)
    set(AVMPEG_OUT ${CMAKE_CURRENT_BINARY_DIR}/libavmpeg)
    # Ring input (AV_STREAM=2): movies are far larger than the heap.
    set(AVMPEG_STREAM 2 CACHE STRING "libavmpeg input mode: 0 whole file, 1 small buffer, 2 ring")
    # Always runs; make does the dependency tracking and is a no-op when nothing changed.
    add_custom_target(avmpeg_build
        COMMAND ${AVMPEG_MAKE} -j8 -f ${AVMPEG_SRC_DIR}/libavmpeg.mk
                OUT=${AVMPEG_OUT} AV_STREAM=${AVMPEG_STREAM}
        COMMENT "Building libavmpeg (AV_STREAM=${AVMPEG_STREAM})"
        VERBATIM)
    add_library(avmpeg STATIC IMPORTED GLOBAL)
    set_target_properties(avmpeg PROPERTIES IMPORTED_LOCATION ${AVMPEG_OUT}/libavmpeg.a)
    add_dependencies(avmpeg avmpeg_build)
    include_directories(${AVMPEG_SRC_DIR}/src)
endif()

# ------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------
file(GLOB DCSINGE_SRC
    src/dcfmv.c
    src/singe_dreamcast.c
    src/dcsinge_lua_compat.c
    #src/profiler.c
)

set(HYPSEUS_LUA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hypseus-singe/lua)
set(HYPSEUS_LUA_SRC
    ${HYPSEUS_LUA_DIR}/lapi.c
    ${HYPSEUS_LUA_DIR}/lauxlib.c
    ${HYPSEUS_LUA_DIR}/lbaselib.c
    ${HYPSEUS_LUA_DIR}/lbit.c
    ${HYPSEUS_LUA_DIR}/lcode.c
    ${HYPSEUS_LUA_DIR}/ldblib.c
    ${HYPSEUS_LUA_DIR}/ldebug.c
    ${HYPSEUS_LUA_DIR}/ldo.c
    ${HYPSEUS_LUA_DIR}/ldump.c
    ${HYPSEUS_LUA_DIR}/lfs.c
    ${HYPSEUS_LUA_DIR}/lfunc.c
    ${HYPSEUS_LUA_DIR}/lgc.c
    ${HYPSEUS_LUA_DIR}/linit.c
    ${HYPSEUS_LUA_DIR}/liolib.c
    ${HYPSEUS_LUA_DIR}/llex.c
    ${HYPSEUS_LUA_DIR}/lmathlib.c
    ${HYPSEUS_LUA_DIR}/lmem.c
    ${HYPSEUS_LUA_DIR}/loadlib.c
    ${HYPSEUS_LUA_DIR}/lobject.c
    ${HYPSEUS_LUA_DIR}/lopcodes.c
    ${HYPSEUS_LUA_DIR}/loslib.c
    ${HYPSEUS_LUA_DIR}/lparser.c
    ${HYPSEUS_LUA_DIR}/lrandom.c
    ${HYPSEUS_LUA_DIR}/lstate.c
    ${HYPSEUS_LUA_DIR}/lstring.c
    ${HYPSEUS_LUA_DIR}/lstrlib.c
    ${HYPSEUS_LUA_DIR}/ltable.c
    ${HYPSEUS_LUA_DIR}/ltablib.c
    ${HYPSEUS_LUA_DIR}/ltm.c
    ${HYPSEUS_LUA_DIR}/lundump.c
    ${HYPSEUS_LUA_DIR}/lvm.c
    ${HYPSEUS_LUA_DIR}/lzio.c
    ${HYPSEUS_LUA_DIR}/print.c
)

# ------------------------------------------------------------------
# Executable
# ------------------------------------------------------------------
add_executable(singe_dreamcast
    ${DCSINGE_SRC}
    ${HYPSEUS_LUA_SRC}
    ${romdisk}
)

if(DCSINGE_ENABLE_KOSFAT_STORAGE)
    target_compile_definitions(singe_dreamcast PRIVATE DCSINGE_ENABLE_KOSFAT_STORAGE=1)
endif()

if(DCSINGE_ENABLE_MPEG)
    target_compile_definitions(singe_dreamcast PRIVATE DCSINGE_ENABLE_MPEG=1)
endif()

target_compile_definitions(singe_dreamcast PRIVATE
    SINGE_USE_IO_MUTEX=$<BOOL:${DCSINGE_USE_IO_MUTEX}>
    DCFMV_USE_IO_MUTEX=$<BOOL:${DCFMV_USE_IO_MUTEX}>
)

# ------------------------------------------------------------------
# Libraries (KOS + compression)
# ------------------------------------------------------------------
target_link_libraries(singe_dreamcast
    freetype
    png
    z
    mp3
    lz4
    sh4zam
    zstd
    bz2
    $<$<BOOL:${DCSINGE_ENABLE_MPEG}>:avmpeg>
    m
    $<$<BOOL:${DCSINGE_ENABLE_KOSFAT_STORAGE}>:kosfat>
    # KOS libraries should be last:
    kallisti
)

# Output name
set_target_properties(singe_dreamcast PROPERTIES
    OUTPUT_NAME "singe_dreamcast"
)
