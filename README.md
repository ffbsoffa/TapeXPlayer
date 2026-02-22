# TapeXPlayer

**TapeXPlayer** is experimental video player representing a software implementation of professional videotape recorder approach in digital format. The project arose from interest in how the logic of Betacam format videotape recorders can be embodied in software code. 

<img width="1392" height="860" alt="TapeXPlayer_Instance_#1_alisa_soundedit3_mov_2025_10_09_01_58_58" src="https://github.com/user-attachments/assets/241ac645-8536-4d18-9547-f4495213bb54" />

 
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)]()
[![Architecture](https://img.shields.io/badge/arch-Universal%20Binary%20(x86__64%20%2B%20arm64)-brightgreen)]()

---

## ✨ Key Features

TapeXPlayer is built with **C**,**C++**, **Objective-C**, **Swift** using industry-standard libraries: **FFmpeg**, **SDL2**, **PortAudio**, and **RtMidi**. It brings professional tape-based video player functionality to modern computers, enabling thorough video sequence examination with frame-accurate control.

- **Smooth Shuttle Control**: Forward/backward playback up to 32x speed with minimal CPU usage
- **Frame-Accurate Seeking**: Timecode-based navigation (HH:MM:SS:FF)
- **MIDI Controller Support**: Full integration with Mackie HUI protocol (tested with Behringer X-Touch One)
- **Memory Locations**: Quick navigation to important points with zoom recall
- **Real-Time Zoom & Pan**: Mouse-based zoom with thumbnail preview
- **Screenshot Capture**: Export frames with timecode overlay
- **Hardware Acceleration**: VideoToolbox (macOS), with Metal and FFmpeg fallbacks
- **Smart Caching**: Low-resolution proxy for smooth scrubbing
- **Performance**: ~52% CPU @ 32x shuttle, ~11-32% CPU @ 1x playback

---
## 🖥️ Platform Support

| Platform | Status | Architecture | Build |
|----------|--------|--------------|-------|
| **macOS** | ✅ Available | Universal Binary (Intel + Apple Silicon) | Build 1497 |
| **Linux** | ✅ Available (DEB) | x86_64 | Build 1501 |
| **Windows** | ✅ Available | x86_64 | Build 1505 |

---

## 📦 Installation
### Method 1: Automatic Installation (Recommended)

#### macOS

The application has technical preview status and has not undergone Apple notarization yet.

On first launch, macOS Gatekeeper blocks the unsigned application. To install and bypass this restriction, use the following script:

```bash
curl -fsSL https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/refs/heads/stable/install.sh | bash
```

The script performs the following actions:
1. Downloads the latest version of TapeXPlayer from the GitHub repository
2. Extracts the archive to the `/Applications` directory
3. Removes the quarantine attribute using `xattr -d com.apple.quarantine`

After running the script, the application is ready to use.

### Windows
For automatic installation on Windows, open PowerShell and run:

```powershell
iwr -useb https://raw.githubusercontent.com/ffbsoffa/TapeXPlayer/refs/heads/stable/install.ps1 | iex
```

The script performs the following actions:
1. Downloads the latest version of TapeXPlayer from the GitHub repository
2. Extracts the archive to the `C:\Program Files\TapeXPlayer` directory
3. Creates a desktop shortcut

**Note:** On first launch, Windows Defender SmartScreen may display a warning — select "More info" followed by "Run anyway".

### Method 2: Manual Installation via Terminal

#### macOS

1. Go to the [GitHub releases page](https://github.com/ffbsoffa/TapeXPlayer/releases)
2. Download the ZIP archive with the latest release for macOS
3. Extract the archive and drag `TapeXPlayer.app` to the `/Applications` folder
4. **Important:** Remove the quarantine attribute manually via Terminal:
   ```bash
   xattr -d com.apple.quarantine /Applications/TapeXPlayer.app
   ```
5. Launch the application from the `Applications` folder

On first launch, the system may request permission. Confirm in "Security & Privacy" settings.

#### Windows

1. Download the ZIP archive from the [releases page](https://github.com/ffbsoffa/TapeXPlayer/releases)
2. Extract the archive to a convenient directory (for example, `C:\Program Files\TapeXPlayer`)
3. Run `TapeXPlayer.exe`

On first launch, Windows Defender SmartScreen may display a warning. Select "More info" followed by "Run anyway".

#### Linux

1. Download the DEB package from the [releases page](https://github.com/ffbsoffa/TapeXPlayer/releases)
2. Install the package:
   ```bash
   sudo dpkg -i tapexplayer_*.deb
   sudo apt-get install -f
   ```

---

## System Requirements

**Minimum requirements:**
- Operating system: Windows 10/11, macOS 10.14+, Linux (Ubuntu 20.04+, Fedora 34+)
- Processor: Intel Pentium Gold 7505 or equivalent with hardware decoding support (Intel QSV, AMD VCE)
- Memory: 8 GB RAM
- Video card with built-in hardware video decoder
- Free disk space: 200 MB for installation

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

## 📄 License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

### Third-Party Libraries

TapeXPlayer uses the following open-source libraries:

- **[FFmpeg](https://ffmpeg.org)** - LGPL v2.1+ / GPL v2+ - Video/audio codec library
- **[SDL2](https://www.libsdl.org)** - zlib License - Cross-platform multimedia library
- **[SDL2_ttf](https://github.com/libsdl-org/SDL_ttf)** - zlib License - TrueType font rendering
- **[PortAudio](http://www.portaudio.com)** - MIT-like License - Cross-platform audio I/O
- **[RtMidi](https://github.com/thestk/rtmidi)** - MIT-like License - Cross-platform MIDI I/O
- **[OpenSSL](https://www.openssl.org)** - Apache License 2.0 - Cryptography toolkit

**macOS Frameworks**:
- Cocoa, CoreVideo, VideoToolbox, CoreAudio, CoreMIDI, IOKit, QuartzCore, CoreFoundation, UniformTypeIdentifiers

All trademarks are property of their respective owners.

---

## Links

- **Website**: [ffbsoffa.org](https://ffbsoffa.org)
- **GitHub**: [github.com/ffbsoffa/TapeXPlayer](https://github.com/ffbsoffa/TapeXPlayer)
- **Issues**: [Report a bug](https://github.com/ffbsoffa/TapeXPlayer/issues)
- **Releases**: [Download latest version](https://github.com/ffbsoffa/TapeXPlayer/releases)


---
## 🙏 Acknowledgments

Special thanks to:
- The FFmpeg team for their incredible codec library
- SDL2 developers for the cross-platform framework
- The open-source community for inspiration and support

---

<div align="center">

[⭐ Star this project](https://github.com/ffbsoffa/TapeXPlayer) if you find it useful!

</div>


---

© 2025 Maksim Maloletkin. Licensed under GPL v3.0.
