cmake_minimum_required(VERSION 3.16)

if(DEFINED ENV{KOS_BASE} AND DEFINED ENV{KOS_CC} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_SYSTEM_NAME Generic)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    set(CMAKE_C_COMPILER "$ENV{KOS_CC}" CACHE FILEPATH "KOS SH4 C compiler")
    set(CMAKE_C_FLAGS_INIT "$ENV{KOS_CFLAGS}")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "$ENV{KOS_LDFLAGS}")

    get_filename_component(KOS_TOOLCHAIN_BIN "$ENV{KOS_CC}" DIRECTORY)
    set(CMAKE_AR "${KOS_TOOLCHAIN_BIN}/sh-elf-ar" CACHE FILEPATH "KOS SH4 archiver")
    set(CMAKE_RANLIB "${KOS_TOOLCHAIN_BIN}/sh-elf-ranlib" CACHE FILEPATH "KOS SH4 ranlib")
    set(CMAKE_OBJCOPY "${KOS_TOOLCHAIN_BIN}/sh-elf-objcopy" CACHE FILEPATH "KOS SH4 objcopy")
    set(CMAKE_OBJDUMP "${KOS_TOOLCHAIN_BIN}/sh-elf-objdump" CACHE FILEPATH "KOS SH4 objdump")
    set(CMAKE_SIZE "${KOS_TOOLCHAIN_BIN}/sh-elf-size" CACHE FILEPATH "KOS SH4 size")
endif()

project(DCSinge C)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")

if(NOT DEFINED KOS_PORTS AND DEFINED ENV{KOS_PORTS})
    set(KOS_PORTS "$ENV{KOS_PORTS}" CACHE PATH "KOS ports install prefix")
endif()

if(DEFINED ENV{KOS_LIBS})
    separate_arguments(KOS_LIBS_LIST NATIVE_COMMAND "$ENV{KOS_LIBS}")
endif()

option(DCSINGE_ENABLE_KOSFAT_STORAGE "Enable lazy /sd and /ide asset loading through KOS libkosfat" ON)
option(DCSINGE_USE_IO_MUTEX "Protect Singe Lua/VMU shadow filesystem I/O with a mutex" ON)
option(DCFMV_USE_IO_MUTEX "Protect DCFMV video/audio filesystem I/O with a mutex" ON)
option(DCFMV_USE_IO_MUTEX "Protect DCFMV video/audio filesystem I/O with a mutex" ON)
option(DCSINGE_ENABLE_MPEG "Build the MPEG-1/MP2 movie backend (libavmpeg, vendor/dc-libavmpeg)" ON)
option(DCSINGE_TRUST_LUA_BINARIES "Skip Lua bytecode verifier for Hypseus typed .bin chunks" OFF)
option(DCSINGE_ENABLE_GPROF "Build with KOS gprof instrumentation" OFF)
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
set(LIBMP3_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmp3)
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

set(LIBMP3_SRC
    ${LIBMP3_DIR}/libmp3/main.c
    ${LIBMP3_DIR}/libmp3/sndmp3.c
    ${LIBMP3_DIR}/xingmp3/cdct.c
    ${LIBMP3_DIR}/xingmp3/csbt.c
    ${LIBMP3_DIR}/xingmp3/cup.c
    ${LIBMP3_DIR}/xingmp3/cupl3.c
    ${LIBMP3_DIR}/xingmp3/cwinm.c
    ${LIBMP3_DIR}/xingmp3/dec8.c
    ${LIBMP3_DIR}/xingmp3/hwin.c
    ${LIBMP3_DIR}/xingmp3/icdct.c
    ${LIBMP3_DIR}/xingmp3/isbt.c
    ${LIBMP3_DIR}/xingmp3/iup.c
    ${LIBMP3_DIR}/xingmp3/iwinm.c
    ${LIBMP3_DIR}/xingmp3/l3dq.c
    ${LIBMP3_DIR}/xingmp3/l3init.c
    ${LIBMP3_DIR}/xingmp3/mdct.c
    ${LIBMP3_DIR}/xingmp3/mhead.c
    ${LIBMP3_DIR}/xingmp3/msis.c
    ${LIBMP3_DIR}/xingmp3/uph.c
    ${LIBMP3_DIR}/xingmp3/upsf.c
)

add_library(dcsinge_libmp3 STATIC ${LIBMP3_SRC})
target_include_directories(dcsinge_libmp3 PUBLIC
    ${LIBMP3_DIR}/include
)
target_include_directories(dcsinge_libmp3 PRIVATE
    ${LIBMP3_DIR}/xingmp3
)
target_compile_definitions(dcsinge_libmp3 PRIVATE LITTLE_ENDIAN=1)
target_compile_options(dcsinge_libmp3 PRIVATE -std=c17)

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
    $<$<BOOL:${DCSINGE_TRUST_LUA_BINARIES}>:LUAC_TRUST_BINARIES=1>
)

if(DCSINGE_ENABLE_GPROF)
    # KOS enables -fomit-frame-pointer by default, which GCC rejects with -pg.
    target_compile_options(singe_dreamcast PRIVATE
        -pg
        -fno-omit-frame-pointer
    )
    target_link_options(singe_dreamcast PRIVATE -pg)

    # CMake invokes sh-elf-gcc directly, so reproduce the libgprof link step
    # that the kos-cc wrapper normally adds when it sees -pg.
    set(DCSINGE_GPROF_LIBS
        "-Wl,--whole-archive"
        gprof
        "-Wl,--no-whole-archive"
    )
endif()

# ------------------------------------------------------------------
# Libraries (KOS + compression)
# ------------------------------------------------------------------
target_link_libraries(singe_dreamcast
    dcsinge_libmp3
    freetype
    png
    z
    lz4
    sh4zam
    zstd
    bz2
    $<$<BOOL:${DCSINGE_ENABLE_MPEG}>:avmpeg>
    m
    $<$<BOOL:${DCSINGE_ENABLE_KOSFAT_STORAGE}>:kosfat>
    ${DCSINGE_GPROF_LIBS}
    ${KOS_LIBS_LIST}
)

# Output name
set_target_properties(singe_dreamcast PROPERTIES
    OUTPUT_NAME "singe_dreamcast"
    SUFFIX ".elf"
)
