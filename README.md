# TapeXPlayer

**TapeXPlayer** is experimental video player representing a software implementation of professional videotape recorder approach in digital format. The project arose from interest in how the logic of Betacam format videotape recorders can be embodied in software code. 

<img width="1392" height="860" alt="TapeXPlayer_Instance_#1_alisa_soundedit3_mov_2025_10_09_01_58_58" src="https://github.com/user-attachments/assets/241ac645-8536-4d18-9547-f4495213bb54" />

 
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform](https://img.shields.io/badge/platform-macOS%20%7C%20Linux%20%7C%20Windows-lightgrey)]()
[![Architecture](https://img.shields.io/badge/arch-Universal%20Binary%20(x86__64%20%2B%20arm64)-brightgreen)]()

---

## ✨ Key Features

TapeXPlayer is built with **C**,**C++**, **Objective-C**, **Swift** using industry-standard libraries: **FFmpeg**, **SDL2**, **PortAudio**, and **RtMidi**. It brings professional tape-based video player functionality to modern computers, enabling thorough video sequence examination with frame-accurate control.

- 🎬 **Smooth Shuttle Control**: Forward/backward playback up to 32x speed with minimal CPU usage
- 🎯 **Frame-Accurate Seeking**: Timecode-based navigation (HH:MM:SS:FF)
- 🎛️ **MIDI Controller Support**: Full integration with Mackie HUI protocol (tested with Behringer X-Touch One)
- 📍 **Memory Locations**: Quick navigation to important points with zoom recall
- 🎨 **Real-Time Zoom & Pan**: Mouse-based zoom with thumbnail preview
- 📸 **Screenshot Capture**: Export frames with timecode overlay
- 🚀 **Hardware Acceleration**: VideoToolbox (macOS), with Metal and FFmpeg fallbacks
- 💾 **Smart Caching**: Low-resolution proxy for smooth scrubbing
- 📊 **Performance**: ~52% CPU @ 32x shuttle, ~11-32% CPU @ 1x playback

---
## 🖥️ Platform Support

| Platform | Status | Architecture | Build |
|----------|--------|--------------|-------|
| **macOS** | ✅ Available | Universal Binary (Intel + Apple Silicon) | Build 1223 |
| **Linux** | ✅ Available (DEB) | x86_64 | Build 1179 |
| **Windows** | 🚧 In Development | x86_64 | - |

---

## 📦 Installation

### macOS

#### Method 1: Automatic Installation (Recommended)

1. Download the latest release from [Releases](https://github.com/ffbsoffa/TapeXPlayer/releases)
2. Extract the archive
3. Double-click **`install.command`**
4. The app will automatically install to `/Applications`
5. Launch from Launchpad or `/Applications`

#### Method 2: Manual Installation via Terminal

```bash
# Navigate to the extracted folder
cd /path/to/TapeXPlayer

# Remove quarantine attribute
xattr -cr TapeXPlayer.app

# Optional: Move to Applications
mv TapeXPlayer.app /Applications/

# Launch
open /Applications/TapeXPlayer.app
```

#### Method 3: System Preferences

1. Try to launch `TapeXPlayer.app`
2. macOS will show a security warning
3. Open **System Preferences** → **Security & Privacy**
4. Click **"Open Anyway"** next to TapeXPlayer
5. Confirm to open the application

> ⚠️ **Important**: macOS will show a security warning on first launch because the app is not notarized by Apple. This is normal for open-source projects distributed via GitHub. The app is safe to use.

---

## 💻 System Requirements

### macOS
- **OS**: macOS 13.0 Ventura or later
- **CPU**: Apple Silicon (M1/M2/M3) or Intel processor
- **RAM**: 4 GB minimum, 8 GB recommended
- **GPU**: Metal-capable graphics

### Linux (Coming Soon)
- Modern Linux distribution (Ubuntu 20.04+, Fedora 35+, etc.)
- x86_64 or arm64 processor
- 4 GB RAM minimum

### Windows (Coming Soon)
- Windows 10 or later
- x86_64 processor
- 4 GB RAM minimum

---

## 📹 Supported Formats

### Video Codecs
- **Containers**: MP4, MOV, AVI, MKV, WebM
- **Codecs**: H.264, H.265/HEVC, ProRes, DNxHD, VP9, AV1
- All formats supported by FFmpeg

### Audio Codecs
- **Formats**: MP3, WAV, FLAC, AAC, M4A, OGG, Opus
- **Channels**: Stereo and multi-channel audio

---

## 🎮 Controls & Shortcuts

### Playback Control

| Shortcut | Action |
|----------|--------|
| `Space` | Play/Pause (smart logic) |
| `P` | Play |
| `S` | Stop |
| `R` | Toggle reverse playback |

### Speed Control

| Shortcut | Action |
|----------|--------|
| `↑` / `↓` | Speed step up/down (1x→3x→10x→18x→24x→32x) |
| `1` | Set speed to 1x |
| `2` | Set speed to 3x |
| `+` / `-` | Fine speed adjustment |

### Navigation

| Shortcut | Action |
|----------|--------|
| `←` / `→` | Seek -10s / +10s |
| `Shift+←` / `Shift+→` | Seek -1 minute / +1 minute |
| `Home` / `End` | Go to start / end |
| `Cmd+G` or `NumPad *` | Timecode seek mode |
| `Return` | Create Memory Location |

### Zoom & Pan

| Shortcut | Action |
|----------|--------|
| `Cmd+Z` | Zoom In |
| `Shift+Z` | Zoom Out |
| `X` | Reset Zoom |
| `V` | Toggle thumbnail preview |
| `Mouse Wheel` | Zoom in/out |
| `Shift+Alt+Drag` | Pan zoom area |

### Shuttle Control

| Action | Control |
|--------|---------|
| **Mouse Shuttle** | `Shift+Ctrl+Drag` left/right |
| **Speed Range** | -32x to +32x |

### File & Windows

| Shortcut | Action |
|----------|--------|
| `Cmd+O` | Open file |
| `Shift+Cmd+O` | Open in new instance |
| `Cmd+N` | New window |
| `Cmd+W` | Close window |
| `Cmd+M` | Minimize window |

### Tools

| Shortcut | Action |
|----------|--------|
| `Cmd+C` | Take screenshot with timecode |
| `Cmd+,` | Settings (Preferences) |
| `Cmd+I` | Show inspector |
| `Shift+Cmd+M` | Memory Locations |
| `T` | Toggle timecode/frame display |
| `I` | Show info (console) |
| `Cmd+Q` | Quit |

> 📖 Full shortcuts reference available in `FSTPKeyboard.cpp` in the repository

---

## ⚙️ Features Deep Dive

### Playback Engine
- **Frame-Accurate Playback**: Professional-grade accuracy for scientific analysis
- **Hardware Acceleration**: Automatic selection of best backend (VideoToolbox → Metal → FFmpeg)
- **Variable Speed**: 0.1x to 32x in both directions
- **Smart Proxy Caching**: Automatic low-resolution proxy generation for smooth scrubbing
- **Multiple Instances**: Run several players simultaneously

### Audio System
- **Configurable Device**: Select any audio output device
- **Buffer Size Control**: Adjust for latency vs. stability (128 - 4096 samples)
- **Master Volume**: Global volume control with VU meters
- **Multi-Channel**: Full support for surround sound formats
- **Sample Rate**: Automatic conversion to match device

### Memory Locations
- **Quick Navigation**: Store up to 999 positions
- **Zoom Recall**: Automatically restore zoom settings
- **Timecode Display**: HH:MM:SS:FF format
- **Import/Export**: Save and load location sets
- **Comments**: Add notes to each location

### MIDI Control (Mackie HUI Protocol)
- **Transport Control**: Play, Stop, Rewind, Fast Forward
- **Jog/Shuttle**: Frame-accurate control via hardware
- **VU Meters**: Real-time audio level display on controller
- **Timecode Display**: Show current position on controller screen
- **Button Feedback**: LED feedback for transport state

**Tested Controllers**:
- ✅ Behringer X-Touch One
- ✅ Any Mackie HUI-compatible controller

**Setup**:
1. Open Settings (`Cmd+,`) → MIDI tab
2. Enable "Enable MIDI Controller"
3. Select Input/Output ports for your device
4. Configure controller to HUI/Mackie mode

### Settings

**Audio Tab**:
- Audio device selection
- Buffer size (128 - 4096 samples)
- Master volume control

**Video & Sync Tab**:
- Frame offset (-10 to +10 frames) for monitor sync compensation
- Auto-freeze inactive players (resource management)

**MIDI Tab**:
- Enable/disable MIDI controller
- Input/Output port selection

> ⚠️ **Note**: Changes to audio buffer size require app restart

---

## 🛠️ Building from Source

### Prerequisites

**macOS**:
```bash
# Install Homebrew
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install dependencies
brew install sdl2 sdl2_ttf portaudio ffmpeg openssl rtmidi
```

**Linux** (Coming Soon):
```bash
# Ubuntu/Debian
sudo apt install build-essential libsdl2-dev libsdl2-ttf-dev \
                 libportaudio2 libavcodec-dev libavformat-dev \
                 libavutil-dev libswscale-dev libssl-dev librtmidi-dev

# Fedora
sudo dnf install SDL2-devel SDL2_ttf-devel portaudio-devel \
                 ffmpeg-devel openssl-devel rtmidi-devel
```

### Build Instructions

```bash
# Clone repository
git clone https://github.com/ffbsoffa/TapeXPlayer.git
cd TapeXPlayer/source

# Build executable (macOS)
make -j8 CODE_NAME=Albatross

# Create app bundle
make bundle

# Create Universal Binary (Intel + Apple Silicon)
make bundle-universal

# Run
open ../builds/TapeXPlayer.app
```

### Build Targets

| Target | Description |
|--------|-------------|
| `make` | Build executable |
| `make bundle` | Create self-contained .app bundle (current architecture) |
| `make bundle-universal` | Create Universal Binary + auto-install to /Applications |
| `make clean` | Clean build artifacts |
| `make rebuild` | Clean and rebuild |

---

## 🐛 Troubleshooting

### macOS Issues

**Q: "TapeXPlayer cannot be opened because the developer cannot be verified"**

**A**: Use `install.command` or run:
```bash
xattr -cr TapeXPlayer.app
```

**Q: Application doesn't launch after installation**

**A**: Make sure you executed `xattr -cr` before first launch.

**Q: Video doesn't play**

**A**: Check if the codec is supported. TapeXPlayer supports all standard formats via FFmpeg. Try with a known working format like H.264/MP4.

**Q: Low performance / stuttering**

**A**:
1. Open Settings → Video & Sync
2. The app automatically creates low-resolution proxies for smooth scrubbing
3. Wait for proxy generation to complete (shown in console)
4. Adjust frame offset if needed

**Q: How to connect MIDI controller?**

**A**:
1. Open Settings (`Cmd+,`) → MIDI tab
2. Check "Enable MIDI Controller"
3. Select Input/Output ports for your device
4. Configure controller to HUI/Mackie mode
5. Supported: Mackie HUI / X-Touch One

**Q: How to sync video with external monitor?**

**A**: Settings → Video & Sync → Frame Offset. Use -10 to +10 frames to compensate for monitor delay.

**Q: Changed Buffer Size in settings but no effect**

**A**: Audio buffer size changes require application restart.

---

## 📚 Documentation

- [Build Instructions](docs/BUILDING.md) *(Coming Soon)*
- [MIDI Protocol Reference](docs/MIDI.md) *(Coming Soon)*
- [API Documentation](docs/API.md) *(Coming Soon)*

---

## 🤝 Contributing

Contributions are welcome! Please feel free to submit a Pull Request. For major changes, please open an issue first to discuss what you would like to change.

### Development

```bash
# Clone and build
git clone https://github.com/ffbsoffa/TapeXPlayer.git
cd TapeXPlayer/source
make -j8 CODE_NAME=Albatross

# Run tests
make test
make test-run

# Clean build
make clean
```

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

## 🔗 Links

- **Website**: [ffbsoffa.org](https://ffbsoffa.org)
- **GitHub**: [github.com/ffbsoffa/TapeXPlayer](https://github.com/ffbsoffa/TapeXPlayer)
- **Issues**: [Report a bug](https://github.com/ffbsoffa/TapeXPlayer/issues)
- **Releases**: [Download latest version](https://github.com/ffbsoffa/TapeXPlayer/releases)

---

## 👨‍💻 Author

**Maksim Maloletkin (FFB_soffa)**

© 2025 Maksim Maloletkin. Licensed under GPL v3.0.

---

## 📖 Academic Publication

The prototype of TapeXPlayer was used in academic research:

> Maloletkin M.F. 'Brave Ephemeral World'. Reflections on Non-Linearity in Screen Arts. *Hudozhestvennaya kul'tura* [Art & Culture Studies], 2025, no. 2, pp. 414–447. https://doi.org/10.51678/2226-0072-2025-2-414-447. (In Russian)

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
