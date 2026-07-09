#include "FSTPProxyConverter.h"
#include "FSTPHardwareDetection.h"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <chrono>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace FSTP {

// ---------------------------------------------------------------------------
// Platform subprocess helpers (anonymous namespace — internal linkage only)
// ---------------------------------------------------------------------------
namespace {

#ifdef _WIN32
// Launch a command via CreateProcessW (Unicode-safe, no cmd.exe ANSI conversion).
// Returns process exit code, or -1 on launch failure.
static int RunCommandWin(const std::string& utf8Command,
                         const std::function<void(const std::string&)>& lineCallback)
{
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Command.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return -1;
    std::wstring wcmd(wlen - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Command.c_str(), -1, &wcmd[0], wlen);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) return -1;
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb          = sizeof(STARTUPINFOW);
    si.hStdOutput  = hWrite;
    si.hStdError   = hWrite;
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(
        nullptr, &wcmd[0],
        nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi
    );

    CloseHandle(hWrite);
    if (!ok) { CloseHandle(hRead); return -1; }

    char buf[4096];
    DWORD nRead;
    std::string lineBuffer;
    while (ReadFile(hRead, buf, sizeof(buf) - 1, &nRead, nullptr) && nRead > 0) {
        buf[nRead] = '\0';
        lineBuffer += buf;
        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos) {
            lineCallback(lineBuffer.substr(0, pos));
            lineBuffer.erase(0, pos + 1);
        }
    }
    if (!lineBuffer.empty()) lineCallback(lineBuffer);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exitCode);

    CloseHandle(hRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(exitCode);
}
#endif // _WIN32


// Encode pipeline modes, tried in order: FullVT → AME → Software (macOS)
// or HWDecode → Software (Windows/Linux).
enum class EncodeMode { FullVT, AME, Software, HWDecode };

// Build video filter + encoder flags for the chosen mode.
//
// FullVT  (macOS): frames stay in VideoToolbox GPU memory throughout.
//                  scale_vt resizes in hardware, h264_videotoolbox (AME) encodes.
//                  BuildCommand adds -hwaccel/-hwaccel_output_format on the input side.
//                  Eliminates all CPU work for decode+scale+encode.
// AME     (macOS): CPU decode + scale (neighbor) → h264_videotoolbox via AME chip.
//                  Frees all CPU cores for decode/scale; encode costs no CPU.
// HWDecode (Win/Linux): GPU decode (D3D11VA / VA-API) → CPU scale → libx264.
//                  Encode flags identical to Software; BuildCommand adds -hwaccel
//                  on the input side. Offloads the decode half on weak CPUs
//                  (MINIMUM profile); if the GPU path dies, ffmpeg exits non-zero
//                  and the caller retries with Software.
// Software        : CPU decode + scale (neighbor) → libx264. Cross-platform fallback.
static std::string BuildEncodeFlags(const ProxyConverter::Params& p, EncodeMode mode) {
    const std::string w = std::to_string(p.proxyWidth);
    const std::string h = std::to_string(p.proxyHeight);
    // Bitrate ≈ 8 kbps per 1000 px. Bumped 4.5×→8× to kill the blockiness directly (Stage 2
    // seek-per-frame decouples proxy quality from shuttle speed, and GOP=4 spreads bits less
    // efficiently, so it wants the headroom). At 540p (960×540) this gives ~4.1 Mbps. Floor keeps
    // tiny proxies from going blocky.
    int kbps = p.proxyWidth * p.proxyHeight * 16 / 2000;  // px × 8 / 1000
    if (kbps < 700) kbps = 700;
    const std::string bv = std::to_string(kbps) + "k";

#ifdef __APPLE__
    if (mode == EncodeMode::FullVT) {
        // Fully hardware pipeline: VT decode → scale_vt (HW) → AME encode.
        // No CPU pixel-format conversion; no GPU↔CPU transfer.
        // scale_vt=W:H: VideoToolbox hardware scaler (FFmpeg lavfi filter).
        return
            " -vf \"scale_vt=" + w + ":" + h + "\""
            " -colorspace "      + p.colorSpace     +
            " -color_primaries " + p.colorPrimaries +
            " -color_trc "       + p.colorTrc       +
            " -color_range "     + p.colorRange     +
            " -c:v h264_videotoolbox -profile:v baseline"
            " -g 4 -b:v " + bv + " -allow_sw 1 -realtime 1"
            " -an";
    }
    if (mode == EncodeMode::AME) {
        // CPU decode + scale → AME encode (independent chip, no CPU cost).
        // NV12: VideoToolbox native pixel format, avoids extra yuv420p→nv12 step.
        // neighbor: nearest-neighbour — fastest CPU downscale, fine for scrubbing proxy.
        std::string vfilter = "scale=" + w + ":" + h + ":flags=neighbor,format=nv12";
        return
            " -vf \""            + vfilter + "\""
            " -colorspace "      + p.colorSpace     +
            " -color_primaries " + p.colorPrimaries +
            " -color_trc "       + p.colorTrc       +
            " -color_range "     + p.colorRange     +
            " -c:v h264_videotoolbox -profile:v baseline"
            " -g 4 -b:v " + bv + " -allow_sw 1 -realtime 1"
            " -an";
    }
#else
    (void)mode;
#endif

    // Software encoder (libx264) — cross-platform, always available.
    std::string x264Params =
        "colorprimaries=" + p.colorPrimaries +
        ":transfer="      + p.colorTrc +
        ":colormatrix="   + (p.colorSpace == "bt2020nc" ? std::string("bt2020nc") : p.colorSpace) +
        (p.colorRange == "pc" ? ":fullrange=on" : ":fullrange=off") +
        ":keyint=4:min-keyint=4";

    // neighbor: nearest-neighbour downscale — fastest CPU option, fine for scrubbing proxy.
    std::string vfilter = "scale=" + w + ":" + h + ":flags=neighbor,format=yuv420p";

    return
        " -vf \""            + vfilter + "\""
        " -colorspace "      + p.colorSpace     +
        " -color_primaries " + p.colorPrimaries +
        " -color_trc "       + p.colorTrc       +
        " -color_range "     + p.colorRange     +
        " -c:v libx264 -profile:v baseline -preset fast -g 4 -b:v " + bv +
        " -x264-params \""   + x264Params + "\""
        " -an";
}

