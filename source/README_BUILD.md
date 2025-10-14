# TapeXPlayer 2026 - Universal Build System

Unified Makefile for cross-platform builds (macOS + Linux).

## Quick Start

### Linux (Ubuntu/Debian)

#### Option 1: Install from DEB package (Recommended)

```bash
# Download the latest DEB package
# or build it yourself:
make deb

# Install
sudo dpkg -i ../builds/deb/tapexplayer_*.deb
sudo apt-get install -f  # Install dependencies

# Launch from applications menu or:
tapexplayer
```

#### Option 2: Build from source

```bash
# Install dependencies
sudo apt-get install build-essential pkg-config \
    libsdl2-dev libsdl2-ttf-dev libportaudio2 libportaudio-ocaml-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev \
    libssl-dev librtmidi-dev libgtk-3-dev

# Build
make

# Run
make run

# Or create DEB package
make deb
```

### macOS (Intel + Apple Silicon)

```bash
# Install dependencies via Homebrew
brew install sdl2 sdl2_ttf portaudio ffmpeg openssl rtmidi

# Build for current architecture
make

# Create universal binary (x86_64 + arm64)
make bundle-universal

# Run
open /Applications/TapeXPlayer.app
```

## Available Commands

### Common (both platforms)

- `make` - Build executable
- `make clean` - Clean build artifacts
- `make rebuild` - Rebuild from scratch
- `make run` - Build and run
- `make help` - Show platform-specific help
- `make show-sources` - List all source files

### Linux Only

- `make deb` - Create DEB package
- `make install-deb` - Build and install DEB package
- `make uninstall` - Uninstall DEB package

### macOS Only

- `make app` - Create .app bundle
- `make bundle` - Create self-contained .app with libraries
- `make bundle-universal` - Create Universal Binary (x86_64 + arm64)

## Platform Detection

The Makefile automatically detects your platform and uses appropriate settings:

### Linux Configuration
- **Compiler**: g++
- **Optimization**: `-O2 -march=x86-64-v3 -msse2 -msse4.1 -mavx -mavx2`
- **Libraries**: SDL2, GTK+3, FFmpeg, PortAudio
- **UI**: GTK+3 dialogs, SDL2 rendering
- **Output**: `../builds/binaries/TapeXPlayer_linux`
- **Objects**: `../builds/obj_linux/`

### macOS Configuration
- **Compiler**: swiftc + g++
- **Languages**: Swift, C++, Objective-C++
- **Libraries**: SDL2, FFmpeg, PortAudio, Cocoa frameworks
- **UI**: Native SwiftUI dialogs, SDL2 rendering
- **Output**: `../builds/binaries/TapeXPlayer`
- **Objects**: `../builds/obj/`

## Build System Features

### Automatic Platform Detection
```makefile
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)
```

### Smart Source Filtering
- **Linux**: Excludes `darwin/` and `windows/` directories
- **macOS**: Includes `.mm` (Objective-C++) and `.swift` files
- **Both**: Excludes `_old.cpp` and `example_usage.cpp` files

### Architecture-Specific Builds (macOS)
- **Intel (x86_64)**: Uses `/usr/local` (Homebrew Intel)
- **Apple Silicon (arm64)**: Uses `/opt/homebrew` (Homebrew ARM)
- **Universal Binary**: Combines both architectures

## Build Output Structure

```
../builds/
├── binaries/
│   ├── TapeXPlayer           # macOS executable
│   └── TapeXPlayer_linux     # Linux executable
├── obj/                       # macOS object files
├── obj_linux/                 # Linux object files
├── deb/                       # Linux DEB packages
│   └── tapexplayer_*.deb     # Installable package
└── TapeXPlayer.app/          # macOS app bundle (optional)
    ├── Contents/
    │   ├── MacOS/
    │   ├── Resources/
    │   └── Frameworks/        # Bundled libraries
```

## Linux DEB Package

The DEB package includes:
- **Executable**: `/usr/bin/tapexplayer`
- **Desktop file**: `/usr/share/applications/tapexplayer.desktop`
- **Icons**: `/usr/share/icons/hicolor/*/apps/tapexplayer.png` (16-1024px)
- **Documentation**: `/usr/share/doc/tapexplayer/`

### Package Details
- **Package name**: tapexplayer
- **Version**: 2026.01.YYYYMMDD
- **Architecture**: amd64
- **Size**: ~4MB
- **Dependencies**: Automatically handled by apt

### Creating DEB Package
```bash
# Build executable first
make

# Create DEB package
make deb

# Output: ../builds/deb/tapexplayer_2026.01.YYYYMMDD_amd64.deb
```

### Installing DEB Package
```bash
# Install with dpkg
sudo dpkg -i ../builds/deb/tapexplayer_*.deb
sudo apt-get install -f  # Fix dependencies

# Or use the shortcut
make install-deb

# Launch from applications menu or:
tapexplayer /path/to/video.mp4
```

### Uninstalling
```bash
# Using apt (recommended)
sudo apt-get remove tapexplayer

# Or use the shortcut
make uninstall
```

## Troubleshooting

### Linux: Missing dependencies
```bash
# Check installed packages
dpkg -l | grep -E 'sdl2|ffmpeg|gtk'

# Reinstall if needed
sudo apt-get install --reinstall <package-name>
```

### macOS: Homebrew path issues
```bash
# For Apple Silicon
brew --prefix  # Should show /opt/homebrew

# For Intel
brew --prefix  # Should show /usr/local
```

### Build failures
```bash
# Clean and rebuild
make clean
make rebuild

# Check source files
make show-sources

# Verbose build (see full commands)
make -n
```

## Linux-Specific Notes

### Heap Corruption Fixes Applied ✅
1. **Video Frame Cleanup Order**: AVFrames freed before decoder contexts
2. **Pixel Buffer Management**: Buffers cleared before player shutdown
3. **GTK/SDL X11 Conflict**: Uses `_exit()` for clean shutdown

### SIMD Optimizations
- **Enabled**: SSE2, SSE4.1, AVX, AVX2
- **Disabled**: AVX-512 (causes heap corruption on Intel Celeron)
- **Architecture**: x86-64-v3 baseline

### GTK+ Integration
- **Version**: GTK+ 3.0
- **Features**: File dialogs, About dialog, Settings dialog
- **Display**: X11 (Wayland compatible)

## macOS-Specific Notes

### Build Numbers
- Auto-incremented on each build
- Stored in `.build_number`
- Displayed in About dialog

### Code Signing
- Ad-hoc signing for local builds
- Developer ID required for distribution
- Use `make bundle-universal` for release builds

### Universal Binaries
```bash
# Build both architectures
make bundle-universal

# Check architectures
lipo -info ../builds/TapeXPlayer.app/Contents/MacOS/TapeXPlayer
```

## Performance

### Parallel Builds
```bash
# Use all CPU cores
make -j$(nproc)           # Linux
make -j$(sysctl -n hw.ncpu)  # macOS
```

### Compilation Time
- **Linux**: ~30-60 seconds (31 files)
- **macOS**: ~60-120 seconds (includes Swift compilation)
- **Universal Binary**: ~3-5 minutes (builds both architectures)

## Version Information

- **Version**: 2026.01
- **Code Name**: Albatross
- **Build Date**: Auto-generated
- **Platforms**: Linux (x86_64), macOS (x86_64 + arm64)

## Contributing

When adding new files:
1. Place platform-specific code in `darwin/`, `linux/`, or `windows/` directories
2. The build system will automatically filter based on platform
3. Test on both platforms before committing

## License

See main project LICENSE file.
