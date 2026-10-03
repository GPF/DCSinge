# DCSinge Project — Agent Instructions

## Project Overview
DCSinge is a Dreamcast port of the Hypseus Singe engine-based game "DCSinge" (Laser Game Player with Singe scripting). Located at `/home/gpf/code/dreamcast/DCSinge`.

## Key Files
- **Main C source**: `src/singe_dreamcast.c` (~10000+ lines)
- **Lua game script**: `spacerocks/singe/spacerocks/game.singe`
- **Build system**: CMake + KOS toolchain
- **Build output**: `build/singe_dreamcast.elf` → `dragons_lair_classic_dcsinge.cdi`

## Current Cops Status

### MP3 Playback
- MP3 playback is working on hardware.
- Important fix: initialize the MP3 stream system before `setup_lua()` when `enable_mp3=1`. Lua loads music handles during setup, and late MP3 init was clearing the track table, causing `musicPlay(handle)` to fail as an invalid handle.
- `[MusicAPI] musicLoad/musicPlay/musicStop` logging is enabled enough to confirm whether Lua calls play/stop.

### Bezel Support
- Cops expects `mainBezelLoaded()` to return true; returning false can make the title exit.
- Hypseus loads a full-screen bezel from `bezels/<file>.png` and `mainBezelLoaded()` reflects the real texture load state.
- Dreamcast implementation now auto-loads `.dt` bezels before Lua setup from:
  - `data/<GAME_DIR>/bezels/<game>.dt`
  - `data/bezels/<game>.dt`
  - `bezel_<game>.dt` fallbacks
- Cops bezel asset is `data/Cops/bezels/cops.dt`, generated from the original `bezel_cops.png`.
- `dc.sh` includes `/$GAME_DIR/bezels/*.dt` in the sort file and skips the PNG source when the sibling `.dt` exists.

### System Menu (Settings Overlay)
- Trigger changed from `Start + L + R` to **`Start + Y`** held 500ms. See `system_menu_handle_input`.
- Menu layout and panel quads are authored in logical overlay coordinates and transformed through the same overlay-to-bezel/content path. The 1024x512 POT bezel texture size is storage detail and must not drive menu placement.
- While the menu is active, bezel submission is skipped so opaque bezel pixels cannot cover the panel. Keep `g_bezel_loaded` true so `mainBezelLoaded()` and Lua game logic remain unchanged.
- Hardware validation confirmed that suppressing the bezel while the menu is open makes the complete menu readable over the FMV area.
- FMV and MP3 playback both pause while the menu is open and resume when it closes, preserving their relative synchronization.
- `vendor/libmp3` comes from the KallistiOS/libmp3 `master` branch used by kos-ports, base commit `1a13602`, and adds `mp3_pause()` / `mp3_resume()`. DCSinge links this vendored static library instead of installed `-lmp3`.
- The vendored stream callback returns an aligned silence buffer while paused without consuming PCM, bitstream, decoder, or file state. Hardware validation confirmed playback resumes from the correct position.
- Color palette: brighter navy/white/yellow for readability over FMV frames.
- L/R trigger page navigation preserved.

### Cops Controller Selection Menu / Overlay Transform
- Cops Lua sets `resX=896`, `resY=504`, `BASEOVERLAY=OVERLAY_OVERSIZE`, then calls `setOverlaySize(4, 896, 504)`. Those are Cops logical overlay coordinates, not the Dreamcast framebuffer size and not the bezel `.dt` texture size.
- The Cops bezel texture can be 640x480 content padded/stored as 1024x512 POT; do not confuse that texture size with the Lua overlay coordinate system.
- The 1/2 player controller selection screen was visually shifted because its text is authored/centered in Cops' 896x504 overlay space while the active bezel/overlay transform path was still treating it like bezel-space.
- Current workaround in `src/singe_dreamcast.c`: `overlay_draw_text()` detects `"CONTROL-DEVICE ASSIGNMENTS"` and sets `g_overlay_force_video_space_frame` so that frame's text uses the normal video-space transform. This makes the menu render correctly, but it is a temporary game/string-specific hook.
- TODO: Replace the string hook with a generic overlay transform rule based on logical overlay size and active bezel/layout state. Keep `mainBezelLoaded()` true and keep Cops `resX/resY` at 896x504 unless there is a proven reason to change Lua semantics.

