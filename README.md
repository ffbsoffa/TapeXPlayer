# TapeXPlayer

TapeXPlayer is a specialized video player designed for scientific analysis and research purposes. It combines the functionality of a traditional video (tape)player with advanced features for precise frame-by-frame analysis, timecode display, and variable speed playback.

## Features

- High-precision video playback with frame-accurate seeking
- Variable speed playback (0.25x to 16x) with smooth transitions
- Reverse playback capability
- Real-time timecode display in HH:MM:SS:FF format
- Debug mode for visualizing frame decoding status
- Volume control
- Support for high-resolution video files
- Efficient frame caching and memory management

## System Requirements

- C++17 compatible compiler
- SDL2 library
- FFmpeg libraries (libavcodec, libavformat, libavutil, libswresample, libswscale)
- GStreamer library
- CMake (for building)

## Building the Project

1. Ensure you have all the required libraries installed on your system.
2. Clone the repository:
   ```
   git clone https://github.com/yourusername/TapeXPlayer.git
   cd TapeXPlayer
   ```
3. Create a build directory and run CMake:
   ```
   mkdir build
   cd build
   cmake ..
   ```
4. Build the project:
   ```
   make
   ```

## Usage

To run TapeXPlayer, use the following command:

```
./TapeXPlayer <path_to_video_file>
```

### Controls

- **Space**: Play/Pause
- **Left Arrow**: Toggle reverse playback
- **Up Arrow**: Increase playback speed (2x, 4x, 8x, 16x)
- **Down Arrow**: Decrease playback speed (1/2x, 1/4x)
- **+**: Increase volume
- **-**: Decrease volume
- **D**: Toggle debug mode
- **Q**: Quit the application

### Display Information

The player displays the following information in real-time:

- Current timecode (HH:MM:SS:FF)
- Playback speed
- Playback direction (Forward/Reverse)
- Current frame number
- Playback status (Playing/Paused)
- Debug mode status (ON/OFF)

### Debug Mode

When debug mode is enabled, a color-coded bar at the bottom of the video display shows the status of frame decoding:

- Red: Empty frames (not yet decoded)
- Blue: Low-resolution frames
- Green: Full-resolution frames

The white vertical line indicates the current playback position.

## Performance Considerations

TapeXPlayer is designed to handle high-resolution video files efficiently. However, performance may vary depending on your system specifications and the video file characteristics. For optimal performance:

1. Ensure your system meets or exceeds the minimum requirements.
2. Use SSDs for faster file access.
3. Close unnecessary background applications.
4. For extremely large files, consider pre-processing the video to a more manageable resolution or codec.

## Known Limitations

- The player currently supports a limited range of video codecs. Check FFmpeg documentation for supported formats.
- Extremely high resolution videos (e.g., 8K) may experience performance issues on lower-end systems.
- The player is optimized for scientific analysis and may not provide the same level of performance as general-purpose media players for casual viewing.

## Troubleshooting

If you encounter issues:

1. Ensure all required libraries are correctly installed and up to date.
2. Check that the video file is not corrupted and is in a supported format.
3. Try running the player with a different video file to isolate file-specific issues.
4. Check the console output for any error messages or warnings.

## Contributing

Contributions to TapeXPlayer are welcome. Please follow these steps:

1. Fork the repository.
2. Create a new branch for your feature or bug fix.
3. Commit your changes with clear, descriptive commit messages.
4. Push your branch and submit a pull request.

## License

TapeXPlayer is released under the [MIT License](LICENSE).

## Contact

For questions, suggestions, or support, please open an issue on the GitHub repository or contact the maintainer at [your-email@example.com].