#if !defined(__APPLE__)
// Windows/Linux: whether to try GPU decode for proxy conversion at all.
// Same gate as the player's GetDecoderStrategy: a real GPU with score >= 50.
// On machines without a GPU (or in a VM) go straight to Software, no wasted attempt.
static bool HWDecodeUsable() {
    if (!g_hardware_detection) return false;
    const FSTPGPUInfo& gpu = g_hardware_detection->GetGPUInfo();
    return gpu.HasAnyHWAccel() && gpu.hw_decode_score >= 50.0;
}
#endif

// Build a full FFmpeg command for one segment (or the whole file if ss<0).
// ss   = start offset in seconds  (-1 = from beginning)
// dur  = segment duration in seconds (-1 = to end of file)
// mode = FullVT / AME / Software  (see EncodeMode)
static std::string BuildCommand(const ProxyConverter::Params& p,
                                const fs::path& destPath,
                                double ss,
                                double dur,
                                EncodeMode mode = EncodeMode::AME)
{
    std::ostringstream cmd;
    // No AudioInputFlag: proxy is video-only (-an), audio decode is skipped entirely.
    cmd << "ffmpeg -nostdin -y -progress pipe:1";

#ifdef __APPLE__
    if (mode == EncodeMode::FullVT) {
        // Keep decoded frames in VideoToolbox GPU memory.
        // videotoolbox_vld: opaque VT surface format, compatible with scale_vt
        // and h264_videotoolbox — no CPU transfer at any stage.
        cmd << " -hwaccel videotoolbox -hwaccel_output_format videotoolbox_vld";
    }
#elif defined(_WIN32)
    if (mode == EncodeMode::HWDecode) {
        // GPU decode on input; without -hwaccel_output_format frames are
        // automatically downloaded to CPU memory for scale/libx264 below.
        cmd << " -hwaccel d3d11va";
    }
#elif defined(__linux__)
    if (mode == EncodeMode::HWDecode) {
        cmd << " -hwaccel vaapi";
        // Explicitly pass the working node found by the probe in DetectGPU_Linux
        // (ffmpeg's default, renderD128, may be the wrong GPU).
        if (g_hardware_detection && !g_hardware_detection->GetGPUInfo().vaapi_devices.empty()) {
            cmd << " -hwaccel_device "
                << g_hardware_detection->GetGPUInfo().vaapi_devices[0];
        }
    }
#endif

    if (ss >= 0.0) cmd << " -ss " << std::fixed << std::setprecision(3) << ss;
    if (dur > 0.0) cmd << " -t "  << std::fixed << std::setprecision(3) << dur;

    cmd << " -i \"" << p.inputFilename << "\"";
    cmd << BuildEncodeFlags(p, mode);
    if (p.proxyFps > 0.0) {
        // HALF-FPS: decimate to proxyFps (e.g. 50→25) as clean CFR. Halves frames → halves
        // encode time AND shuttle decode load. The player auto-detects the ~2:1 ratio and maps
        // original frame F → proxy slot F/2, so content stays aligned (no drift).
        cmd << " -r " << std::fixed << std::setprecision(3) << p.proxyFps;
    } else {
        // FRAME-LOCK: exactly one output frame per input frame — no vsync drop/dup. ffmpeg
        // blocks rather than drops when the encoder can't keep up, so this stays frame-exact
        // under any load. Guarantees proxy frame count == source frame count.
        cmd << " -fps_mode passthrough";
    }
    // Display aspect ratio: stretches an anamorphic (4:3-pixel) proxy back to the source aspect on
    // screen. ffmpeg derives the stream SAR = DAR / (w/h) and writes it; the decode + display read
    // it (frame->sample_aspect_ratio → pixel-buffer SAR → aspect-fit). Harmless for square proxies
    // (SAR resolves to 1:1).
    if (p.proxyDarNum > 0 && p.proxyDarDen > 0) {
        cmd << " -aspect " << p.proxyDarNum << ":" << p.proxyDarDen;
    }
    cmd << " \"" << destPath.string() << "\"";

#ifndef _WIN32
    cmd << " 2>&1";
#endif

    return cmd.str();
}