### Aim Assist / bDebug
- Do **not** globally force Lua `bDebug=true` at startup anymore. That exposed Cops bezel/debug drawing in the wrong place and produced blue/yellow overlay junk over video.
- Aim assist temporarily sets Lua `bDebug=true` only inside hidden `drawHitboxes()` capture, then restores the previous Lua value.
- `hitbox_draw=0` hides visible debug rectangles while still allowing hidden capture.
- Cops hitboxes must be stored in the same transformed screen space used for drawing, not raw Lua rectangle coordinates. Raw Cops hitbox X values were about 200+ pixels off from the Dreamcast cursor comparison.
- Current `data/singe.cfg` uses cheat-style Cops tuning:
  - `aim_assist_when_firing=0`
  - `aim_assist_strength=0.85`
  - `aim_assist_max_step=36`
  - `aim_assist_radius=0`
  - `aim_assist_hitbox_timeout_ms=450`
- Next hardware log should show `[SHOT_TRACE] last_hitbox_center` close to `overlay=(x,y)` when a red hitbox is active.

### Cops Driving Mode / Wheel Support — Active Task
- Current project focus: get Cops driving mode working correctly on Dreamcast controls, including steering wheel support if the KOS controller path exposes usable analog/wheel data.
- Cops main script loads `data/Cops/singe/cops/cops.bin` from `data/Cops/singe/cops/cops.singe`.
- Lua inspection note: Cops source is not available. `cops.bin` is Hypseus typed Lua 5.1 bytecode; it can be normalized and decompiled, but the result is not original source and local names/control flow may be imperfect. Use decompiled output for investigation only.
- Decompiler workspace has been moved to `/home/gpf/Downloads/singedecomp/`:
  - Decompiled Lua: `/home/gpf/Downloads/singedecomp/cops.decompiled.lua`
  - Normalized Lua 5.1 bytecode: `/home/gpf/Downloads/singedecomp/cops.normal.luac`
  - Main bytecode listing: `/home/gpf/Downloads/singedecomp/cops.luac.txt`
  - Focused helper listings include `cops.mouse_index_select.luac.txt`, `cops.mouse_text_array.luac.txt`, and `cops_overlay_bezel_gate.luac.txt`.
  - Tools: `/home/gpf/Downloads/singedecomp/tools/typed_luac_dump`, `typed_luac_list`, `unluac.jar`, and their sources/build script.
- Regenerate normalized/decompiled files with:
  - `/home/gpf/Downloads/singedecomp/tools/build_typed_luac_list.sh`
  - `/home/gpf/Downloads/singedecomp/tools/typed_luac_dump data/Cops/singe/cops/cops.bin /home/gpf/Downloads/singedecomp/cops.normal.luac`
  - `java -jar /home/gpf/Downloads/singedecomp/tools/unluac.jar /home/gpf/Downloads/singedecomp/cops.normal.luac > /home/gpf/Downloads/singedecomp/cops.decompiled.lua`
  - `/home/gpf/Downloads/singedecomp/tools/typed_luac_list data/Cops/singe/cops/cops.bin > /home/gpf/Downloads/singedecomp/cops.luac.txt`
