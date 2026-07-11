#pragma once

#include <SDL2/SDL.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>
#include "FSTPYUVRenderer.h"
#include "FSTPBetacamEffect.h"

// Forward declaration for AVFrame (zero-copy architecture)
struct AVFrame;

/**
 * @brief Safe pixel buffer manager for synchronous rendering
 *
 * ZERO-COPY ARCHITECTURE:
 * - Stores shared_ptr<AVFrame> instead of copying data
 * - Eliminates 2 memcpy operations (AVFrame→buffer→PixelBuffer)
 * - Supports jog/shuttle up to 32x with low_res and full_res V2 decoders
 */
class FSTPPixelBufferManager {
public:
    /**
     * @brief Pixel buffer information (ZERO-COPY)
     */
    struct PixelBuffer {
        // ZERO-COPY: store shared_ptr instead of copying data
        std::shared_ptr<AVFrame> av_frame;  // Decoded frame (refcounted)

        // Metadata (duplicated for fast access without dereferencing AVFrame)
        int width = 0;                      // Frame width
        int height = 0;                     // Frame height
        Uint32 format = SDL_PIXELFORMAT_IYUV; // Pixel format (YUV420P)
        double timestamp = 0.0;             // Timestamp
        int frame_number = 0;               // Frame number
        bool is_valid = false;              // Data validity flag
        double playback_speed = 1.0;        // Playback speed
        double current_time = 0.0;          // Playback position (seconds)
        double total_duration = 0.0;        // Total duration (seconds)
        double frame_rate = 25.0;           // Source FPS
        bool new_frame = false;             // True when freshly submitted

        // SAR (Sample Aspect Ratio) for anamorphic content
        // Used to compute display aspect ratio: DAR = SAR × (width / height)
        int sar_num = 1;                    // SAR numerator (default 1:1 square pixels)
        int sar_den = 1;                    // SAR denominator

        // Adjacent frames for Betacam slow-motion compositing
        std::shared_ptr<AVFrame> prev_frame;  // Frame N-1 (for forward compositing)
        std::shared_ptr<AVFrame> next_frame;  // Frame N+1 (for reverse compositing)

        PixelBuffer() = default;

        // shared_ptr manages copying/assignment itself
        PixelBuffer(const PixelBuffer&) = default;
        PixelBuffer& operator=(const PixelBuffer&) = default;
    };

private:
    static constexpr int MAX_PLAYERS = 5;
    
    // Buffers for each player (double buffering)
    PixelBuffer m_pixel_buffers[MAX_PLAYERS][2];  // [player_id][buffer_index]
    std::atomic<int> m_write_buffer_index[MAX_PLAYERS]; // Write buffer index
    std::atomic<int> m_read_buffer_index[MAX_PLAYERS];  // Read buffer index
    std::atomic<bool> m_buffer_ready[MAX_PLAYERS];      // Buffer ready for reading

    // Mutexes for buffer protection
    std::mutex m_buffer_mutex[MAX_PLAYERS];

    // Previous frame number tracking (actual data stored in FSTPBetacamEffect)
    int64_t m_prev_frame_number[MAX_PLAYERS] = {-1, -1, -1, -1, -1};

    // Statistics
    std::atomic<size_t> m_total_bytes_processed{0};
    std::atomic<size_t> m_total_frames_processed{0};

    struct EffectScratch {
        std::vector<uint8_t> plane0;
        std::vector<uint8_t> plane1;
        std::vector<uint8_t> plane2;
        Uint32 format = SDL_PIXELFORMAT_IYUV;
        int width = 0;
        int height = 0;
    };

    FSTPBetacamEffect m_betacam_effect;
    std::array<FSTPBetacamEffect::PlaybackMetrics, MAX_PLAYERS> m_playback_metrics{};
    std::array<EffectScratch, MAX_PLAYERS> m_effect_scratch{};
    std::array<int, MAX_PLAYERS> m_last_effect_frame{};
    std::array<int, MAX_PLAYERS> m_last_full_res{};   // last frame proxy(≤480)/full-res(>480), -1=unknown
    std::array<int, MAX_PLAYERS> m_last_shuttle{};    // last frame shuttle(≥2×)/not, -1=unknown

