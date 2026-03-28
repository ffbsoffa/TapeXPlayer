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
    , hold_timer(0) {}

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
    const double currentTime = metrics.position_seconds;
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
    bool isSlowMotion = (rawPlaybackRate < 0.9) || isReverseNormalSpeed;
    bool isFastShuttle = (rawPlaybackRate >= kEffectThreshold);
    bool shouldShowEffect = isSlowMotion || isFastShuttle;

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

    if (!holdActive) {
        if (!shouldShowEffect || !timelineValid) {
            // При нормальной скорости (1.0×) эффекта нет — гасим hsync немедленно.
            // Без этого UpdateHsyncEffect не вызывается (ранний return),
            // и hsyncCurrentSkew «замерзает» в ненулевом значении.
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

    // Update HSync loss effect state (triggers at 1.5x-2.2x speed)
    double fps = (metrics.frame_rate > 1.0) ? metrics.frame_rate : 60.0;
    UpdateHsyncEffect(state, absPlaybackRate, fps);

    uint8_t** dst_data = frame_ctx.planes;
    int* dst_linesize = frame_ctx.linesize;
    const int textureWidth = frame_ctx.width;
    const int textureHeight = frame_ctx.height;
    const int pitch = frame_ctx.linesize[0];
    const Uint32 lastSdlPixFormat = frame_ctx.pixel_format;

    auto randomInt = [&](int min_val, int max_val) {
        std::uniform_int_distribution<int> dist(min_val, max_val);
        return dist(state.rng);
    };
    auto randomFloat = [&](double min_val, double max_val) {
        std::uniform_real_distribution<double> dist(min_val, max_val);
        return dist(state.rng);
    };

    // === SLOW MOTION FRAME COMPOSITING (DECODER-PROVIDED FRAMES) ===
    // Decoder provides adjacent frames: prev_source_frame (N-1) and next_source_frame (N+1)
    // This eliminates internal frame caching and enables seamless direction changes
    //
    // Physics: Video head scans from top to bottom
    // - Forward: above stripe = current (N), below stripe = prev (N-1)
    // - Reverse: above stripe = next (N+1), below stripe = current (N)
    //
    // Support both planar YUV (IYUV/YV12) and semi-planar (NV12)
    bool isYUVFormat = (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV ||
                        lastSdlPixFormat == SDL_PIXELFORMAT_YV12 ||
                        lastSdlPixFormat == SDL_PIXELFORMAT_NV12);
    bool isNV12 = (lastSdlPixFormat == SDL_PIXELFORMAT_NV12);

    // For compositing we need Y plane and at least U plane (NV12 has UV interleaved)
    bool have_planes = dst_data[0] && dst_data[1] && (isNV12 || dst_data[2]);

    if (isSlowMotion && isYUVFormat && have_planes && textureHeight > 0 && textureWidth > 0) {
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
        double scroll_phase = std::fmod(frame_exact, 1.0);
        if (scroll_phase < 0) scroll_phase += 1.0;
        // At 1× reverse the tape moves backward → invert phase so seam scrolls downward
        if (isReverseNormalSpeed) scroll_phase = 1.0 - scroll_phase;

        // Calculate stripe height to match stripe rendering formula exactly
        double resolutionScale = textureHeight / 480.0;
        int slowMotionStripeHeight = static_cast<int>(textureHeight * 0.25);
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

            for (int y = 0; y < textureHeight; ++y) {
                // ABOVE stripe = composite (N+1); reverse handled by scroll_phase flip
                bool use_composite = (y < stripe_center_y);

                if (use_composite) {
                    // Copy Y plane from composite frame
                    std::memcpy(dst_data[0] + y * pitch,
                               composite_frame->data[0] + y * comp_y_pitch,
                               textureWidth);

                    // Copy UV planes (every 2nd Y line)
                    if (y % 2 == 0) {
                        int uv_y = y / 2;
                        if (isNV12) {
                            // NV12: UV interleaved in single plane, width = textureWidth
                            std::memcpy(dst_data[1] + uv_y * dst_linesize[1],
                                       composite_frame->data[1] + uv_y * comp_uv_pitch,
                                       textureWidth);
                        } else {
                            // Planar YUV: separate U and V planes, width = textureWidth/2
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
        }
        state.last_composited_frame_number = frame_ctx.frame_number;
    }
    skip_compositing:;

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

    // Skip stripe generation ONLY if not fast AND not slow motion
    // Slow motion (< 0.9×) also needs to show the pause stripe
    if (!state.is_fast && !isSlowMotion) {
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
    const double resolutionScale = static_cast<double>(textureHeight) / 1080.0;
    // Slow motion stripe: ~25% of frame height (120px for 480p, 270px for 1080p)
    // This is the "still frame" noise band seen on real Betacam during pause
    const int slowMotionStripeHeight = std::max(1, textureHeight / 4);
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
        stripeHeight = static_cast<int>(baseStripeHeight * (1.0 - t * 0.3));
        stripeSpacing = static_cast<int>(baseStripeSpacing * (1.0 - t * 0.2));
    } else if (absPlaybackRate >= 3.7 && absPlaybackRate < 14.0) {
        double t = (absPlaybackRate - 3.7) / 10.3;
        t = std::pow(t, 0.7);
        stripeHeight = static_cast<int>(baseStripeHeight * 0.7 * (1.0 - t) + currentMinStripeHeight * t);
        stripeSpacing = static_cast<int>(baseStripeSpacing * 0.8 * (1.0 - t) + minStripeSpacing * t);
    } else { // ≥ 14.0x
        stripeSpacing = minStripeSpacing;

        const double thickPixels = 14.0; // desired thickness in lower fast-forward range
        const double thinPixels = 5.0;    // target thickness for extreme speeds

        double targetMinPixels;
        if (absPlaybackRate < 18.0) {
            targetMinPixels = thickPixels;
        } else if (absPlaybackRate <= 24.0) {
            double t = (absPlaybackRate - 18.0) / (24.0 - 18.0);
            t = clamp_val(t, 0.0, 1.0);
            targetMinPixels = thickPixels - (thickPixels - thinPixels) * t;
        } else {
            targetMinPixels = thinPixels;
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

    // === ANTI-FLICKER DETUNING ===
    // At exact integer speeds (2x, 3x, 4x...), stripes can "beat" with 60fps rendering
    // causing visible flicker. Real VCRs added slight offset to avoid this.
    // Based on observations: 3x→3.123x, 4x→4.188x gives smooth motion.
    // Apply detuning for speeds 2x-8x (9x+ already smooth)
    double effectiveSpeedMultiplier = 1.0;
    if (absPlaybackRate >= 2.0 && absPlaybackRate < 9.0) {
        // Add small fractional offset that increases with speed
        // This breaks the integer alignment that causes beating
        double detuneOffset = absPlaybackRate * 0.045;  // ~0.09 at 2x, ~0.36 at 8x
        effectiveSpeedMultiplier = (absPlaybackRate + detuneOffset) / absPlaybackRate;
    }

    double frame_exact = currentTime * fps_for_calc * effectiveSpeedMultiplier;
    double raw_scroll_phase = std::fmod(frame_exact, 1.0);  // 0.0-1.0
    if (raw_scroll_phase < 0) raw_scroll_phase += 1.0;

    // NO inversion needed for reverse!
    // When position_seconds decreases (reverse), scroll_phase naturally decreases
    // → stripe moves from bottom to top (correct reverse behavior)
    // The old inversion was wrong - it reversed the natural reverse direction
    //
    // EXCEPTION: at 1× reverse, tape misalignment causes artifacts scrolling DOWNWARD,
    // so we invert the phase to get the correct top-to-bottom direction.
    if (isReverseNormalSpeed) raw_scroll_phase = 1.0 - raw_scroll_phase;

    // Use raw scroll_phase directly (no temporal smoothing)
    // This ensures compositing seam and stripe are always perfectly aligned
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
        // Slow motion or 1× reverse: single stripe (head switching noise bar)
        num_stripes = 1;
        fractional_stripes = 1.0;
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
            int extraMargin = static_cast<int>(10.0 * (textureHeight / 480.0));
            int travel_distance = textureHeight + finalStripeHeight + extraMargin * 2;
            stripe_y = static_cast<int>(stripe_phase * travel_distance) - finalStripeHeight / 2 - extraMargin;
        } else {
            // Shuttle: extended travel so stripe exits smoothly at edges
            // At phase=0: stripe fully above visible area
            // At phase=1: stripe fully below visible area
            int travel_distance = textureHeight + finalStripeHeight;
            stripe_y = static_cast<int>(stripe_phase * travel_distance) - finalStripeHeight / 2;
        }

        // Individual position jitter for each stripe (uneven spacing)
        // Makes stripes look analog/authentic, not digitally perfect
        // Scale jitter with resolution (3-5 pixels at 480p)
        if (absPlaybackRate >= 1.1) {
            int maxJitter = static_cast<int>(4.0 * (textureHeight / 480.0));
            int positionJitter = randomInt(-maxJitter, maxJitter);
            stripe_y += positionJitter;
        }

        // Add frame-to-frame height variation for realism
        // At slow motion/pause: noticeable variation (tape instability)
        // At fast shuttle: smaller proportional variation
        int height_variation = 0;
        if (absPlaybackRate < 0.9 || isReverseNormalSpeed) {
            // Slow motion/pause/1× reverse: ±8-12% variation (tape head tracking instability)
            int variation = std::max(2, static_cast<int>(finalStripeHeight * 0.10));
            height_variation = randomInt(-variation, variation);
        } else if (absPlaybackRate >= 16.0) {
            int variation = std::max(1, static_cast<int>(0.6 * resolutionScale));
            height_variation = randomInt(-variation, variation);
        } else if (absPlaybackRate >= 4.0) {
            int variation = std::max(1, static_cast<int>(finalStripeHeight * 0.08));
            height_variation = randomInt(-variation, variation);
        } else if (absPlaybackRate >= 1.1) {
            // Low shuttle speeds (1.1x-4x): moderate variation
            int variation = std::max(1, static_cast<int>(finalStripeHeight * 0.06));
            height_variation = randomInt(-variation, variation);
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
                satelliteHeight = randomInt(1, 2);
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
                satelliteHeight = randomInt(1, 3);
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
                // NOISE EDGE: Subtle noise with strong desaturation
                // Minimal digital noise, focus on desaturation
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = 0; i < textureWidth / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        // Strong desaturation, very subtle noise
                        float keepOriginal = 0.55f + (randomInt(0, 10) / 100.0f);
                        int noise = randomInt(-3, 3);  // Very subtle noise
                        group[0] = static_cast<uint8_t>(clamp_val(128 + noise, 122, 134));  // U toward grey
                        group[1] = static_cast<uint8_t>(clamp_val(
                            static_cast<int>(group[1] * keepOriginal + 128 * (1.0f - keepOriginal)), 16, 235));
                        group[2] = static_cast<uint8_t>(clamp_val(128 + noise, 122, 134));  // V toward grey
                        group[3] = static_cast<uint8_t>(clamp_val(
                            static_cast<int>(group[3] * keepOriginal + 128 * (1.0f - keepOriginal)), 16, 235));
                    }
                } else {
                    // IYUV/NV12: Strong desaturation, subtle noise to Y plane
                    for (int x = 0; x < textureWidth; ++x) {
                        int noise = randomInt(-4, 4);  // Very subtle
                        float keepOriginal = 0.50f + (randomInt(0, 15) / 100.0f);  // 50-65% original
                        int origVal = rowStart[x];
                        int newVal = static_cast<int>(origVal * keepOriginal + (128 + noise) * (1.0f - keepOriginal));
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

                if (snowY >= 0 && snowY < textureHeight && snowX >= 0 && snowX < textureWidth) {
                    uint8_t* rowStart = dst_data[0] + snowY * pitch;
                    if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                        int group_idx = snowX / 2;
                        int in_group_idx = snowX % 2;
                        uint8_t* group = rowStart + group_idx * 4;
                        if (group_idx * 4 + (1 + in_group_idx * 2) < pitch) {
                            group[0] = 128;
                            group[1 + in_group_idx * 2] = 235;
                            group[2] = 128;
                        }
                    } else {
                        rowStart[snowX] = 235;
                    }
                }

                for (int k = 1; k < tailLength; ++k) {
                    int xPos = snowX + k;
                    if (xPos >= textureWidth) {
                        continue;
                    }
                    double fadeFactor = std::exp(-0.15 * k);
                    int brightness = 128 + static_cast<int>(107 * fadeFactor);
                    if (snowY >= 0 && snowY < textureHeight && xPos >= 0) {
                        uint8_t* rowStart = dst_data[0] + snowY * pitch;
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

        // Dark outline directly below solid grey zone - gradual appearance from 2x
        // Real Betacam shows thin dark edge at head switch point (boundary of solid grey)
        if (absPlaybackRate >= 2.0) {
            // Calculate darkness: gradual appearance, then darker at higher speeds
            uint8_t targetYValue;
            if (absPlaybackRate < 4.0) {
                // 2x-4x: gradual fade-in from barely visible to noticeable
                double t = (absPlaybackRate - 2.0) / 2.0;  // 0.0 at 2x, 1.0 at 4x
                t = t * t;  // Smooth ease-in
                targetYValue = static_cast<uint8_t>(120 - t * 60);  // 120 → 60
            } else if (absPlaybackRate < 8.0) {
                // 4x-8x: continue darkening
                double t = (absPlaybackRate - 4.0) / 4.0;
                targetYValue = static_cast<uint8_t>(60 - t * 28);  // 60 → 32
            } else {
                targetYValue = 32;  // 8x+: dark but visible
            }

            // Draw at solidEndY (directly after solid grey, within noise edge if present)
            // This keeps it connected to the solid grey zone
            int outlineY = solidEndY;
            if (outlineY >= 0 && outlineY < textureHeight) {
                uint8_t* rowStart = dst_data[0] + outlineY * pitch;
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    for (int i = 0; i < textureWidth / 2; ++i) {
                        uint8_t* group = rowStart + i * 4;
                        group[1] = targetYValue;
                        group[3] = targetYValue;
                    }
                } else {
                    std::memset(rowStart, targetYValue, textureWidth);
                }
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

    // Edge fade
    if ((kEdgeFadeLeft > 0 || kEdgeFadeRight > 0) &&
        textureWidth > (kEdgeFadeLeft + kEdgeFadeRight) && pitch > 0) {
        for (int y = 0; y < textureHeight; ++y) {
            uint8_t* rowStart = dst_data[0] + y * pitch;
            // Left edge
            for (int x = 0; x < static_cast<int>(kEdgeFadeLeft); ++x) {
                float fade = (kEdgeFadeLeft > 1)
                                 ? static_cast<float>(x) / static_cast<float>(kEdgeFadeLeft - 1)
                                 : 1.0f;
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    int group_idx = x / 2;
                    int in_group_idx = x % 2;
                    uint8_t* group = rowStart + group_idx * 4;
                    if (group_idx * 4 + (1 + in_group_idx * 2) < pitch) {
                        uint8_t currentY = group[1 + in_group_idx * 2];
                        group[1 + in_group_idx * 2] =
                            static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                    }
                } else {
                    uint8_t currentY = rowStart[x];
                    rowStart[x] = static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                }
            }
            // Right edge
            for (int x = 0; x < static_cast<int>(kEdgeFadeRight); ++x) {
                int realX = textureWidth - 1 - x;
                float fade = (kEdgeFadeRight > 1)
                                 ? static_cast<float>(x) / static_cast<float>(kEdgeFadeRight - 1)
                                 : 1.0f;
                if (lastSdlPixFormat == SDL_PIXELFORMAT_UYVY) {
                    int group_idx = realX / 2;
                    int in_group_idx = realX % 2;
                    uint8_t* group = rowStart + group_idx * 4;
                    if (group_idx * 4 + (1 + in_group_idx * 2) < pitch) {
                        uint8_t currentY = group[1 + in_group_idx * 2];
                        group[1 + in_group_idx * 2] =
                            static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                    }
                } else {
                    uint8_t currentY = rowStart[realX];
                    rowStart[realX] = static_cast<uint8_t>(currentY * fade + 16.0f * (1.0f - fade));
                }
            }
        }
        effectApplied = true;
    }

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
    bool isSlowMotion = (absSpeed < 0.9) || (absSpeed >= 0.9 && absSpeed <= 1.1 && metrics.is_reverse);
    bool isFastShuttle = (absSpeed >= kEffectThreshold);

    // Effect applies to slow motion OR fast shuttle
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

    // === SLOW MOTION: Image Y offset follows stripe/audio time ===
    // The video frame moves slightly with the stripe position
    // At frame boundary (scroll_phase = 0 or 1): offset = 0
    // At mid-frame (scroll_phase = 0.5): offset = max (±5 pixels)
    if (isSlowMotion && metrics.frame_rate > 0) {
        double fps = metrics.frame_rate;
        double currentTime = metrics.position_seconds;

        // Calculate scroll_phase (same as stripe positioning)
        double frame_exact = currentTime * fps;
        double scroll_phase = std::fmod(frame_exact, 1.0);
        if (scroll_phase < 0) scroll_phase += 1.0;

        // Triangle wave: 0 at boundaries, max at center
        // scroll_phase 0.0 → 0.5: offset increases
        // scroll_phase 0.5 → 1.0: offset decreases back to 0
        double triangle = (scroll_phase < 0.5)
                         ? (scroll_phase * 2.0)         // 0→1 as phase goes 0→0.5
                         : (2.0 - scroll_phase * 2.0);  // 1→0 as phase goes 0.5→1

        // Max offset: ±5 pixels
        constexpr double maxYOffset = 5.0;
        double yOffset = triangle * maxYOffset;

        // Direction: forward = pull down (positive), reverse = pull up (negative)
        if (metrics.is_reverse) {
            yOffset = -yOffset;
        }

        dst.y += static_cast<int>(std::round(yOffset));
        modified = true;
    }

    return modified;
}

// ========== HSYNC LOSS EFFECT (Restored from original effects_renderer.mm) ==========

void FSTPBetacamEffect::UpdateHsyncEffect(PlayerState& state, double abs_playback_rate, double /*fps*/) {
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

    if (shouldTrigger && !state.hsync.hsyncLossActive) {
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

    // Calculate tear line position in screen coordinates
    int actualTearLineScreenY = dest_rect.y + static_cast<int>(hsync.tearLineNormalized * dest_rect.h);
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
            float currentLineSkew = hsync.hsyncMaxSkewAmount * normalizedYInSkewArea;

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
