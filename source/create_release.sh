#!/bin/bash
# create_release.sh - Create release archive for GitHub
# Usage: ./create_release.sh <build> <codename>

set -e

BUILD="$1"
CODENAME="$2"

if [ -z "$BUILD" ]; then
    echo "Usage: $0 <build> <codename>"
    echo "Example: $0 1020 Albatross"
    exit 1
fi

APP_PATH="../builds/TapeXPlayer.app"
if [ ! -d "$APP_PATH" ]; then
    echo "❌ Error: $APP_PATH not found"
    echo "First run: make bundle"
    exit 1
fi

# Determine release name
if [ -n "$CODENAME" ]; then
    RELEASE_NAME="TapeXPlayer-${CODENAME}-build${BUILD}"
else
    RELEASE_NAME="TapeXPlayer-build${BUILD}"
fi

RELEASE_DIR="../builds/release"
RELEASE_CONTENT="$RELEASE_DIR/$RELEASE_NAME"
RELEASE_ZIP="../builds/${RELEASE_NAME}.zip"

echo "📦 Creating GitHub Release: $RELEASE_NAME"
echo ""

# Clean and create folder
rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_CONTENT"

# Copy application
echo "1️⃣  Copying TapeXPlayer.app..."
cp -R "$APP_PATH" "$RELEASE_CONTENT/"

# Create README
echo "2️⃣  Generating README.txt..."
cat > "$RELEASE_CONTENT/README.txt" << EOF
================================================================================
                    TapeXPlayer "${CODENAME}"
                            Build ${BUILD}
================================================================================

Professional Video Player for macOS
macOS 13.0 Ventura or later required
Universal Binary (Intel + Apple Silicon)

================================================================================
INSTALLATION / УСТАНОВКА
================================================================================

⚠️  ВАЖНО! macOS покажет предупреждение при первом запуске, так как
    приложение не нотаризовано Apple. Это нормально для open-source
    проектов распространяемых через GitHub.

СПОСОБ 1 (Рекомендуется) - Используйте install.command:
─────────────────────────────────────────────────────────
1. Двойной клик на "install.command"
2. Приложение автоматически установится в /Applications
3. Готово! Запускайте из Launchpad или /Applications

СПОСОБ 2 - Ручная установка через Terminal:
─────────────────────────────────────────────────────────
1. Откройте Terminal (Finder → Applications → Utilities → Terminal)
2. Перейдите в папку со скачанным архивом
3. Выполните команду:

   xattr -cr TapeXPlayer.app

4. Переместите TapeXPlayer.app в /Applications (опционально)
5. Запустите приложение

СПОСОБ 3 - Через System Preferences:
─────────────────────────────────────────────────────────
1. Попробуйте запустить TapeXPlayer.app
2. Система покажет предупреждение
3. Откройте: System Preferences → Security & Privacy
4. Нажмите "Open Anyway" рядом с TapeXPlayer
5. Подтвердите открытие

================================================================================
SYSTEM REQUIREMENTS / СИСТЕМНЫЕ ТРЕБОВАНИЯ
================================================================================

• macOS 13.0 Ventura or later
• Apple Silicon (M1/M2/M3) or Intel processor
• 4 GB RAM minimum, 8 GB recommended
• Metal-capable GPU

================================================================================
SUPPORTED FORMATS / ПОДДЕРЖИВАЕМЫЕ ФОРМАТЫ
================================================================================

Видео / Video:
  • MP4, MOV, AVI, MKV
  • H.264, H.265/HEVC, ProRes, DNxHD
  • Все форматы поддерживаемые FFmpeg

Аудио / Audio:
  • MP3, WAV, FLAC, AAC, M4A, OGG
  • Multi-channel audio support

================================================================================
FEATURES / ВОЗМОЖНОСТИ
================================================================================

Воспроизведение / Playback:
  • Professional frame-accurate video playback
  • Hardware acceleration (VideoToolbox, Metal, FFmpeg)
  • Variable speed: 0.1x - 32x (forward/reverse)
  • Low-resolution proxy caching for smooth scrubbing
  • Multiple player instances support

Управление / Control:
  • Timecode-based seeking (HH:MM:SS:FF)
  • Mouse shuttle control (Shift+Ctrl+Drag)
  • Real-time zoom and pan with mouse
  • Memory Locations for quick navigation
  • MIDI controller support (Mackie HUI / X-Touch One)