- Useful bytecode/decompiled anchors for driving:
  - `inputToDriving` around `cops.luac.txt:9685` and `cops.decompiled.lua:8103`
  - `steerSegmentsLeft` / `steerSegmentsRight` around `cops.luac.txt:10500` and `cops.decompiled.lua:9333` / `cops.decompiled.lua:9375`
  - steering display/use site around `cops.luac.txt:15987` and `cops.decompiled.lua:13220`
  - `dip_Steering` default around `cops.luac.txt:1083`
  - `drivingAxis`, `drivingZones`, `carZone`, `playerZone`, `mappedZone` around the same globals block as `inputToDriving`; decompiled globals start around `cops.decompiled.lua:8084`
- Start investigation by tracing how Dreamcast input populates Lua globals/functions used by Cops: `controllerPad`, `controllerTriggers`, `controllerHowMany`, `controllerGetPadding`, mouse globals, and any analog axis state. Keep fixes small and hardware-testable.
- Hypseus behavior checked in `~/code/hypseus-singe`: `controllerSetPadding(true)` sets gamepad mouse/device id padding to `100`; `controllerGetPadding()` returns that value. Cops expects controller/driver device ids `>=100` and mouse/lightgun ids below `100`.
- Current `src/singe_dreamcast.c` work-in-progress implements generic padding/shared driver controls, not a Cops string/path special case:
  - `GControllerPad` defaults to `0`.
  - `controllerSetPadding(true)` sets `GControllerPad=100` and logs `[ControllerAPI] controllerSetPadding(1) -> pad=100`.
  - `controllerGetPadding()` returns `GControllerPad`.
  - Normal gun mouse movement still calls `onMouseMoved(..., port)`.
  - With `shared_driver_controls=1`, the padded driver device gets its own `onMouseMoved(..., port + 100)` feed.
  - Face buttons/virtual gun events remain unpadded device `0/1`.
  - Analog L/R trigger events are tagged as controller-device events `100/101`, so R trigger can act as Cops gas pedal without stealing the shooter device.
- Latest hardware result:
  - Driving mode is enabled and playable. The old disabled-driving symptom is fixed by the controller padding/device-count behavior.
  - Shooter assignment works with device `0`; driver/gas assignment works with R trigger as device `100`.
  - Police Course starts and Cops updates `mouse3x`, `carZone`, `playerZone`, `mappedZone`, `fuelLeft`, and `controllerDoRumble()` strength during driving.
  - The useful C-side log line is `[RUMBLE] controllerDoRumble(...) currentFrame=... mouse3=(...) carZone=... playerZone=... mappedZone=... fuelLeft=...`.
  - Recent `out.log` showed bounded steering after the coordinate transform fix: `mouse3x` stayed roughly within the intended 0..896 range instead of overshooting to 1045.
  - Crash/death still occurs after several turns; latest death path was `discSkipToFrame(2020)` with `mouse3=(715,252) carZone=-1 playerZone=3 mappedZone=2 fuelLeft=23`, followed by death clip frames around `2020..2193`.

### Driving Mode Investigation Findings (2025-08-02)
- **Centering is correct**: Initial `mouse3=(448,252)` IS centered for the 896x504 overlay. Starting position is fine.
- **Driving model**: Cops driving is an FMV where the car's path is pre-determined by frame number. The player matches their steering wheel position (`mouse3x`) to where the car needs to be. It's NOT mouse-style accumulated steering — it's a direct position mapping.
- **Zone mapping** (from decompiled Lua `cops.decompiled.lua`):
  - `getDrivingZones(mouse3x)` maps overlay X to driving zones. Two modes exist based on `DOPT_XINVERT`:
    - Normal: ranges 0-55→-4, 55-111→-3, 111-167→-2, 167-223→-1, 223-279→0, 279-335→1, 335-391→2, 391-447→3, >447→4
    - XINVERT: ranges 0-98→-4, 98-197→-3, 197-296→-2, 296-395→-1, 395-499→0, 499-598→1, 598-697→2, 697-796→3, >796→4
  - `inputToDriving` is a lookup table mapping normalized driving axis (-4..4) → output zones (-2..2). Sign is preserved.
  - `mapZone(playerZone)` applies `inputToDriving` to produce `mappedZone`.
  - Driving logic: `carZone = getZones(currentFrame)` (pre-recorded, frame-dependent), `playerZone = getDrivingZones(mouse3x)`, then compare `mappedZone ~= carZone`.
