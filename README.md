# DCSinge
A Dreamcast-native Singe / Hypseus-Singe runtime by Troy Davis (GPF).

DCSinge is a native Dreamcast port focused on FMV + Lua script driven games
(Dragon's Lair style, Singe 1.x/2.x style, and lightgun-heavy titles).
It runs on KallistiOS, renders with PVR, and uses a Dreamcast-optimized
`.dcmv` movie path instead of desktop video playback stacks.

## Current Engine Status
- Native `.dcmv` playback (VQ texture frames + optional compressed blocks + audio).
- Lua-driven Singe runtime with core overlay/text/sprite/input/game-flow hooks.
- Joypad + analog-to-mouse emulation for crosshair games.
- Configurable aim-assist pipeline using captured Lua hitboxes.
- Optional hitbox capture without hitbox drawing (`hitbox_draw=0`) for playable debug.
- Optional MP3 playback path (when enabled in config and files are cleaned).

## Repository Layout (high level)
```text
data/
  singe.cfg                    # active game/runtime config
  <game>.dcmv or frame_file
  <game>/singe/<game>/...

src/
  singe_dreamcast.c            # runtime + input + config + Lua bindings

tools/
  trim_wavs.py                 # WAV length/downsample helper
  png_pad_to_pot.sh            # PNG power-of-two pad helper
  clean_mp3.sh                 # MP3 metadata cleanup helper

dc.sh                          # CDI build helper
dctrace.py                     # profiler trace decode/graph helper
```

## singe.cfg (runtime config)
DCSinge reads `singe.cfg` from `/pc/data/` first, then `/cd/data/`.

Common keys:

```ini
game_dir=maddog2-hd/
frame_file=maddog2-mp.dcmv
# or: video_file=...
script_file=singe/maddog2-hd/maddog2-hd.singe
chunk_name=@maddog2-hd.singe
game_name=Mad Dog McCree 2

# audio toggles
disable_audio=0
enable_mp3=1

# crosshair alignment
crosshair_offset=-32
# crosshair_offset_x=-32
# crosshair_offset_y=0

# hitbox debug visualization
hitbox_draw=0

# mouse X send mode into Lua
# 0=offset, 1=direct, 2=inverse
mouse_send_mode=0

# analog -> mouse tuning
joymouse_deadzone=12
joymouse_response=1.8
joymouse_smooth=0.22
joymouse_speed=20

# aim assist
aim_assist=1
aim_assist_when_firing=1
aim_assist_red_only=1
aim_assist_strength=0.24
aim_assist_max_step=10
aim_assist_radius=48
aim_assist_hitbox_timeout_ms=150
```

For games with no usable FMV/container audio, use `disable_audio=1` and `enable_mp3=1` 

Button mapping keys supported:
- `btn_a`, `btn_b`, `btn_x`, `btn_y`, `btn_ltrigger`, `btn_rtrigger`, `btn_start`
- `btn2_a`, `btn2_b`, `btn2_x`, `btn2_y`, `btn2_ltrigger`, `btn2_rtrigger`, `btn2_start`

## Lightgun/Crosshair Notes
- Lua hitbox calls are captured in C and reused by aim-assist.
- With `aim_assist_red_only=1`, assist only uses red-coded hitboxes.
- `hitbox_draw=0` keeps capture active while avoiding full-screen debug box clutter.
- Crosshair sprite offsets are applied separately so gun titles can be aligned
  without shifting unrelated UI sprites.

## Asset Prep Workflow
### 1) Pad PNGs to power-of-two
```bash
tools/png_pad_to_pot.sh data/<game>/singe/<game>/images
```

### 2) Trim (or downsample) oversized WAV SFX
Dry run:
```bash
tools/trim_wavs.py data/<game>/singe/<game>/assets
```

Apply trims in place:
```bash
tools/trim_wavs.py --apply data/<game>/singe/<game>/assets
```

Optional downsample mode:
```bash
tools/trim_wavs.py --downsample --apply data/<game>/singe/<game>/assets
```

### 3) Clean MP3 metadata/tags for Dreamcast playback
```bash
tools/clean_mp3.sh data/<game>/singe/<game>
```

This strips ID3/Xing metadata and rewrites files in place so files begin with
MPEG frame data (more reliable for the KOS MP3 path).

## Build
With KOS environment loaded:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Result:
- `build/singe_dreamcast.elf`

## PVR Overlay Batching
DCSinge can submit Lua-driven 2D overlay batches directly into the active
transparent PVR vertex buffer. The submission pattern is adapted from
JNMARTIN's `pvr_dma_rendering/main_dma.c` demo: use `pvr_vertbuf_tail()`,
write compiled headers/vertices into the active list buffer, then call
`pvr_vertbuf_written()`.

Only that PVR vertex-buffer write pattern is used. DCSinge does not use the
demo's 3D transform, clipping, camera, perspective, matrix, or near-Z pipeline.
The optimization is limited to 2D overlay primitives such as batched lines,
plots, and boxes.

The FMV/DCFMV render path intentionally remains on its existing
`pvr_poly_cxt_txr()` / `pvr_poly_compile()` setup so strided compressed VQ
textures keep their exact YUV422/RGB565, twiddled/nontwiddled, and
`PVR_TEXTURE_MODULO` behavior.

Compile-time switches in `src/singe_dreamcast.c`:
- `DCSINGE_USE_PVR_VERTBUF_BATCH=0` disables direct vertex-buffer overlay batches.
- `DCSINGE_DEBUG_PVR_BATCH=1` prints lightweight batch counters.

## Disc Image Helper
Create a CDI with current `data/singe.cfg` game name:

```bash
./dc.sh
```

## FMV Encoding Toolchain
`.dcmv` creation is handled by the companion project:
- https://github.com/GPF/dreamcast-fmv

## Included/Targeted Game Work
This repo has been used to validate and tune multiple Singe titles including:
- SpaceRocks
- Crime Patrol HD
- Mad Dog McCree 2 (MP/HD layouts)
- Dragon's Lair / Space Ace style content

You can keep per-game tuning in each game's `singe.cfg`, then swap into
`data/singe.cfg` for active testing/builds.

## Credits
- Widge: SpaceRocks author and Dreamcast-specific content support.
- DirtBagXon: Hypseus/Singe guidance and testing support.
- JNMARTIN: `pvr_dma_rendering` reference pattern for direct PVR vertex-buffer submission.
- KallistiOS team and Dreamcast homebrew community.
- GPF: DCSinge runtime, `.dcmv` pipeline integration, Dreamcast port work.
