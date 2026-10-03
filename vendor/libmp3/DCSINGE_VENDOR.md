Vendored from KallistiOS/libmp3.

Source: https://github.com/KallistiOS/libmp3.git
Branch: master
Base commit: 1a1360238f98a8dc2d91e752c366ffb8d24e41ff

DCSinge local changes:
- Add mp3_pause(), mp3_resume(), and mp3_is_paused() public APIs.
- Keep the KOS sound stream alive while paused by returning silence from the
  stream callback without consuming MP3 decoder, PCM, bitstream, or file state.
- Fix a few legacy C declarations so the library builds under this project's
  CMake/GCC toolchain.
