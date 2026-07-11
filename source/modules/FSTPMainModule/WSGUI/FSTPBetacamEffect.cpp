#include "FSTPBetacamEffect.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <tuple>
#include <vector>

extern "C" {
#include <libavutil/frame.h>
}

namespace {
constexpr double kEffectThreshold = 1.2;
constexpr double kMinActiveTime = 0.1;
constexpr double kEdgeFadeLeft = 3.0;
constexpr double kEdgeFadeRight = 2.0;
constexpr double kTwoPi = 6.28318530717958647692;

// Helper clamp function (C++11/14 compatible)
template<typename T>
T clamp_val(T value, T min_val, T max_val) {
    return std::max(min_val, std::min(value, max_val));
}

// Apply track mask to frame based on visible regions from drum physics
// This simulates the physical reality of Betacam SP: only portions of the tape
// where the video head is reading valid track data show image; guard bands are dark
void ApplyTrackMask(
    uint8_t* y_plane, int y_pitch,
    uint8_t* u_plane, int u_pitch,
    uint8_t* v_plane, int v_pitch,
    int width, int height,
    const std::vector<FSTPBetacamEffect::BetacamDrumPhysics::VisibleRegion>& regions,
    std::mt19937& rng,
    double playback_rate
) {
    if (!y_plane || y_pitch <= 0 || height <= 0) {
        return;
    }

    // Build scanline visibility mask
    std::vector<bool> visible(height, false);

    for (const auto& region : regions) {
        for (int y = region.y_start; y < region.y_end; y++) {
            if (y >= 0 && y < height) {
                visible[y] = true;
            }
        }
    }

    // Apply mask to Y plane (luma)
    std::uniform_int_distribution<int> noise_dist(12, 24);

    for (int y = 0; y < height; y++) {
        if (!visible[y]) {
            // Guard band: dark with subtle noise (no recorded data here)
            uint8_t* row = y_plane + y * y_pitch;
            uint8_t noise_base = noise_dist(rng);

            for (int x = 0; x < width; x++) {
                // Add horizontal variation for texture
                int variation = (x % 4) - 2;  // -2 to +2
                int final_value = noise_base + variation;
                row[x] = static_cast<uint8_t>(clamp_val(final_value, 0, 255));
            }
        }
    }

    // Apply mask to chroma planes (neutral gray for guard bands)
    if (u_plane && v_plane && u_pitch > 0 && v_pitch > 0) {
        for (int y = 0; y < height / 2; y++) {
            // Check if BOTH field lines are invisible (for interlaced video)
            int y1 = y * 2;
            int y2 = y * 2 + 1;
            bool both_invisible = (y1 < height && y2 < height && !visible[y1] && !visible[y2]);

            if (both_invisible) {
                uint8_t* u_row = u_plane + y * u_pitch;
                uint8_t* v_row = v_plane + y * v_pitch;

                for (int x = 0; x < width / 2; x++) {
                    u_row[x] = 128;  // Neutral chroma
                    v_row[x] = 128;
                }
            }
        }
    }
}
}

FSTPBetacamEffect::PlayerState::PlayerState()
    : rng(std::random_device{}())
    , last_speed(1.0)
    , transition_timer(0)
    , transition_direction(0)
    , transition_use_top(false)
    , is_fast(false)
    , smooth_stripe_height(-1.0)
    , smooth_stripe_spacing(-1.0)
    , hold_timer(0) {
    // Seed the fast per-pixel generator from the (already seeded) mt19937.
    // OR with 1 guarantees a non-zero state — xorshift32 is stuck at 0.
    xrng = static_cast<uint32_t>(rng()) | 1u;
}

FSTPBetacamEffect::FSTPBetacamEffect() = default;

void FSTPBetacamEffect::UpdatePlaybackMetrics(int player_id, const PlaybackMetrics& metrics) {
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return;
    }
    m_players[player_id].metrics = metrics;
}

void FSTPBetacamEffect::ResetPlayer(int player_id) {
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return;
    }
    m_players[player_id] = PlayerState{};
}

void FSTPBetacamEffect::RequestDropoutBurst(int player_id, DropoutKind kind) {
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return;
    }
    // One-shot flag carrying the kind; the burst itself (alternation, rare extended duration,
    // RNG) is started inside ApplyPixelFX on the effect thread. Plain write — consistent with
    // the rest of PlayerState's cross-thread fields; a missed/duplicated trigger is harmless.
    m_players[player_id].doc_request = static_cast<int>(kind);
}

bool FSTPBetacamEffect::HasPendingDropout(int player_id) const {
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return false;
    }
    const PlayerState& s = m_players[player_id];
    if (s.doc_request != 0) {
        return true;
    }
    if (s.doc_duration_ms > 0) {
        double elapsed = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - s.doc_start).count();
        return elapsed < s.doc_duration_ms;
    }
    return false;
}

void FSTPBetacamEffect::ApplyEdgeFade(uint8_t* y_plane, int pitch, int width, int height, uint32_t format) {
    if (!m_enabled || !y_plane || pitch <= 0 || width <= 0 || height <= 0) {
        return;
    }
    // Scale the fade width with resolution (authored at the 360-line proxy) so the soft border
    // looks the same at full-res as it did on the proxy, instead of a 3 px sliver at 1080p.
    double scale = height / 360.0;
    int fadeL = std::max(1, static_cast<int>(std::lround(kEdgeFadeLeft  * scale)));
    int fadeR = std::max(1, static_cast<int>(std::lround(kEdgeFadeRight * scale)));
    if (width <= fadeL + fadeR) {
        return;
    }
    for (int y = 0; y < height; ++y) {
        uint8_t* rowStart = y_plane + static_cast<size_t>(y) * pitch;
        for (int x = 0; x < fadeL; ++x) {
            float fade = (fadeL > 1) ? static_cast<float>(x) / static_cast<float>(fadeL - 1) : 1.0f;
            if (format == SDL_PIXELFORMAT_UYVY) {
                int gi = x / 2, ig = x % 2;
                uint8_t* group = rowStart + gi * 4;
                if (gi * 4 + (1 + ig * 2) < pitch) {
                    uint8_t cy = group[1 + ig * 2];
                    group[1 + ig * 2] = static_cast<uint8_t>(cy * fade + 16.0f * (1.0f - fade));
                }
            } else {
                uint8_t cy = rowStart[x];
                rowStart[x] = static_cast<uint8_t>(cy * fade + 16.0f * (1.0f - fade));
            }
        }
        for (int x = 0; x < fadeR; ++x) {
            int realX = width - 1 - x;
            float fade = (fadeR > 1) ? static_cast<float>(x) / static_cast<float>(fadeR - 1) : 1.0f;
            if (format == SDL_PIXELFORMAT_UYVY) {
                int gi = realX / 2, ig = realX % 2;
                uint8_t* group = rowStart + gi * 4;
                if (gi * 4 + (1 + ig * 2) < pitch) {
                    uint8_t cy = group[1 + ig * 2];
                    group[1 + ig * 2] = static_cast<uint8_t>(cy * fade + 16.0f * (1.0f - fade));
                }
            } else {
                uint8_t cy = rowStart[realX];
                rowStart[realX] = static_cast<uint8_t>(cy * fade + 16.0f * (1.0f - fade));
            }
        }
    }
}

void FSTPBetacamEffect::ApplyAnalogSmear(uint8_t* y_plane, int y_pitch,
        uint8_t* u_plane, uint8_t* v_plane, int u_pitch, int v_pitch,
        int width, int height, uint32_t format) {
    if (!m_enabled || !y_plane || width < 2 || height < 1) return;
    // One-pole IIR (out = in*(256-k)/256 + prev*k/256), trailing left→right. Chroma smears
    // more than luma. Strength scales with resolution so the trail is a consistent fraction of
    // width (a fixed k over-smears the low-res proxy). Calibrated at full-res (~1080).
    double scale = std::max(0.30, std::min(1.0, height / 1080.0));
    double rL = 0.575 * scale;  // was 1.15 — smear cut another 50% on owner's request
    double rC = 1.425 * scale;  // was 2.85 — chroma smear cut to match
    const int khLuma   = static_cast<int>(std::lround(256.0 * rL / (1.0 + rL)));
    const int khChroma = static_cast<int>(std::lround(256.0 * rC / (1.0 + rC)));

    for (int yy = 0; yy < height; ++yy) {
        uint8_t* row = y_plane + static_cast<size_t>(yy) * y_pitch;
        int prev = row[0];
        for (int xx = 1; xx < width; ++xx) {
            int cur = (row[xx] * (256 - khLuma) + prev * khLuma) >> 8;
            row[xx] = static_cast<uint8_t>(cur);
            prev = cur;
        }
    }

    int cw = width / 2, ch = height / 2;
    if (cw < 2 || ch < 1) return;
    if (format == SDL_PIXELFORMAT_NV12 && u_plane) {
        for (int yy = 0; yy < ch; ++yy) {
            uint8_t* row = u_plane + static_cast<size_t>(yy) * u_pitch;  // interleaved UV
            int pu = row[0], pv = row[1];
            for (int xx = 1; xx < cw; ++xx) {
                int cu = (row[xx * 2]     * (256 - khChroma) + pu * khChroma) >> 8;
                int cv = (row[xx * 2 + 1] * (256 - khChroma) + pv * khChroma) >> 8;
                row[xx * 2]     = static_cast<uint8_t>(cu); pu = cu;
                row[xx * 2 + 1] = static_cast<uint8_t>(cv); pv = cv;
            }
        }
    } else if ((format == SDL_PIXELFORMAT_IYUV || format == SDL_PIXELFORMAT_YV12) && u_plane && v_plane) {
        for (int yy = 0; yy < ch; ++yy) {
            uint8_t* urow = u_plane + static_cast<size_t>(yy) * u_pitch;
            uint8_t* vrow = v_plane + static_cast<size_t>(yy) * v_pitch;
            int pu = urow[0], pv = vrow[0];
            for (int xx = 1; xx < cw; ++xx) {
                int cu = (urow[xx] * (256 - khChroma) + pu * khChroma) >> 8; urow[xx] = static_cast<uint8_t>(cu); pu = cu;
                int cv = (vrow[xx] * (256 - khChroma) + pv * khChroma) >> 8; vrow[xx] = static_cast<uint8_t>(cv); pv = cv;
            }
        }
    }
}

void FSTPBetacamEffect::ApplySoftEdges(uint8_t* y_plane, int y_pitch,
        int width, int height, uint32_t /*format*/) {
    if (!m_enabled || !y_plane || width < 1 || height < 3) return;
    // Modest vertical 3-tap blur (out = (above + 2*cur + below)/4) blended back
    // toward the original by `mix`. Softens the hard horizontal edges of the grey
    // Betacam bands so they read as soft SD rather than razor-sharp digital. Luma
    // only (chroma is already subsampled). Uses the ORIGINAL neighbour rows (a one
    // row backup) so the blur is symmetric and frame-rate stable.
    const int mix = 96; // 0..256, ~37% toward blurred — subtle, tunable
    std::vector<uint8_t> above(width), curOrig(width);
    std::memcpy(above.data(), y_plane, width);            // row 0 (top edge clamp)
    for (int yy = 1; yy < height - 1; ++yy) {
        uint8_t* cur = y_plane + static_cast<size_t>(yy) * y_pitch;
        const uint8_t* below = y_plane + static_cast<size_t>(yy + 1) * y_pitch; // original
        std::memcpy(curOrig.data(), cur, width);
        for (int xx = 0; xx < width; ++xx) {
            int blurred = (above[xx] + 2 * curOrig[xx] + below[xx]) >> 2;
            cur[xx] = static_cast<uint8_t>(curOrig[xx] + (((blurred - curOrig[xx]) * mix) >> 8));
        }
        std::memcpy(above.data(), curOrig.data(), width); // next row's "above" = this row's original
    }
}

FSTPBetacamEffect::PlayerState& FSTPBetacamEffect::GetPlayerState(int player_id) {
    static PlayerState dummy_state{};
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return dummy_state;
    }
    return m_players[player_id];
}

const FSTPBetacamEffect::PlayerState& FSTPBetacamEffect::GetPlayerState(int player_id) const {
    static PlayerState dummy_state{};
    if (player_id < 0 || player_id >= kMaxPlayers) {
        return dummy_state;
    }
    return m_players[player_id];
}