- **The "EWWW" sound**: Lua calls `soundPlay(sndSteer)` whenever `mappedZone ~= carZone`; the spoken audio content lives outside the bytecode. It means the current steering position is wrong, not that a particular road zone is inherently bad.
- **Crash root cause**: Every frame with `mappedZone ~= carZone` accumulates `fuelDrain += fuelDelta`; crossing `offCourseLimit` removes fuel. `carZone=-1` means the route expects a moderate-left wheel position. At frame 217 the first turn starts while the player is still centered, so fuel loss begins until the player reaches `mappedZone=-1`.
- **Death sequence** (frame 2020+): `carZone=-1`, `playerZone=3`, `mappedZone=2` — persistent mismatch drains fuel until death. mouse3 oscillates wildly (91..736) during death frames.
- **Log anomaly**: `DRIVER_MOUSE` `raw=(272,252) → input=0.00, target=448.0` is suspicious. `raw` in the log is actually the smoothed `GDriverMouseX/Y` output (overlay coordinates), NOT the raw joystick `lx` value. The actual `lx` (KOS joyx, -128..127) is different. This means the log shows where the cursor IS, not what the stick input WAS.
- **Steering direction/mode confirmed**: Runtime values match the `DOPT_XINVERT` zone table: `mouse3x=448` maps to center, lower X maps left, and higher X maps right. Perceived inversion came from delayed correction/oversteer, not reversed C math.
- Current implementation change: the Dreamcast/controller driving path no longer reuses gun cursor `relX` as accumulated mouse-style steering. With `shared_driver_controls=1`, analog stick X now maps to a center-based driver steering target and `GDriverMouseX` eases toward that target each frame. Stick neutral targets center, so steering should spring back instead of staying off-center after a turn. The current tuning lives in `data/singe.cfg` via `shared_driver_deadzone`, `shared_driver_range`, and `shared_driver_smooth`.
- Bytecode explains the old disabled-driving symptom: if `p3Active` is false, Cops initializes `drivingArray` as already complete. Keep this in mind, but current logs show `player3index=100` and active driving/rumble state.
- Driver coordinate notes:
  - Sending raw overlay X directly was wrong because Cops/Hypseus ratio correction also applies to the driver mouse path; sending raw `896` produced `mouse3x=1045` in Lua and over-drove steering.
  - Current code sends driver X through the configured gun mouse transform (`mouse_send_mode=2` inverse in `data/singe.cfg`) but passes relative delta as `0`, avoiding the earlier double-step.
  - Logs include `[DRIVER_MOUSE] ... input=... target=... raw=(...) sent=(...) sendX{offset=...,direct=...,inverse=...} luaMouse3After=(...)`.
  - `data/singe.cfg` now has `shared_driver_deadzone`, `shared_driver_range`, and `shared_driver_smooth` for this controller steering model.
- Aim-assist note:
  - Hidden `drawHitboxes()` capture is skipped once Cops assigns the padded driver (`player3index >= 100`) because the Cops hitbox path errors during driving and can spam logs/overlay work.

