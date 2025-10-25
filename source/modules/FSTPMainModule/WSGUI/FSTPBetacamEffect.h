#pragma once

#include <SDL2/SDL.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <random>

struct AVFrame;

/**
 * Betacam-style visual effect that emulates tape rewind/fast-forward artefacts.
 *
 * The effect works in two stages:
 *  1. CPU-side pixel mutations applied on decoded YUV planes before the frame is uploaded.
 *  2. Render-time jitter calculations that slightly offset the final destination rectangle.
 *
 * The original implementation lived inside the legacy render loop.  In the new architecture
 * we reuse the effect here and feed the results into FSTPPixelBufferManager / WindowManager.
 */
class FSTPBetacamEffect {
public:
    static constexpr int kMaxPlayers = 5;

    struct PlaybackMetrics {
        double playback_rate = 1.0;     // Signed playback rate (negative for reverse)
        double position_seconds = 0.0;  // Current playback time
        double duration_seconds = 0.0;  // Total duration of clip
        double frame_rate = 25.0;       // Source frame rate (used for timing)
        bool   is_reverse = false;      // Convenience flag for consumers
    };

    struct FrameContext {
        Uint32 pixel_format = SDL_PIXELFORMAT_IYUV;
        int    width = 0;
        int    height = 0;
        int    frame_number = 0;
        bool   new_frame = false;   // true when a new decoded frame was submitted

        uint8_t* planes[3] = {nullptr, nullptr, nullptr};
        int      linesize[3] = {0, 0, 0};
        const AVFrame* source_frame = nullptr;  // Optional original frame for reference
    };

    struct RenderContext {
        SDL_Rect* dest_rect = nullptr;
        int window_width = 0;
        int window_height = 0;
        float target_aspect_ratio = 1.0f;
        int frame_number = 0;
        bool new_frame = false;
    };

    FSTPBetacamEffect();

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void UpdatePlaybackMetrics(int player_id, const PlaybackMetrics& metrics);
    void ResetPlayer(int player_id);

    /**
     * Mutates the provided YUV planes in-place to mimic rewind artefacts.
     * Returns true when effect altered the frame.
     */
    bool ApplyPixelFX(int player_id, FrameContext& frame_ctx);

    /**
     * Adjusts destination rectangle (jitter) for the Betacam look.
     * Returns true when any modification was applied.
     */
    bool ApplyRenderJitter(int player_id, RenderContext& render_ctx);

private:
    struct PlayerState {
        PlaybackMetrics metrics{};
        int last_frame_number = -1;
        bool last_new_frame = false;
        double cycle_offset = 0.0;
        std::mt19937 rng;

        // Jitter control
        double jitter_phase = 0.0;
        int jitter_seed = 0;

        double last_speed = 1.0;
        int transition_timer = 0;
        int transition_direction = 0; // 1 = entering fast, -1 = exiting
        bool transition_use_top = false;
        bool is_fast = false;

        double smooth_stripe_height = -1.0;
        double smooth_stripe_spacing = -1.0;
        int hold_timer = 0;

        PlayerState();
    };

    bool m_enabled = false;
    std::array<PlayerState, kMaxPlayers> m_players;

    static constexpr int kTransitionFrames = 6;

    PlayerState& GetPlayerState(int player_id);
    const PlayerState& GetPlayerState(int player_id) const;
};