// Write an ffmpeg concat list file for the given segment paths.
static bool WriteConcatList(const fs::path& listPath,
                            const std::vector<fs::path>& segments)
{
    std::ofstream f(listPath);
    if (!f) return false;
    for (const auto& seg : segments) {
        f << "file '" << seg.string() << "'\n";
    }
    return true;
}

// Concatenate segment files into a single output using stream-copy (no re-encode).
static bool ConcatenateSegments(const std::vector<fs::path>& segments,
                                const fs::path& outputPath)
{
    fs::path listPath = outputPath.parent_path() / (outputPath.stem().string() + "_concat.txt");
    if (!WriteConcatList(listPath, segments)) return false;

    std::string cmd =
        "ffmpeg -nostdin -y -f concat -safe 0 -i \""
        + listPath.string()
        + "\" -c copy \""
        + outputPath.string()
        + "\"";

#ifndef _WIN32
    cmd += " 2>&1";
#endif

    int status = ProxyConverter::RunCommand(cmd, [](const std::string&) {});
    fs::remove(listPath);
    return (status == 0);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// ProxyConverter — public interface
// ---------------------------------------------------------------------------

int ProxyConverter::RunCommand(const std::string& command,
                               const std::function<void(const std::string&)>& lineCallback)
{
#ifdef _WIN32
    return RunCommandWin(command, lineCallback);
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1;

    char buffer[4096];
    std::string lineBuffer;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        lineBuffer += buffer;
        size_t pos;
        while ((pos = lineBuffer.find('\n')) != std::string::npos) {
            lineCallback(lineBuffer.substr(0, pos));
            lineBuffer.erase(0, pos + 1);
        }
    }
    if (!lineBuffer.empty()) lineCallback(lineBuffer);

    return pclose(pipe);
#endif
}

double ProxyConverter::QueryDuration(const std::string& filename)
{
    std::string command =
        "ffprobe -v error -show_entries format=duration"
        " -of default=noprint_wrappers=1:nokey=1 \"" + filename + "\"";

    std::string result;
#ifdef _WIN32
    RunCommandWin(command, [&](const std::string& line) { result += line; });
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1.0;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
#endif

    try { return std::stod(result); }
    catch (...) { return -1.0; }
}

