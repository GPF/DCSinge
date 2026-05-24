cmake_minimum_required(VERSION 3.16)
project(DCSinge C)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
option(DCSINGE_ENABLE_KOSFAT_STORAGE "Enable lazy /sd and /ide asset loading through KOS libkosfat" ON)
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

target_compile_options(singe_dreamcast PRIVATE
    -iquote ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hypseus-singe
    -iquote ${CMAKE_CURRENT_SOURCE_DIR}/vendor/hypseus-singe/lua
)

if(DCSINGE_ENABLE_KOSFAT_STORAGE)
    target_compile_definitions(singe_dreamcast PRIVATE DCSINGE_ENABLE_KOSFAT_STORAGE=1)
endif()

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
    m
    $<$<BOOL:${DCSINGE_ENABLE_KOSFAT_STORAGE}>:kosfat>
    # KOS libraries should be last:
    kallisti
)

# Output name
set_target_properties(singe_dreamcast PROPERTIES
    OUTPUT_NAME "singe_dreamcast"
)