bool FSTPBetacamEffect::ApplyPixelFX(int player_id, FrameContext& frame_ctx) {
    if (!m_enabled) {
        return false;
    }
    if (frame_ctx.planes[0] == nullptr || frame_ctx.width <= 0 || frame_ctx.height <= 0 || frame_ctx.linesize[0] <= 0) {
        return false;
    }

    PlayerState& state = GetPlayerState(player_id);
    const PlaybackMetrics& metrics = state.metrics;

    const double currentPlaybackRate = metrics.playback_rate;
    const double rawPlaybackRate = std::abs(currentPlaybackRate);
    // Use file-relative position: position_seconds includes timecode offset, duration_seconds does not.
    const double currentTime = metrics.position_seconds - metrics.timecode_offset_seconds;
    const double totalDuration = metrics.duration_seconds;
    // 1× reverse: tape moves backward at normal speed → helical scan misaligned → tracking artifacts
    const bool isReverseNormalSpeed = (rawPlaybackRate >= 0.9 && rawPlaybackRate <= 1.1 && metrics.is_reverse);

    const bool isNewFrame = frame_ctx.new_frame;
    if (isNewFrame) {
        double fps = (metrics.frame_rate > 1.0) ? metrics.frame_rate : 60.0;
        int holdFrames = static_cast<int>(std::round(fps * 0.5));
        holdFrames = clamp_val(holdFrames, 15, 90);
        state.hold_timer = holdFrames;
    }

    bool holdActive = (!isNewFrame && state.hold_timer > 0);
    bool timelineValid = (currentTime > kMinActiveTime) &&
                         (totalDuration <= 0.0 || (totalDuration - currentTime) > kMinActiveTime);

    // Effect is active when:
    // - Slow motion / pause (< 0.9×) - shows single stripe like real Betacam pause
    // - Fast shuttle (> 1.2×) - shows multiple stripes
    // - Normal playback (0.9× - 1.1×) - NO effect (perfect tracking)
    // 1× reverse is treated the same as 1× forward: no effect, no compositing.
    // The tracking-artifact model was physically correct but too expensive at 60fps
    // (per-row composite memcpy + noise generation caused visible FPS drops).
    bool isSlowMotion = (rawPlaybackRate < 0.9);
    bool isFastShuttle = (rawPlaybackRate >= kEffectThreshold);
    // 1× reverse shows the single tracking stripe ONLY when the user opted in (Betacam settings).
    // Off by default: the flicker distracts from frame-by-frame analysis, the product's core use.
    // It reuses the cheap single-stripe path (same as slow-mo), NOT the old expensive per-row
    // tracking-artifact model, so re-enabling it here doesn't bring back those FPS drops.
    bool shouldShowEffect = isSlowMotion || isFastShuttle ||
                            (isReverseNormalSpeed && m_reverse_stripe_enabled);

    // IMPORTANT: At pause/slow motion, ALWAYS show effect regardless of hold timer
    // This ensures the pause stripe is visible even after hold_timer expires
    if (isSlowMotion) {
        holdActive = true;  // Force effect to stay active at pause

        // Generate new grey zone offset when ENTERING slow motion/pause
        // This offset persists during the pause, only changes on new pause
        if (!state.was_slow_motion) {
            // Entering slow motion - generate random offset from center
            // Range: -0.18 to +0.18 (18% of stripe height shift from center)
            std::uniform_int_distribution<int> dist(-18, 18);
            state.grey_zone_offset = dist(state.rng) / 100.0;
        }
    }
    state.was_slow_motion = isSlowMotion;

    // === DROPOUT-COMPENSATION (DOC) BURST ===
    // Started via RequestDropoutBurst() from outside: the resume-from-pause key (spacebar)
    // and proxy↔full-res source switches (both detected in FSTPPixelBufferManager, which sees
    // every frame's resolution even at 1× where this effect is otherwise gated off). Each
    // trigger alternates aggressive (heavy bands) ↔ light (thin lines), and RARELY produces an
    // extended (longer-lived) dropout. Lifetime is wall-clock based so it's frame-rate independent.
    bool   docActive = false;
    double docIntensity = 0.0;
    {
        auto now = std::chrono::steady_clock::now();
        if (state.doc_request != 0) {
            int kind = state.doc_request;   // 1 = Gentle (pause-exit), 2 = Alternating (proxy switch)
            state.doc_request = 0;
            bool gentle = (kind == 1);
            bool heavy;
            if (gentle) {
                heavy = false;                                  // pause-exit: keep it soft
            } else {
                heavy = state.doc_exit_heavy;
                state.doc_exit_heavy = !state.doc_exit_heavy;   // alternate heavy/light on switches
            }
            state.doc_gentle = gentle;
            state.doc_heavy  = heavy;
            int baseMs = heavy ? 300 : (gentle ? 200 : 250);   // toned down (was 450/300/380)
            if (!gentle) {                                      // gentle never gets the rare long one
                std::uniform_int_distribution<int> ext(0, 99);
                if (ext(state.rng) < 7) baseMs = heavy ? 900 : 750;  // rarer + shorter extended (was 15% / 1400/1100)
            }
            state.doc_duration_ms = baseMs;
            state.doc_start = now;
        }

        if (state.doc_duration_ms > 0) {
            double elapsed = std::chrono::duration<double, std::milli>(now - state.doc_start).count();
            if (elapsed < state.doc_duration_ms) {
                docActive = true;
                docIntensity = 1.0 - elapsed / static_cast<double>(state.doc_duration_ms);  // 1→0
            }
        }
    }

    if (!holdActive) {
        if ((!shouldShowEffect || !timelineValid) && !docActive) {
            // At normal speed (1.0×) there is no effect — kill hsync immediately.
            // Without this UpdateHsyncEffect is never called (early return), and
            // hsyncCurrentSkew "freezes" at a nonzero value.
            if (state.hsync.hsyncLossActive) {
                state.hsync.hsyncLossActive = false;
                state.hsync.hsyncCurrentSkew = 0.0f;
            }
            state.last_speed = rawPlaybackRate;
            state.is_fast = false;
            state.smooth_stripe_height = -1.0;
            state.smooth_stripe_spacing = -1.0;
            state.hold_timer = 0;
            return false;
        }
    }

    double absPlaybackRate = rawPlaybackRate;
    if (holdActive && !shouldShowEffect) {
        absPlaybackRate = std::max(state.last_speed, rawPlaybackRate);
    }

    // Update HSync loss effect state (triggers at 1.5x-2.2x speed).
    // When the main effect is no longer active (e.g. jumped back to 1×), allow any
    // in-flight animation to finish naturally (it's time-based, ~150ms), but block
    // new triggers — otherwise holdActive keeps re-triggering HSync at the old speed.
    double fps = (metrics.frame_rate > 1.0) ? metrics.frame_rate : 60.0;
    UpdateHsyncEffect(state, absPlaybackRate, fps, /*allow_new_triggers=*/shouldShowEffect);

    uint8_t** dst_data = frame_ctx.planes;
    int* dst_linesize = frame_ctx.linesize;
    const int textureWidth = frame_ctx.width;
    const int textureHeight = frame_ctx.height;
    const int pitch = frame_ctx.linesize[0];
    const Uint32 lastSdlPixFormat = frame_ctx.pixel_format;

    // Global effect scale. All artefact pixel sizes are authored at the 360p proxy
    // scale — the resolution where the Betacam effect looks/behaves best (the app's own
    // proxy). This factor scales the "absolute pixel" elements (snow, satellites, ragged
    // edges) proportionally on full-res frames so the look matches the 360p proxy.
    // 1.0 at 360p (a no-op for the proxy), ~2.0 at 720p, ~3.0 at 1080p.
    // (Stripe geometry scales via effectiveHeight/resolutionScale once uncapped below.)
    const double effectScale = textureHeight / 360.0;

    // Fast xorshift32: cheap uniform noise for the per-pixel hot loops.
    // Replaces per-call std::uniform_int_distribution + std::mt19937, which were
    // constructed and invoked for every noise/snow/satellite pixel.
    auto xrand = [&]() -> uint32_t {
        uint32_t x = state.xrng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        state.xrng = x;
        return x;
    };
    auto randomInt = [&](int min_val, int max_val) {
        if (max_val <= min_val) return min_val;
        uint32_t range = static_cast<uint32_t>(max_val - min_val + 1);
        return min_val + static_cast<int>(xrand() % range);
    };

    // Paint a horizontal grey run [x0,x1) on luma row y, neutralising chroma over the
    // same span. Used for the 1–2 px micro-ragged edges of the solid grey band so the
    // head-switch boundary isn't a perfectly straight line (matches real Betacam/S-VHS).
    // Grey only — colour of the surrounding image is never shifted.
    auto paintGreyRun = [&](int y, int x0, int x1) {
        if (y < 0 || y >= textureHeight) return;
        x0 = std::max(0, x0);
        x1 = std::min(textureWidth, x1);
        if (x1 <= x0) return;
        uint8_t* rowStart = dst_data[0] + y * pitch;
        if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
            for (int i = x0 / 2; i < (x1 + 1) / 2; ++i) {
                uint8_t* group = rowStart + i * 4;
                group[0] = 128; group[1] = 128; group[2] = 128; group[3] = 128;
            }
            return;
        }
        std::memset(rowStart + x0, 128, x1 - x0);
        int uvY = y / 2;
        if (uvY >= textureHeight / 2) return;
        int cx0 = x0 / 2;
        int cx1 = (x1 + 1) / 2;
        if (cx1 <= cx0) return;
        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
            if (dst_data[1]) std::memset(dst_data[1] + uvY * dst_linesize[1] + cx0, 128, cx1 - cx0);
            if (dst_data[2]) std::memset(dst_data[2] + uvY * dst_linesize[2] + cx0, 128, cx1 - cx0);
        } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
            if (dst_data[1]) {
                uint8_t* uv = dst_data[1] + uvY * dst_linesize[1];
                for (int i = cx0; i < cx1; ++i) { uv[i * 2] = 128; uv[i * 2 + 1] = 128; }
            }
        }
    };

    // === RAGGED GREY EDGE — single head-switch step per edge ===
    // Each grey band gets exactly ONE horizontal break on its top edge and ONE on its
    // bottom edge (not many bumps). Left of the break the edge is "free" (grey not filled
    // in — the noise edge shows); right of the break the grey is fully filled, stepped
    // up/down by jagAmp (1–2 px @360). The break X is drawn per stripe from the xorshift
    // RNG and re-rolled every frame, so it jerks aggressively across the width (interlaced
    // "partial signal loss" / dropout jitter) — deliberately NOT a smooth drift. Break
    // positions are computed per stripe below.
    const int jagAmp = std::max(2, static_cast<int>(std::lround(1.5 * effectScale)));

    // === SLOW MOTION FRAME COMPOSITING (DECODER-PROVIDED FRAMES) ===
    // Decoder provides adjacent frames: prev_source_frame (N-1) and next_source_frame (N+1)
    // This eliminates internal frame caching and enables seamless direction changes
    //
    // Physics: video head scans top → bottom. Base buffer holds frame N = floor(T*fps).
    // At time T (between N and N+1) the head has already read N+1's track on top:
    // - Above stripe = frame N+1 (next_source_frame) — newer, just read by the head
    // - Below stripe = frame N   (the base buffer)    — residual from the previous pass
    // Same N+1-above / N-below assignment for both directions; only the seam sweep flips.
    //
    // Support both planar YUV (IYUV/YV12) and semi-planar (NV12)
    bool isYUVFormat = (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV ||
                        lastSdlPixFormat == SDL_PIXELFORMAT_YV12 ||
                        lastSdlPixFormat == SDL_PIXELFORMAT_NV12);
    bool isNV12 = (lastSdlPixFormat == SDL_PIXELFORMAT_NV12);

    // For compositing we need Y plane and at least U plane (NV12 has UV interleaved)
    bool have_planes = dst_data[0] && dst_data[1] && (isNV12 || dst_data[2]);

    // 1× REVERSE tracking-bar scroll. On real Betacam SP a 1× reverse pass shows a grey tracking
    // bar scrolling smoothly DOWNWARD from the top (confirmed against hardware). Two things were
    // wrong: (1) direction was inverted (it drifted up), (2) it was tied to the frame index, so at
    // 1× — where frames change ~25×/s — it made a FULL sweep every frame = a 25 Hz strobe, not a
    // scroll. Fix: drive it from the WALL CLOCK at a few Hz, increasing phase = downward (bigger
    // phase → larger stripe_center_y → lower on screen). Computed ONCE here and shared by the
    // compositing seam and the stripe overlay so the two stay locked together.
    double reverse_glide_phase = 0.0;
    if (isReverseNormalSpeed) {
        auto now_rev = std::chrono::steady_clock::now();
        double dt_rev = state.shuttle_comb_init
                      ? std::chrono::duration<double>(now_rev - state.shuttle_comb_last_t).count()
                      : 0.0;
        state.shuttle_comb_last_t = now_rev;
        state.shuttle_comb_init = true;
        if (dt_rev < 0.0 || dt_rev > 0.1) dt_rev = 0.0;   // ignore stalls / first frame
        constexpr double kReverseScrollHz = 1.5;          // downward sweeps per second — tune to taste
        state.shuttle_comb_phase += kReverseScrollHz * dt_rev;
        state.shuttle_comb_phase -= std::floor(state.shuttle_comb_phase);
        reverse_glide_phase = state.shuttle_comb_phase;
    }

    if ((isSlowMotion || isReverseNormalSpeed) && isYUVFormat && have_planes && textureHeight > 0 && textureWidth > 0) {
        // OPTIMIZATION: During pure pause (speed≈0) the compositing result is identical
        // every render frame — AVFrame data persists in-place after first application.
        // Skip re-compositing when the same video frame is presented again.
        bool isPurePause = (rawPlaybackRate < 0.05) && !isReverseNormalSpeed;
        // Frame aligned: audio snapped to boundary, stripe is gone.
        // Render clean frame (no compositing) and invalidate cache so next
        // pause cycle re-composites from scratch.
        if (isPurePause && metrics.frame_aligned) {
            state.last_composited_frame_number = -1;
            goto skip_compositing;
        }
        // Calculate stripe position using same formula as stripe rendering
        double fps_for_calc = (metrics.frame_rate > 0) ? metrics.frame_rate : 25.0;
        double frame_exact = currentTime * fps_for_calc;
        // Phase RELATIVE to the actually-displayed frame (frame_ctx.frame_number), not frac() of a
        // separately-sampled currentTime. The shown frame and currentTime are sampled at different
        // pipeline moments and disagree by 1 at a frame boundary → the seam wrapped while the
        // composite frames (keyed to frame_number) had not advanced → a spike on each new frame.
        // Keying the phase to the shown frame keeps seam + frames in lockstep; the clamp absorbs skew.
        double scroll_phase = frame_exact - static_cast<double>(frame_ctx.frame_number);
        if (scroll_phase < 0.0) scroll_phase = 0.0;
        else if (scroll_phase > 1.0) scroll_phase = 1.0;
        // At 1× reverse the seam follows the slow downward wall-clock glide (see above), NOT the
        // per-frame phase — so the composite boundary tracks the grey bar instead of strobing.
        if (isReverseNormalSpeed) scroll_phase = reverse_glide_phase;

        // Calculate stripe height to match stripe rendering formula exactly
        // Scale effect geometry with the actual frame height so artefacts keep the same
        // proportions on full-res as on the 480p proxy. (Was capped to 480p, which froze
        // artefact pixel sizes and made the whole effect look miniature on full-res frames.)
        const int effectiveHeight_comp = textureHeight;
        double resolutionScale = static_cast<double>(effectiveHeight_comp) / 480.0;
        int slowMotionStripeHeight = static_cast<int>(effectiveHeight_comp * 0.25);
        double slowFactor = absPlaybackRate / 0.9;
        int stripeHeight = static_cast<int>(slowMotionStripeHeight * (1.0 - slowFactor * 0.3));
        stripeHeight = std::max(static_cast<int>(3 * resolutionScale), stripeHeight);

        // Stripe Y position - MUST match stripe rendering formula exactly
        // In stripe rendering: stripe_y IS the center, startY = stripe_y - height/2
        int extraMargin = static_cast<int>(10.0 * resolutionScale);
        int travel_distance = textureHeight + stripeHeight + extraMargin * 2;
        int stripe_center_y = static_cast<int>(scroll_phase * travel_distance) - stripeHeight / 2 - extraMargin;

        // Helical scan physical model (Betacam SP drum):
        // The drum head always scans TOP → BOTTOM.
        // FindFrameByTime returns floor(T*fps) = frame N (the frame whose start ≤ T).
        // At time T between frame N and N+1 the head is reading N+1's track:
        //   ABOVE stripe = frame N+1 (data already read by the head this pass)
        //   BELOW stripe = frame N   (residual left from the previous drum pass)
        // For reverse: scroll_phase is inverted (1-phase) so the stripe sweeps the
        // opposite direction, with the same frame N+1 above / frame N below assignment.
        bool isReverse = metrics.is_reverse;
        const AVFrame* composite_frame = frame_ctx.next_source_frame;  // N+1 for both directions

        // Validate composite frame
        bool have_composite = composite_frame &&
                              composite_frame->data[0] &&
                              composite_frame->data[1] &&
                              (isNV12 || composite_frame->data[2]) &&
                              composite_frame->width == textureWidth &&
                              composite_frame->height == textureHeight;

        if (have_composite) {
            const int comp_y_pitch = composite_frame->linesize[0];
            const int comp_uv_pitch = composite_frame->linesize[1];

            // Iterate only over rows that need compositing (above stripe center).
            // Avoids the per-row branch that was costing ~N/2 mispredictions at 1080p.
            int comp_rows = std::max(0, std::min(stripe_center_y, textureHeight));
            for (int y = 0; y < comp_rows; ++y) {
                std::memcpy(dst_data[0] + y * pitch,
                           composite_frame->data[0] + y * comp_y_pitch,
                           textureWidth);

                if (y % 2 == 0) {
                    int uv_y = y / 2;
                    if (isNV12) {
                        std::memcpy(dst_data[1] + uv_y * dst_linesize[1],
                                   composite_frame->data[1] + uv_y * comp_uv_pitch,
                                   textureWidth);
                    } else {
                        std::memcpy(dst_data[1] + uv_y * dst_linesize[1],
                                   composite_frame->data[1] + uv_y * comp_uv_pitch,
                                   textureWidth / 2);
                        if (dst_data[2] && composite_frame->data[2]) {
                            int comp_v_pitch = composite_frame->linesize[2];
                            std::memcpy(dst_data[2] + uv_y * dst_linesize[2],
                                       composite_frame->data[2] + uv_y * comp_v_pitch,
                                       textureWidth / 2);
                        }
                    }
                }
            }
        }
        state.last_composited_frame_number = frame_ctx.frame_number;
    }
    skip_compositing:;

    // Horizontal analog smear — applied AFTER compositing (so slow-mo compositing doesn't
    // wipe it, unlike before) but BEFORE the stripe/dropout overlays (they stay crisp). At 1×
    // the manager applies it instead.
    if (frame_ctx.smear) {
        ApplyAnalogSmear(dst_data[0], pitch, dst_data[1], dst_data[2],
                         dst_linesize[1], dst_linesize[2], textureWidth, textureHeight, lastSdlPixFormat);
    }

    // Soft L/R border — applied AFTER compositing (so the composited region is faded too and
    // the left edge no longer flickers) but BEFORE the stripe/dropout overlays (so the grey
    // stripe stays in FRONT of the border). At 1× this is done by the manager instead.
    if (frame_ctx.edge_fade) {
        ApplyEdgeFade(dst_data[0], pitch, textureWidth, textureHeight, lastSdlPixFormat);
    }

    bool effectApplied = false;

    const double fastThreshold = 2.0;

    // Apply B&W desaturation for very high speeds
    if (absPlaybackRate >= 10.0) {
        effectApplied = true;
        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
            if (dst_data[1] && dst_linesize[1] > 0) {
                std::memset(dst_data[1], 128, dst_linesize[1] * textureHeight / 2);
            }
            if (dst_data[2] && dst_linesize[2] > 0) {
                std::memset(dst_data[2], 128, dst_linesize[2] * textureHeight / 2);
            }
        } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
            if (dst_data[1] && dst_linesize[1] > 0) {
                for (int y = 0; y < textureHeight / 2; ++y) {
                    uint8_t* uvPlane = dst_data[1] + y * dst_linesize[1];
                    for (int x = 0; x < textureWidth / 2; ++x) {
                        uvPlane[x * 2] = 128;
                        uvPlane[x * 2 + 1] = 128;
                    }
                }
            }
        }
    }

    const double threshold = fastThreshold;

    auto startTransition = [&](int direction) {
        if (state.transition_timer == 0) {
            state.transition_timer = kTransitionFrames;
            state.transition_direction = direction;
            state.transition_use_top = !state.transition_use_top;
        }
    };

    if (state.transition_timer == 0) {
        if (!state.is_fast && rawPlaybackRate >= threshold) {
            startTransition(+1);
        } else if (state.is_fast && rawPlaybackRate < threshold) {
            startTransition(-1);
        }
    }

    auto renderTransitionalStripe = [&](int stripeStart, int stripeHeight) {
        stripeStart = std::max(0, stripeStart);
        for (int y = stripeStart; y < stripeStart + stripeHeight && y < textureHeight; ++y) {
            uint8_t* row = dst_data[0] + y * pitch;
            std::memset(row, 128, textureWidth);

            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                int chromaY = y / 2;
                if (dst_data[1] && chromaY < textureHeight / 2) {
                    std::memset(dst_data[1] + chromaY * dst_linesize[1], 128, textureWidth / 2);
                }
                if (dst_data[2] && chromaY < textureHeight / 2) {
                    std::memset(dst_data[2] + chromaY * dst_linesize[2], 128, textureWidth / 2);
                }
            } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                int chromaY = y / 2;
                if (dst_data[1] && chromaY < textureHeight / 2) {
                    uint8_t* uv = dst_data[1] + chromaY * dst_linesize[1];
                    for (int x = 0; x < textureWidth / 2; ++x) {
                        uv[x * 2] = 128;
                        uv[x * 2 + 1] = 128;
                    }
                }
            }
        }
    };

    if (state.transition_timer > 0) {
        double progress = static_cast<double>(state.transition_timer) / static_cast<double>(kTransitionFrames);
        int stripeHeight = std::max(1, static_cast<int>((0.30 + 0.45 * progress) * textureHeight));
        bool useTop = state.transition_use_top;
        int stripeStart = useTop ? 0 : (textureHeight - stripeHeight);
        renderTransitionalStripe(stripeStart, stripeHeight);
        state.transition_timer--;
        if (state.transition_timer == 0) {
            if (state.transition_direction > 0) {
                state.is_fast = (rawPlaybackRate >= threshold);
            } else if (state.transition_direction < 0) {
                state.is_fast = (rawPlaybackRate >= threshold);
            }
            state.transition_direction = 0;
        }
        state.last_speed = absPlaybackRate;
        effectApplied = true;
        if (!isNewFrame && state.hold_timer > 0) {
            state.hold_timer = std::max(0, state.hold_timer - 1);
        }
        return true;
    }

    // === DOC (DROPOUT COMPENSATION) BURST ===
    // Runs of "lost" lines hold the last good line above them (1H) and repeat it downward →
    // the band collapses into vertical streaks (the aggressive Betacam weak-signal look).
    // Re-rolled every frame via the xorshift RNG so it flickers; intensity decays over the
    // burst. Heavy = a few large contiguous bands (biased to the bottom, where signal is
    // worst); light = many thin 1–few-line repeats. Colour preserved (chroma copied).
    if (docActive && textureHeight > 4 && textureWidth > 0) {
        double tInt = docIntensity;
        auto repeatLineDown = [&](int seedY, int y0, int runH) {
            if (seedY < 0) return;
            for (int y = y0; y < y0 + runH && y < textureHeight; ++y) {
                if (y <= 0) continue;
                // Luma row (for UYVY this copies the full packed Y+C row, so colour rides along).
                std::memcpy(dst_data[0] + y * pitch, dst_data[0] + seedY * pitch, pitch);
                if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                    int cy = y / 2, csy = seedY / 2;
                    if (dst_data[1] && cy < textureHeight / 2)
                        std::memcpy(dst_data[1] + cy * dst_linesize[1], dst_data[1] + csy * dst_linesize[1], textureWidth / 2);
                    if (dst_data[2] && cy < textureHeight / 2)
                        std::memcpy(dst_data[2] + cy * dst_linesize[2], dst_data[2] + csy * dst_linesize[2], textureWidth / 2);
                } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                    int cy = y / 2, csy = seedY / 2;
                    if (dst_data[1] && cy < textureHeight / 2)
                        std::memcpy(dst_data[1] + cy * dst_linesize[1], dst_data[1] + csy * dst_linesize[1], textureWidth);
                }
            }
        };
        int runs = state.doc_heavy
                 ? std::max(1, static_cast<int>(std::lround(2.0 * tInt)))   // few big bands (was 3.0 — toned down)
                 : std::max(1, static_cast<int>(std::lround((state.doc_gentle ? 3.0 : 5.0) * tInt)));  // thin lines (was 4.0/9.0)
        for (int r = 0; r < runs; ++r) {
            int runH, y0;
            if (state.doc_heavy) {
                int band = std::max(2, static_cast<int>(textureHeight * 0.18 * tInt));  // thinner heavy bands (was 0.28)
                runH = randomInt(std::max(2, band / 3), std::max(3, band));
                if (randomInt(0, 99) < 60) {  // bottom-weighted
                    int lo = std::max(1, textureHeight - runH - 1 - static_cast<int>(textureHeight * 0.35));
                    y0 = randomInt(lo, std::max(lo, textureHeight - runH - 1));
                } else {
                    y0 = randomInt(1, std::max(1, textureHeight - runH - 1));
                }
            } else {
                runH = randomInt(1, std::max(1, static_cast<int>(std::lround(3.0 * effectScale))));
                y0 = randomInt(1, std::max(1, textureHeight - runH - 1));
            }
            y0 = std::max(1, std::min(y0, textureHeight - runH - 1));
            repeatLineDown(y0 - 1, y0, runH);  // hold the last good line above the run
        }
        effectApplied = true;
    }

    // Skip stripe generation ONLY if not fast AND not slow motion AND not 1× reverse.
    // Slow motion (< 0.9×) shows the pause stripe; opt-in 1× reverse shows the same single
    // tracking stripe (isReverseNormalSpeed is only ever true here when the user enabled it,
    // since shouldShowEffect gates it — see line ~330). Without this clause the early return
    // below skipped ALL stripe generation at 1× reverse, so the stripe never drew.
    if (!state.is_fast && !isSlowMotion && !isReverseNormalSpeed) {
        state.last_speed = absPlaybackRate;
        state.is_fast = false;
        state.smooth_stripe_height = -1.0;
        state.smooth_stripe_spacing = -1.0;
        if (!isNewFrame && state.hold_timer > 0) {
            state.hold_timer = std::max(0, state.hold_timer - 1);
        } else {
            state.hold_timer = 0;
        }
        return effectApplied;
    }

    // Stripe parameter calculation
    // Scale effect geometry with the actual frame height: stripe sizes / spacings /
    // margins / jitter are proportional to resolution, so the full-res look matches the
    // 480p proxy scaled up. (Previously capped to 480p, which froze artefact sizes and
    // made full-res miniature.) Stripe positions are still calculated against textureHeight.
    const int effectiveHeight = textureHeight;
    const double resolutionScale = static_cast<double>(effectiveHeight) / 1080.0;
    // Slow motion stripe: ~25% of 480p frame height = 120px, regardless of actual res
    const int slowMotionStripeHeight = std::max(1, effectiveHeight / 4);
    const int baseStripeHeight = static_cast<int>(85 * resolutionScale);
    const int baseStripeSpacing = static_cast<int>(450 * resolutionScale);
    const int minStripeSpacing = static_cast<int>(62 * resolutionScale);
    int currentMinStripeHeight = std::max(1, static_cast<int>(14.0 * resolutionScale));
    const int midStripeHeight = static_cast<int>(50 * resolutionScale);

    int stripeHeight = baseStripeHeight;
    int stripeSpacing = baseStripeSpacing;

    // SLOW MOTION (< 0.9×) OR 1× REVERSE: Single stripe
    if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
        // At pause (0×) or very slow: fixed stripe height
        // Slight variation based on speed for visual interest
        double slowFactor = absPlaybackRate / 0.9;  // 0.0 at pause, 1.0 at 0.9×
        // Stripe gets slightly smaller as speed increases toward normal
        stripeHeight = static_cast<int>(slowMotionStripeHeight * (1.0 - slowFactor * 0.3));
        stripeHeight = std::max(currentMinStripeHeight, stripeHeight);
        stripeSpacing = baseStripeSpacing;
    } else if (absPlaybackRate >= 1.1 && absPlaybackRate < 2.0) {
        double t = (absPlaybackRate - 1.1) / 0.9;
        t = t * t * (3 - 2 * t);
        stripeHeight = static_cast<int>(baseStripeHeight * (1.0 + (1.0 - t) * 0.5));
        stripeSpacing = baseStripeSpacing;
    } else if (absPlaybackRate >= 2.0 && absPlaybackRate < 3.7) {
        double t = (absPlaybackRate - 2.0) / 1.7;
        stripeHeight = static_cast<int>(baseStripeHeight * (1.0 - t * 0.25));
        stripeSpacing = static_cast<int>(baseStripeSpacing * (1.0 - t * 0.2));
    } else if (absPlaybackRate >= 3.7 && absPlaybackRate < 14.0) {
        // Mid shuttle (≈4–14×) previously thinned out too aggressively, which read as
        // "unnaturally thin" and gave the emulation away. Keep the grey band fuller:
        // gentle LINEAR taper toward a thicker mid-shuttle target (not the very thin
        // currentMinStripeHeight). Continuous with the ≥14× plateau (24·scale) below.
        double t = (absPlaybackRate - 3.7) / 10.3;
        const int midShuttleHeight = static_cast<int>(24 * resolutionScale);
        stripeHeight = static_cast<int>(baseStripeHeight * 0.75 * (1.0 - t) + midShuttleHeight * t);
        stripeSpacing = static_cast<int>(baseStripeSpacing * 0.8 * (1.0 - t) + minStripeSpacing * t);
    } else { // ≥ 14.0x
        stripeSpacing = minStripeSpacing;

        // Fuller through ≈8–14×, then thin out again across 15–18× (thin as it used
        // to be), continuing to a very thin band at extreme speeds. fullMid matches the
        // 3.7–14× branch at 14× for a seamless join; the sqrt curve makes the band thin
        // quickly right after 14× so 15–18× already reads as thin.
        const double fullMidPixels = 24.0; // join with 3.7–14× branch at 14×
        const double thinPixels    = 10.0; // 15–18× thin band (as it used to be)
        const double minPixels     = 6.0;  // extreme speeds (24×+)

        double targetMinPixels;
        if (absPlaybackRate <= 18.0) {
            double t = (absPlaybackRate - 14.0) / (18.0 - 14.0);  // 0 at 14×, 1 at 18×
            t = clamp_val(t, 0.0, 1.0);
            t = std::sqrt(t);  // quick initial thinning right after 14×
            targetMinPixels = fullMidPixels - (fullMidPixels - thinPixels) * t;
        } else if (absPlaybackRate <= 28.0) {
            double t = (absPlaybackRate - 18.0) / (28.0 - 18.0);
            t = clamp_val(t, 0.0, 1.0);
            targetMinPixels = thinPixels - (thinPixels - minPixels) * t;
        } else {
            targetMinPixels = minPixels;
        }

        currentMinStripeHeight = std::max(1, static_cast<int>(targetMinPixels * resolutionScale));
        stripeHeight = currentMinStripeHeight;
    }

    // === PHYSICS-BASED STRIPE POSITIONING ===
    // Based on helical scan: stripes = track boundaries on tape
    //
    // Key formula: stripe_spacing = frame_height / playback_rate
    // - At 2×: spacing = 50% of frame (1 stripe visible, divides frame in half)
    // - At 10×: spacing = 10% of frame (≈8-10 stripes evenly distributed)
    //
    // Rendering consideration: display runs at 60fps, video may be 25fps
    // So we need to calculate based on audio time, not frame count
    //
    // Stripe direction:
    // - Forward playback: stripes move TOP to BOTTOM
    // - Reverse playback: stripes move BOTTOM to TOP

    double fps_for_calc = (metrics.frame_rate > 1.0) ? metrics.frame_rate : 25.0;
    bool is_reverse = metrics.is_reverse;

    // Calculate stripe scroll phase from audio time
    // frame_exact = audio_time × fps gives exact frame position
    // fractional part (0.0-1.0) = position between frames = stripe position
    //
    // Example: paused at 600.012 sec, 25fps
    //   frame_exact = 600.012 × 25 = 15000.3
    //   fractional = 0.3 → stripe at 30% from top
    //
    // At pause: stripe is FIXED (audio time doesn't change)
    // At slow motion: stripe moves slowly with audio time
    // At shuttle: stripes move fast with audio time

    // === ANTI-STROBE ===
    // Two regimes for the shuttle comb (cosmetic; actual playback is governed by audio):
    //  • STEADY 3× and 10× (the integer "analysis" speeds the owner parks on): a SMOOTH wall-clock
    //    glide — the comb drifts at a constant rate in stripe-periods/second, accumulated from REAL
    //    time so it's refresh/fps/RESOLUTION-independent (tuned at the 540p base; phase-based so it
    //    looks identical on proxy 540p and full-res). The narrow ±0.25 bands sit ENTIRELY inside a
    //    constant stripe-count range (count changes at ~2.3/3.3 and ~9.3/10.3 fall outside), so no
    //    new stripe forms while smooth → nothing is given away.
    //  • EVERY OTHER 2–14× speed: the multiplier-detune masking — its slight per-frame beat HIDES
    //    the formation of new grey stripes during speed ramps (the smooth glide exposed that).
    //  • ≥14×: race straight off audio time (dense comb already masks any beat).
    // Slow-mo & 1× reverse keep their frame-tied phase.
    double frame_exact = currentTime * fps_for_calc;
    double raw_scroll_phase;
    if (isSlowMotion) {
        // SLOW MOTION: tie the visible seam to the DISPLAYED frame (same basis as the compositing
        // seam above) so it never wraps ahead of the composite frames → no per-new-frame spike.
        raw_scroll_phase = (currentTime * fps_for_calc) - static_cast<double>(frame_ctx.frame_number);
        if (raw_scroll_phase < 0.0) raw_scroll_phase = 0.0;
        else if (raw_scroll_phase > 1.0) raw_scroll_phase = 1.0;
    } else if (isReverseNormalSpeed) {
        // 1× reverse: slow downward wall-clock glide, shared with the compositing seam above so the
        // grey bar and the composite boundary move together (computed once as reverse_glide_phase).
        raw_scroll_phase = reverse_glide_phase;
    } else if (absPlaybackRate < 14.0) {
        // Smooth only within ±0.25 of 3× or 10×; masking elsewhere. Band edges are crossed only
        // while RAMPING, where a 1-frame seam is invisible.
        bool smoothBand = (std::abs(absPlaybackRate - 3.0) < 0.25) ||
                          (std::abs(absPlaybackRate - 10.0) < 0.25);
        if (smoothBand) {
            // Constant VISIBLE comb drift = kGlideHz periods/sec (÷ stripe count). 540p-tuned, but
            // phase-based → identical at any resolution.
            constexpr double kGlideHz = 1.4;
            double N = std::max(1.0, std::round(absPlaybackRate - 1.0));   // visible stripe count in-band
            auto now = std::chrono::steady_clock::now();
            double dt = state.shuttle_comb_init
                      ? std::chrono::duration<double>(now - state.shuttle_comb_last_t).count()
                      : 0.0;
            state.shuttle_comb_last_t = now;
            state.shuttle_comb_init = true;
            if (dt < 0.0 || dt > 0.1) dt = 0.0;
            double dir = metrics.is_reverse ? -1.0 : 1.0;
            state.shuttle_comb_phase += dir * kGlideHz * dt / N;
            state.shuttle_comb_phase -= std::floor(state.shuttle_comb_phase);
            raw_scroll_phase = state.shuttle_comb_phase;
        } else {
            // Masking multiplier-detune: tiny per-frame beat hides new-stripe formation on ramps.
            constexpr double kRefreshHz     = 60.0;
            constexpr double kGlidePerFrame = 0.06;
            double advancePerFrame = absPlaybackRate * fps_for_calc / kRefreshHz;
            double mult = 1.0;
            if (advancePerFrame > kGlidePerFrame) {
                double base    = std::round(advancePerFrame - kGlidePerFrame);
                double desired = base + kGlidePerFrame;
                if (desired > 0.05) mult = desired / advancePerFrame;
            }
            raw_scroll_phase = std::fmod(currentTime * fps_for_calc * mult, 1.0);
            if (raw_scroll_phase < 0) raw_scroll_phase += 1.0;
            // Seed the accumulator so ENTERING a smooth band continues seamlessly from here.
            state.shuttle_comb_phase   = raw_scroll_phase;
            state.shuttle_comb_last_t  = std::chrono::steady_clock::now();
            state.shuttle_comb_init    = true;
        }
    } else {
        // ≥14×: race straight off audio time.
        raw_scroll_phase = std::fmod(frame_exact, 1.0);
        if (raw_scroll_phase < 0) raw_scroll_phase += 1.0;
        state.shuttle_comb_phase   = raw_scroll_phase;
        state.shuttle_comb_last_t  = std::chrono::steady_clock::now();
        state.shuttle_comb_init    = true;
    }

    double scroll_phase = raw_scroll_phase;
    state.prev_scroll_phase = scroll_phase;
    state.smoothed_scroll_phase = scroll_phase;

    // Number of stripes based on playback rate
    // Based on helical scan physics:
    // - At 1×: head reads 1 track = 0 track boundaries = 0 stripes
    // - At 2×: head crosses 2 tracks = 1 boundary = 1 stripe
    // - At 3×: head crosses 3 tracks = 2 boundaries = 2 stripes
    // - At Nx: N-1 stripes (track boundaries crossed)
    // Formula: num_stripes = speed - 1
    //
    // Use fractional stripe count to allow smooth transitions at integer boundaries
    // e.g., at 2.8x → 1.8 stripes (1 full + 0.8 partial)
    //       at 3.2x → 2.2 stripes (2 full + 0.2 partial)
    double fractional_stripes = 0.0;
    int num_stripes = 0;
    double partial_stripe_opacity = 0.0;  // Opacity of the "newest" stripe (0.0-1.0)

    if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
        // Slow motion or 1× reverse: single stripe (head switching noise bar).
        // EXCEPTION: a pure pause snapped to a frame boundary (frame_aligned) = the head is tracking
        // the recorded frame perfectly → NO head-switch stripe. Without this the stripe sits at
        // scroll_phase≈0 and leaves a sliver at the top edge (the stripe didn't fully vanish). Matches
        // the compositing skip above, which already goes clean when aligned.
        bool aligned_pause = (rawPlaybackRate < 0.05) && metrics.frame_aligned && !isReverseNormalSpeed;
        num_stripes = aligned_pause ? 0 : 1;
        fractional_stripes = static_cast<double>(num_stripes);
    } else if (absPlaybackRate > 1.1) {
        // Fast motion: stripes = track boundaries crossed = speed - 1
        int targetSpacing = static_cast<int>(18.0 * resolutionScale);
        int estimatedStripeHeight = std::max(2, static_cast<int>(stripeHeight * 0.8));
        int stripeUnit = estimatedStripeHeight + targetSpacing;
        int minStripesForSpacing = std::max(1, textureHeight / stripeUnit);

        // Helical scan physics: fractional stripes = speed - 1
        // This allows smooth transitions at integer boundaries
        fractional_stripes = absPlaybackRate - 1.0;
        if (fractional_stripes < 1.0) fractional_stripes = 1.0;

        // Base integer stripes (floor of fractional)
        int baseStripes = static_cast<int>(std::floor(fractional_stripes));

        // Partial stripe opacity = fractional part
        // e.g., 2.3 stripes → 2 full + 0.3 opacity partial
        partial_stripe_opacity = fractional_stripes - baseStripes;

        // Gradual transition from 14x to 24x for dense stripe mode
        if (absPlaybackRate < 14.0) {
            num_stripes = baseStripes;
            // Add partial stripe if opacity > threshold (gradual appearance)
            if (partial_stripe_opacity > 0.3) {
                num_stripes = baseStripes + 1;
                // Remap opacity: 0.3-1.0 → 0.0-1.0
                partial_stripe_opacity = (partial_stripe_opacity - 0.3) / 0.7;
            } else {
                partial_stripe_opacity = 0.0;  // No partial stripe yet
            }
        } else if (absPlaybackRate >= 24.0) {
            num_stripes = std::max(baseStripes, minStripesForSpacing);
            partial_stripe_opacity = 0.0;  // All stripes full at high speeds
        } else {
            // Smooth interpolation from 14x to 24x
            double t = (absPlaybackRate - 14.0) / (24.0 - 14.0);
            t = t * t * (3.0 - 2.0 * t);
            int targetStripes = std::max(baseStripes, minStripesForSpacing);
            num_stripes = static_cast<int>(baseStripes + (targetStripes - baseStripes) * t);
            num_stripes = std::max(baseStripes, num_stripes);
            partial_stripe_opacity = 0.0;
        }
    }
    // else: 0.9-1.1× = normal playback, no stripes

    // stripePositions: {startY, height, hasTopEdge, hasBottomEdge}
    // hasTopEdge/hasBottomEdge indicate if noise edge should be drawn at that boundary
    std::vector<std::tuple<int, int, bool, bool>> stripePositions;

    // Smooth stripe height for visual consistency
    // BUT: in slow motion, ALWAYS set height immediately (no smooth transition)
    // This prevents the stripe from shrinking/growing when stopping from fast speed
    double targetStripeHeight = static_cast<double>(stripeHeight);

    if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
        // Slow motion / 1× reverse: always use target height immediately
        state.smooth_stripe_height = targetStripeHeight;
    } else if (state.smooth_stripe_height < 0.0) {
        // First initialization
        state.smooth_stripe_height = targetStripeHeight;
    } else {
        // Fast/normal speeds: smooth transitions
        double smoothingAlpha = (absPlaybackRate < 10.0) ? 0.18 : 0.4;
        state.smooth_stripe_height += smoothingAlpha * (targetStripeHeight - state.smooth_stripe_height);
    }
    int finalStripeHeight = std::max(1, static_cast<int>(std::round(state.smooth_stripe_height)));

    // Generate stripe positions - distributed across frame with individual variation
    // Spacing = frame_height / num_stripes (e.g., at 10×: spacing = 10% of height)
    // Each stripe has individual random offset for uneven, authentic look
    for (int i = 0; i < num_stripes; i++) {
        // Each stripe offset by (i / num_stripes) of the frame
        // All stripes scroll together with scroll_phase
        double stripe_offset = static_cast<double>(i) / static_cast<double>(num_stripes);
        double stripe_phase = std::fmod(scroll_phase + stripe_offset, 1.0);

        // Convert phase to Y position with extended travel
        // Extended travel ensures stripe smoothly enters/exits visible area
        // instead of suddenly appearing at edges
        int stripe_y;
        if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
            // Slow motion / 1× reverse: larger margin for frame alignment
            int extraMargin = static_cast<int>(10.0 * (effectiveHeight / 480.0));
            int travel_distance = textureHeight + finalStripeHeight + extraMargin * 2;
            stripe_y = static_cast<int>(stripe_phase * travel_distance) - finalStripeHeight / 2 - extraMargin;
        } else {
            // Shuttle: extended travel so stripe exits smoothly at edges
            // At phase=0: stripe fully above visible area
            // At phase=1: stripe fully below visible area
            int travel_distance = textureHeight + finalStripeHeight;
            stripe_y = static_cast<int>(stripe_phase * travel_distance) - finalStripeHeight / 2;
        }

        // Per-slot pseudo-random that is STABLE across frames, so the comb of stripes
        // scrolls smoothly instead of teleporting every frame (the per-frame randomInt was
        // the visible "jitter"). Derived from the slot index i → each slot keeps its
        // uneven offset/height while the whole comb glides. Slow-mo/pause still uses
        // per-frame randomness (tape instability on a held, non-scrolling frame).
        auto slotRand = [&](int salt) -> uint32_t {
            uint32_t h = (static_cast<uint32_t>(i) * 2654435761u)
                       ^ (static_cast<uint32_t>(salt) * 40503u);
            h ^= h >> 13; h *= 2246822519u; h ^= h >> 16; return h;
        };

        // Individual position offset for uneven (analog) spacing.
        if (absPlaybackRate >= 1.1) {
            int maxJitter = std::max(1, static_cast<int>(2.5 * (effectiveHeight / 480.0)));
            int slotOffset = static_cast<int>(slotRand(1) % static_cast<uint32_t>(2 * maxJitter + 1)) - maxJitter;
            stripe_y += slotOffset;
        }

        // Stripe height variation.
        int height_variation = 0;
        if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
            // Slow motion/pause/1× reverse: per-frame variation (tape head tracking instability)
            int variation = std::max(2, static_cast<int>(finalStripeHeight * 0.10));
            height_variation = randomInt(-variation, variation);
        } else if (absPlaybackRate >= 16.0) {
            int variation = std::max(1, static_cast<int>(0.6 * resolutionScale));
            height_variation = static_cast<int>(slotRand(2) % static_cast<uint32_t>(2 * variation + 1)) - variation;
        } else if (absPlaybackRate >= 4.0) {
            int variation = std::max(1, static_cast<int>(finalStripeHeight * 0.05));
            height_variation = static_cast<int>(slotRand(2) % static_cast<uint32_t>(2 * variation + 1)) - variation;
        } else if (absPlaybackRate >= 1.1) {
            // Low shuttle speeds (1.1x-4x): moderate, but stable per slot
            int variation = std::max(1, static_cast<int>(finalStripeHeight * 0.04));
            height_variation = static_cast<int>(slotRand(2) % static_cast<uint32_t>(2 * variation + 1)) - variation;
        }
        int this_stripe_height = std::max(1, finalStripeHeight + height_variation);

        // Center stripe on calculated Y position
        int startY = stripe_y - this_stripe_height / 2;

        // Handle edge cases - NO WRAP-AROUND
        // Stripe should smoothly exit at bottom and enter at top (or vice versa)
        // stripePositions format: {startY, height, hasTopEdge, hasBottomEdge}
        if (startY < 0) {
            // Stripe partially above top edge - only show visible portion
            int visible_height = this_stripe_height + startY;
            if (visible_height > 0) {
                // Partial stripe at top: NO top edge (clipped), YES bottom edge (true edge)
                stripePositions.emplace_back(std::make_tuple(0, visible_height, false, true));
            }
            // NO wrap to bottom - stripe exits at top
        } else if (startY + this_stripe_height > textureHeight) {
            // Stripe partially below bottom edge - only show visible portion
            int visible_height = textureHeight - startY;
            if (visible_height > 0) {
                // Partial stripe at bottom: YES top edge (true edge), NO bottom edge (clipped)
                stripePositions.emplace_back(std::make_tuple(startY, visible_height, true, false));
            }
            // NO wrap to top - stripe exits at bottom
        } else {
            // Normal case - stripe fully visible, both edges are true stripe edges
            stripePositions.emplace_back(std::make_tuple(startY, this_stripe_height, true, true));
        }
    }

    if (!stripePositions.empty()) {
        effectApplied = true;
    }

    // B&W zones under stripes (2x-10x)
    if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
        for (const auto& pos : stripePositions) {
            int startY = std::get<0>(pos);
            int currentStripeHeight = std::get<1>(pos);
            if (currentStripeHeight <= 0) {
                continue;
            }

            int bwZoneHeight = static_cast<int>(currentStripeHeight * 1.75);
            int heightDifference = bwZoneHeight - currentStripeHeight;
            int y_bw = startY - heightDifference / 2;

            if (y_bw < textureHeight && y_bw + bwZoneHeight > 0) {
                int bwStartY = std::max(0, y_bw);
                int bwEndY = std::min(textureHeight, y_bw + bwZoneHeight);
                for (int yPos = bwStartY; yPos < bwEndY; ++yPos) {
                    uint8_t* rowStart = dst_data[0] + yPos * pitch;
                    if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                        for (int i = 0; i < textureWidth / 2; ++i) {
                            uint8_t* group = rowStart + i * 4;
                            group[0] = 128;
                            group[1] = static_cast<uint8_t>(group[1] * 0.85f);
                            group[2] = 128;
                            group[3] = static_cast<uint8_t>(group[3] * 0.85f);
                        }
                    } else {
                        for (int x = 0; x < textureWidth; ++x) {
                            rowStart[x] = static_cast<uint8_t>(rowStart[x] * 0.85f);
                        }
                        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                            int uvY = yPos / 2;
                            if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                                std::memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2);
                                std::memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2);
                            }
                        } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                            int uvY = yPos / 2;
                            if (dst_data[1] && uvY < textureHeight / 2) {
                                uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                                for (int x = 0; x < textureWidth / 2; ++x) {
                                    uvPlane[x * 2] = 128;
                                    uvPlane[x * 2 + 1] = 128;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Grey stripes with noise edges
    // Solid grey zone reduced by ~25%, noise edges only at TRUE stripe boundaries
    for (const auto& pos : stripePositions) {
        int startY = std::get<0>(pos);
        int stripeH = std::get<1>(pos);
        bool hasTopEdge = std::get<2>(pos);      // true = draw noise at top
        bool hasBottomEdge = std::get<3>(pos);   // true = draw noise at bottom
        int endY = startY + stripeH;

        if (startY < 0 || startY >= textureHeight) {
            continue;
        }
        if (endY > textureHeight) {
            endY = textureHeight;
        }
        if (stripeH <= 0) {
            continue;
        }

        // Calculate edge zones with INDEPENDENT variation for top and bottom
        // Base: ~12% of stripe height each
        int baseEdgeHeight = std::max(1, stripeH * 12 / 100);

        // Independent variation for top and bottom edges (not synchronized)
        int topEdgeVariation = 0;
        int bottomEdgeVariation = 0;
        if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
            // SLOW MOTION / 1× REVERSE: Use persistent offset for asymmetry + independent jitter
            int persistentShift = static_cast<int>(state.grey_zone_offset * stripeH * 0.5);
            topEdgeVariation = persistentShift + randomInt(-2, 2);
            bottomEdgeVariation = -persistentShift + randomInt(-2, 2);  // Opposite direction
        } else {
            // FAST SHUTTLE: Random per-frame variation
            topEdgeVariation = randomInt(-baseEdgeHeight/3, baseEdgeHeight/3);
            bottomEdgeVariation = randomInt(-baseEdgeHeight/3, baseEdgeHeight/3);
        }

        int topEdgeHeight = hasTopEdge ? std::max(1, baseEdgeHeight + topEdgeVariation) : 0;
        int bottomEdgeHeight = hasBottomEdge ? std::max(1, baseEdgeHeight + bottomEdgeVariation) : 0;

        int solidStartY = startY + topEdgeHeight;
        int solidEndY = endY - bottomEdgeHeight;

        // Ensure solid zone exists (at least 50% of stripe height)
        if (solidEndY - solidStartY < stripeH / 2) {
            solidStartY = startY + stripeH / 4;
            solidEndY = endY - stripeH / 4;
        }

        // === RAGGED GREY EDGE (single head-switch step per edge) ===
        // One horizontal break on top + one on bottom. Left of break = "free" (noise edge
        // shows); right of break = grey fully filled (stepped by ragStep). The break X is
        // re-rolled every frame from the xorshift RNG → aggressive dropout jitter, not a
        // smooth slide. Skip on thin/very-fast stripes and where scanline-duplication takes
        // over (>=14×).
        bool doRagged = (absPlaybackRate < 14.0) &&
                        ((solidEndY - solidStartY) >= (3 * jagAmp + 2));
        int ragStep   = doRagged ? std::max(1, randomInt(1, jagAmp)) : 0;  // 1–2 px @360
        int ragBreakT = doRagged ? randomInt(0, textureWidth - 1) : 0;     // top break X (xorshift)
        int ragBreakB = doRagged ? randomInt(0, textureWidth - 1) : 0;     // bottom break X (xorshift)

        // === SATELLITE STRIPE (disintegration effect) ===
        // Thin grey lines appear in noise edge zone - works at all speeds
        // More frequent at slow motion, less frequent at fast speeds
        // On pause: use cooldown to prevent satellite from appearing "stuck"
        int satelliteY = -1;  // -1 = no satellite
        int satelliteHeight = 0;

        // Cooldown mechanism for pause mode to prevent persistent satellite
        static thread_local int satelliteCooldown = 0;
        static thread_local int satelliteActiveFrames = 0;

        if (absPlaybackRate < 0.9) {
            // Pause/slow motion: satellite appears briefly then has cooldown
            if (satelliteCooldown > 0) {
                satelliteCooldown--;
            } else if (satelliteActiveFrames > 0) {
                // Show satellite for a few frames
                satelliteActiveFrames--;
                satelliteHeight = std::max(1, static_cast<int>(std::lround(randomInt(1, 2) * effectScale)));
                bool atTop = randomInt(0, 1) == 0;
                if (atTop && hasTopEdge && topEdgeHeight > satelliteHeight + 2) {
                    satelliteY = startY + randomInt(2, topEdgeHeight - satelliteHeight - 1);
                } else if (!atTop && hasBottomEdge && bottomEdgeHeight > satelliteHeight + 2) {
                    satelliteY = solidEndY + randomInt(1, bottomEdgeHeight - satelliteHeight - 1);
                }
                if (satelliteActiveFrames == 0) {
                    satelliteCooldown = randomInt(60, 180);  // 1-3 second cooldown at 60fps
                }
            } else if (randomInt(0, 100) < 3) {  // 3% chance to start satellite
                satelliteActiveFrames = randomInt(3, 8);  // Show for 3-8 frames
            }
        } else {
            // Fast speeds: normal per-frame chance (no cooldown needed, stripe moves)
            int satelliteChance = (absPlaybackRate < 4.0) ? 8 :
                                  (absPlaybackRate < 10.0) ? 5 : 3;
            if (randomInt(0, 100) < satelliteChance) {
                satelliteHeight = std::max(1, static_cast<int>(std::lround(randomInt(1, 3) * effectScale)));
                bool atTop = randomInt(0, 1) == 0;
                if (atTop && hasTopEdge && topEdgeHeight > satelliteHeight + 2) {
                    satelliteY = startY + randomInt(2, topEdgeHeight - satelliteHeight - 1);
                } else if (!atTop && hasBottomEdge && bottomEdgeHeight > satelliteHeight + 2) {
                    satelliteY = solidEndY + randomInt(1, bottomEdgeHeight - satelliteHeight - 1);
                }
            }
        }

        for (int y = startY; y < endY; ++y) {
            uint8_t* rowStart = dst_data[0] + y * pitch;

            // Determine if this line is in edge zone, solid zone, or satellite
            bool isTopEdge = hasTopEdge && (y < solidStartY);
            bool isBottomEdge = hasBottomEdge && (y >= solidEndY);
            bool isSatellite = (satelliteY >= 0) && (y >= satelliteY) && (y < satelliteY + satelliteHeight);
            bool isEdgeZone = (isTopEdge || isBottomEdge) && !isSatellite;

            // SATELLITE STRIPE: Render as solid grey (thin disintegration line)
            if (isSatellite) {
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = 0; i < textureWidth / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        group[0] = 128; group[1] = 128; group[2] = 128; group[3] = 128;
                    }
                } else {
                    std::memset(rowStart, 128, textureWidth);
                    if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                        int uvY = y / 2;
                        if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                            std::memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2);
                            std::memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2);
                        }
                    } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                        int uvY = y / 2;
                        if (dst_data[1] && uvY < textureHeight / 2) {
                            uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                            for (int x = 0; x < textureWidth / 2; ++x) {
                                uvPlane[x * 2] = 128;
                                uvPlane[x * 2 + 1] = 128;
                            }
                        }
                    }
                }
            } else if (isEdgeZone) {
                // NOISE EDGE: subtle noise + strong desaturation.
                // In slow-mo / pause / 1× reverse the picture INSIDE the edge (where the
                // satellites sit) should read darker AND more contrasty "like at 10×":
                // crush blacks + stretch the luma, and mix toward a darker pedestal (96)
                // instead of mid-grey 128. Shuttle keeps the original mid-grey wash.
                const bool slowEdges = (absPlaybackRate < 0.9 || isReverseNormalSpeed);
                const int  edgePed   = slowEdges ? 96 : 128;
                auto edgeY = [&](int v) -> int {
                    return slowEdges ? static_cast<int>((v - 32) * 1.12f) : v;
                };
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = 0; i < textureWidth / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        // Strong desaturation, very subtle noise
                        float keepOriginal = 0.55f + (randomInt(0, 10) / 100.0f);
                        int noise = randomInt(-3, 3);  // Very subtle noise
                        group[0] = static_cast<uint8_t>(clamp_val(128 + noise, 122, 134));  // U toward grey
                        group[1] = static_cast<uint8_t>(clamp_val(
                            static_cast<int>(edgeY(group[1]) * keepOriginal + edgePed * (1.0f - keepOriginal)), 16, 235));
                        group[2] = static_cast<uint8_t>(clamp_val(128 + noise, 122, 134));  // V toward grey
                        group[3] = static_cast<uint8_t>(clamp_val(
                            static_cast<int>(edgeY(group[3]) * keepOriginal + edgePed * (1.0f - keepOriginal)), 16, 235));
                    }
                } else {
                    // IYUV/NV12: Strong desaturation, subtle noise to Y plane
                    for (int x = 0; x < textureWidth; ++x) {
                        int noise = randomInt(-4, 4);  // Very subtle
                        float keepOriginal = 0.50f + (randomInt(0, 15) / 100.0f);  // 50-65% original
                        int origVal = edgeY(rowStart[x]);
                        int newVal = static_cast<int>(origVal * keepOriginal + (edgePed + noise) * (1.0f - keepOriginal));
                        rowStart[x] = static_cast<uint8_t>(clamp_val(newVal, 16, 235));
                    }
                    // UV planes: 80% desaturation in edge zones (very strong)
                    if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                        int uvY = y / 2;
                        if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                            uint8_t* uRow = dst_data[1] + uvY * dst_linesize[1];
                            uint8_t* vRow = dst_data[2] + uvY * dst_linesize[2];
                            for (int x = 0; x < textureWidth / 2; ++x) {
                                // 80% desaturation (20% original + 80% grey)
                                uRow[x] = static_cast<uint8_t>((uRow[x] + 128 * 4) / 5);
                                vRow[x] = static_cast<uint8_t>((vRow[x] + 128 * 4) / 5);
                            }
                        }
                    } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                        int uvY = y / 2;
                        if (dst_data[1] && uvY < textureHeight / 2) {
                            uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                            for (int x = 0; x < textureWidth / 2; ++x) {
                                uvPlane[x * 2] = static_cast<uint8_t>((uvPlane[x * 2] + 128 * 4) / 5);
                                uvPlane[x * 2 + 1] = static_cast<uint8_t>((uvPlane[x * 2 + 1] + 128 * 4) / 5);
                            }
                        }
                    }
                }
            } else {
                // SOLID GREY ZONE (center of stripe)
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = 0; i < textureWidth / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        group[0] = 128;
                        group[1] = static_cast<uint8_t>(group[1] * 0.5f + 64.0f);
                        group[2] = 128;
                        group[3] = static_cast<uint8_t>(group[3] * 0.5f + 64.0f);
                    }
                } else {
                    std::memset(rowStart, 128, textureWidth);
                    if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                        int uvY = y / 2;
                        if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                            std::memset(dst_data[1] + uvY * dst_linesize[1], 128, textureWidth / 2);
                            std::memset(dst_data[2] + uvY * dst_linesize[2], 128, textureWidth / 2);
                        }
                    } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                        int uvY = y / 2;
                        if (dst_data[1] && uvY < textureHeight / 2) {
                            uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                            for (int x = 0; x < textureWidth / 2; ++x) {
                                uvPlane[x * 2] = 128;
                                uvPlane[x * 2 + 1] = 128;
                            }
                        }
                    }
                }
            }
        }

        // Apply the single-step ragged edges: the "filled" side (right of the break) gets
        // grey extended by ragStep into the noise-edge zone; the "free" side (left of the
        // break) keeps the noise edge. One step on top, one on bottom.
        if (doRagged) {
            if (hasTopEdge)
                for (int d = 1; d <= ragStep; ++d) paintGreyRun(solidStartY - d, ragBreakT, textureWidth);
            if (hasBottomEdge)
                for (int d = 0; d <  ragStep; ++d) paintGreyRun(solidEndY + d, ragBreakB, textureWidth);
        }

        // === EXTERNAL MICRO-SATELLITES ===
        // Thin desaturated lines that appear OUTSIDE the stripe area
        // These are separate from the main stripe, appearing above/below it
        int extSatChance = (absPlaybackRate < 0.9) ? 18 :
                           (absPlaybackRate < 4.0) ? 12 :
                           (absPlaybackRate < 10.0) ? 8 : 5;

        if (randomInt(0, 100) < extSatChance) {
            // External satellite above the stripe (1-3 pixels before startY)
            int extSatDistance = randomInt(2, 6);  // Distance from stripe
            int extSatY = startY - extSatDistance;
            if (extSatY >= 0 && extSatY < textureHeight) {
                uint8_t* rowStart = dst_data[0] + extSatY * pitch;
                // Desaturated line (blend toward grey, ~60% desaturation)
                for (int x = 0; x < textureWidth; ++x) {
                    int origVal = rowStart[x];
                    rowStart[x] = static_cast<uint8_t>((origVal * 2 + 128 * 3) / 5);
                }
                // Desaturate UV
                if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                    int uvY = extSatY / 2;
                    if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                        uint8_t* uRow = dst_data[1] + uvY * dst_linesize[1];
                        uint8_t* vRow = dst_data[2] + uvY * dst_linesize[2];
                        for (int x = 0; x < textureWidth / 2; ++x) {
                            uRow[x] = static_cast<uint8_t>((uRow[x] + 128 * 3) / 4);
                            vRow[x] = static_cast<uint8_t>((vRow[x] + 128 * 3) / 4);
                        }
                    }
                } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                    int uvY = extSatY / 2;
                    if (dst_data[1] && uvY < textureHeight / 2) {
                        uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                        for (int x = 0; x < textureWidth / 2; ++x) {
                            uvPlane[x * 2] = static_cast<uint8_t>((uvPlane[x * 2] + 128 * 3) / 4);
                            uvPlane[x * 2 + 1] = static_cast<uint8_t>((uvPlane[x * 2 + 1] + 128 * 3) / 4);
                        }
                    }
                }
            }
        }

        if (randomInt(0, 100) < extSatChance) {
            // External satellite below the stripe (1-3 pixels after endY)
            int extSatDistance = randomInt(2, 6);
            int extSatY = endY + extSatDistance;
            if (extSatY >= 0 && extSatY < textureHeight) {
                uint8_t* rowStart = dst_data[0] + extSatY * pitch;
                for (int x = 0; x < textureWidth; ++x) {
                    int origVal = rowStart[x];
                    rowStart[x] = static_cast<uint8_t>((origVal * 2 + 128 * 3) / 5);
                }
                if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) {
                    int uvY = extSatY / 2;
                    if (dst_data[1] && dst_data[2] && uvY < textureHeight / 2) {
                        uint8_t* uRow = dst_data[1] + uvY * dst_linesize[1];
                        uint8_t* vRow = dst_data[2] + uvY * dst_linesize[2];
                        for (int x = 0; x < textureWidth / 2; ++x) {
                            uRow[x] = static_cast<uint8_t>((uRow[x] + 128 * 3) / 4);
                            vRow[x] = static_cast<uint8_t>((vRow[x] + 128 * 3) / 4);
                        }
                    }
                } else if (lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                    int uvY = extSatY / 2;
                    if (dst_data[1] && uvY < textureHeight / 2) {
                        uint8_t* uvPlane = dst_data[1] + uvY * dst_linesize[1];
                        for (int x = 0; x < textureWidth / 2; ++x) {
                            uvPlane[x * 2] = static_cast<uint8_t>((uvPlane[x * 2] + 128 * 3) / 4);
                            uvPlane[x * 2 + 1] = static_cast<uint8_t>((uvPlane[x * 2 + 1] + 128 * 3) / 4);
                        }
                    }
                }
            }
        }

        // Snow effect inside stripes
        // For fast speeds (>=4x): regular snow
        // For slow motion/pause (<0.9x): subtle snow (electromagnetic dropouts)
        bool showSnow = (absPlaybackRate >= 4.0) || (absPlaybackRate < 0.9);
        if (showSnow) {
            int snowCount;
            int tailBase;
            int tailLength;

            if (absPlaybackRate < 0.9) {
                // SLOW MOTION/PAUSE: Very subtle snow (1-3 particles, short tails)
                // Simulates electromagnetic dropouts during still frame
                snowCount = randomInt(1, 3);
                tailBase = 5 + randomInt(0, 8);
                tailLength = tailBase;
            } else {
                // FAST SHUTTLE: Regular snow
                snowCount = std::max(8, textureWidth / 80);
                if (absPlaybackRate > 10.0) {
                    snowCount = static_cast<int>(snowCount * 1.5);
                }
                double speedFactor = std::sqrt(std::max(1.0, absPlaybackRate));
                tailBase = 10 + randomInt(0, 19);
                tailLength = tailBase + static_cast<int>(speedFactor * 5.0);
            }

            // Resolution scaling: snow is authored at the 480p scale. Scale the head
            // thickness and tail length by effectScale so a particle keeps its 480p
            // proportions on full-res frames. No-op at <=480p (effectScale == 1).
            const int snowThickness = std::max(1, static_cast<int>(std::lround(effectScale)));
            tailLength = std::max(1, static_cast<int>(tailLength * effectScale));

            // Pick Y position for snow
            int snowY = startY;
            for (int j = 0; j < snowCount; ++j) {
                int snowX = randomInt(0, std::max(0, textureWidth - 1));

                // Snow ALWAYS on first or last line of solid grey
                // (boundary between solid grey and noise edge)
                // This is the authentic behavior for all speeds
                int choice = randomInt(0, 2);  // 0=top, 1=bottom, 2=both (pick one)
                if (choice == 0) {
                    snowY = solidStartY;  // First line of solid grey
                } else if (choice == 1) {
                    snowY = std::max(solidStartY, solidEndY - 1);  // Last line of solid grey
                } else {
                    // "Both" - alternate between particles
                    snowY = (j % 2 == 0) ? solidStartY : std::max(solidStartY, solidEndY - 1);
                }

                // Head (k==0, bright 235) + horizontal tail, drawn over snowThickness rows
                // so the particle keeps its 480p proportions at higher resolutions.
                for (int k = 0; k < tailLength; ++k) {
                    int xPos = snowX + k;
                    if (xPos >= textureWidth) {
                        break;
                    }
                    int brightness;
                    if (k == 0) {
                        brightness = 235;
                    } else {
                        // Fade over the SCALED length so the tail shape matches 480p.
                        double fadeFactor = std::exp(-0.15 * k / effectScale);
                        brightness = 128 + static_cast<int>(107 * fadeFactor);
                    }
                    for (int dy = 0; dy < snowThickness; ++dy) {
                        int yRow = snowY + dy;
                        if (yRow < 0 || yRow >= textureHeight) {
                            continue;
                        }
                        uint8_t* rowStart = dst_data[0] + yRow * pitch;
                        if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                            int group_idx = xPos / 2;
                            int in_group_idx = xPos % 2;
                            uint8_t* group = rowStart + group_idx * 4;
                            if (group_idx * 4 + (1 + in_group_idx * 2) < pitch) {
                                group[0] = 128;
                                group[1 + in_group_idx * 2] = static_cast<uint8_t>(brightness);
                                group[2] = 128;
                            }
                        } else {
                            rowStart[xPos] = static_cast<uint8_t>(brightness);
                        }
                    }
                }
            }
        }

        // Thin, FAINT dark line just below the solid grey (head-switch point).
        // Kept barely visible (request: "even thinner, barely visible") — a soft 1 px line,
        // not a near-black bar. Follows the ragged grey bottom when raggedness is active.
        if (absPlaybackRate >= 2.0) {
            // Soft darkness: a gentle dip from grey, never approaching black.
            uint8_t targetYValue;
            if (absPlaybackRate < 4.0) {
                double t = (absPlaybackRate - 2.0) / 2.0;  // 0.0 at 2x, 1.0 at 4x
                t = t * t;  // Smooth ease-in
                targetYValue = static_cast<uint8_t>(122 - t * 18);  // 122 → 104
            } else if (absPlaybackRate < 8.0) {
                double t = (absPlaybackRate - 4.0) / 4.0;
                targetYValue = static_cast<uint8_t>(104 - t * 14);  // 104 → 90
            } else {
                targetYValue = 90;  // 8x+: faint, barely-visible line
            }

            auto drawDarkRun = [&](int y, int x0, int x1) {
                if (y < 0 || y >= textureHeight) return;
                x0 = std::max(0, x0);
                x1 = std::min(textureWidth, x1);
                if (x1 <= x0) return;
                uint8_t* rowStart = dst_data[0] + y * pitch;
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = x0 / 2; i < (x1 + 1) / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        group[1] = targetYValue;
                        group[3] = targetYValue;
                    }
                } else {
                    std::memset(rowStart + x0, targetYValue, x1 - x0);
                }
            };

            // Follow the ragged bottom (sit just under the deepest grey bulge of each
            // segment); otherwise a single straight line at solidEndY.
            if (doRagged && hasBottomEdge) {
                // Follow the single bottom step: base line on the free part, stepped line on
                // the filled part.
                drawDarkRun(solidEndY, 0, ragBreakB);
                drawDarkRun(solidEndY + ragStep, ragBreakB, textureWidth);
            } else {
                drawDarkRun(solidEndY, 0, textureWidth);
            }
        }
    }

    // === SCANLINE DUPLICATION EFFECT ===
    // Gradual effect starting at 14x, full intensity at 24x+
    // Fills entire areas BETWEEN grey stripes with duplicated scanlines
    // At 10x+ image is already B&W (desaturated above), so no chroma manipulation needed
    constexpr double kScanlineStartSpeed = 14.0;
    constexpr double kScanlineFullSpeed = 24.0;

    if (absPlaybackRate >= kScanlineStartSpeed && pitch > 0) {
        // Calculate effect intensity: 0.0 at 14x, 1.0 at 24x+
        double scanlineIntensity = (absPlaybackRate - kScanlineStartSpeed) / (kScanlineFullSpeed - kScanlineStartSpeed);
        scanlineIntensity = std::min(1.0, std::max(0.0, scanlineIntensity));

        // Get original frame planes for sampling
        const uint8_t* origYPlane = nullptr;
        const uint8_t* origUPlane = nullptr;
        const uint8_t* origVPlane = nullptr;
        int origYPitch = pitch;
        int origUPitch = (dst_linesize[1] > 0) ? dst_linesize[1] : pitch;
        int origVPitch = (dst_linesize[2] > 0) ? dst_linesize[2] : pitch;

        if (frame_ctx.source_frame) {
            origYPlane = frame_ctx.source_frame->data[0];
            origUPlane = frame_ctx.source_frame->data[1];
            origVPlane = frame_ctx.source_frame->data[2];
            origYPitch = frame_ctx.source_frame->linesize[0];
            if (frame_ctx.source_frame->linesize[1] > 0) {
                origUPitch = frame_ctx.source_frame->linesize[1];
            }
            if (frame_ctx.source_frame->linesize[2] > 0) {
                origVPitch = frame_ctx.source_frame->linesize[2];
            }
        }

        // Build stripe mask to protect grey stripes from duplication
        std::vector<bool> stripeMask(textureHeight, false);
        for (const auto& pos : stripePositions) {
            int startY = std::max(0, std::get<0>(pos));
            int endY = std::min(textureHeight, std::get<0>(pos) + std::get<1>(pos));

            // Mark the stripe itself
            for (int yy = startY; yy < endY; ++yy) {
                stripeMask[yy] = true;
            }

            // Also protect dark outline line (endY) from scanline duplication
            if (absPlaybackRate >= 2.0 && endY < textureHeight) {
                stripeMask[endY] = true;
            }
        }

        // Find clear areas (gaps between stripes) and fill with duplicated scanlines
        bool inClearArea = false;
        int clearStart = 0;
        for (int y = 0; y < textureHeight; ++y) {
            if (!stripeMask[y] && !inClearArea) {
                clearStart = y;
                inClearArea = true;
            }

            bool reachedStripe = stripeMask[y];
            bool reachedEnd = (y == textureHeight - 1);
            if ((reachedStripe || reachedEnd) && inClearArea) {
                int clearEnd = reachedStripe ? y : (y + 1);
                if (clearEnd > textureHeight) {
                    clearEnd = textureHeight;
                }

                int clearHeight = clearEnd - clearStart;
                // Apply intensity - at lower speeds, only fill portion of clear area
                int fillHeight = static_cast<int>(clearHeight * scanlineIntensity);

                if (fillHeight > 1) {
                    // Sample from middle of fill zone
                    int sampleY = std::clamp(clearStart, 0, textureHeight - 1);

                    if (origYPlane) {
                        const uint8_t* sourceLinePtr = origYPlane + sampleY * origYPitch;
                        uint8_t* destRow = dst_data[0] + clearStart * pitch;
                        std::memcpy(destRow, sourceLinePtr, std::min(pitch, origYPitch));

                        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV || lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                            int sampleChromaY = sampleY / 2;
                            size_t chromaWidth = (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) ? textureWidth / 2 : textureWidth;
                            if (origUPlane && dst_data[1] && dst_linesize[1] > 0 && sampleChromaY < textureHeight / 2) {
                                uint8_t* dstU = dst_data[1] + (clearStart / 2) * dst_linesize[1];
                                std::memset(dstU, 128, chromaWidth);
                            }
                            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV && origVPlane && dst_data[2] && dst_linesize[2] > 0 && sampleChromaY < textureHeight / 2) {
                                uint8_t* dstV = dst_data[2] + (clearStart / 2) * dst_linesize[2];
                                std::memset(dstV, 128, textureWidth / 2);
                            }
                        }
                    }

                    // Duplicate the sampled line to fill the area
                    for (int destY = clearStart + 1; destY < clearStart + fillHeight && destY < clearEnd; ++destY) {
                        std::memcpy(dst_data[0] + destY * pitch, dst_data[0] + clearStart * pitch, pitch);

                        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV || lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                            int destChromaY = destY / 2;
                            int seedChromaY = clearStart / 2;

                            if (dst_data[1] && dst_linesize[1] > 0 && destChromaY < textureHeight / 2) {
                                const uint8_t* seedChromaPtr = dst_data[1] + seedChromaY * dst_linesize[1];
                                uint8_t* destChromaPtr = dst_data[1] + destChromaY * dst_linesize[1];
                                size_t chromaWidth = (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) ? textureWidth / 2 : textureWidth;
                                std::memcpy(destChromaPtr, seedChromaPtr, chromaWidth);
                                std::memset(destChromaPtr, 128, chromaWidth);
                            }

                            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV && dst_data[2] && dst_linesize[2] > 0 && destChromaY < textureHeight / 2) {
                                const uint8_t* seedChromaPtrV = dst_data[2] + seedChromaY * dst_linesize[2];
                                uint8_t* destChromaPtrV = dst_data[2] + destChromaY * dst_linesize[2];
                                std::memcpy(destChromaPtrV, seedChromaPtrV, textureWidth / 2);
                                std::memset(destChromaPtrV, 128, textureWidth / 2);
                            }
                        }
                    }
                }

                inClearArea = false;
            }
        }
    }

    // Edge fade moved to FSTPBetacamEffect::ApplyEdgeFade(), applied by the pixel-buffer
    // manager on EVERY frame (incl. 1×) so the soft L/R border is always present and doesn't
    // snap sharp at normal speed.

    state.last_frame_number = frame_ctx.frame_number;
    state.last_new_frame = frame_ctx.new_frame;
    state.last_speed = absPlaybackRate;
    if (!isNewFrame && state.hold_timer > 0) {
        state.hold_timer = std::max(0, state.hold_timer - 1);
    }
    state.is_fast = (rawPlaybackRate >= fastThreshold);

    return effectApplied;
}