int64_t ProxyConverter::CountFrames(const std::string& path)
{
    std::string command =
        "ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames"
        " -of default=noprint_wrappers=1:nokey=1 \"" + path + "\"";

    std::string result;
#ifdef _WIN32
    RunCommandWin(command, [&](const std::string& line) { result += line; });
#else
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return -1;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe)) result += buffer;
    pclose(pipe);
#endif

    try { return std::stoll(result); }
    catch (...) { return -1; }
}

bool ProxyConverter::VerifyProxy(const std::string& proxyPath,
                                 const std::string& sourcePath,
                                 double sourceDuration, double sourceFps)
{
    if (sourceDuration <= 0.0) return true;   // unknown source — cannot verify, accept

    // PRIMARY check: FRAME COUNT. A frame dropped mid-file keeps the duration intact (the last
    // frame's PTS is unchanged) but lowers the count, so a duration check alone misses internal
    // gaps. The proxy must hold one frame per source frame for the time→index match to stay 1:1.
    if (sourceFps > 1.0) {
        int64_t actual = CountFrames(proxyPath);
        if (actual > 0) {
            // Compare against the SOURCE's REAL frame count (container nb_frames) — a true 1:1
            // source↔proxy check. `duration × fps` is only an estimate and routinely differs from
            // the real count by a few frames (container duration ≠ exact frame count, edit lists,
            // truncated final GOP), which falsely rejected otherwise-perfect proxies — e.g. an AV1
            // source where the proxy (5369) matched the source exactly but the estimate said 5373.
            int64_t srcCount = CountFrames(sourcePath);
            bool    exact    = (srcCount > 0);
            int64_t expected = exact ? srcCount : std::llround(sourceDuration * sourceFps);
            // Strict (±3) when comparing two real counts; looser when only the estimate exists
            // (a handful of frames on thousands never hurts the time→index mapping).
            int64_t tol      = exact ? 3 : std::max<int64_t>(15, expected / 500);
            int64_t diff     = std::llabs(actual - expected);
            if (diff > tol) {
                std::cerr << "[ProxyConverter] ❌ VERIFY FAILED: proxy " << actual
                          << " frames vs source " << (exact ? "" : "~") << expected
                          << " (diff " << diff << " > " << tol << ") — frames were dropped"
                          << std::endl;
                return false;
            }
            std::cout << "✅ [Proxy] Verified frame-exact (" << actual << " vs "
                      << (exact ? "" : "~") << expected << " frames)" << std::endl;
            return true;
        }
        // nb_frames unavailable in container metadata → fall through to duration check.
    }

    // FALLBACK check: DURATION (when fps / frame-count is unavailable).
    double proxyDuration = QueryDuration(proxyPath);
    if (proxyDuration <= 0.0) {
        std::cerr << "[ProxyConverter] VERIFY FAILED: proxy unreadable: " << proxyPath << std::endl;
        return false;
    }
    double frame = (sourceFps > 1.0) ? (1.0 / sourceFps) : 0.04;
    double tol   = std::max(0.1, 3.0 * frame);
    double diff  = std::fabs(proxyDuration - sourceDuration);
    if (diff > tol) {
        std::cerr << "[ProxyConverter] ❌ VERIFY FAILED: proxy " << std::fixed << std::setprecision(3)
                  << proxyDuration << "s vs source " << sourceDuration << "s (diff " << diff
                  << "s > tol " << tol << "s)" << std::endl;
        return false;
    }
    std::cout << "✅ [Proxy] Verified by duration (" << std::fixed << std::setprecision(3)
              << proxyDuration << "s vs " << sourceDuration << "s)" << std::endl;
    return true;
}

int ProxyConverter::OptimalSegments(double durationSeconds)
{
    // Each FFmpeg instance uses multiple CPU threads. On typical hardware:
    //   - 4-core:  2 parallel segments give best throughput
    //   - 8-core:  4 parallel segments
    //   - 12-core: 4-6 parallel segments
    // Short files don't benefit from many segments (overhead of concat).
    unsigned int hw = std::thread::hardware_concurrency();
    int byCore = static_cast<int>(std::max(1u, hw / 2));
    byCore = std::min(byCore, 6); // safety cap

    // Short clips: a single continuous pass. Segmenting a short clip is a net LOSS — the
    // fixed overhead (N ffmpeg spawns + concat pass + a VerifyProxy ffprobe round-trip, and
    // a full single-pass RE-ENCODE if the stitched result isn't frame-locked) dwarfs the tiny
    // parallel win, so a short h264 that would encode in a fraction of a second single-pass
    // instead pays for 2 encodes. Below this threshold, ConvertParallel routes to ConvertSingle.
    if (durationSeconds < 120.0) return 1;
    // Under 5 minutes: 2 segments (concat overhead still matters)
    if (durationSeconds < 300.0) return std::min(byCore, 2);
    // Under 10 minutes: up to 3
    if (durationSeconds < 600.0) return std::min(byCore, 3);
    // 10+ minutes: full parallel
    return byCore;
}

