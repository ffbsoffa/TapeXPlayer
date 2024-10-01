# TapeXPlayer

**TapeXPlayer** is a video player developed as a like-simulation of media playback on magnetic tapes, based on the philosophy of professional Betacam video recorders. The program is designed for scientific purposes, particularly for frame-by-frame analysis of films.
![TapeXPlayer 2024-10-01 05-21-52](https://github.com/user-attachments/assets/e47e35d5-a984-4cf0-928d-e46e38ca0eb0)
### Key Features
TapeXPlayer is written in C++ using the **FFmpeg**, **SDL**, **GStreamer**, and **OpenSSH 3.0** libraries. Additionally, it uses the [Spleen font](https://github.com/fcambus/spleen/tree/master), distributed under the BSD2-ClauseLicense. The primary goal of the program is to adapt the functionality of professional video recorders for use on modern computers. TapeXPlayer helps to thoroughly examine the video sequence, simplifying frame-by-frame analysis and playback control. This version is intended for Mac computers running on Apple Silicon & Intel.

- Smooth playback forward and backward with shuttle control up to 16x speed, with minimal CPU and memory usage.
- Fast seek by timecode.
- Supports multiple video formats thanks to the **FFmpeg** library.
- Minimal system resource consumption at high playback speeds (expected: ~132% CPU @ 16x shuttle, and ~27-41% CPU – playback @ 1x).

[Watch the demo video](https://www.youtube.com/watch?v=Mm4c1lp7Gz0)


### Running the Program
Run the program via the command line using the following format:
```
./TapeXPlayer <path to video file>
```

Example:
```
./TapeXPlayer test.mp4
```

### Important Notes
- TapeXPlayer creates cached versions of the video to ensure smooth playback and seeking. The cache is saved at the following path: `/Users/<username>/Library/Caches/TapeXPlayer`. Make sure there is enough free space on the disk to store the cache.
- The program may have issues with video formats other than H.264 or files with a resolution higher than 1080p.
- Video playback may experience lag when running the program on Intel processors. For the best experience, it is recommended to use computers with Apple Silicon processors.

### Controls:
- **Spacebar** — Play/Pause.
- **G** — Switch to frame seek mode based on timecode. Enter time in the following format: HHMMSSFF (hours, minutes, seconds, frames).
- **Up/Down Arrows** — Control playback speed (from normal to 16x).
- **Left Arrow** — Change playback direction (forward/backward).
- **Shift + Left/Right Arrows** — Slow-motion playback (jog mode).

### System Requirements
- Mac with Apple Silicon (or Intel)
- macOS 11.0 (Big Sur) or newer

### Libraries and Dependencies:
- **GStreamer** — for multimedia framework support.
- **FFmpeg** — for decoding and encoding video and audio.
- **SDL** — for cross-platform multimedia capabilities.
- **OpenSSH 3.0** — for file generation with unique identifiers, optimizing workload.
- [**Spleen Font**](https://github.com/fcambus/spleen/tree/master) — used under BSD2-ClauseLicense

### Contributions and Contact:
If you'd like to contribute to the project or need consultation, feel free to reach out at **mail@ffbsoffa.org**. Contributions are welcome through pull requests.
