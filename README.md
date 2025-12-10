# DCSinge  
A Dreamcast-native port of the Singe / Hypseus-Singe FMV engine  
by Troy Davis (GPF)

DCSinge is a ground-up Dreamcast reimplementation of the Singe/Hypseus FMV
engine used in laserdisc-style games such as Dragon’s Lair, Space Ace, Crime
Patrol, Mad Dog McCree, Hologram Time Traveler, and modern Singe 2 fan titles.

Unlike Hypseus (PC), DCSinge uses **no SDL and no FFmpeg**.  
Everything is implemented directly on Dreamcast hardware:

- **PVR** for rendering  
- **AICA ADPCM** for audio  
- **Lua 5.4** for game logic  
- **KallistiOS** for filesystem, threading, and system services  

DCSinge replaces MPEG-1 video decoding with a Dreamcast-optimized `.dcmv`
container format based on preprocessed VQ textures and ADPCM audio.

---

# ✨ Features

## ✔ Dreamcast-Native FMV Playback
- Plays `.dcmv` files containing:
  - VQ-compressed PVR texture frames  
  - Optional LZ4 or Zstandard compression  
  - Frame offset table  
  - **AICA ADPCM (4-bit) audio stream**
- Video synced to audio via `psTimer()`  
- Optional threaded preloading  
- Zero runtime video decoding overhead  

## ✔ Lua-Based Singe Engine (Hypseus-Compatible)
DCSinge implements the Singe Lua API used by Hypseus, including:

- `start_clip()`, `stop_clip()`, frame-range playback  
- Overlay drawing (`sep_draw_box`, `sep_draw_text`, `sep_draw_sprite`)  
- Timer and event callbacks  
- Input handling  
- Branching and scene transitions  

Runs `.singe` scripts directly.

## ✔ Native Dreamcast Hardware Integration
- PVR textured quad rendering (no SDL abstraction)  
- Maple controller input  
- AICA streaming audio  
- Optional Jump Pack rumble  
- Supports ROM disk, ISO/CDI, and GDEMU  

---

# 🔍 Differences from Hypseus Singe

| Feature | Hypseus (PC) | DCSinge (Dreamcast) |
|--------|--------------|---------------------|
| Video | FFmpeg MPEG-1 (`.m2v`) | **Custom `.dcmv` (VQ frames)** |
| Audio | WAV / OGG | **AICA ADPCM or cleaned MP3/WAV** |
| Rendering | SDL2 | **Native PVR** |
| Timing | SDL timers | `psTimer()` |
| Input | SDL | Maple |
| Script engine | Lua 5.3 | Lua 5.4 |
| Filesystem | Desktop FS | ROM disk / CD / SD |

DCSinge preserves **Singe Lua behavior**, not FFmpeg or SDL renderer behavior.

---

# 🧪 Included Example: *SpaceRocks*  
### (Custom Dreamcast Edition Provided by the Author)


https://github.com/user-attachments/assets/01ef5080-945f-4ad0-ada4-743b0a34a83a


DCSinge includes a working test port of the Singe 2 fan game **SpaceRocks**,  
bundled **with permission from its creator, Widge™**.

To support Dreamcast hardware, **Widge™** provided:

- A custom **640×480 MPEG-1 `.m2v`** file for `.dcmv` conversion  
- An updated **`spacerocks.singe`** script compatible with the DCSinge folder layout  
- Additional refinements for Dreamcast playback  

Delivered via DirtBagXon® on 11/13/25:

> “Widge has made some resolution changes to SpaceRocks LUA and video for DC…”  
> <https://app.filen.io/#/f/5c906266-0185-46d4-884f-9408d0d35e04%23756b3659626f46356472376552614c466e744959504152526831302d616c6b4d>

This example demonstrates:

- FMV playback  
- Lua logic  
- Sprites and overlays  
- Input mapping  
- Audio playback  

All creative rights remain with **Widge™**.

---

# 📁 Game Data Layout

A typical game folder looks like:

data/
├── singe.cfg
├── spacerocks.dcmv
└── spacerocks/
└── singe/
└── spacerocks/
├── spacerocks.singe
├── images/.png
└── sounds/.mp3 or *.wav

### Example `singe.cfg`