// Run one FFmpeg command, parse its -progress pipe output, clean up on failure.
// Returns true on success.
static bool ExecuteFFmpeg(const std::string& cmd,
                          const fs::path& dest,
                          double totalDuration,
                          const std::function<void(int)>& reportProgress)
{
    auto processLine = [&](const std::string& line) {
        if (!reportProgress) return;
        size_t pos = line.find("out_time_ms=");
        if (pos == std::string::npos) return;
        try {
            int64_t time_us = std::stoll(line.substr(pos + 12));
            double t = time_us / 1000000.0;
            if (totalDuration > 0.0 && t > 0.0) {
                int pct = static_cast<int>((t / totalDuration) * 100.0);
                reportProgress(std::max(0, std::min(100, pct)));
            }
        } catch (...) {}
    };

    int status = ProxyConverter::RunCommand(cmd, processLine);
    if (status != 0) {
        if (fs::exists(dest)) fs::remove(dest);
        return false;
    }
    return true;
}

bool ProxyConverter::ConvertSingle(const Params& params)
{
    const fs::path& dest = params.outputPath;
    double totalDuration = (params.totalDuration > 0.0) ? params.totalDuration
                                                        : QueryDuration(params.inputFilename);
    // Frames-per-second the finished proxy should have (half-fps proxies hold fewer frames).
    const double vfps = (params.proxyFps > 0.0) ? params.proxyFps : params.sourceFps;

    int lastPct = -1;
    auto reportProgress = [&](int pct) {
        if (pct - lastPct >= 5 || pct == 0 || pct == 100) {
            std::cout << "🎬 [Proxy] " << pct << "%" << std::endl;
            lastPct = pct;
        }
        if (params.progressCallback) params.progressCallback(pct);
    };
    reportProgress(0);

    std::cout << "🎬 [Proxy] Single pass, duration: "
              << std::fixed << std::setprecision(1) << totalDuration << "s" << std::endl;

    // Run one encoder mode, timing it; on success log WHICH encoder won and how fast, so a slow
    // path (e.g. an unexpected fallback) is immediately visible instead of being silent.
    auto tryMode = [&](EncodeMode mode, const char* name) -> bool {
        auto t0 = std::chrono::steady_clock::now();
        bool ok = ExecuteFFmpeg(BuildCommand(params, dest, -1.0, -1.0, mode),
                                 dest, totalDuration, reportProgress) &&
                  VerifyProxy(dest.string(), params.inputFilename, totalDuration, vfps);
        if (!ok) return false;
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        double rt   = (totalDuration > 0.0 && secs > 0.1) ? totalDuration / secs : 0.0;
        std::cout << "✅ [Proxy] Encoder: " << name << " — "
                  << std::fixed << std::setprecision(1) << secs << "s ("
                  << std::setprecision(1) << rt << "× realtime)" << std::endl;
        reportProgress(100);
        return true;
    };

#ifdef __APPLE__
    // macOS: frame-exact ffmpeg pipeline (-fps_mode passthrough → ffmpeg BLOCKS rather than drops,
    // so the proxy is a true 1:1 copy; VerifyProxy rejects any short proxy). The old AVAssetReader
    // path silently dropped frames under load and jumped on playback.
    //
    // Encoder ORDER is chosen for SPEED. Measured on 1080p50 (Apple Silicon): VideoToolbox hardware
    // DECODE is the bottleneck — ~5× SLOWER than software decode (12.5s vs 2.6s for 60s) — so the
    // "fully on-GPU" FullVT path is actually the SLOWEST (~5.6× realtime). The fast path is SOFTWARE
    // decode + neighbour scale → the VT (AME) encode chip: ~2× faster end-to-end (~12× realtime),
    // same VT-encode quality, same GOP=4 (so the shuttle is unaffected).
    //
    // 1. AME: software decode + neighbour scale → h264_videotoolbox (AME chip). Fastest good path.
    if (tryMode(EncodeMode::AME, "AME (sw decode + VideoToolbox)")) return true;
    std::cerr << "[ProxyConverter] AME failed, falling back to libx264..." << std::endl;
#endif

#if !defined(__APPLE__)
    // Windows/Linux: GPU input decode (D3D11VA / VA-API) + libx264. The output
    // is identical to the Software path (same libx264), so VerifyProxy is shared;
    // if the GPU path fails, ffmpeg exits with an error and Software runs below.
    if (HWDecodeUsable()) {
        if (tryMode(EncodeMode::HWDecode, "hw decode + libx264")) return true;
        std::cerr << "[ProxyConverter] HW decode failed, falling back to software decode..." << std::endl;
    }
#endif

    // 2. Software (libx264 + neighbor scale) — cross-platform.
    if (tryMode(EncodeMode::Software, "libx264 (software)")) return true;

#ifdef __APPLE__
    // 3. FullVT — LAST resort only (slowest; VT hardware decode is the bottleneck), kept for streams
    // that decode only in VideoToolbox if the software decode above choked.
    if (tryMode(EncodeMode::FullVT, "FullVT (VideoToolbox, last-resort)")) return true;
#endif

    std::cerr << "[ProxyConverter] ConvertSingle failed." << std::endl;
    return false;
}