    // --- Film grain: subtle chroma + lighter luma noise over the whole displayed image ---
    // Precomputed soft (bilinear) grain tile, sized from a 576p base; added in one cheap pass
    // at texture-update rate (no per-pixel RNG per frame → minimal CPU).
    struct GrainTile {
        int width = 0, height = 0;        // frame dims this tile was built for
        int lumaW = 0, lumaH = 0;         // luma tile dims (frame + pad)
        int chromaW = 0, chromaH = 0;     // chroma tile dims (half + pad)
        std::vector<int8_t> luma;         // signed luma grain, lumaW*lumaH
        std::vector<int8_t> chromaUV;     // interleaved U,V signed grain, chromaW*chromaH*2
    };
    std::array<GrainTile, MAX_PLAYERS> m_grain{};
    std::array<uint32_t, MAX_PLAYERS> m_grain_phase{};  // xorshift phase for per-frame scroll offset
    bool m_grain_enabled = true;
    bool m_edgefade_enabled = true;   // soft L/R border on every frame (incl. 1×)
    bool m_smear_enabled = true;      // subtle horizontal analog smear (limited bandwidth)
    void EnsureGrainTile(int player_id, int width, int height);
    void ApplyFilmGrain(int player_id, uint8_t* y_plane, int y_pitch,
                        uint8_t* u_plane, uint8_t* v_plane, int u_pitch, int v_pitch,
                        int width, int height, uint32_t format);
    // (ApplyAnalogSmear moved to FSTPBetacamEffect — must run post-composite.)

public:
    FSTPPixelBufferManager();
    ~FSTPPixelBufferManager();

    /**
     * @brief Initialize manager
     */
    bool Initialize();

    /**
     * @brief Shutdown
     */
    void Shutdown();

