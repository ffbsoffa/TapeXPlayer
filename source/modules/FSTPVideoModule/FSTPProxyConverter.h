#pragma once

#include <string>
#include <functional>
#include <filesystem>
#include <cstdint>

namespace FSTP {

// Handles proxy video creation.
//
//   macOS primary  — ObjCProxyEncoder (AVFoundation: AVAssetReader → AME, ~13s/4min 1080p)
//   macOS fallback — subprocess FullVT (scale_vt + h264_videotoolbox) → AME → libx264
//   Linux/Windows  — subprocess FullVT → AME → libx264
//
// LowResDecoder owns the decision logic (when to create a proxy, GOP analysis,
// codec detection). ProxyConverter owns the execution (how to create it).
class ProxyConverter {
public:
    // All parameters needed to run proxy conversion.
    // Populated by LowResDecoder::convertToLowRes after source analysis.
    struct Params {
        std::string           inputFilename;
        std::filesystem::path outputPath;

        // Proxy output dimensions (H.264-aligned). Pixels may be anamorphic (see proxyDar*).
        int proxyWidth  = 640;
        int proxyHeight = 360;

        // Display aspect ratio written into the proxy stream (-aspect). When the proxy is encoded on
        // an anamorphic 4:3 pixel grid for wide sources, this stretches it back to the source aspect
        // on screen. 0:0 → don't set (square pixels).
        int proxyDarNum = 0;
        int proxyDarDen = 0;

        // Color metadata strings matched to source (bt709, bt470bg, bt2020nc, …)
        std::string colorSpace     = "bt709";
        std::string colorPrimaries = "bt709";
        std::string colorTrc       = "bt709";
        std::string colorRange     = "tv";   // "tv" = limited, "pc" = full

        // Duration in seconds from libav (avoids ffprobe subprocess).
        // Set to -1.0 if unknown; ConvertParallel will fall back to QueryDuration().
        double totalDuration = -1.0;

        // Source frame rate from libav. Used to verify the finished proxy is frame-locked
        // to the source (proxy duration must match source duration within a few frames).
        // 0.0 → verification falls back to a small absolute duration tolerance.
        double sourceFps = 0.0;

        // Target proxy frame rate. >0 decimates the proxy to this fps (e.g. 50→25) via `-r`,
        // halving both encode time and shuttle decode load; the player auto-detects the ~2:1
        // ratio and uses its half-fps index. 0.0 → keep every source frame (passthrough).
        double proxyFps = 0.0;

        // Called with 0-100 as conversion progresses.
        std::function<void(int)> progressCallback;

        // Reserved for future progressive proxy (AVFoundation fMP4 path).
        // Will be called as soon as the proxy is readable (first GOP written).
        std::function<void()> progressiveReadyCallback;
    };

    // Convert using a single sequential FFmpeg process.
    // Returns true on success; outputPath contains the result file.
    static bool ConvertSingle(const Params& params);

    // Convert by splitting the file into N parallel FFmpeg processes, then
    // concatenating the segments with stream-copy (no re-encode).
    // numSegments=0 → auto-detect based on hardware_concurrency and duration.
    // Falls back to ConvertSingle if duration cannot be queried or N<=1.
    static bool ConvertParallel(const Params& params, int numSegments = 0);

    // Return the recommended number of parallel segments for a given duration,
    // based on std::thread::hardware_concurrency().
    static int OptimalSegments(double durationSeconds);

    // Query video duration in seconds via ffprobe.
    // Returns -1.0 on failure.
    static double QueryDuration(const std::string& filename);

    // Query the video stream frame count (container nb_frames) via ffprobe.
    // Returns -1 if unavailable.
    static int64_t CountFrames(const std::string& path);

    // Verify a finished proxy is frame-locked to the source: its duration must match
    // sourceDuration within a few frames. A frame-exact proxy is within ~1 frame; dropped
    // frames make the proxy seconds short. Cheap (container metadata only).
    // Returns true if the proxy is trustworthy (or the source duration is unknown).
    static bool VerifyProxy(const std::string& proxyPath,
                            double sourceDuration, double sourceFps);

    // Execute a shell command and stream stdout line-by-line to lineCallback.
    // Returns the process exit code, or -1 on launch failure.
    static int RunCommand(
        const std::string& command,
        const std::function<void(const std::string&)>& lineCallback
    );
};

} // namespace FSTP