Аудио / Audio:
  • Configurable audio device and buffer size
  • Master volume control
  • VU meters for audio level monitoring
  • Multi-channel audio support

Настройки / Settings (Cmd+,):
  • Audio: Device selection, buffer size, master volume
  • Video & Sync: Frame offset (-10 to +10 frames)
  • MIDI: Enable/disable, input/output port selection
  • Auto-freeze inactive players (resource management)

Дополнительно / Additional:
  • Screenshot capture with timecode overlay (Cmd+C)
  • Inspector window for file information
  • Native macOS menu integration
  • SwiftUI-based About and Settings dialogs

================================================================================
SHORTCUTS / ГОРЯЧИЕ КЛАВИШИ
================================================================================

Воспроизведение / Playback:
  Space          Play/Pause (smart logic)
  P              Play
  S              Stop
  R              Reverse toggle

Скорость / Speed:
  ↑ / ↓          Speed step up/down (1x→3x→10x→18x→24x→32x)
  1              Set speed to 1x
  2              Set speed to 3x
  + / -          Fine speed adjustment

Навигация / Navigation:
  ← / →          Seek -10s / +10s (Shift: -1min / +1min)
  Home / End     Go to start / end
  Cmd+G          Timecode seek mode
  NumPad *       Timecode seek mode
  Return         Create Memory Location

Зум / Zoom:
  Cmd+Z          Zoom In
  Shift+Z        Zoom Out
  X              Reset Zoom
  V              Toggle thumbnail
  Mouse Wheel    Zoom in/out
  Shift+Alt+Drag Pan zoom area

Файл и окна / File & Windows:
  Cmd+O          Open file
  Shift+Cmd+O    Open in new instance
  Cmd+N          New window
  Shift+Cmd+N    New window (from Window menu)
  Cmd+W          Close window
  Cmd+M          Minimize window

Инструменты / Tools:
  Cmd+C          Take screenshot
  Cmd+,          Settings (Preferences)
  Cmd+I          Show inspector
  Shift+Cmd+M    Memory Locations
  T              Toggle timecode/frame display
  I              Show info (console)
  Cmd+Q          Quit

Full shortcuts reference: см. FSTPKeyboard.cpp в репозитории

================================================================================
TROUBLESHOOTING / РЕШЕНИЕ ПРОБЛЕМ
================================================================================

Q: "TapeXPlayer cannot be opened because the developer cannot be verified"
A: Используйте install.command или выполните: xattr -cr TapeXPlayer.app

Q: Приложение не запускается после установки
A: Убедитесь что выполнили xattr -cr перед первым запуском

Q: Видео не воспроизводится
A: Проверьте поддержку кодека. Приложение поддерживает все стандартные
   форматы через FFmpeg.

Q: Низкая производительность
A: Откройте Settings → Video & Sync и настройте параметры. Приложение
   автоматически создаёт low-resolution proxy для плавного скроббинга.

Q: Как подключить MIDI контроллер?
A: 1. Откройте Settings (Cmd+,) → вкладка MIDI
   2. Установите галочку "Enable MIDI Controller"
   3. Выберите Input/Output порты для вашего устройства
   4. Поддерживается: Mackie HUI / X-Touch One
   5. Настройте контроллер в режим HUI/Mackie

Q: Как настроить синхронизацию видео с внешним монитором?
A: Settings → Video & Sync → Frame Offset
   Используйте -10 to +10 frames для компенсации задержки монитора

Q: Изменил Buffer Size в настройках, но эффекта нет
A: Изменения размера аудио буфера требуют перезапуска приложения

================================================================================
LINKS / ССЫЛКИ
================================================================================

• GitHub: https://github.com/ffbsoffa/tapexplayer
• Issues: https://github.com/ffbsoffa/tapexplayer/issues
• Documentation: See BUILD_INSTRUCTIONS.md in source
• Keyboard shortcuts: source/modules/FSTPMainModule/WSGUI/FSTPKeyboard.cpp

================================================================================
LICENSE / ЛИЦЕНЗИЯ
================================================================================

Open Source - см. LICENSE файл в репозитории

================================================================================
                        Thank you for using TapeXPlayer!
================================================================================

Build Date: $(date +"%B %d, %Y")
Build Number: ${BUILD}
Architecture: Universal (x86_64 + arm64)

EOF

