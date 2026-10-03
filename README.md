# DCSinge
A Dreamcast-native Singe / Hypseus-Singe runtime by Troy Davis (GPF).

DCSinge is a native Dreamcast port focused on FMV + Lua script driven games
(Dragon's Lair style, Singe 1.x/2.x style, and lightgun-heavy titles).
It runs on KallistiOS, renders with PVR, and uses a Dreamcast-optimized
`.dcmv` movie path instead of desktop video playback stacks.

## Current Engine Status
- Native `.dcmv` playback (VQ texture frames + optional compressed blocks + audio).
- MPEG-1 video + MP2 audio playback (`.mpg` + `.pidx` seek index) via the vendored `dc-libavmpeg` decoder. See [MPEG Movie Backend](#mpeg-movie-backend).
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
  <game>.mpg + <game>.pidx     # MPEG backend movie + seek index (optional)
  <game>/singe/<game>/...

src/
  singe_dreamcast.c            # runtime + input + config + Lua bindings
  dcfmv.c / dcfmv.h            # movie playback (dcmv v1/v6 and MPEG backends)

vendor/
  dc-libavmpeg/                # git submodule: MPEG-1/MP2 decoder
  libmp3/                      # vendored libmp3 with mp3_pause()/mp3_resume()

tools/
  trim_wavs.py                 # WAV length/downsample helper
  png_pad_to_pot.sh            # PNG power-of-two pad helper
  clean_mp3.sh                 # MP3 metadata cleanup helper
  encode_mpeg.sh               # video + audio -> .mpg + .pidx
  build_pidx.py                # .mpg seek index builder (used by encode_mpeg.sh)

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
git submodule update --init vendor/dc-libavmpeg   # first time only
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The MPEG backend is on by default (`DCSINGE_ENABLE_MPEG=ON`) and CMake stops with an error if `vendor/dc-libavmpeg` is empty. To build without it (dcmv only), configure with `-DDCSINGE_ENABLE_MPEG=OFF`.

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

## MPEG Movie Backend
Besides `.dcmv`, DCSinge can play MPEG-1 video + MP2 audio program streams, decoded on the SH-4 by [dc-libavmpeg](https://github.com/GPF/dc-libavmpeg) (vendored as a git submodule in `vendor/dc-libavmpeg`, built by CMake as the `avmpeg` target).

How a movie is picked:
1. The frame file entry resolves by name. For each location DCSinge tries `<stem>.dcmv` first, then `.mpg`, then `.mpeg`, so a `.dcmv` wins if both exist.
2. `dcfmv` then sniffs the file contents, not the extension: `DCMV` + version 1 or 6 selects the dcmv backends, and an MPEG pack start code (`00 00 01 BA`) selects the MPEG backend.

Each `.mpg` needs a `.pidx` seek index with the same stem beside it (`lair.mpg` + `lair.pidx`). Without it the open fails with `missing seek index`. Seeks land on closed-GOP I-frames, so keep the GOP short.

### Encoding with `tools/encode_mpeg.sh`
Starts from the normal Hypseus files (`game.m2v` + `game.ogg`) and produces `game.mpg` + `game.pidx`. Needs `ffmpeg` and `python3` (`ffprobe` is used to print source info but the script runs without it). It encodes the movie and builds the `.pidx` in one step:

```bash
# Dragon's Lair style: 4:3, 23.976 fps, 22050 Hz mono (the defaults)
tools/encode_mpeg.sh lair.m2v lair.ogg lair.mpg --expect 33759

# stereo source, 29.97 fps
tools/encode_mpeg.sh cops.m2v cops.wav cops.mpg --fps 30000/1001 --channels 2 --abit 96k
```

| Option | Default | Notes |
|---|---|---|
| `--size WxH` | `320x240` | output size |
| `--fps N/D` | `24000/1001` | real rate of the source; a raw `.m2v` has no timestamps. Every frame is kept. Try `24` if audio ends early |
| `--vbit RATE` | `1500k` | video bitrate |
| `--gop N` | `12` | closed GOP length; a seek decodes at most N-1 frames |
| `--rate HZ` | `22050` | audio sample rate |
| `--channels N` | `1` | 1 or 2 |
| `--abit RATE` | `64k` | use `96k` for stereo |
| `--expect N` | off | fail unless the video has exactly N pictures (catches wrong frame rate or telecine) |
| `--limit SECS` | off | encode only the first SECS seconds to try settings |
| `--vf FILTER` | none | ffmpeg filter before the scale, e.g. `yadif=0` or `crop=704:464:8:8` |

The defaults also use 2 B-frames and closed GOPs on purpose: they give clean I-frame seek points and bound how many frames a seek has to decode. Frame rate matters because a raw `.m2v` has no timestamps. If the audio finishes before the video, try `--fps 24` instead of `24000/1001`, and use `--expect` with the known frame count to catch a wrong rate before you copy files to the disc.

#### Rebuilding only the index
`encode_mpeg.sh` already builds the index. To regenerate it for an existing `.mpg`:

```bash
python3 tools/build_pidx.py lair.mpg lair.pidx
```

The `.pidx` must match the exact `.mpg` it came from, so rebuild it whenever the video is re-encoded. The builder only indexes I-frames that are the first picture in a PES packet and parses MPEG-1 streams only, so files from other encoders may seek less precisely. Use `encode_mpeg.sh` output where you can.

#### Converting several files
```bash
for video in *.m2v; do
    base="${video%.m2v}"
    [ -f "$base.ogg" ] && tools/encode_mpeg.sh "$base.m2v" "$base.ogg" "$base.mpg"
done
```

Put the `.mpg` and `.pidx` in `data/` next to where the `.dcmv` would go. `dc.sh` packs everything under `data/`, but only `*.dcmv` is sorted to the outer disc edge by default (`dcsinge.sort`).

CMake option `AVMPEG_STREAM` (default `2`) selects libavmpeg's input mode: `0` whole file in memory, `1` small buffer, `2` ring buffer. Leave it at the default unless you are tuning memory use.

Runtime logs: `[MPEG] opened <file>: WxH fps, frames, Hz ch` on open, and a `[MPEGSTAT]` line every 5 s (decode time, `a_underruns`, `drops`, `late_ticks`).

## FMV Encoding Toolchain
`.dcmv` creation is handled by the companion project:
- https://github.com/GPF/dreamcast-fmv

MPEG movies use `tools/encode_mpeg.sh` above.

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