bool FSTPBetacamEffect::ApplyRenderJitter(int player_id, RenderContext& render_ctx) {
    if (!m_enabled || !render_ctx.dest_rect) {
        return false;
    }

    PlayerState& state = GetPlayerState(player_id);
    const PlaybackMetrics& metrics = state.metrics;

    double absSpeed = std::abs(metrics.playback_rate);
    bool isSlowMotion = (absSpeed < 0.9);
    bool isFastShuttle = (absSpeed >= kEffectThreshold);

    // Effect applies to slow motion OR fast shuttle; 1× reverse treated like 1× forward.
    if (!isSlowMotion && !isFastShuttle) {
        return false;
    }

    SDL_Rect& dst = *render_ctx.dest_rect;

    // Slight aspect correction just in case
    if (render_ctx.target_aspect_ratio > 0.0f && render_ctx.window_width > 0 && render_ctx.window_height > 0) {
        float aspectRatio = render_ctx.target_aspect_ratio;
        if (render_ctx.window_width / aspectRatio <= render_ctx.window_height) {
            dst.w = render_ctx.window_width;
            dst.h = static_cast<int>(render_ctx.window_width / aspectRatio);
        } else {
            dst.h = render_ctx.window_height;
            dst.w = static_cast<int>(render_ctx.window_height * aspectRatio);
        }
        dst.x = (render_ctx.window_width - dst.w) / 2;
        dst.y = (render_ctx.window_height - dst.h) / 2;
    }

    bool modified = false;

    // Jitter amplitude - subtle and realistic (like real Betacam SP)
    // Real Betacam tape machines have very subtle vertical jitter during shuttle
    double jitterAmplitude = 0.0;
    if (absSpeed >= 1.3 && absSpeed < 2.0) {
        // Low shuttle speeds: minimal jitter (was 10-19px, now 0-1px)
        double t = (absSpeed - 1.3) / 0.7;
        jitterAmplitude = 0.0 + t * 1.0;  // 0 to 1 pixel
    } else if (absSpeed >= 2.0 && absSpeed < 4.0) {
        // Medium speeds: subtle jitter (was 2px, now 0.8-1.2px)
        jitterAmplitude = 1.0;
    } else if (absSpeed >= 4.0 && absSpeed < 16.0) {
        // High speeds: gradual increase (was 1.4-3.6px, now 1.0-2.0px)
        double t = (absSpeed - 4.0) / 12.0;
        jitterAmplitude = 1.0 + t * 1.0;  // 1.0 to 2.0 pixels
    } else if (absSpeed >= 16.0) {
        // Very high speeds: moderate jitter (was missing, now 2.0-2.5px)
        double t = std::min(1.0, (absSpeed - 16.0) / 16.0);  // clamp at 32x
        jitterAmplitude = 2.0 + t * 0.5;  // 2.0 to 2.5 pixels max
    }

    if (jitterAmplitude > 0.0 && render_ctx.new_frame) {
        modified = true;
        if (absSpeed >= 0.20 && absSpeed < 1.0) {
            // Very slow speeds: small alternating offset
            int baseOffset = static_cast<int>(std::floor(jitterAmplitude));
            int offset = (render_ctx.frame_number % 2 == 0) ? baseOffset : -baseOffset;
            dst.y += offset;
        } else if (absSpeed >= 1.3 && absSpeed < 2.0) {
            // 1.3x-2.0x: very subtle random jitter (was skipped entirely)
            std::normal_distribution<double> normalDist(0.0, jitterAmplitude);
            int jitter = static_cast<int>(std::round(normalDist(state.rng)));
            dst.y += jitter;
        } else {
            // All other speeds: random jitter with normal distribution
            std::normal_distribution<double> normalDist(0.0, jitterAmplitude);
            int jitter = static_cast<int>(std::round(normalDist(state.rng)));
            dst.y += jitter;
        }
    }

    // === SLOW MOTION: image is elastically PULLED + a touch of rigid nudge ===
    // A real Betacam transport's vertical servo "hunts" at slow speed: the picture is
    // mostly STRETCHED (one edge anchored, the opposite edge dragged) but the whole raster
    // also JUMPS a little the same way. We combine both — a big edge-anchored stretch
    // (pull) plus a small whole-frame translate (shift, the original idea) — for a more
    // aggressive, more realistic hunt. Triangle timing kept (0 at frame boundaries, max
    // mid-frame — deliberately NOT a sine; that linear ramp is the Betacam character).
    //   Forward → pull/nudge DOWN  (anchor top,    stretch bottom edge down)
    //   Reverse → pull/nudge UP    (anchor bottom, stretch top    edge up)
    if (isSlowMotion && metrics.frame_rate > 0) {
        double fps = metrics.frame_rate;
        double currentTime = metrics.position_seconds;

        // Calculate scroll_phase (same as stripe positioning)
        double frame_exact = currentTime * fps;
        double scroll_phase = std::fmod(frame_exact, 1.0);
        if (scroll_phase < 0) scroll_phase += 1.0;

        // Triangle wave: 0 at boundaries, max at center
        // scroll_phase 0.0 → 0.5: pull increases
        // scroll_phase 0.5 → 1.0: pull relaxes back to 0
        double triangle = (scroll_phase < 0.5)
                         ? (scroll_phase * 2.0)         // 0→1 as phase goes 0→0.5
                         : (2.0 - scroll_phase * 2.0);  // 1→0 as phase goes 0.5→1

        // Dragged edge STRETCHES (pull); whole frame also NUDGES a little the same way
        // (shift, the old rigid translate). Combined = jump + stretch = aggressive hunt.
        constexpr double maxPull  = 9.0;  // stretch on the dragged edge
        constexpr double maxShift = 3.0;  // small whole-frame translate (the "old idea")
        int pull  = static_cast<int>(std::round(triangle * maxPull));
        int shift = static_cast<int>(std::round(triangle * maxShift));

        if (pull > 0 || shift > 0) {
            if (metrics.is_reverse) {
                dst.y -= (pull + shift);  // top dragged up by pull + frame nudged up by shift
                dst.h += pull;            // bottom ends up nudged up by shift only
            } else {
                dst.y += shift;           // whole frame nudged down by shift
                dst.h += pull;            // bottom dragged down by pull (top only by shift)
            }
            modified = true;
        }
    }

    return modified;
}