# Create install script
echo "3️⃣  Creating install.command..."
cat > "$RELEASE_CONTENT/install.command" << 'EOF'
#!/bin/bash
# TapeXPlayer Installer
# Automatic installation with quarantine attribute removal

clear
echo "════════════════════════════════════════════════════════════════"
echo "           TapeXPlayer Installer for macOS"
echo "════════════════════════════════════════════════════════════════"
echo ""

# Determine script path
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
APP_PATH="$SCRIPT_DIR/TapeXPlayer.app"

if [ ! -d "$APP_PATH" ]; then
    echo "❌ Error: TapeXPlayer.app not found in current folder"
    echo ""
    read -p "Press Enter to exit..."
    exit 1
fi

echo "1️⃣  Removing quarantine attributes..."
xattr -cr "$APP_PATH"
echo "   ✅ Done"
echo ""

echo "2️⃣  Installing to /Applications..."
if [ -d "/Applications/TapeXPlayer.app" ]; then
    echo "   ⚠️  TapeXPlayer is already installed in /Applications"
    read -p "   Replace existing version? (y/n): " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        echo "   ⏭  Installation cancelled"
        echo ""
        echo "Application is ready to use from current folder."
        read -p "Press Enter to exit..."
        exit 0
    fi
    rm -rf "/Applications/TapeXPlayer.app"
fi

cp -R "$APP_PATH" "/Applications/"
echo "   ✅ Done"
echo ""

echo "════════════════════════════════════════════════════════════════"
echo "           ✅ Installation completed successfully!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "TapeXPlayer installed to /Applications"
echo ""
echo "Launch the application:"
echo "  • From Launchpad"
echo "  • From Finder → Applications"
echo "  • Or run: open -a TapeXPlayer"
echo ""
read -p "Press Enter to exit..."
EOF

chmod +x "$RELEASE_CONTENT/install.command"

# Create uninstall script
echo "4️⃣  Creating uninstall.command..."
cat > "$RELEASE_CONTENT/uninstall.command" << 'EOF'
#!/bin/bash
# TapeXPlayer Uninstaller

clear
echo "════════════════════════════════════════════════════════════════"
echo "           TapeXPlayer Uninstaller"
echo "════════════════════════════════════════════════════════════════"
echo ""

if [ ! -d "/Applications/TapeXPlayer.app" ]; then
    echo "TapeXPlayer is not installed in /Applications"
    echo ""
    read -p "Press Enter to exit..."
    exit 0
fi

echo "⚠️  This will remove TapeXPlayer from /Applications"
echo ""
read -p "Continue? (y/n): " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Uninstallation cancelled"
    read -p "Press Enter to exit..."
    exit 0
fi

echo ""
echo "Removing application..."
rm -rf "/Applications/TapeXPlayer.app"
echo "✅ TapeXPlayer removed"
echo ""

echo "Remove settings and cache? (~/Library/Preferences and ~/.fstp)"
read -p "(y/n): " -n 1 -r
echo
if [[ $REPLY =~ ^[Yy]$ ]]; then
    rm -f ~/Library/Preferences/com.tapexplayer.settings
    rm -rf ~/.fstp
    echo "✅ Settings and cache removed"
fi

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "           ✅ Uninstallation complete"
echo "════════════════════════════════════════════════════════════════"
echo ""
read -p "Press Enter to exit..."
EOF

chmod +x "$RELEASE_CONTENT/uninstall.command"

# Create ZIP
echo "5️⃣  Creating ZIP archive..."
rm -f "$RELEASE_ZIP"
cd "$RELEASE_DIR"
zip -r -q "../../$(basename "$RELEASE_ZIP")" "$(basename "$RELEASE_CONTENT")"
cd - > /dev/null

# Cleanup
rm -rf "$RELEASE_DIR"

echo ""
echo "════════════════════════════════════════════════════════════════"
echo "✅ Release created successfully!"
echo "════════════════════════════════════════════════════════════════"
echo ""
echo "File: $RELEASE_ZIP"
echo "Size: $(du -h "$RELEASE_ZIP" | cut -f1)"
echo ""
echo "Contents:"
echo "  • TapeXPlayer.app (Universal Binary)"
echo "  • README.txt (bilingual instructions)"
echo "  • install.command (automatic installation)"
echo "  • uninstall.command (removal)"
echo ""
echo "Ready to upload to GitHub Releases! 🚀"
echo ""
