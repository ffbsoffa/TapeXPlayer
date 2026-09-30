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

TapeXPlayer builds with a plain `makefile` (no CMake) from the `source/` directory, the same way on all three platforms. The CI workflows in [`.github/workflows/`](.github/workflows) build every platform on each push; if this guide and CI ever disagree, CI is right.

### Quick start

```bash
git clone https://github.com/ffbsoffa/TapeXPlayer.git
cd TapeXPlayer/source
make check        # checks the compiler and libraries, prints the install command for anything missing
make -j8          # builds; runs the same check first
make run          # starts the player
```

`make check` output when something is missing:

```
Checking build dependencies (linux)...
  ✓ C++23        g++ (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0
  ✓ FFmpeg       libavcodec 60.31.102
  ✗ SDL2_ttf     not found → sudo apt install libsdl2-ttf-dev
  ✓ PortAudio    19
  ...
Can't build yet.
Install the missing packages:

  sudo apt install libsdl2-ttf-dev
```

Run the printed command, then `make` again. The per-platform steps below list everything up front.

### macOS

Tested: macOS 15 on Apple Silicon (CI). The finished app runs on macOS 13+; the universal app also runs on Intel Macs.

1. **Xcode 16 or newer** (needs macOS 14.5+). The compiler must accept `-std=c++23`; older Xcode does not.
   ```bash
   xcode-select --install                          # command line tools, if missing
   sudo xcode-select -s /Applications/Xcode.app    # if several Xcodes are installed
   ```
