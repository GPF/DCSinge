cmake_minimum_required(VERSION 3.16)
project(DCSinge C)

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
    png
    z
    lfs
    lz4
    zstd
    freetype
    lua
    mp3
    bz2
    m
)

# Output name
set_target_properties(singe_dreamcast PROPERTIES
    OUTPUT_NAME "singe_dreamcast"
)