    /**
     * @brief [ZERO-COPY] Submit AVFrame directly (thread-safe, RECOMMENDED)
     *
     * @param player_id Player ID (0-4)
     * @param av_frame shared_ptr to decoded frame (YUV420P)
     * @param timestamp Timestamp
     * @param frame_number Frame number
     * @param prev_frame Optional: previous frame (N-1) for forward compositing
     * @param next_frame Optional: next frame (N+1) for reverse compositing
     * @return true if successful
     */
    bool SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                      double timestamp, int frame_number,
                      std::shared_ptr<AVFrame> prev_frame = nullptr,
                      std::shared_ptr<AVFrame> next_frame = nullptr);

    /**
     * @brief [LEGACY] Submit pixel data (with copying, deprecated)
     *
     * @param player_id Player ID (0-4)
     * @param pixel_data Pointer to pixel data
     * @param width Frame width
     * @param height Frame height
     * @param format SDL pixel format
     * @param timestamp Timestamp
     * @param frame_number Frame number
     * @return true if successful
     */
    bool SubmitPixelData(int player_id, const uint8_t* pixel_data, int width, int height,
                        Uint32 format, double timestamp, int frame_number);


    /**
     * @brief Get ready pixel data for rendering (called from main loop)
     *
     * @param player_id Player ID (0-4)
     * @return Pointer to buffer or nullptr if no data
     */
    const PixelBuffer* GetPixelBuffer(int player_id);

    /**
     * @brief Get previous frame for slow motion compositing
     *
     * @param player_id Player ID (0-4)
     * @return Pointer to previous frame buffer or nullptr
     */
    const PixelBuffer* GetPreviousFrame(int player_id) const;

    /**
     * @brief Update playback metrics used by Betacam effect.
     */
    void UpdatePlaybackMetrics(int player_id, const FSTPBetacamEffect::PlaybackMetrics& metrics);

    /**
     * @brief Enable or disable Betacam visual effect.
     */
    void SetBetacamEffectEnabled(bool enabled);

    /**
     * @brief Check if Betacam effect is enabled.
     */
    bool IsBetacamEffectEnabled() const { return m_betacam_effect.IsEnabled(); }

    /**
     * @brief Enable/disable the opt-in 1× reverse tracking stripe (off by default).
     */
    void SetReverseStripeEnabled(bool enabled) { m_betacam_effect.SetReverseStripeEnabled(enabled); }
    bool IsReverseStripeEnabled() const { return m_betacam_effect.IsReverseStripeEnabled(); }

    // Always-on baseline Betacam elements (analog smear + soft L/R edge fade). These are normally
    // applied inside ApplyPixelFX (via FrameContext.smear / .edge_fade); presentation output reads
    // these flags to mirror them, and uses ApplyBaselineSmearEdgeFade() when the per-speed effect
    // did not run (e.g. at 1×) so the presentation screen carries the FULL Betacam look.
    bool IsSmearEnabled() const { return m_smear_enabled; }
    bool IsEdgeFadeEnabled() const { return m_edgefade_enabled; }
    void ApplyBaselineSmearEdgeFade(uint8_t* y, int y_pitch, uint8_t* u, uint8_t* v,
                                    int u_pitch, int v_pitch, int width, int height, Uint32 format);

    /**
     * @brief Fire a one-shot Betacam dropout-compensation burst (resume-from-pause = gentle form).
     */
    void TriggerDropoutBurst(int player_id) {
        m_betacam_effect.RequestDropoutBurst(player_id, FSTPBetacamEffect::DropoutKind::Gentle);
    }

    /**
     * @brief True while a dropout burst is queued/active — render loop uses this to bypass
     *        the FPS-saving throttles so the dropout shows smoothly (even at 1×).
     */
    bool HasPendingDropout(int player_id) const { return m_betacam_effect.HasPendingDropout(player_id); }

    /**
     * @brief True when film grain is being applied — the render loop uses this to run at full
     *        rate so the grain animates at ~60fps instead of the 25fps timestamp adaptation.
     */
    bool IsFilmGrainActive() const { return m_grain_enabled && m_betacam_effect.IsEnabled(); }

    /**
     * @brief Expose Betacam pixel effect for external rendering (e.g. presentation mode).
     */
    bool ApplyPixelFX(int player_id, FSTPBetacamEffect::FrameContext& ctx);

    /**
     * @brief Apply render-time jitter adjustments for Betacam effect.
     */
    bool ApplyRenderJitter(int player_id, FSTPBetacamEffect::RenderContext& render_ctx);

    /**
     * @brief Render texture with HSync loss effect if active.
     * @return true if hsync effect was applied (caller should skip normal render)
     */
    bool RenderWithHsync(int player_id, SDL_Renderer* renderer, SDL_Texture* texture,
                         int texture_width, int texture_height, const SDL_Rect& dest_rect);

    /**
     * @brief Create/update SDL texture from pixel buffer (synchronous)
     *
     * @param renderer SDL renderer
     * @param buffer Pixel buffer
     * @param existing_texture Existing texture (may be nullptr)
     * @return New or updated texture
     */
    SDL_Texture* CreateOrUpdateTexture(int player_id, SDL_Renderer* renderer, const PixelBuffer* buffer,
                                      SDL_Texture* existing_texture);

    /**
     * @brief Check if data is ready for player
     */
    bool IsDataReady(int player_id) const;

    /**
     * @brief Get statistics
     */
    size_t GetTotalBytesProcessed() const { return m_total_bytes_processed.load(); }
    size_t GetTotalFramesProcessed() const { return m_total_frames_processed.load(); }

    /**
     * @brief Clear buffers for player
     */
    void ClearPlayerBuffers(int player_id);

    // === Color metadata (per-player) ===
public:
    struct ColorMetadata {
        int colorspace = -1;       // AVColorSpace
        int color_range = -1;      // AVColorRange
        int color_primaries = -1;  // AVColorPrimaries
        int color_trc = -1;        // AVColorTransferCharacteristic
    };

    void UpdateColorMetadata(int player_id, const ColorMetadata& meta);
    ColorMetadata GetColorMetadata(int player_id) const;
    
private:
    /**
     * @brief Calculate pixel data size
     */
    size_t CalculatePixelDataSize(int width, int height, Uint32 format) const;

    /**
     * @brief Validate player parameters
     */
    bool ValidatePlayerID(int player_id) const;

    // Store last color metadata per player
    ColorMetadata m_color_metadata[MAX_PLAYERS];

    // YUV renderers for each player (cross-platform optimization)
    std::unique_ptr<FSTPYUVRenderer> m_yuv_renderers[MAX_PLAYERS];

    // Flag for using YUV mode
    bool m_use_yuv_mode[MAX_PLAYERS];
};
#include <memory>
struct SwsContext;