// ========== HSYNC LOSS EFFECT (Restored from original effects_renderer.mm) ==========

void FSTPBetacamEffect::UpdateHsyncEffect(PlayerState& state, double abs_playback_rate, double /*fps*/,
                                           bool allow_new_triggers) {
    constexpr std::chrono::milliseconds HSYNC_MIN_INTERVAL(300);  // Reduced: 300ms between hsync triggers

    auto now = std::chrono::steady_clock::now();

    // Detect abrupt speed changes that trigger hsync
    double speedDelta = std::abs(abs_playback_rate - state.last_speed);
    bool abruptSpeedChange = false;

    // Trigger on: slow→fast (jump > 1.5x) or fast→slow/stop (drop from >2x to <1x)
    if (speedDelta > 1.5) {
        abruptSpeedChange = true;
    } else if (state.last_speed > 2.0 && abs_playback_rate < 1.0) {
        abruptSpeedChange = true;
    } else if (state.last_speed < 1.0 && abs_playback_rate > 2.0) {
        abruptSpeedChange = true;
    }

    // HSync loss occurs in 1.5x-2.2x speed range OR on abrupt speed change
    bool hsyncConditionMet = (abs_playback_rate >= 1.5 && abs_playback_rate <= 2.2);
    bool shouldTrigger = hsyncConditionMet || abruptSpeedChange;

    if (shouldTrigger && !state.hsync.hsyncLossActive && allow_new_triggers) {
        if (now - state.hsync.hsyncLastTriggerTime > HSYNC_MIN_INTERVAL) {
            // Random chance to trigger:
            // - Abrupt speed change: 1 in 4 chance (25%) - more frequent but not always
            // - Normal speed range (1.5-2.2x): 1 in 30 chance (~3%)
            bool doTrigger = false;
            if (abruptSpeedChange) {
                std::uniform_int_distribution<> distrib(1, 4);
                doTrigger = (distrib(state.rng) == 1);
            } else {
                std::uniform_int_distribution<> distrib(1, 30);
                doTrigger = (distrib(state.rng) == 1);
            }

            if (doTrigger) {
                // Trigger hsync loss
                state.hsync.hsyncLossActive = true;
                state.hsync.hsyncEffectStartTime = now;

                // Faster duration: 100ms - 180ms (was 400-600ms)
                state.hsync.hsyncEffectDurationMs = 100 + (state.rng() % 81);

                state.hsync.hsyncLastTriggerTime = now;
            }
        }
    }

    // Update active hsync effect (time-based, independent of frame rate)
    if (state.hsync.hsyncLossActive) {
        // Calculate elapsed time since effect started
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state.hsync.hsyncEffectStartTime
        ).count();

        // Calculate progress (0.0 to 1.0) based on wall-clock time
        float overallProgress = static_cast<float>(elapsed) /
                                static_cast<float>(state.hsync.hsyncEffectDurationMs);
        overallProgress = std::max(0.0f, std::min(1.0f, overallProgress));

        // Skew follows sine wave (smooth in/out)
        state.hsync.hsyncCurrentSkew = state.hsync.hsyncMaxSkewAmount * std::sin(overallProgress * M_PI);

        // Tear line wobbles
        state.hsync.tearLineNormalized = 0.5f + 0.3f * std::sin(overallProgress * M_PI * 4.0f);

        // End effect when duration reached
        if (elapsed >= state.hsync.hsyncEffectDurationMs) {
            state.hsync.hsyncLossActive = false;
            state.hsync.hsyncCurrentSkew = 0.0f;
        }
    } else {
        state.hsync.hsyncCurrentSkew = 0.0f;
    }
}

