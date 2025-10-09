#pragma once

#include <SDL2/SDL.h>
#include <memory>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <cstring>
#include "FSTPYUVRenderer.h"

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

    // Statistics
    std::atomic<size_t> m_total_bytes_processed{0};
    std::atomic<size_t> m_total_frames_processed{0};

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
     * @return true if successful
     */
    bool SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                      double timestamp, int frame_number);

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
