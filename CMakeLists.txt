cmake_minimum_required(VERSION 3.16)
project(DCSinge C)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
# ------------------------------------------------------------------
# Include directories
# ------------------------------------------------------------------
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${KOS_PORTS}/include/freetype2
)

# ------------------------------------------------------------------
# Source files
# ------------------------------------------------------------------
file(GLOB DCSINGE_SRC
    src/singe_dreamcast.c
)

# ------------------------------------------------------------------
# Executable
# ------------------------------------------------------------------
add_executable(singe_dreamcast
    ${DCSINGE_SRC}
    ${romdisk}
)

# ------------------------------------------------------------------
# Libraries (KOS + compression)
# ------------------------------------------------------------------
target_link_libraries(singe_dreamcast
    freetype
    png
    z
    lua
    mp3
    lz4
    lfs
    zstd
    bz2
    m
    # KOS libraries should be last:
    kallisti
)

# Output name
set_target_properties(singe_dreamcast PROPERTIES
    OUTPUT_NAME "singe_dreamcast"
)