### Latest Driving/Rumble Results (2026-07-15)
- Resetting the system menu to disc defaults improved the first logged driving match rate from about 30% to 46.5%. With range `260` and smooth `0.14`, the player held the correct first-left zone for about 20 frames before full-left crossed the narrow `mouse3x=197` boundary into excessive-left `mappedZone=-2`.
- Analog range is now `249`, so full joystick left/right stays inside Cops' normal `mappedZone=-1/+1` bands. D-pad uses a wider overlay-relative range and is reserved for hard `-2/+2` turns.
- Police Course is continuous position matching, not timed button presses or obstacle dodging. Outside listed turn intervals `getZones(frame)` returns `0`, so center the stick on straight road. Hold joystick left/right for moderate `-1/+1` intervals; hold D-pad left/right for hard `-2/+2` intervals.
- First Police Course sequence from the decompiled table:
  - frames `165..216`: straight/center
  - `217..280`: moderate left (hold joystick left)
  - `281..305`: center
  - `306..355`: moderate left
  - `356..375`: center
  - `376..460`: moderate right
  - `461..490`: center
  - `491..553`: moderate right
  - `554..577`: hard right (hold D-pad right)
  - `578..589`: moderate right
  - `590..604`: center
  - `605..620`: moderate left
  - `621..640`: hard left (hold D-pad left)
  - `641..650`: moderate left
  - `651..687`: center
- Dreamcast Jump Pack support is implemented with the KOS structured `purupuru_rumble()` API. Cops strength changes map to: center/strength 1 = off, strength 2 = light, strength 3 = strong, strength 4 = maximum. Commands send only on strength changes, stop after 150ms without calls, retry a busy Maple stop, and stop during shutdown. Hardware confirmed rumble feels appropriate. `[RUMBLE_HW]` confirms successful sends.
- Current rumble describes steering magnitude/position, not correctness: strong rumble can be a correct hard turn or an oversteer. A possible later improvement is error-based feedback: no rumble when `mappedZone == carZone`, stronger rumble with larger mismatch, while the VFD arrows provide direction.
- Hardware audio regression warning: changing VMU override loading to ignore saved driver-tuning keys caused MP3/FMV audio to cut out/slow at the title. Reverting that VMU loader change restored sound. Do not reintroduce it without isolating the timing interaction. Apply new disc tuning through the menu: **Start+Y**, then **Y** reset defaults, **X** save, **B** close.
- The attempted custom steering gauge never appeared on hardware and added per-frame translucent PVR work; it was completely removed.

### Next Hardware Test
- Reset/save disc defaults again so VMU stores the new analog range `249`, then verify title MP3/FMV audio remains clean before entering Police Course.
- Confirm `[RUMBLE_HW] id=0 strength=...` appears and the Jump Pack becomes quiet at center, light for joystick turns, and strong for D-pad hard turns.
- Drive the first sequence using held positions rather than taps. At frame 217, hold joystick left until the center interval begins at frame 281; do not countersteer early.
- Update `out.log` after a full attempt. The current 260-line rumble budget is consumed mostly before/largely at the start of driving; next diagnostic code change should log only zone/fuel transitions so the complete course is captured without adding heavy per-frame I/O.

## General Guidelines
- **Personality**: Be friendly, direct, and a little quirky. Light humor welcome when debugging.
- **Workflow**: Troy prefers small, testable patches over big rewrites.
- **Hardware**: Real Dreamcast hardware results beat emulator results. Be suspicious of alignment, cache flushing, DMA, PVR/TA flags, vertex formats, color packing, endian issues, uninitialized memory, and timing.
- **Live `kos-tool` runs**: Do not manually kill `kos-tool`/`dc-tool-ip` while the Dreamcast program is still running. Let Troy exit the game on hardware so the program returns cleanly, unless the transfer/tool is genuinely non-responsive.
- **Code investigation**: Use targeted grep/sed/read. Read at most 120 lines per command. Explain what question the next read answers before reading more files. Separate facts from guesses.
- **Patching**: Make smallest safe change. Keep changes easy to revert and test on hardware.
- **Output style**: Keep responses under 400 words unless showing patches or log excerpts.

## Build Instructions
1. Source toolchain: source /opt/toolchains/dc/kos/environ.sh
2. Configure: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
3. Build: cmake --build build
4. CDI generation: bash dc.sh (generates game-specific .cdi)
5. Hardware test: transfer .cdi to DC via nettransfer or CD-R