2. **[Homebrew](https://brew.sh)** and the libraries:
   ```bash
   brew install ffmpeg sdl2 sdl2_ttf portaudio openssl@3 rtmidi
   brew install lua          # optional: turns on Lua extensions
   ```
3. **Build and run:**
   ```bash
   make -j$(sysctl -n hw.ncpu)    # → ../builds/binaries/TapeXPlayer
   make run
   ```
4. **Make an app** (optional):
   ```bash
   make bundle      # → ../builds/TapeXPlayer.app, libraries copied inside, ad-hoc signed
   ```

**Universal (arm64 + Intel) app.** Homebrew no longer supports Intel Macs, so the x86_64 libraries are built from source once (about 7 minutes on an M1), then `bundle-universal` uses them:
```bash
../.github/scripts/build-macos-x86_64-deps.sh ~/x86_64-deps    # needs: brew install nasm meson ninja cmake
make bundle-universal X86_64_PREFIX=~/x86_64-deps SKIP_INSTALL=1
```
Without `SKIP_INSTALL=1` the result is also copied to `/Applications`.

### Linux

Tested: Ubuntu 24.04 x86_64 (CI). Also expected to work on Ubuntu 22.04+ / Debian 12+ and on arm64.

1. **Compiler and libraries.** GCC 11 or newer is needed for C++23.

   Debian / Ubuntu:
   ```bash
   sudo apt update
   sudo apt install build-essential pkg-config \
       libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev libswscale-dev libswresample-dev \
       libsdl2-dev libsdl2-ttf-dev portaudio19-dev libssl-dev librtmidi-dev libgtk-3-dev
   ```
   Fedora (not tested in CI; `ffmpeg-free` has no H.264/HEVC decoding, use `ffmpeg-devel` from RPM Fusion for that):
   ```bash
   sudo dnf install gcc-c++ make pkgconf-pkg-config SDL2-devel SDL2_ttf-devel portaudio-devel \
       ffmpeg-free-devel rtmidi-devel openssl-devel gtk3-devel
   ```
   Arch (not tested in CI):
   ```bash
   sudo pacman -S --needed base-devel pkgconf sdl2 sdl2_ttf portaudio ffmpeg rtmidi openssl gtk3
   ```
   Elsewhere: the development packages of FFmpeg, SDL2, SDL2_ttf, PortAudio, RtMidi, OpenSSL and GTK 3, plus `pkg-config`. `make check` finds them through `pkg-config`.
2. **Build and run:**
   ```bash
   make -j$(nproc)     # → ../builds/binaries/TapeXPlayer_linux
   make run
   ```
3. **Make a package** (optional, Debian/Ubuntu):
   ```bash
   make deb            # → ../builds/binaries/deb/tapexplayer_<version>.<build>_<arch>.deb
   make install-deb    # builds the .deb and installs it with dpkg
   ```

### Windows

Tested: GitHub's `windows-latest` runner, x64 (CI). Windows builds use [MSYS2](https://www.msys2.org) with the MinGW-w64 toolchain.

1. **Install MSYS2** from [msys2.org](https://www.msys2.org), then open the **MSYS2 MINGW64** shell from the Start Menu. Only this shell works: MSYS, UCRT64 and CLANG64 use different library folders, and the makefile links against `/mingw64`.
2. **Quickest way:** the bootstrap script installs the toolchain and libraries, clones the repository, builds and bundles the DLLs.
   ```bash
   curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/stable/source/bootstrap_win.sh -o bootstrap_win.sh
   bash bootstrap_win.sh
   ```
   If `pacman` closes the window while updating, open **MSYS2 MINGW64** again and re-run the script.
3. **Or by hand:**
   ```bash
   pacman -Syu
   pacman -S --needed make git zip \
       mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_ttf \
       mingw-w64-x86_64-portaudio mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-openssl \
       mingw-w64-x86_64-rtmidi mingw-w64-x86_64-nsis
   git clone https://github.com/ffbsoffa/TapeXPlayer.git
   cd TapeXPlayer/source
   make -j$(nproc)                 # → ../builds/binaries/TapeXPlayer.exe
   ```
4. **Make it run outside MSYS2.** The `.exe` needs its DLLs next to it:
   ```bash
   ./bundle_dlls.sh                # → ../builds/win-bundle/ (TapeXPlayer.exe + every DLL)
   ./create_win_installer.sh x64   # optional → ../builds/TapeXPlayer-Setup-x64-build<N>.exe
   ```

### make reference

| Command | What it does |
|---|---|
| `make` | Check dependencies, build the executable |
| `make check` | Only the dependency check |
| `make run` | Build and start |
| `make clean` / `make rebuild` | Delete build files / delete and build again |
| `make help` | List the targets for this platform |
| `make bundle` | macOS: self-contained `.app` |
| `make bundle-universal` | macOS: arm64 + x86_64 `.app` (see above) |
| `make deb` / `make install-deb` | Linux: `.deb` package / build and install it |
| `make win-release` | Windows: zip of `../builds/win-bundle/` |

| Option | Effect |
|---|---|
| `FREEZE_BUILD_NUMBER=1` | Keep `source/.build_number` as is (every other `make` increments it) |
| `SKIP_CHECK=1` | Skip the dependency check |
| `CXX=g++-13` | Use another compiler |
| `SKIP_INSTALL=1` | `bundle-universal`: don't copy to `/Applications` |
| `X86_64_PREFIX=<dir>` | `bundle-universal`: where the x86_64 libraries are |

### Troubleshooting

**`make check` shows ✗** — run the install command it prints, then `make` again.

**`C++23 ✗` or `unrecognized command-line option '-std=c++23'`** — the compiler is too old. macOS: install Xcode 16+ and select it with `xcode-select -s`. Linux: install GCC 11+ (`sudo apt install g++-13`) and build with `make CXX=g++-13`. Windows: `pacman -Syu`.

**Random crashes or heap corruption after `git pull`, switching branches or editing a `.h` file** — the makefile does not track header dependencies, so old object files survive. Run `make clean && make`.

**`git status` shows `.build_number` and `BuildInfo` files as changed** — `make` stamps the build number into them. Build with `FREEZE_BUILD_NUMBER=1` or restore them with `git checkout -- source/.build_number source/modules/FSTPMainModule/WSGUI/*/BuildInfo.* source/modules/FSTPMainModule/WSGUI/darwin/sdl/BuildInfo.swift`.

**macOS: `Killed: 9` when starting a copied binary** — copying breaks the ad-hoc signature. Re-sign it: `codesign -s - --force <path to TapeXPlayer>`.

**macOS: a bundle you built crashes on another Mac with an SDL3 error** — Homebrew's `sdl2` is `sdl2-compat`, which needs SDL3 at run time. Fine on your own Mac; for an app you give to others, build real SDL2 as [`macos-build.yml`](.github/workflows/macos-build.yml) does.

**Windows: `Wrong MSYS2 shell`** — close it and open **MSYS2 MINGW64**.

**Windows: `libSDL2.dll was not found` when starting the `.exe` from Explorer** — run `./bundle_dlls.sh` and start `../builds/win-bundle/TapeXPlayer.exe`.

**Linux: the release `.deb` won't install (unmet dependencies)** — it is built on Ubuntu 24.04; on older systems build from source.

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
