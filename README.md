# TapeXPlayer

**TapeXPlayer** is experimental video player representing a software implementation of professional videotape recorder approach in digital format. The project arose from interest in how the logic of Betacam format videotape recorders can be embodied in software code. 

<img width="1392" height="860" alt="TapeXPlayer_Instance_#1_alisa_soundedit3_mov_2025_10_09_01_58_58" src="https://github.com/user-attachments/assets/241ac645-8536-4d18-9547-f4495213bb54" />

 
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)]()
[![Architecture](https://img.shields.io/badge/arch-Universal%20Binary%20(x86__64%20%2B%20arm64)-brightgreen)]()

---

## Key Features

TapeXPlayer is built with **C**,**C++**, **Objective-C**, **Swift** using industry-standard libraries: **FFmpeg**, **SDL2**, **PortAudio**, and **RtMidi**. It brings professional tape-based video player functionality to modern computers, enabling thorough video sequence examination with frame-accurate control.

- **Smooth Shuttle Control**: Forward/backward playback up to 32x speed with minimal CPU usage
- **Frame-Accurate Seeking**: Timecode-based navigation (HH:MM:SS:FF)
- **MIDI Controller Support**: Supports integration with Mackie HUI protocol (tested with Behringer X-Touch One)
- **Memory Locations**: Quick navigation to important points with zoom recall
- **Real-Time Zoom & Pan**: Mouse-based zoom with thumbnail preview
- **Screenshot Capture**: Export frames with timecode overlay
- **Hardware Acceleration**: VideoToolbox (macOS), with Metal and FFmpeg fallbacks
- **Smart Caching**: Low-resolution proxy for smooth scrubbing
- **Performance**: ~52% CPU @ 32x shuttle, ~11-32% CPU @ 1x playback

---
## Platform Support

| Platform | Status | Architecture | Build |
|----------|--------|--------------|-------|
| **macOS** | ✅ Available | Universal Binary (Intel + Apple Silicon) | Build 1866 |
| **Linux** | ✅ Available (DEB) | x86_64 | Build 1866 |
| **Windows** | ✅ Available | x86_64 | Builds 1866 |

---

## Installation

Prebuilt packages are on the [releases page](https://github.com/ffbsoffa/TapeXPlayer/releases).

#### macOS

1. Download `TapeXPlayer-mac-universal-build….zip` and unpack it
2. Drag `TapeXPlayer.app` to `/Applications`
3. The app is not notarized yet, so remove the quarantine flag once:
   ```bash
   xattr -d com.apple.quarantine /Applications/TapeXPlayer.app
   ```
4. Launch it from `Applications`. If macOS still asks, confirm in System Settings → Privacy & Security → Open Anyway

#### Windows

1. Download `TapeXPlayer-Setup-x64-build….exe` and run it
2. The installer adds Start Menu and desktop shortcuts and an entry in "Add or Remove Programs"

The build is not code-signed yet — if SmartScreen warns, choose "More info" → "Run anyway".

#### Linux

Download `tapexplayer_…_amd64.deb` and install it:
```bash
sudo apt install ./tapexplayer_*_amd64.deb
```
The package is built on Ubuntu 24.04. On older releases or other distributions, build from source (below).

---

## Building from source

TapeXPlayer builds with a plain `makefile` (no CMake) from the `source/` directory. The same makefile detects the platform. The CI workflows in [`.github/workflows/`](.github/workflows) build every platform on each push and are the reference if something here drifts.

```bash
git clone https://github.com/ffbsoffa/TapeXPlayer.git
cd TapeXPlayer/source
```

Every local `make` bumps `source/.build_number`. Add `FREEZE_BUILD_NUMBER=1` to keep it unchanged. `make help` lists the targets for your platform.

### macOS

Requirements: Xcode 16+ (Apple clang with `-std=c++23`; Xcode 16 itself needs macOS 14.5+) and [Homebrew](https://brew.sh). The resulting app runs on macOS 13+.

```bash
xcode-select --install          # if the command line tools are missing
brew install ffmpeg sdl2 sdl2_ttf portaudio openssl@3 rtmidi
brew install lua                # optional: enables Lua extensions
```

```bash
make -j$(sysctl -n hw.ncpu)     # -> builds/binaries/TapeXPlayer
make bundle                     # -> builds/TapeXPlayer.app (libraries copied in, ad-hoc signed)
```

`make bundle-universal` builds an arm64 + x86_64 app and copies it to `/Applications` unless you pass `SKIP_INSTALL=1`. Homebrew no longer supports Intel, so the x86_64 libraries are built from source first: `../.github/scripts/build-macos-x86_64-deps.sh <dir>`, then `make bundle-universal X86_64_PREFIX=<dir>` (default `/usr/local`).

Homebrew's `sdl2` is now `sdl2-compat` (SDL2 API on top of SDL3). It is fine for local builds; release builds use real SDL2 built from source, see [`macos-build.yml`](.github/workflows/macos-build.yml).

### Linux (Debian / Ubuntu)

Requirements: Ubuntu 22.04+ or Debian 12+, GCC 11+. CI builds on Ubuntu 24.04.

```bash
sudo apt update
sudo apt install build-essential pkg-config \
    libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev \
    libsdl2-dev libsdl2-ttf-dev portaudio19-dev libssl-dev librtmidi-dev \
    libgtk-3-dev libva-dev
```

```bash
make -j$(nproc)                 # -> builds/binaries/TapeXPlayer_linux
make deb                        # -> builds/binaries/deb/tapexplayer_<version>.<build>_<arch>.deb
```

On other distributions install the development packages of the same libraries (FFmpeg, SDL2, SDL2_ttf, PortAudio, OpenSSL, RtMidi, GTK 3) and `pkg-config`; the makefile finds GTK through `pkg-config`, everything else through the standard include/lib paths. x86_64 and arm64 are supported.

### Windows

Windows builds use [MSYS2](https://www.msys2.org) with the MinGW-w64 toolchain. Use the **MSYS2 MINGW64** shell, not MSYS or UCRT64: the makefile links against `/mingw64`.

The quickest way is the bootstrap script. It installs the toolchain and dependencies, clones the repo, builds and bundles the DLLs:
```bash
curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/stable/source/bootstrap_win.sh -o bootstrap_win.sh
bash bootstrap_win.sh
```

Or by hand:
```bash
pacman -S --needed make git zip \
    mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf \
    mingw-w64-x86_64-portaudio mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-openssl \
    mingw-w64-x86_64-rtmidi mingw-w64-x86_64-nsis
```

```bash
make -j$(nproc)                 # -> builds/binaries/TapeXPlayer.exe
./bundle_dlls.sh                # -> builds/win-bundle/ (exe + all DLLs, runs without MSYS2)
./create_win_installer.sh x64   # -> builds/TapeXPlayer-Setup-x64-build<N>.exe (NSIS)
```

---

## System Requirements

**Minimum requirements:**
- Operating system: Windows 10 (1903+) / 11 x64, macOS 13 Ventura+, Linux x86_64 (Ubuntu 22.04+ / Debian 12+)
- Processor: Intel Pentium Gold 7505 or equivalent with hardware decoding support (Intel QSV, AMD VCE)
- Memory: 8 GB RAM
- Video card with built-in hardware video decoder

When running on minimum requirements, the application automatically disables the full-resolution decoder and uses only the lightweight low-resolution decoder designed for shuttle mode. This preserves interface responsiveness but limits playback of material in original quality.

**Recommended requirements (full resolution with smooth shuttling):**
- Processor: Intel Core i7 6th generation or newer (or AMD Ryzen 5 3600+)
- Memory: 16 GB RAM
- Video card: discrete GPU with H.264/H.265 hardware decoding support
- For macOS on Intel: system with Videotoolbox and hybrid decoding support (CPU + GPU)
- For macOS on Apple Silicon (M1/M2/M3): built-in media engine provides full hardware decoding

On the recommended configuration, the application ensures smooth operation of both decoders at full resolution. Tested on the following systems:

- **MacBook Pro 2016 (Intel Core i7, integrated + discrete GPU):** uses a hybrid approach — part of the decoding is performed by the CPU, part is offloaded to the Videotoolbox hardware decoder.
- **iMac M1 (Apple Silicon):** uses exclusively hardware decoding via Videotoolbox, without CPU involvement. The built-in media engine ensures timely frame decoding by both decoders.

--- 

## License

TapeXPlayer is distributed under the **GNU General Public License version 3.0 (GPL-3.0)**.

The choice of GPL-3.0 is driven by the use of the FFmpeg library with GPL components (libx264 for H.264, specific codecs and filters). Under the terms of the GPL, software that uses GPL libraries must be distributed under the same license.


### FFmpeg

**License:** LGPL 2.1+ / GPL 2.0+ (depending on configuration)
**Usage:** Decoding video and audio streams, working with file containers
**Website:** https://ffmpeg.org/

Components used:
- **libavformat**: demultiplexing containers (MP4, MOV, MKV, AVI)
- **libavcodec**: video decoding (H.264, H.265, ProRes, DNxHD) and audio decoding (AAC, MP3, PCM)
- **libswscale**: color space conversion (YUV ↔ RGB)
- **libswresample**: audio resampling
- **libavutil**: utility functions

Hardware decoding support: VideoToolbox (macOS), VA-API (Linux), DXVA2 (Windows).

### SDL2

**License:** zlib License
**Usage:** Cross-platform window system, rendering, input event handling
**Website:** https://www.libsdl.org/

Functionality:
- Creating and managing application windows
- Initializing graphics renderers (Metal, OpenGL, Direct3D, Vulkan)
- Rendering YUV textures
- Processing keyboard and mouse events
- Timers and time measurement

### SDL_ttf

**License:** zlib License
**Usage:** Rendering TrueType fonts for on-screen display (OSD)
**Website:** https://github.com/libsdl-org/SDL_ttf

Rasterization of timecode, speed indicators, and VU meters. The font is embedded in the binary as a byte array.

### PortAudio

**License:** MIT License
**Usage:** Audio playback through system APIs
**Website:** http://www.portaudio.com/

Audio output abstraction for CoreAudio (macOS), ALSA/PulseAudio (Linux), WASAPI/DirectSound (Windows). Registers a callback function to fill the audio buffer.

### RtMidi

**License:** MIT-style License
**Usage:** Integration with MIDI controllers
**Website:** https://github.com/thestk/rtmidi

Support for the Mackie Control protocol for controlling playback via physical faders and buttons. Abstraction for CoreMIDI (macOS), ALSA (Linux), WinMM (Windows).

### Platform-Specific APIs

#### macOS

- **Cocoa**: native dialogs (`NSOpenPanel`)
- **VideoToolbox**: hardware video decoding
- **CoreAudio**: audio output (via PortAudio)
- **CoreMIDI**: MIDI devices (via RtMidi)
- **Metal**: graphics API (via SDL2)

**License:** Proprietary Apple frameworks

#### Linux

- **GTK+ 3** (LGPL 2.1+): native dialogs, context menus
- **X11/Wayland**: window system (via SDL2)
- **VA-API**: hardware video decoding
- **ALSA/PulseAudio**: audio subsystem (via PortAudio)

#### Windows

- **Win32 API**: native dialogs (IFileOpenDialog), window integration
- **Direct3D 11/12**: graphics API (via SDL2)
- **DXVA2**: hardware video decoding
- **WASAPI**: audio subsystem (via PortAudio)

**License:** Proprietary Microsoft APIs

---

## Links

- **GitHub**: [github.com/ffbsoffa/TapeXPlayer](https://github.com/ffbsoffa/TapeXPlayer)
- **Issues**: [Report a bug](https://github.com/ffbsoffa/TapeXPlayer/issues)
- **Releases**: [Download latest version](https://github.com/ffbsoffa/TapeXPlayer/releases)
  

---
## Acknowledgments

Special thanks to:
- The FFmpeg team for their incredible codec library
- SDL2 developers for the cross-platform framework
- The open-source community for inspiration and support

---

<div align="center">

[⭐ Star this project](https://github.com/ffbsoffa/TapeXPlayer) if you find it useful!

</div>


---

© 2026 Maksim Maloletkin. Licensed under GPL v3.0.