bool FSTPBetacamEffect::RenderWithHsync(
    int player_id,
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    int texture_width,
    int texture_height,
    const SDL_Rect& dest_rect
) {
    if (!m_enabled || !renderer || !texture) {
        return false;
    }

    const PlayerState& state = GetPlayerState(player_id);
    const HsyncEffectState& hsync = state.hsync;

    // Check if hsync effect is active
    if (!hsync.hsyncLossActive || hsync.hsyncMaxSkewAmount == 0.0f ||
        dest_rect.h <= 0 || texture_height <= 0) {
        // No hsync effect - return false so caller does normal render
        return false;
    }

    // SELF-EXPIRING, WALL-CLOCK DRIVEN — fixes the long-standing "horizontal shift freezes"
    // bug. RenderWithHsync runs on every presented frame, but UpdateHsyncEffect (which
    // advances/animates and clears the effect) only runs when a NEW source frame is
    // processed. When the display refreshes without a new frame (pause, slow source, a speed
    // change that produces no new frames) the effect's timer never advanced and this drew a
    // FROZEN max-skew tear forever. Recompute progress from the wall clock here so the tear
    // always rolls out and disappears on time, independent of when UpdateHsyncEffect last
    // ran. (hsyncLossActive itself is cleared later by UpdateHsyncEffect.)
    int durationMs = (hsync.hsyncEffectDurationMs > 0) ? hsync.hsyncEffectDurationMs : 150;
    double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - hsync.hsyncEffectStartTime).count();
    if (elapsedMs >= durationMs) {
        // Lifetime over — render normally, never a frozen tear.
        return false;
    }
    float progress = static_cast<float>(elapsedMs / durationMs);
    progress = std::max(0.0f, std::min(1.0f, progress));
    // Smooth in/out skew + tear-line wobble, recomputed locally (cached values only update
    // on new frames, so relying on them is what allowed the freeze).
    float skewMag  = hsync.hsyncMaxSkewAmount * std::sin(progress * static_cast<float>(M_PI));
    float tearNorm = 0.5f + 0.3f * std::sin(progress * static_cast<float>(M_PI) * 4.0f);

    // Calculate tear line position in screen coordinates
    int actualTearLineScreenY = dest_rect.y + static_cast<int>(tearNorm * dest_rect.h);
    actualTearLineScreenY = std::max(dest_rect.y, std::min(actualTearLineScreenY, dest_rect.y + dest_rect.h - 1));

    // ========== PART 1: Below the tear (renders normally) ==========
    SDL_Rect srcBelow, dstBelow;
    dstBelow.x = dest_rect.x;
    dstBelow.y = actualTearLineScreenY;
    dstBelow.w = dest_rect.w;
    dstBelow.h = (dest_rect.y + dest_rect.h) - actualTearLineScreenY;

    srcBelow.x = 0;
    srcBelow.y = static_cast<int>((static_cast<float>(dstBelow.y - dest_rect.y) / dest_rect.h) * texture_height);
    srcBelow.w = texture_width;
    srcBelow.h = static_cast<int>((static_cast<float>(dstBelow.h) / dest_rect.h) * texture_height);

    if (srcBelow.y + srcBelow.h > texture_height) {
        srcBelow.h = texture_height - srcBelow.y;
    }

    if (srcBelow.h > 0 && dstBelow.h > 0) {
        SDL_RenderCopy(renderer, texture, &srcBelow, &dstBelow);
    }

    // ========== PART 2: Above the tear (skewed, line-by-line) ==========
    int topPartScreenHeight = actualTearLineScreenY - dest_rect.y;
    if (topPartScreenHeight > 0) {
        for (int y_screen = dest_rect.y; y_screen < actualTearLineScreenY; ++y_screen) {
            SDL_Rect srcLine, dstLine;

            // Calculate skew for this line (increases towards tear line)
            float normalizedYInSkewArea = static_cast<float>(actualTearLineScreenY - 1 - y_screen) /
                                          std::max(1, topPartScreenHeight - 1);
            float currentLineSkew = skewMag * normalizedYInSkewArea;

            // Destination line (horizontally shifted)
            dstLine.x = dest_rect.x + static_cast<int>(currentLineSkew);
            dstLine.y = y_screen;
            dstLine.w = dest_rect.w;
            dstLine.h = 1;

            // Source line (from texture)
            srcLine.x = 0;
            srcLine.y = static_cast<int>((static_cast<float>(y_screen - dest_rect.y) / dest_rect.h) * texture_height);
            srcLine.w = texture_width;
            srcLine.h = 1;

            // Clipping: handle lines shifted outside dest_rect
            if (dstLine.x < dest_rect.x) {
                int offset = dest_rect.x - dstLine.x;
                srcLine.x += static_cast<int>((float)offset / dstLine.w * srcLine.w);
                srcLine.w -= static_cast<int>((float)offset / dstLine.w * srcLine.w);
                dstLine.w -= offset;
                dstLine.x = dest_rect.x;
            }
            if (dstLine.x + dstLine.w > dest_rect.x + dest_rect.w) {
                int overflow = (dstLine.x + dstLine.w) - (dest_rect.x + dest_rect.w);
                srcLine.w -= static_cast<int>((float)overflow / dstLine.w * srcLine.w);
                dstLine.w -= overflow;
            }

            // Render line if valid
            if (srcLine.y >= 0 && srcLine.y < texture_height && srcLine.w > 0 && dstLine.w > 0) {
                SDL_RenderCopy(renderer, texture, &srcLine, &dstLine);
            }
        }
    }

    // Return true to indicate we handled the rendering
    return true;
}
