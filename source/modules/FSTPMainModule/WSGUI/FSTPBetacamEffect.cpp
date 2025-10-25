#include "FSTPBetacamEffect.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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

    const bool isNewFrame = frame_ctx.new_frame;
    if (isNewFrame) {
        double fps = (metrics.frame_rate > 1.0) ? metrics.frame_rate : 60.0;
        int holdFrames = static_cast<int>(std::round(fps * 0.5));
        holdFrames = std::clamp(holdFrames, 15, 90);
        state.hold_timer = holdFrames;
    }

    bool holdActive = (!isNewFrame && state.hold_timer > 0);
    bool timelineValid = (currentTime > kMinActiveTime) &&
                         (totalDuration <= 0.0 || (totalDuration - currentTime) > kMinActiveTime);

    if (!holdActive) {
        if (rawPlaybackRate < kEffectThreshold || !timelineValid) {
            state.last_speed = rawPlaybackRate;
            state.is_fast = false;
            state.smooth_stripe_height = -1.0;
            state.smooth_stripe_spacing = -1.0;
            state.hold_timer = 0;
            return false;
        }
    }

    double absPlaybackRate = rawPlaybackRate;
    if (holdActive && rawPlaybackRate < kEffectThreshold) {
        absPlaybackRate = std::max(state.last_speed, rawPlaybackRate);
    }

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

    if (!state.is_fast) {
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
    const int maxStripeHeight = static_cast<int>(720 * resolutionScale);
    const int baseStripeHeight = static_cast<int>(85 * resolutionScale);
    const int baseStripeSpacing = static_cast<int>(450 * resolutionScale);
    const int minStripeSpacing = static_cast<int>(62 * resolutionScale);
    int currentMinStripeHeight = std::max(1, static_cast<int>(14.0 * resolutionScale));
    const int midStripeHeight = static_cast<int>(50 * resolutionScale);

    int stripeHeight = baseStripeHeight;
    int stripeSpacing = baseStripeSpacing;

    if ((absPlaybackRate >= 0.2 && absPlaybackRate < 0.9) ||
        (absPlaybackRate >= 1.1 && absPlaybackRate < 2.0)) {
        double t = (absPlaybackRate < 0.9)
                       ? (absPlaybackRate - 0.2) / 0.7
                       : (absPlaybackRate - 1.2) / 0.8;
        t = t * t * (3 - 2 * t);
        stripeHeight = static_cast<int>(maxStripeHeight * (1 - t) + midStripeHeight * t);
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
            t = std::clamp(t, 0.0, 1.0);
            targetMinPixels = thickPixels - (thickPixels - thinPixels) * t;
        } else {
            targetMinPixels = thinPixels;
        }

        currentMinStripeHeight = std::max(1, static_cast<int>(targetMinPixels * resolutionScale));
        stripeHeight = currentMinStripeHeight;
    }

    // Cycle progress for scrolling
    double cycleProgress = 0.0;
    const double baseDuration = 1.5;
    if (absPlaybackRate < 4.0) {
        double cycleDuration = baseDuration;
        cycleProgress = std::fmod(currentTime, cycleDuration) / cycleDuration;
    } else if (absPlaybackRate < 10.0) {
        double mediumSpeedDuration = std::max(0.2, baseDuration / (absPlaybackRate / 3.0));
        cycleProgress = std::fmod(currentTime, mediumSpeedDuration) / mediumSpeedDuration;
    } else {
        double adjustedDuration = std::max(0.08, baseDuration / (absPlaybackRate / 6.0));
        cycleProgress = std::fmod(currentTime, adjustedDuration) / adjustedDuration;
    }

    double currentSpacingVariationFactor = 0.0;
    if (absPlaybackRate >= 8.0 && absPlaybackRate < 16.0) {
        currentSpacingVariationFactor = 0.01;
    } else if (absPlaybackRate >= 16.0) {
        currentSpacingVariationFactor = 0.005;
    }

    std::vector<std::pair<int, int>> stripePositions;

    int baseSpacingForSpeed = stripeSpacing;
    const double spacingAt12x = 100.0 * resolutionScale;
    const double spacingAt24x = 18.0 * resolutionScale;

    if (absPlaybackRate >= 2.0 && absPlaybackRate < 3.5) {
        baseSpacingForSpeed = stripeSpacing + static_cast<int>((10.0 + 76.0) * resolutionScale);
    } else if (absPlaybackRate >= 8.0 && absPlaybackRate < 12.0) {
        baseSpacingForSpeed = static_cast<int>(spacingAt12x);
    } else if (absPlaybackRate >= 12.0) {
        double t = std::min(1.0, (absPlaybackRate - 12.0) / (24.0 - 12.0));
        baseSpacingForSpeed = static_cast<int>(spacingAt12x * (1.0 - t) + spacingAt24x * t);
    }

    const int minAllowedSpacing = std::max(1, minStripeSpacing / 4);

    int averagePatternUnitHeight = baseStripeHeight + baseSpacingForSpeed;
    if (averagePatternUnitHeight <= 0) {
        averagePatternUnitHeight = 1;
    }
    double initialOffset = std::fmod(cycleProgress * averagePatternUnitHeight, averagePatternUnitHeight);
    int currentY = static_cast<int>(initialOffset) - averagePatternUnitHeight;

    // Smooth stripe parameters to avoid visible jumps when speed changes slightly (notably < 10x)
    double targetStripeHeight = static_cast<double>(stripeHeight);
    double targetSpacing = static_cast<double>(baseSpacingForSpeed);

    if (absPlaybackRate >= 2.0 && absPlaybackRate < 18.0) {
        double normalized = std::clamp((absPlaybackRate - 2.0) / 16.0, 0.0, 1.0);
        double heightScale = 0.85 + normalized * 0.45;
        double spacingScale = 1.05 - normalized * 0.30;
        double wobblePhase = (2.0 + normalized * 3.0) * kTwoPi * cycleProgress;
        double wobble = 0.04 * std::sin(wobblePhase);
        heightScale += wobble;
        spacingScale += -0.5 * wobble;

        targetStripeHeight = std::max(1.0, targetStripeHeight * heightScale);
        targetSpacing = std::max(static_cast<double>(minAllowedSpacing), targetSpacing * spacingScale);
    }

    double smoothingAlpha = (absPlaybackRate < 10.0) ? 0.18 : 0.4;
    double secondaryAlpha = (absPlaybackRate < 8.0) ? 0.25 : smoothingAlpha;
    if (state.smooth_stripe_height < 0.0) {
        state.smooth_stripe_height = targetStripeHeight;
    } else {
        state.smooth_stripe_height += secondaryAlpha * (targetStripeHeight - state.smooth_stripe_height);
    }
    if (state.smooth_stripe_spacing < 0.0) {
        state.smooth_stripe_spacing = targetSpacing;
    } else {
        state.smooth_stripe_spacing += smoothingAlpha * (targetSpacing - state.smooth_stripe_spacing);
    }
    stripeHeight = std::max(1, static_cast<int>(std::round(state.smooth_stripe_height)));
    baseSpacingForSpeed = std::max(1, static_cast<int>(std::round(state.smooth_stripe_spacing)));

    while (currentY < textureHeight) {
        int finalStripeHeight;
        if (absPlaybackRate >= 16.0) {
            int minHeight = std::max(1, currentMinStripeHeight);
            int variation = std::max(1, static_cast<int>(0.6 * resolutionScale));
            int maxHeight = std::max(minHeight + 1, minHeight + variation);
            finalStripeHeight = randomInt(minHeight, maxHeight);
        } else if (absPlaybackRate < 8.0) {
            int variation = std::max(1, static_cast<int>(stripeHeight * 0.12));
            finalStripeHeight = std::max(1, stripeHeight + randomInt(-variation, variation));
        } else {
            int rand_component = randomInt(-1, 1);
            double heightVariation = rand_component * resolutionScale;
            finalStripeHeight = std::max(static_cast<int>(stripeHeight + heightVariation), currentMinStripeHeight);
        }
        finalStripeHeight = std::max(1, finalStripeHeight);

        int currentStripeSpacing = baseSpacingForSpeed;
        if (absPlaybackRate >= 16.0) {
            int randomOffset = randomInt(-2, 2);
            currentStripeSpacing = std::max(1, baseSpacingForSpeed + randomOffset);
        } else if (absPlaybackRate >= 8.0) {
            double offset = currentSpacingVariationFactor * 2.0 * randomFloat(-0.5, 0.5);
            int randomSpacingOffset = static_cast<int>(baseSpacingForSpeed * offset);
            currentStripeSpacing = std::max(minAllowedSpacing, baseSpacingForSpeed + randomSpacingOffset);
        } else {
            double gentleWave = std::sin((stripePositions.size() * 0.8 + cycleProgress * kTwoPi) * 0.75);
            int gentleOffset = static_cast<int>(std::round(baseSpacingForSpeed * 0.05 * gentleWave));
            currentStripeSpacing = std::max(minAllowedSpacing, baseSpacingForSpeed + gentleOffset);
        }

        int startY = std::max(0, currentY);
        int endY = std::min(textureHeight, currentY + finalStripeHeight);

        if (endY > startY && startY < textureHeight) {
            stripePositions.emplace_back(startY, endY - startY);
        }

        currentY += finalStripeHeight + currentStripeSpacing;
        if (static_cast<int>(stripePositions.size()) > textureHeight) {
            break;
        }
    }

    if (!stripePositions.empty()) {
        effectApplied = true;
    }

    if (!stripePositions.empty()) {
        effectApplied = true;
    }

    // B&W zones under stripes (2x-10x)
    if (absPlaybackRate >= 2.0 && absPlaybackRate < 10.0) {
        for (const auto& pos : stripePositions) {
            int startY = pos.first;
            int currentStripeHeight = pos.second;
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

    // Grey stripes
    for (const auto& pos : stripePositions) {
        int startY = pos.first;
        int stripeH = pos.second;
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

        for (int y = startY; y < endY; ++y) {
            uint8_t* rowStart = dst_data[0] + y * pitch;
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

        // Snow effect inside stripes for fast speeds
        if (absPlaybackRate >= 4.0) {
            int snowY = startY;
            int snowCount = std::max(8, textureWidth / 80);
            if (absPlaybackRate > 10.0) {
                snowCount = static_cast<int>(snowCount * 1.5);
            }
            for (int j = 0; j < snowCount; ++j) {
                int snowX = randomInt(0, std::max(0, textureWidth - 1));
                double speedFactor = std::sqrt(std::max(1.0, absPlaybackRate));
                int tailBase = 10 + randomInt(0, 19);
                int tailLength = tailBase + static_cast<int>(speedFactor * 5.0);

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

        // Black outline under stripes for higher speeds
        if (absPlaybackRate >= 8.0) {
            const double outlineStartSpeed = 8.0;
            const double outlineFullSpeed = 14.0;
            double t = std::clamp((absPlaybackRate - outlineStartSpeed) / (outlineFullSpeed - outlineStartSpeed), 0.0, 1.0);
            uint8_t targetYValue = static_cast<uint8_t>(16 + (128 - 16) * (1.0 - t));

            int currentOutlineHeight = 1;
            for (int h = 0; h < currentOutlineHeight; ++h) {
                int outlineY = endY + h;
                if (outlineY >= textureHeight) {
                    continue;
                }
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

    // Scanline duplication effect (>=16x)
    if (absPlaybackRate >= 22.0 && pitch > 0) {
        const uint8_t* origYPlane = nullptr;
        const uint8_t* origUPlane = nullptr;
        const uint8_t* origVPlane = nullptr;
        int origYPitch = pitch;
        int origUPitch = (dst_linesize[1] > 0) ? dst_linesize[1] : pitch;
        int origVPitch = (dst_linesize[2] > 0) ? dst_linesize[2] : pitch;

        // Access original frame planes when available to keep the first line authentic
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

        std::vector<bool> stripeMask(textureHeight, false);
        for (const auto& pos : stripePositions) {
            int startY = std::max(0, pos.first);
            int endY = std::min(textureHeight, pos.first + pos.second);
            for (int yy = startY; yy < endY; ++yy) {
                stripeMask[yy] = true;
            }
        }

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
                if (clearHeight > 1) {
                    if (origYPlane) {
                        int sampleY = std::clamp(clearStart, 0, textureHeight - 1);
                        const uint8_t* sourceLinePtr = origYPlane + sampleY * origYPitch;
                        uint8_t* destRow = dst_data[0] + clearStart * pitch;
                        std::memcpy(destRow, sourceLinePtr, std::min(pitch, origYPitch));

                        if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV || lastSdlPixFormat == SDL_PIXELFORMAT_NV12) {
                            int sampleChromaY = sampleY / 2;
                            size_t chromaWidth = (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV) ? textureWidth / 2 : textureWidth;
                            if (origUPlane && dst_data[1] && dst_linesize[1] > 0 && sampleChromaY < textureHeight / 2) {
                                const uint8_t* srcU = origUPlane + sampleChromaY * origUPitch;
                                uint8_t* dstU = dst_data[1] + (clearStart / 2) * dst_linesize[1];
                                std::memcpy(dstU, srcU, std::min(chromaWidth, static_cast<size_t>(origUPitch)));
                                std::memset(dstU, 128, chromaWidth);
                            }
                            if (lastSdlPixFormat == SDL_PIXELFORMAT_IYUV && origVPlane && dst_data[2] && dst_linesize[2] > 0 && sampleChromaY < textureHeight / 2) {
                                const uint8_t* srcV = origVPlane + sampleChromaY * origVPitch;
                                uint8_t* dstV = dst_data[2] + (clearStart / 2) * dst_linesize[2];
                                size_t vWidth = std::min(static_cast<size_t>(textureWidth / 2), static_cast<size_t>(origVPitch));
                                std::memcpy(dstV, srcV, vWidth);
                                std::memset(dstV, 128, vWidth);
                            }
                        }
                    }

                    for (int destY = clearStart + 1; destY < clearEnd; ++destY) {
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
    if (absSpeed < kEffectThreshold) {
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

    // Jitter amplitude based on speed bands (mirrors legacy logic roughly)
    double jitterAmplitude = 0.0;
    if (absSpeed >= 1.3 && absSpeed < 1.9) {
        double t = (absSpeed - 1.3) / 0.6;
        jitterAmplitude = 10.0 + t * 9.0;
    } else if (absSpeed >= 1.9 && absSpeed < 4.0) {
        jitterAmplitude = 2.0;
    } else if (absSpeed >= 4.0 && absSpeed < 16.0) {
        double t = (absSpeed - 4.0) / 12.0;
        jitterAmplitude = 1.4 + t * 2.2;
    } else if (absSpeed >= 20.0) {
        jitterAmplitude = 2.0;
    }

    if (jitterAmplitude > 0.0 && render_ctx.new_frame) {
        modified = true;
        if (absSpeed >= 0.20 && absSpeed < 1.0) {
            int baseOffset = static_cast<int>(std::floor(jitterAmplitude));
            int offset = (render_ctx.frame_number % 2 == 0) ? baseOffset : -baseOffset;
            dst.y += offset;
        } else if (absSpeed >= 1.3 && absSpeed < 2.0) {
            // Skip jitter for this band (legacy behaviour) – keep center.
        } else {
            std::normal_distribution<double> normalDist(0.0, jitterAmplitude);
            int jitter = static_cast<int>(std::round(normalDist(state.rng)));
            dst.y += jitter;
        }
    }

    return modified;
}
