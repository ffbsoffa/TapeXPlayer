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
        double playback_rate = 1.0;          // Signed playback rate (negative for reverse)
        double position_seconds = 0.0;       // Current playback time (includes timecode offset)
        double duration_seconds = 0.0;       // Total duration of clip (does NOT include timecode offset)
        double timecode_offset_seconds = 0.0;// Timecode start offset; subtract from position for file-relative time
        double frame_rate = 25.0;            // Source frame rate (used for timing)
        bool   is_reverse = false;           // Convenience flag for consumers
        bool   frame_aligned = false;        // Audio snapped to frame boundary — stripe gone, render clean frame
    };

    struct FrameContext {
        Uint32 pixel_format = SDL_PIXELFORMAT_IYUV;
        int    width = 0;
        int    height = 0;
        int    frame_number = 0;
        bool   new_frame = false;   // true when a new decoded frame was submitted
        bool   edge_fade = false;   // apply soft L/R border (after compositing, under the stripe)
        bool   smear = false;       // apply horizontal analog smear (after compositing too)

        uint8_t* planes[3] = {nullptr, nullptr, nullptr};
        int      linesize[3] = {0, 0, 0};
        const AVFrame* source_frame = nullptr;       // Current frame for reference
        const AVFrame* prev_source_frame = nullptr;  // Previous frame (N-1) for forward compositing
        const AVFrame* next_source_frame = nullptr;  // Next frame (N+1) for reverse compositing
        int prev_frame_number = -1;                  // Frame number of previous frame
        int next_frame_number = -1;                  // Frame number of next frame
    };

    struct RenderContext {
        SDL_Rect* dest_rect = nullptr;
        int window_width = 0;
        int window_height = 0;
        float target_aspect_ratio = 1.0f;
        int frame_number = 0;
        bool new_frame = false;
    };

    // HSync loss effect state (restored from original effects_renderer.mm)
    // Now uses wall-clock time for animation (not tied to frame changes)
    struct HsyncEffectState {
        bool hsyncLossActive = false;
        std::chrono::steady_clock::time_point hsyncEffectStartTime;  // When effect started
        int hsyncEffectDurationMs = 0;                               // Duration in milliseconds
        std::chrono::steady_clock::time_point hsyncLastTriggerTime;
        float hsyncCurrentSkew = 0.0f;
        float hsyncMaxSkewAmount = 50.0f;  // Maximum horizontal skew in pixels
        float tearLineNormalized = 0.5f;   // Position of tear line (0.0-1.0)
    };

    FSTPBetacamEffect();

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled; }

    void UpdatePlaybackMetrics(int player_id, const PlaybackMetrics& metrics);
    void ResetPlayer(int player_id);

    // Kind of dropout burst: Gentle = soft thin form (resume-from-pause); Alternating =
    // aggressive ↔ light each time (proxy↔full-res switches).
    enum class DropoutKind : int { Gentle = 1, Alternating = 2 };

    // Fire a one-shot dropout-compensation burst (resume-from-pause key, proxy↔full-res switch).
    // Sets a one-shot flag (carrying the kind) consumed on the effect thread inside ApplyPixelFX.
    void RequestDropoutBurst(int player_id, DropoutKind kind = DropoutKind::Alternating);

    // True while a dropout burst is queued or still playing — lets the manager run the effect
    // even at 1× (where it is otherwise gated off) so dropouts show at normal speed too.
    bool HasPendingDropout(int player_id) const;

    // Soft left/right edge fade (border darkening) on the luma plane. Exposed so the manager
    // applies it on EVERY frame (incl. 1×), not only when the gated stripe effect runs — so
    // the soft border doesn't snap sharp at normal speed. Width scales with resolution.
    void ApplyEdgeFade(uint8_t* y_plane, int pitch, int width, int height, uint32_t format);

    // Subtle horizontal analog smear (one-pole IIR per row; luma light, chroma stronger,
    // scales with resolution). Exposed like ApplyEdgeFade so it can run post-composite (so
    // slow-mo compositing doesn't wipe it) and at 1× via the manager.
    void ApplyAnalogSmear(uint8_t* y_plane, int y_pitch,
                          uint8_t* u_plane, uint8_t* v_plane, int u_pitch, int v_pitch,
                          int width, int height, uint32_t format);

    // Modest vertical luma soften (3-tap blur, blended back toward the original).
    // Runs AFTER the grey stripe/dropout bands are composited so their hard
    // horizontal edges read as soft SD, not razor-sharp digital. Luma only.
    void ApplySoftEdges(uint8_t* y_plane, int y_pitch, int width, int height, uint32_t format);

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

    /**
     * Renders texture with HSync loss effect (horizontal skew/tear).
     * This replaces normal SDL_RenderCopy when hsync effect is active.
     * Returns true if hsync effect was applied (caller should skip normal render).
     */
    bool RenderWithHsync(int player_id, SDL_Renderer* renderer, SDL_Texture* texture,
                         int texture_width, int texture_height, const SDL_Rect& dest_rect);

    // === Betacam SP Drum Physics Model ===
    // Moved to public section so helper functions can use VisibleRegion type
    // Simulates physical rotation of video drum and tape movement
    // to generate realistic track-based noise patterns
    struct BetacamDrumPhysics {
        // PAL (25 fps) Betacam SP physical constants
        static constexpr double DRUM_HZ = 25.0;           // 1500 RPM / 60 = 25 Hz
        static constexpr double TRACK_PITCH_MM = 0.159;   // Distance between tracks
        static constexpr double FIELDS_PER_FRAME = 2.0;   // Interlaced video

        // Physical state
        double drum_phase = 0.0;        // 0.0-1.0, current drum rotation phase
        double tape_position_mm = 0.0;  // Tape position in millimeters

        // Update drum state for new frame
        void UpdateForFrame(double playback_rate, double frame_rate) {
            // Drum rotates ALWAYS (even on pause!)
            double drum_rotations_per_frame = DRUM_HZ / frame_rate;
            drum_phase += drum_rotations_per_frame;
            drum_phase = std::fmod(drum_phase, 1.0);

            // Tape moves only when playback_rate != 0
            double tape_speed_mm_per_frame = playback_rate * TRACK_PITCH_MM;
            tape_position_mm += tape_speed_mm_per_frame;
        }

        // Structure for visible scanline regions
        struct VisibleRegion {
            int y_start;
            int y_end;
            double fade_in = 0.0;   // 0.0-1.0, soft edge at start
            double fade_out = 0.0;  // 0.0-1.0, soft edge at end
        };

        // Calculate which Y scanlines are visible based on track positions
        std::vector<VisibleRegion> GetVisibleScanlines(
            int frame_height,
            double playback_rate
        ) const {
            std::vector<VisibleRegion> regions;
            double abs_rate = std::abs(playback_rate);

            if (abs_rate < 0.1) {
                // === STILL MODE (pause on shuttle) ===
                // Entire frame visible except jump line where head resets
                double jump_position = drum_phase;
                int jump_y = static_cast<int>(jump_position * frame_height);
                int band_height = frame_height / 20;  // ~5% height

                // Top region (before jump)
                if (jump_y > band_height) {
                    regions.push_back({
                        0,
                        jump_y - band_height / 2,
                        0.0, 0.0
                    });
                }

                // Bottom region (after jump)
                if (jump_y < frame_height - band_height) {
                    regions.push_back({
                        jump_y + band_height / 2,
                        frame_height,
                        0.0, 0.0
                    });
                }

            } else if (abs_rate >= 0.9 && abs_rate <= 1.1) {
                // === NORMAL PLAYBACK (1×) ===
                // Full frame visible (clean playback, no effect)
                regions.push_back({0, frame_height, 0.0, 0.0});

            } else {
                // === SHUTTLE MODE (>1×) ===
                // Multiple tracks visible, separated by guard bands
                int num_tracks = static_cast<int>(std::ceil(abs_rate));

                for (int i = 0; i < num_tracks; i++) {
                    // Phase of this track
                    double track_phase = std::fmod(
                        (static_cast<double>(i) + drum_phase * num_tracks) / num_tracks,
                        1.0
                    );

                    // Y position of this track on screen
                    int track_y_center = static_cast<int>(track_phase * frame_height);

                    // Visible height of each track
                    // At 10×: each track ~8-10% of frame height
                    // At 2×: each track ~40-45% of frame height
                    int track_height = static_cast<int>(
                        frame_height / (num_tracks + 2.0)
                    );

                    int y_start = std::max(0, track_y_center - track_height / 2);
                    int y_end = std::min(frame_height, track_y_center + track_height / 2);

                    regions.push_back({y_start, y_end, 0.0, 0.0});
                }
            }

            return regions;
        }
    };