bool ProxyConverter::ConvertParallel(const Params& params, int numSegments)
{
#ifdef __APPLE__
    // macOS: a single continuous pass via ConvertSingle (software decode + VT/AME encode) is fast
    // (~12× realtime) AND frame-exact, so we never segment here. Segmentation wouldn't help: the VT
    // encoder is a single AME chip, so parallel segments would just contend. The old AVFoundation
    // AVAssetReader pipeline was dropped: it silently lost frames under load, producing proxy gaps
    // that jumped on playback.
    return ConvertSingle(params);
#endif

    double totalDuration = (params.totalDuration > 0.0) ? params.totalDuration
                                                        : QueryDuration(params.inputFilename);
    const double vfps = (params.proxyFps > 0.0) ? params.proxyFps : params.sourceFps;
    if (totalDuration <= 0.0) {
        std::cerr << "[ProxyConverter] Cannot query duration, falling back to single pass" << std::endl;
        return ConvertSingle(params);
    }

    if (numSegments <= 0) numSegments = OptimalSegments(totalDuration);
    if (numSegments <= 1) return ConvertSingle(params);

    std::cout << "🚀 [Proxy] Parallel conversion: " << numSegments
              << " segments, duration=" << std::fixed << std::setprecision(1)
              << totalDuration << "s" << std::endl;

    double segDur = totalDuration / numSegments;
    fs::path tempDir = params.outputPath.parent_path() / "proxy_tmp";
    fs::create_directories(tempDir);

    std::vector<fs::path> segPaths(numSegments);
    for (int i = 0; i < numSegments; ++i) {
        std::ostringstream name;
        name << "seg_" << std::setw(2) << std::setfill('0') << i << ".mp4";
        segPaths[i] = tempDir / name.str();
    }

    // Per-segment progress tracked atomically, aggregated for UI
    std::vector<std::atomic<int>> segProgress(numSegments);
    for (auto& p : segProgress) p.store(0);
    std::mutex progressMutex;
    int lastReported = -1;

    auto reportAggregated = [&]() {
        int sum = 0;
        for (const auto& sp : segProgress) sum += sp.load();
        int pct = sum / numSegments;
        std::lock_guard<std::mutex> lk(progressMutex);
        if (pct - lastReported >= 5 || pct == 0 || pct == 100) {
            std::cout << "🎬 [Proxy] Parallel progress: " << pct << "%" << std::endl;
            lastReported = pct;
            if (params.progressCallback) params.progressCallback(pct);
        }
    };

    if (params.progressCallback) params.progressCallback(0);

    std::vector<std::thread> threads;
    std::vector<bool> results(numSegments, false);

    for (int i = 0; i < numSegments; ++i) {
        threads.emplace_back([&, i]() {
            double ss     = i * segDur;
            double dur    = (i == numSegments - 1) ? -1.0 : segDur;
            double refDur = (dur > 0.0) ? dur : (totalDuration - ss);

            // Progress callback for this segment: updates atomic, triggers aggregated report
            auto segReport = [&](int pct) {
                segProgress[i].store(std::max(0, std::min(100, pct)));
                reportAggregated();
            };

#ifdef __APPLE__
            // 1. Full VT pipeline (scale_vt + AME)
            std::cout << "🎬 [Proxy seg" << i << "] Trying FullVT (scale_vt + AME)..." << std::endl;
            if (ExecuteFFmpeg(BuildCommand(params, segPaths[i], ss, dur, EncodeMode::FullVT),
                              segPaths[i], refDur, segReport)) {
                std::cout << "✅ [Proxy seg" << i << "] FullVT succeeded" << std::endl;
                results[i] = true;
                segProgress[i].store(100);
                return;
            }
            // 2. AME + CPU neighbor scale
            std::cout << "⚠️  [Proxy seg" << i << "] FullVT failed, trying AME+neighbor..." << std::endl;
            if (ExecuteFFmpeg(BuildCommand(params, segPaths[i], ss, dur, EncodeMode::AME),
                              segPaths[i], refDur, segReport)) {
                std::cout << "✅ [Proxy seg" << i << "] AME succeeded" << std::endl;
                results[i] = true;
                segProgress[i].store(100);
                return;
            }
            std::cout << "⚠️  [Proxy seg" << i << "] AME failed, falling back to libx264..." << std::endl;
#else
            // Windows/Linux: GPU input decode + libx264; on failure, software below.
            if (HWDecodeUsable()) {
                std::cout << "🎬 [Proxy seg" << i << "] Trying HW decode + libx264..." << std::endl;
                if (ExecuteFFmpeg(BuildCommand(params, segPaths[i], ss, dur, EncodeMode::HWDecode),
                                  segPaths[i], refDur, segReport)) {
                    std::cout << "✅ [Proxy seg" << i << "] HW decode succeeded" << std::endl;
                    results[i] = true;
                    segProgress[i].store(100);
                    return;
                }
                std::cout << "⚠️  [Proxy seg" << i << "] HW decode failed, falling back to libx264..." << std::endl;
            }
#endif
            // 3. Software fallback (libx264 + neighbor)
            results[i] = ExecuteFFmpeg(BuildCommand(params, segPaths[i], ss, dur, EncodeMode::Software),
                                       segPaths[i], refDur, segReport);
            if (results[i]) std::cout << "✅ [Proxy seg" << i << "] libx264 succeeded" << std::endl;
            segProgress[i].store(results[i] ? 100 : 0);
        });
    }

    for (auto& t : threads) t.join();

    // Check all segments succeeded
    for (int i = 0; i < numSegments; ++i) {
        if (!results[i]) {
            std::cerr << "[ProxyConverter] Segment " << i << " failed, falling back to single pass" << std::endl;
            for (const auto& seg : segPaths) {
                if (fs::exists(seg)) fs::remove(seg);
            }
            fs::remove(tempDir);
            return ConvertSingle(params);
        }
    }

    // Concatenate segments into final output
    std::cout << "🔗 [Proxy] Concatenating " << numSegments << " segments..." << std::endl;
    bool ok = ConcatenateSegments(segPaths, params.outputPath);

    // Cleanup temp files
    for (const auto& seg : segPaths) {
        if (fs::exists(seg)) fs::remove(seg);
    }
    if (fs::exists(tempDir)) fs::remove(tempDir);

    // Segment boundaries (input-seek to keyframes) can shift a few frames; verify the stitched
    // result is frame-locked, and if not, fall back to the single continuous pass.
    if (ok && !VerifyProxy(params.outputPath.string(), params.inputFilename, totalDuration, vfps)) {
        std::cerr << "[ProxyConverter] Concatenated proxy not frame-locked, "
                     "falling back to single pass..." << std::endl;
        if (fs::exists(params.outputPath)) fs::remove(params.outputPath);
        return ConvertSingle(params);
    }

    if (ok && params.progressCallback) params.progressCallback(100);
    return ok;
}


} // namespace FSTP