game_dir=spacerocks/
video_file=spacerocks.dcmv
script_file=singe/spacerocks/spacerocks.singe
chunk_name=@spacerocks.singe
game_name=SpaceRocks

---

# 🖼️ Texture Requirements (PNG → POT)

DCSinge loads textures using KOS’s:

png_to_texture()

Dreamcast PVR requires **power-of-two** dimensions.

### ✔ PNG files must be:

- 32 / 64 / 128 / 256 / 512 pixels (width & height)  
- Standard PNG (RGB or RGBA)  
- Not required to be converted to `.pvr`  
- Simply resized to POT dimensions  

All PNGs used in `.singe` scripts should be resized accordingly.

Textures are placed in:

data/<game>/singe/<game>/images/*.png

---

# 🎵 Audio Notes (WAV + MP3)

## WAV Files  
WAV files are kept as-is.  
However:

- Many Singe WAVs exceed **64 KB**, which Dreamcast cannot load fully into AICA  
- These may **fail to play** or play only partially  

Future versions will include WAV → ADPCM conversion and audio chunking.

---

## MP3 Files  
DCSinge’s MP3 decoder requires files to begin with a valid MPEG frame (`FF FB`).  
Most MP3s in Singe fan games contain **ID3 tags**, which break decoding.

Clean files using:

ffmpeg -i input.mp3 -codec copy -map 0:a:0 -write_xing 0 output_clean.mp3
Verify:

xxd -l 8 file.mp3
Expected output:

ff fb …
Cleaned MP3s are already included for SpaceRocks.

🎥 DCMV Movie Format
.dcmv files contain:

VQ-compressed Dreamcast-ready texture frames

Optional Zstd/LZ4 compression

AICA ADPCM audio stream

Frame offset table

FPS, resolution, and metadata

This format eliminates runtime video decoding, enabling smooth FMV playback on Dreamcast.

🛠 FMV Encoding Toolchain
(Required for .dcmv Movies)
DCSinge relies on the Dreamcast FMV Encoder Toolchain:

👉 https://github.com/GPF/dreamcast-fmv

This toolchain includes:

pack_dcmv — Frame+audio packer using LZ4 HC or Zstd

dcaconv — ADPCM encoder

pvrtex — VQ/PVR texture conversion

ffmpeg — Frame extraction

Example playback app (fmv_play.elf)

What it does
✔ Converts MP4/M2V into YUV → VQ frames
✔ Converts audio to ADPCM
✔ Packs into .dcmv with frame offsets & compression blocks
✔ Produces Dreamcast-ready movies for DCSinge

See its README for details and usage examples.

📦 LuaFileSystem (lfs) Support
Some Singe scripts require filesystem access.
DCSinge includes a Dreamcast port of LuaFileSystem (lfs), supporting:

🔧 Building DCSinge
Requirements:

KallistiOS

kos-ports (png, freetype, lua, mp3, zlib, zstd, lz4)

kos-cmake

Build:

bash
Copy code
kos-cmake -S . -B build
cd build
make -j16
Produces:

singe_dreamcast.elf

🚧 Development Status
Working
FMV playback via .dcmv

ADPCM audio

Lua 5.4 engine

Singe API coverage

PNG overlays & sprites via POT PNGs

MP3 playback (clean files only)

Input handling

Full SpaceRocks port (custom provided assets)

In Progress
WAV → ADPCM conversion toolchain

Improved MP3 error handling

VMU save support

Sprite atlas optimization

Compatibility expansion for additional Singe titles

🎯 Project Goals
Bring Singe-style FMV games to Dreamcast hardware

Provide a full FMV + Lua scripting engine for homebrew

Maintain compatibility with existing Singe scripts

Support new FMV-driven Dreamcast games


https://github.com/user-attachments/assets/134f0f68-a6ee-41d7-b785-adefbd6b06c4


🤝 Credits
Widge™ — SpaceRocks author; provided custom Dreamcast-ready assets

DirtBagXon® — Hypseus Singe developer; guidance and coordination

KallistiOS Team — SDK and system support

Dreamcast Homebrew Community

GPF — .dcmv format, FMV encoder, Dreamcast engine port, Lua integration

📣 Follow Updates
Twitter/X: @GPFTroy
GitHub: https://github.com/GPF