private:
    struct PlayerState {
        PlaybackMetrics metrics{};
        int last_frame_number = -1;
        bool last_new_frame = false;
        double cycle_offset = 0.0;
        std::mt19937 rng;
        // Fast xorshift32 state for per-pixel noise. mt19937 is too heavy to call
        // per pixel in the noise/snow/satellite loops; this gives cheap uniform noise
        // that is visually indistinguishable. Seeded from rng in the constructor.
        uint32_t xrng = 0x2545F491u;

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

        // HSync loss effect state (restored from original)
        HsyncEffectState hsync;

        // === NEW: Physical drum simulation ===
        BetacamDrumPhysics drum_physics;  // Physical drum state
        double cached_saturation = 1.0;    // For gradual desaturation 5×→10×
        int transition_jitter_timer = 0;   // Countdown for transition jitter spike

        // Temporal smoothing for stripe scroll (prevents aliasing/beating at certain speeds)
        double prev_scroll_phase = 0.0;
        double smoothed_scroll_phase = 0.0;

        // Wall-clock comb glide (shuttle): the visible stripe comb drifts at a constant
        // rate in stripe-periods/second, accumulated from real elapsed time so it is
        // independent of display refresh, content fps, scrub speed and resolution — kills
        // the strobe/beat at integer speeds (3×, 10×, …).
        double shuttle_comb_phase = 0.0;                          // scroll_phase accumulator [0,1)
        std::chrono::steady_clock::time_point shuttle_comb_last_t{};
        bool shuttle_comb_init = false;                           // false until first dt sample

        // Grey zone offset from center (persists during pause, changes rarely)
        double grey_zone_offset = 0.0;   // -0.2 to +0.2 (fraction of stripe height)
        bool was_slow_motion = false;    // To detect entering slow motion/pause

        // === DOC (dropout-compensation) transient burst ===
        // Line-repeat "reconstruction" of weak/lost signal, triggered on transport upsets
        // (abrupt shuttle→stop = heavy bands; exit from pause = light frequent lines).
        std::chrono::steady_clock::time_point doc_start;       // when the current burst began
        int  doc_duration_ms = 0;                              // burst lifetime (wall clock)
        bool doc_heavy = false;                                // true = aggressive bands; false = thin
        bool doc_exit_heavy = true;                            // alternates heavy/light on switches
        bool doc_gentle = false;                               // true = soft form (pause-exit)
        int  doc_request = 0;                                  // one-shot kind (0 none / 1 gentle / 2 alt)

        // Compositing direction tracking - prevents visual "flip" on rapid direction changes
        // Only updates when a new frame actually arrives, not on playback_rate sign change
        bool composite_direction_reverse = false;  // true = reverse compositing mode
        int64_t last_direction_frame = -1;         // frame number when direction was last determined

        // Redundant-compositing guard: during pause the result is identical every render frame.
        // AVFrame data is modified in-place and persists → skip re-compositing same frame.
        int last_composited_frame_number = -1;

        // Double-buffered frame storage for slow motion compositing
        // stored_frame = last captured frame (current at previous call)
        // prev_frame = frame before stored (the actual "previous" for compositing)
        //
        // When new frame N arrives:
        //   1. prev = stored (so prev now has N-1)
        //   2. stored = N
        // Result: prev always has frame N-1 when current is N

        // "Stored" buffer - most recently saved frame
        std::vector<uint8_t> stored_frame_y;
        std::vector<uint8_t> stored_frame_u;
        std::vector<uint8_t> stored_frame_v;
        int stored_frame_width = 0;
        int stored_frame_height = 0;
        int stored_frame_y_pitch = 0;
        int stored_frame_uv_pitch = 0;
        int64_t stored_frame_number = -1;

        // "Previous" buffer - the frame BEFORE stored (for compositing)
        std::vector<uint8_t> prev_frame_y;
        std::vector<uint8_t> prev_frame_u;
        std::vector<uint8_t> prev_frame_v;
        int prev_frame_width = 0;
        int prev_frame_height = 0;
        int prev_frame_y_pitch = 0;
        int prev_frame_uv_pitch = 0;
        int64_t prev_frame_number = -1;

        PlayerState();
    };

    bool m_enabled = false;
    std::array<PlayerState, kMaxPlayers> m_players;

    static constexpr int kTransitionFrames = 6;

    PlayerState& GetPlayerState(int player_id);
    const PlayerState& GetPlayerState(int player_id) const;

    // Update hsync effect state based on playback rate (1.5x-2.2x triggers hsync loss).
    // allow_new_triggers=false lets a running animation finish but prevents new ones.
    void UpdateHsyncEffect(PlayerState& state, double abs_playback_rate, double fps,
                           bool allow_new_triggers = true);
};
