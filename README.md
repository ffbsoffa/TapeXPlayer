# TapeXPlayer
**TapeXPlayer** is a video player developed as a simulation of media playback on magnetic tapes, based on the philosophy of professional Betacam video recorders. The program is designed for scientific purposes, particularly for frame-by-frame film analysis.
![TapeXPlayer 2024-10-01 05-21-52](https://github.com/user-attachments/assets/e47e35d5-a984-4cf0-928d-e46e38ca0eb0)
### Key Features
TapeXPlayer is written in C++ using the **FFmpeg**, **SDL**, **GStreamer**, and **OpenSSH 3.0** libraries. The primary goal of the program is to adapt the functionality of professional video tape players for use on modern computers. TapeXPlayer helps to thoroughly examine the video sequence, simplifying frame-by-frame analysis and playback control. 

- Smooth playback forward and backward with shuttle control up to 18x speed, with minimal CPU and memory usage
- Fast seek by timecode
- Remote control support via Mackie Control protocol (tested with Behringer X-Touch One)
- Memory Locations for quick navigation to important points
- Supports multiple video formats thanks to the **FFmpeg** library
- Minimal system resource consumption at high playback speeds (expected: ~132% CPU @ 16x shuttle, and ~27-41% CPU – playback @ 1x)

[Watch the demo video](https://www.youtube.com/watch?v=Mm4c1lp7Gz0)


### Installation

#### Prerequisites
1. Install Homebrew if not already installed:
```
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

2. For Apple Silicon (M1/M2) Macs, install required libraries:
```
brew install ffmpeg gstreamer glib sdl2 sdl2_ttf rtmidi portaudio openssl@3
```

3. For Intel Macs, run the following commands:
```
arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
arch -x86_64 /usr/local/bin/brew install ffmpeg gstreamer glib sdl2 sdl2_ttf rtmidi portaudio openssl@3
```

#### Building from Source
1. Clone the repository:
```
git clone https://github.com/ffbsoffa/TapeXPlayer.git
cd TapeXPlayer
```

2. Build the project:
```
make clean
make
```

The build process will create a Universal Binary compatible with both Apple Silicon and Intel processors.

#### Verifying the Installation
1. Check that the binary supports both architectures:
```
lipo -info TapeXPlayer
```
Should output: "Architectures in the fat file: TapeXPlayer are: x86_64 arm64"

2. Test the program:
```
./TapeXPlayer
```

### Running the Program
Run the program via the command line using the following format:
```
./TapeXPlayer <path to video file>
```
or 
```
./TapeXPlayer
```

### Important Notes
- TapeXPlayer creates low-res cached versions of the video to ensure smooth playback and seeking. The cache is saved at the following path: `/Users/<username>/Library/Caches/TapeXPlayer`. Make sure there is enough free space on the disk to store the cache. The number of cached files is limited to 4.

### Controls:
- **Spacebar** — Play/Pause
- **G** — Switch to frame seek mode based on timecode. Enter time in the following format: HHMMSSFF (hours, minutes, seconds, frames)
- **Up/Down Arrows** — Control playback speed (from normal to 16x)
- **Left Arrow** — Change playback direction (forward/backward)
- **Shift + Left/Right Arrows** — Slow-motion playback (jog mode)
- **Alt + 1-8** — Save current position as Memory Location
- **1-8** — Jump to saved Memory Location
- **Remote Control** — Basic transport and jog wheel controls via Mackie Control protocol

### System Requirements
- Mac with Apple Silicon or Intel processor (Universal Binary)
- macOS 11.0 (Big Sur) or newer
- RAM: 8 GB (minimal), 16 GB (recommended)

### Libraries and Dependencies:
- **GStreamer** — for multimedia framework support
- **PortAudio** — for audio processing module
- **FFmpeg** — for decoding and encoding video and audio
- **SDL2** and **SDL2_ttf** — for cross-platform multimedia capabilities and display
- **OpenSSH 3.0** — for file generation with unique identifiers, optimizing workload
- **RtMidi** — for MIDI communication with remote control devices
- [**Spleen Font**](https://github.com/fcambus/spleen/tree/master) — used under BSD2-ClauseLicense
- [**Native File Dialog Extended**](https://github.com/btzy/nativefiledialog-extended) - used under Zlib licence

### External API Integration
TapeXPlayer now provides an API for third-party applications, enabling:
- Remote control of playback functions
- Access to timeline and Memory Locations
- Integration with custom workflows and automation systems

### Contributions and Contact:
If you'd like to contribute to the project, feel free to reach out at **mail@ffbsoffa.org**. Contributions are welcome through pull requests.

### Platform Support:
Currently available for macOS (Universal Binary for both Apple Silicon and Intel). Windows and Linux support are under development.
