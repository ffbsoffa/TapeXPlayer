#include "FSTPPixelBufferManager.h"
#include "FSTPOSDSystem.h"
#include "../../FSTPVideoModule/FSTPPerformanceProfiler.h"
#include "../../FSTPVideoModule/FSTPCallCounter.h"
#include <iostream>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/frame.h>
}

// Debug control: set to true to enable verbose logging
static constexpr bool ENABLE_PIXEL_BUFFER_DEBUG = false;

FSTPPixelBufferManager::FSTPPixelBufferManager() {
    // Initialize atomic variables
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        m_write_buffer_index[i].store(0);
        m_read_buffer_index[i].store(1);
        m_buffer_ready[i].store(false);
        m_use_yuv_mode[i] = false;
        m_last_effect_frame[i] = -1;
        m_last_full_res[i] = -1;
        m_last_shuttle[i] = -1;
        m_grain_phase[i] = 0x9E3779B9u ^ (static_cast<uint32_t>(i) * 2654435761u);
        m_playback_metrics[i] = {};
    }

    std::cout << "🎯 [PIXEL BUFFER] Manager created" << std::endl;
}
void FSTPPixelBufferManager::UpdateColorMetadata(int player_id, const ColorMetadata& meta) {
    if (!ValidatePlayerID(player_id)) return;
    m_color_metadata[player_id] = meta;
}

FSTPPixelBufferManager::ColorMetadata FSTPPixelBufferManager::GetColorMetadata(int player_id) const {
    if (!ValidatePlayerID(player_id)) return {};
    return m_color_metadata[player_id];
}

FSTPPixelBufferManager::~FSTPPixelBufferManager() {
    Shutdown();
}

bool FSTPPixelBufferManager::Initialize() {
    std::cout << "✅ [PIXEL BUFFER] Manager initialized for " << MAX_PLAYERS << " players" << std::endl;
    return true;
}

void FSTPPixelBufferManager::Shutdown() {
    // Clear all buffers
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        ClearPlayerBuffers(i);
    }
    
    std::cout << "🛑 [PIXEL BUFFER] Manager shutdown complete" << std::endl;
}

// ZERO-COPY method - accepts AVFrame directly
// Optional: prev_frame (N-1) and next_frame (N+1) for Betacam slow-motion compositing
bool FSTPPixelBufferManager::SubmitAVFrame(int player_id, std::shared_ptr<AVFrame> av_frame,
                                           double timestamp, int frame_number,
                                           std::shared_ptr<AVFrame> prev_frame,
                                           std::shared_ptr<AVFrame> next_frame) {
    if (!ValidatePlayerID(player_id)) {
        std::cerr << "❌ [ZERO-COPY] Invalid player_id=" << player_id << std::endl;
        return false;
    }

    if (!av_frame) {
        std::cerr << "❌ [ZERO-COPY] NULL av_frame for player " << player_id << std::endl;
        return false;
    }

    if (!av_frame->data[0]) {
        std::cerr << "❌ [ZERO-COPY] av_frame->data[0] is NULL for player " << player_id << std::endl;
        return false;
    }

    // Get index of buffer for writing
    int write_idx = m_write_buffer_index[player_id].load();

    {
        std::lock_guard<std::mutex> lock(m_buffer_mutex[player_id]);

        PixelBuffer& buffer = m_pixel_buffers[player_id][write_idx];

        // Track frame number changes for slow motion compositing
        // (actual pixel data is stored in FSTPBetacamEffect::PlayerState)
        if (buffer.is_valid && buffer.av_frame && buffer.frame_number != frame_number) {
            m_prev_frame_number[player_id] = buffer.frame_number;
        }

        // ZERO-COPY: simply save shared_ptr (increment refcount)
        buffer.av_frame = av_frame;

        // Store adjacent frames for Betacam slow-motion compositing
        buffer.prev_frame = prev_frame;  // Frame N-1 (for forward)
        buffer.next_frame = next_frame;  // Frame N+1 (for reverse)

        // Cache metadata for fast access
        buffer.width = av_frame->width;
        buffer.height = av_frame->height;

        // Extract SAR (Sample Aspect Ratio) for anamorphic content
        buffer.sar_num = av_frame->sample_aspect_ratio.num;
        buffer.sar_den = av_frame->sample_aspect_ratio.den;

        // Validate SAR (if invalid, default to 1:1 square pixels)
        if (buffer.sar_num <= 0 || buffer.sar_den <= 0) {
            buffer.sar_num = 1;
            buffer.sar_den = 1;
        }

        // Define SDL format based on AVFrame format
        if (av_frame->format == AV_PIX_FMT_NV12) {
            buffer.format = SDL_PIXELFORMAT_NV12;
        } else {
            buffer.format = SDL_PIXELFORMAT_IYUV; // YUV420P / YUVJ420P
        }

        const auto& metrics = m_playback_metrics[player_id];
        buffer.playback_speed = metrics.playback_rate;
        buffer.current_time = metrics.position_seconds;
        buffer.total_duration = metrics.duration_seconds;
        buffer.frame_rate = metrics.frame_rate;
        buffer.timestamp = timestamp;
        buffer.frame_number = frame_number;
        buffer.new_frame = true;
        buffer.is_valid = true;

        // Detect full-res mode (resolution > 480p indicates full-resolution decoder)
        // Update OSD to show "LOCK" at 1× speed with full-res
        bool is_full_res = (av_frame->height > 480);
        UpdateOSDFullResMode(player_id, is_full_res);

        // Atomic switch buffers
        int old_read_idx = m_read_buffer_index[player_id].load();
        m_read_buffer_index[player_id].store(write_idx);
        m_write_buffer_index[player_id].store(old_read_idx);
        m_buffer_ready[player_id].store(true);
    }

    // Update statistics (without considering data size - they are not copied!)
    m_total_frames_processed.fetch_add(1);

    if (ENABLE_PIXEL_BUFFER_DEBUG) {
        static size_t debug_counter = 0;
        if (++debug_counter % 100 == 0) {
            std::cout << "✅ [ZERO-COPY SUBMIT] Player " << player_id
                      << ": " << av_frame->width << "x" << av_frame->height
                      << ", frame " << frame_number
                      << ", refcount=" << av_frame.use_count()
                      << ", data[0]=" << (void*)av_frame->data[0] << std::endl;
        }
    }

    return true;
}

// LEGACY method - copies data (deprecated)
bool FSTPPixelBufferManager::SubmitPixelData(int player_id, const uint8_t* pixel_data, int width, int height,
                                            Uint32 format, double timestamp, int frame_number) {
    if (!ValidatePlayerID(player_id) || !pixel_data || width <= 0 || height <= 0) {
        return false;
    }

    size_t data_size = CalculatePixelDataSize(width, height, format);
    if (data_size == 0) {
        std::cerr << "❌ [PIXEL BUFFER] Invalid pixel format: " << format << std::endl;
        return false;
    }

    // Get index of buffer for writing
    int write_idx = m_write_buffer_index[player_id].load();

    {
        std::lock_guard<std::mutex> lock(m_buffer_mutex[player_id]);

        PixelBuffer& buffer = m_pixel_buffers[player_id][write_idx];

        // LEGACY: Clear av_frame if it was
        buffer.av_frame.reset();

        // Prepare buffer
        buffer.width = width;
        buffer.height = height;
        buffer.format = format;
        const auto& metrics = m_playback_metrics[player_id];
        buffer.playback_speed = metrics.playback_rate;
        buffer.current_time = metrics.position_seconds;
        buffer.total_duration = metrics.duration_seconds;
        buffer.frame_rate = metrics.frame_rate;
        buffer.timestamp = timestamp;
        buffer.frame_number = frame_number;
        buffer.new_frame = true;
        buffer.is_valid = true;

        // Atomic switch buffers
        int old_read_idx = m_read_buffer_index[player_id].load();
        m_read_buffer_index[player_id].store(write_idx);
        m_write_buffer_index[player_id].store(old_read_idx);
        m_buffer_ready[player_id].store(true);
    }

    // Update statistics
    m_total_bytes_processed.fetch_add(data_size);
    m_total_frames_processed.fetch_add(1);

    static size_t debug_counter = 0;
    if (++debug_counter % 100 == 0) {
       // std::cout << "📊 [PIXEL BUFFER] Player " << player_id
                  //<< ": " << width << "x" << height
                  //<< ", frame " << frame_number 
                  //<< " (total: " << m_total_frames_processed.load() << " frames)" << std::endl;
    }
    
    return true;
}


const FSTPPixelBufferManager::PixelBuffer* FSTPPixelBufferManager::GetPixelBuffer(int player_id) {
    if (!ValidatePlayerID(player_id) || !m_buffer_ready[player_id].load()) {
        return nullptr;
    }
    
    int read_idx = m_read_buffer_index[player_id].load();
    const PixelBuffer& buffer = m_pixel_buffers[player_id][read_idx];
    
    return buffer.is_valid ? &buffer : nullptr;
}

const FSTPPixelBufferManager::PixelBuffer* FSTPPixelBufferManager::GetPreviousFrame(int player_id) const {
    // Previous frame data is now stored internally in FSTPBetacamEffect
    // This method is kept for API compatibility but returns nullptr
    (void)player_id;
    return nullptr;
}

void FSTPPixelBufferManager::UpdatePlaybackMetrics(int player_id,
                                                   const FSTPBetacamEffect::PlaybackMetrics& metrics) {
    if (!ValidatePlayerID(player_id)) {
        return;
    }
    m_playback_metrics[player_id] = metrics;
    m_betacam_effect.UpdatePlaybackMetrics(player_id, metrics);
}

void FSTPPixelBufferManager::SetBetacamEffectEnabled(bool enabled) {
    const bool was_enabled = m_betacam_effect.IsEnabled();
    if (was_enabled == enabled) {
        return;
    }

    m_betacam_effect.SetEnabled(enabled);

    if (!enabled) {
        for (int i = 0; i < MAX_PLAYERS; ++i) {
            m_betacam_effect.ResetPlayer(i);
            m_last_effect_frame[i] = -1;
        }
    }
}

bool FSTPPixelBufferManager::ApplyRenderJitter(int player_id,
                                               FSTPBetacamEffect::RenderContext& render_ctx) {
    if (!ValidatePlayerID(player_id)) {
        return false;
    }
    return m_betacam_effect.ApplyRenderJitter(player_id, render_ctx);
}

bool FSTPPixelBufferManager::ApplyPixelFX(int player_id, FSTPBetacamEffect::FrameContext& ctx) {
    if (!ValidatePlayerID(player_id)) {
        return false;
    }
    return m_betacam_effect.ApplyPixelFX(player_id, ctx);
}

void FSTPPixelBufferManager::ApplyBaselineSmearEdgeFade(uint8_t* y, int y_pitch, uint8_t* u, uint8_t* v,
                                                        int u_pitch, int v_pitch, int width, int height,
                                                        Uint32 format) {
    if (!y) return;
    if (m_smear_enabled)
        m_betacam_effect.ApplyAnalogSmear(y, y_pitch, u, v, u_pitch, v_pitch, width, height, format);
    if (m_edgefade_enabled)
        m_betacam_effect.ApplyEdgeFade(y, y_pitch, width, height, format);
}

bool FSTPPixelBufferManager::RenderWithHsync(int player_id, SDL_Renderer* renderer, SDL_Texture* texture,
                                              int texture_width, int texture_height, const SDL_Rect& dest_rect) {
    if (!ValidatePlayerID(player_id)) {
        return false;
    }
    return m_betacam_effect.RenderWithHsync(player_id, renderer, texture, texture_width, texture_height, dest_rect);
}

namespace {
// Build a soft (bilinear-upsampled) signed noise field into a strided int8 buffer.
// cell = grain particle size in px; amp = peak amplitude. One-time cost per resolution.
void BuildSoftNoise(int8_t* dst, int stride, int W, int H, int cell, double amp, uint32_t seed) {
    if (W <= 0 || H <= 0) return;
    if (cell < 1) cell = 1;
    int gw = W / cell + 2;
    int gh = H / cell + 2;
    std::vector<float> grid(static_cast<size_t>(gw) * gh);
    uint32_t s = seed | 1u;
    auto rnd = [&]() -> float {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return (static_cast<float>(s) * (1.0f / 4294967295.0f)) * 2.0f - 1.0f;  // [-1,1]
    };
    for (auto& v : grid) v = rnd() * static_cast<float>(amp);
    for (int y = 0; y < H; ++y) {
        int gy = y / cell;
        float fy = static_cast<float>(y - gy * cell) / static_cast<float>(cell);
        const float* r0 = &grid[static_cast<size_t>(gy) * gw];
        const float* r1 = &grid[static_cast<size_t>(gy + 1) * gw];
        for (int x = 0; x < W; ++x) {
            int gx = x / cell;
            float fx = static_cast<float>(x - gx * cell) / static_cast<float>(cell);
            float top = r0[gx] + (r0[gx + 1] - r0[gx]) * fx;
            float bot = r1[gx] + (r1[gx + 1] - r1[gx]) * fx;
            float val = top + (bot - top) * fy;
            int iv = static_cast<int>(std::lround(val));
            if (iv > 127) iv = 127; else if (iv < -127) iv = -127;
            dst[static_cast<size_t>(y * W + x) * stride] = static_cast<int8_t>(iv);
        }
    }
}
} // namespace

void FSTPPixelBufferManager::EnsureGrainTile(int player_id, int width, int height) {
    if (player_id < 0 || player_id >= MAX_PLAYERS || width <= 0 || height <= 0) return;
    GrainTile& g = m_grain[player_id];
    if (g.width == width && g.height == height && !g.luma.empty()) return;  // already built

    g.width = width; g.height = height;
    const int PAD = 32;                 // per-frame scroll headroom
    const double lumaAmp   = 2.0;       // luma grain (toned back down a touch)
    const double chromaAmp = 5.0;       // chroma grain (less than 8, still clearly visible)
    int cell  = std::max(1, static_cast<int>(std::lround(height / 576.0)));  // grain size from 576p base
    int ccell = std::max(1, cell);

    g.lumaW = width + PAD;
    g.lumaH = height + PAD;
    g.luma.assign(static_cast<size_t>(g.lumaW) * g.lumaH, 0);
    BuildSoftNoise(g.luma.data(), 1, g.lumaW, g.lumaH, cell, lumaAmp,
                   0x00C0FFEEu ^ (static_cast<uint32_t>(player_id) * 2654435761u));

    int cw = width / 2, ch = height / 2;
    g.chromaW = cw + PAD;
    g.chromaH = ch + PAD;
    g.chromaUV.assign(static_cast<size_t>(g.chromaW) * g.chromaH * 2, 0);
    // interleaved U,V grain (stride 2), independent seeds
    BuildSoftNoise(g.chromaUV.data(),     2, g.chromaW, g.chromaH, ccell, chromaAmp,
                   0x0BADC0DEu ^ (static_cast<uint32_t>(player_id) * 40503u));
    BuildSoftNoise(g.chromaUV.data() + 1, 2, g.chromaW, g.chromaH, ccell, chromaAmp,
                   0x5EED1234u ^ (static_cast<uint32_t>(player_id) * 2246822519u));
}

void FSTPPixelBufferManager::ApplyFilmGrain(int player_id, uint8_t* y_plane, int y_pitch,
        uint8_t* u_plane, uint8_t* v_plane, int u_pitch, int v_pitch,
        int width, int height, uint32_t format) {
    if (player_id < 0 || player_id >= MAX_PLAYERS || !y_plane || width <= 0 || height <= 0) return;
    GrainTile& g = m_grain[player_id];
    if (g.luma.empty() || g.width != width || g.height != height) return;

    const int PAD = 32;
    uint32_t s = m_grain_phase[player_id];
    auto nextOff = [&](int range) -> int {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<int>(s % static_cast<uint32_t>(range));
    };
    int oxL = nextOff(PAD), oyL = nextOff(PAD);

    // Luma (lighter grain)
    for (int yy = 0; yy < height; ++yy) {
        uint8_t* row = y_plane + static_cast<size_t>(yy) * y_pitch;
        const int8_t* grow = g.luma.data() + (static_cast<size_t>(yy + oyL) * g.lumaW + oxL);
        for (int xx = 0; xx < width; ++xx) {
            int val = row[xx] + grow[xx];
            row[xx] = static_cast<uint8_t>(val < 0 ? 0 : (val > 255 ? 255 : val));
        }
    }

    int cw = width / 2, ch = height / 2;
    if (cw <= 0 || ch <= 0) { m_grain_phase[player_id] = s; return; }
    int oxC = nextOff(PAD), oyC = nextOff(PAD);

    if (format == SDL_PIXELFORMAT_NV12 && u_plane) {
        for (int yy = 0; yy < ch; ++yy) {
            uint8_t* row = u_plane + static_cast<size_t>(yy) * u_pitch;  // interleaved UV
            const int8_t* grow = g.chromaUV.data() + (static_cast<size_t>(yy + oyC) * g.chromaW + oxC) * 2;
            for (int xx = 0; xx < cw; ++xx) {
                int uu = row[xx * 2]     + grow[xx * 2];
                int vv = row[xx * 2 + 1] + grow[xx * 2 + 1];
                row[xx * 2]     = static_cast<uint8_t>(uu < 0 ? 0 : (uu > 255 ? 255 : uu));
                row[xx * 2 + 1] = static_cast<uint8_t>(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
            }
        }
    } else if ((format == SDL_PIXELFORMAT_IYUV || format == SDL_PIXELFORMAT_YV12) && u_plane && v_plane) {
        for (int yy = 0; yy < ch; ++yy) {
            uint8_t* urow = u_plane + static_cast<size_t>(yy) * u_pitch;
            uint8_t* vrow = v_plane + static_cast<size_t>(yy) * v_pitch;
            const int8_t* grow = g.chromaUV.data() + (static_cast<size_t>(yy + oyC) * g.chromaW + oxC) * 2;
            for (int xx = 0; xx < cw; ++xx) {
                int uu = urow[xx] + grow[xx * 2];
                int vv = vrow[xx] + grow[xx * 2 + 1];
                urow[xx] = static_cast<uint8_t>(uu < 0 ? 0 : (uu > 255 ? 255 : uu));
                vrow[xx] = static_cast<uint8_t>(vv < 0 ? 0 : (vv > 255 ? 255 : vv));
            }
        }
    }
    m_grain_phase[player_id] = s;
}

// ApplyAnalogSmear moved to FSTPBetacamEffect — it must run post-composite (so slow-mo
// compositing doesn't wipe it). The manager calls m_betacam_effect.ApplyAnalogSmear() for
// the 1× path and passes frame_ctx.smear for the effect-active path.

SDL_Texture* FSTPPixelBufferManager::CreateOrUpdateTexture(int player_id, SDL_Renderer* renderer, const PixelBuffer* buffer,
                                                          SDL_Texture* existing_texture) {
    FSTP_COUNT_CALL("CreateOrUpdateTexture");

    // CRITICAL: Validate player_id before accessing any arrays
    if (!ValidatePlayerID(player_id)) {
        std::cerr << "❌ [PIXEL BUFFER] CreateOrUpdateTexture: invalid player_id=" << player_id << std::endl;
        return existing_texture;
    }

    if (!renderer || !buffer || !buffer->is_valid) {
        static int invalid_counter = 0;
        if (++invalid_counter % 100 == 1) {
            std::cerr << "❌ [PIXEL BUFFER] CreateOrUpdateTexture: invalid params - renderer="
                      << (renderer ? "OK" : "NULL") << ", buffer=" << (buffer ? "OK" : "NULL")
                      << ", is_valid=" << (buffer ? buffer->is_valid : false) << std::endl;
        }
        return existing_texture;
    }

    // CROSS-PLATFORM OPTIMIZATION: use YUV directly
    bool is_yuv_format = (buffer->format == SDL_PIXELFORMAT_IYUV ||
                          buffer->format == SDL_PIXELFORMAT_YV12 ||
                          buffer->format == SDL_PIXELFORMAT_NV12);

    // DIAGNOSTICS: check av_frame
    if (ENABLE_PIXEL_BUFFER_DEBUG) {
        static int avframe_check = 0;
        if (++avframe_check % 100 == 1) {
            std::cout << "🔍 [PIXEL BUFFER] buffer->av_frame="
                      << (buffer->av_frame ? "VALID" : "NULL")
                      << ", format=" << buffer->format
                      << ", size=" << buffer->width << "x" << buffer->height << std::endl;
        }
    }

    // Check if we need to create or recreate YUV renderer
    if (is_yuv_format) {
        bool needs_recreation = false;

        // Check if renderer exists and matches resolution and format
        if (m_yuv_renderers[player_id]) {
            int current_width, current_height;
            m_yuv_renderers[player_id]->GetSize(current_width, current_height);
            Uint32 current_format = m_yuv_renderers[player_id]->GetTextureFormat();

            if (current_width != buffer->width || current_height != buffer->height ||
                current_format != buffer->format) {
                // Resolution or format changed - need to recreate
                std::cout << "🔄 [PIXEL BUFFER] Settings changed for player " << player_id
                          << ": " << current_width << "x" << current_height
                          << " format=" << current_format
                          << " (" << (current_format == SDL_PIXELFORMAT_NV12 ? "NV12" : "IYUV") << ")"
                          << " → " << buffer->width << "x" << buffer->height
                          << " format=" << buffer->format
                          << " (" << (buffer->format == SDL_PIXELFORMAT_NV12 ? "NV12" : "IYUV") << ")"
                          << " (recreating YUV renderer)" << std::endl;

                // Simply delete renderer - it will delete texture in Cleanup()
                // WindowManager will get new texture and update its buffer (line 603)
                m_yuv_renderers[player_id].reset();
                needs_recreation = true;
            }
        } else {
            needs_recreation = true;
        }

        if (needs_recreation) {
           // std::cout << "🔧 [PIXEL BUFFER] Creating YUV renderer for player " << player_id
           //           << ", size=" << buffer->width << "x" << buffer->height << std::endl;

            // Create new YUV renderer with specified format
            m_yuv_renderers[player_id] = std::make_unique<FSTPYUVRenderer>();
            if (!m_yuv_renderers[player_id]->Initialize(renderer, buffer->width, buffer->height, buffer->format)) {
                std::cerr << "❌ [PIXEL BUFFER] Failed to initialize YUV renderer for player "
                          << player_id << ", size=" << buffer->width << "x" << buffer->height
                          << ", format=" << buffer->format << std::endl;
                m_yuv_renderers[player_id].reset();
                m_use_yuv_mode[player_id] = false;
            } else {
                m_use_yuv_mode[player_id] = true;
                std::cout << "✅ [PIXEL BUFFER] Player " << player_id << " YUV renderer created "
                          << buffer->width << "x" << buffer->height
                          << ", format=" << buffer->format
                          << " (" << (buffer->format == SDL_PIXELFORMAT_NV12 ? "NV12" : "IYUV") << ")"
                          << ", mode="
                          << (m_yuv_renderers[player_id]->GetMode() == YUVRendererMode::NATIVE_YUV
                              ? "NATIVE YUV" : "RGB FALLBACK")
                          << ", texture=" << (void*)m_yuv_renderers[player_id]->GetTexture() << std::endl;

                // FIXED: DO NOT DELETE existing_texture here!
                // Texture lifetime management - responsibility of WindowManager
                // (it will delete old texture on line 536 FSTPWindowManager.cpp)
            }
        }

        // If YUV renderer is ready, use it
        if (m_yuv_renderers[player_id]) {
            // Prepare YUV planes from AVFrame (ZERO-COPY path)
            YUVPlanes planes;
            planes.width = buffer->width;
            planes.height = buffer->height;

            if (buffer->av_frame) {
                const uint8_t* y_plane = buffer->av_frame->data[0];
                const uint8_t* u_plane = buffer->av_frame->data[1];
                const uint8_t* v_plane = buffer->av_frame->data[2];
                int y_pitch = buffer->av_frame->linesize[0];
                int u_pitch = buffer->av_frame->linesize[1];
                int v_pitch = buffer->av_frame->linesize[2];

                if (m_betacam_effect.IsEnabled() && y_plane && y_pitch > 0) {
                    const auto& metrics = m_playback_metrics[player_id];
                    // Effect active at: slow motion (< 0.9×), fast shuttle (>= 1.2×),
                    // OR 1× reverse (normal speed but backward → tracking artifacts)
                    double abs_rate = std::abs(metrics.playback_rate);
                    bool speed_in_effect_range = (abs_rate < 0.9 || abs_rate >= 1.2) ||
                                                 (abs_rate >= 0.9 && abs_rate <= 1.1 && metrics.is_reverse);

                    // EXPERIMENT: fire a DOC dropout on proxy↔full-res source switches. Detect
                    // here — the manager sees every frame's resolution even at 1×, where the
                    // effect is otherwise gated off — by the height crossing the proxy/full-res
                    // boundary (proxy ≤480p, full-res >480p). Only at the flip; settled → nothing.
                    int is_full_res = (buffer->height > 480) ? 1 : 0;
                    if (m_last_full_res[player_id] >= 0 && is_full_res != m_last_full_res[player_id]) {
                        m_betacam_effect.RequestDropoutBurst(player_id, FSTPBetacamEffect::DropoutKind::Alternating);
                    }
                    m_last_full_res[player_id] = is_full_res;

                    // Start a touch EARLIER than the actual resolution flip: the full-res decoder
                    // is stopped/resumed at the 2× boundary, so the proxy↔full-res switch happens
                    // around there. Firing on the speed crossing 2× (either direction) makes the
                    // dropout lead the visible switch slightly → a more graceful transition.
                    int is_shuttle = (abs_rate >= 2.0) ? 1 : 0;
                    if (m_last_shuttle[player_id] >= 0 && is_shuttle != m_last_shuttle[player_id]) {
                        m_betacam_effect.RequestDropoutBurst(player_id, FSTPBetacamEffect::DropoutKind::Alternating);
                    }
                    m_last_shuttle[player_id] = is_shuttle;

                    // Run the effect even at 1× while a DOC burst is queued/active, so dropouts
                    // are visible at normal speed too (otherwise speed_in_effect_range gates it off).
                    bool has_dropout = m_betacam_effect.HasPendingDropout(player_id);

                    // position_seconds includes timecode_offset; duration_seconds is raw clip length.
                    // Use file-relative position for boundary checks so videos with large
                    // timecode offsets (e.g. 01:00:00:00) don't falsely appear past-end.
                    double file_position = metrics.position_seconds - metrics.timecode_offset_seconds;
                    bool candidate_effect = (speed_in_effect_range || has_dropout) &&
                                             (file_position > 0.1) &&
                                             ((metrics.duration_seconds <= 0.0) ||
                                              ((metrics.duration_seconds - file_position) > 0.1)) &&
                                             buffer->width > 0 && buffer->height > 0;

                    // Build a writable scratch when the Betacam effect is active OR film grain
                    // is on (grain must touch the whole image at every speed, incl. 1×).
                    bool want_scratch = (candidate_effect || m_grain_enabled || m_edgefade_enabled || m_smear_enabled);
                    if (want_scratch) {
                        bool have_planes = true;
                        if (buffer->format == SDL_PIXELFORMAT_NV12) {
                            have_planes = (u_plane != nullptr && u_pitch > 0);
                        } else if (buffer->format == SDL_PIXELFORMAT_IYUV ||
                                   buffer->format == SDL_PIXELFORMAT_YV12) {
                            have_planes = (u_plane != nullptr && v_plane != nullptr &&
                                           u_pitch > 0 && v_pitch > 0);
                        }

                        if (have_planes) {
                            EffectScratch& scratch = m_effect_scratch[player_id];
                            scratch.format = buffer->format;
                            scratch.width = buffer->width;
                            scratch.height = buffer->height;

                            scratch.plane0.resize(static_cast<size_t>(y_pitch) * buffer->height);
                            for (int y = 0; y < buffer->height; ++y) {
                                std::memcpy(&scratch.plane0[y * y_pitch],
                                            buffer->av_frame->data[0] + y * buffer->av_frame->linesize[0],
                                            y_pitch);
                            }

                            if (buffer->format == SDL_PIXELFORMAT_IYUV ||
                                buffer->format == SDL_PIXELFORMAT_YV12) {
                                int chroma_height = buffer->height / 2;
                                scratch.plane1.resize(static_cast<size_t>(u_pitch) * chroma_height);
                                scratch.plane2.resize(static_cast<size_t>(v_pitch) * chroma_height);
                                for (int y = 0; y < chroma_height; ++y) {
                                    std::memcpy(&scratch.plane1[y * u_pitch],
                                                buffer->av_frame->data[1] + y * buffer->av_frame->linesize[1],
                                                u_pitch);
                                    std::memcpy(&scratch.plane2[y * v_pitch],
                                                buffer->av_frame->data[2] + y * buffer->av_frame->linesize[2],
                                                v_pitch);
                                }
                            } else if (buffer->format == SDL_PIXELFORMAT_NV12) {
                                int chroma_height = buffer->height / 2;
                                scratch.plane1.resize(static_cast<size_t>(u_pitch) * chroma_height);
                                scratch.plane2.clear();
                                for (int y = 0; y < chroma_height; ++y) {
                                    std::memcpy(&scratch.plane1[y * u_pitch],
                                                buffer->av_frame->data[1] + y * buffer->av_frame->linesize[1],
                                                u_pitch);
                                }
                            } else {
                                scratch.plane1.clear();
                                scratch.plane2.clear();
                            }

                            uint8_t* su0 = scratch.plane1.empty() ? nullptr : scratch.plane1.data();
                            uint8_t* sv0 = scratch.plane2.empty() ? nullptr : scratch.plane2.data();

                            bool fx_applied = false;
                            if (candidate_effect) {
                                FSTPBetacamEffect::FrameContext frame_ctx;
                                frame_ctx.pixel_format = buffer->format;
                                frame_ctx.width = buffer->width;
                                frame_ctx.height = buffer->height;
                                frame_ctx.frame_number = buffer->frame_number;
                                frame_ctx.new_frame = (m_last_effect_frame[player_id] != buffer->frame_number);
                                frame_ctx.planes[0] = scratch.plane0.data();
                                frame_ctx.linesize[0] = y_pitch;
                                frame_ctx.planes[1] = scratch.plane1.empty() ? nullptr : scratch.plane1.data();
                                frame_ctx.linesize[1] = u_pitch;
                                frame_ctx.planes[2] = scratch.plane2.empty() ? nullptr : scratch.plane2.data();
                                frame_ctx.linesize[2] = v_pitch;
                                frame_ctx.source_frame = buffer->av_frame.get();

                                // Adjacent frames for Betacam slow-motion compositing
                                // Provided by decoder: prev_frame (N-1) and next_frame (N+1)
                                frame_ctx.prev_source_frame = buffer->prev_frame.get();
                                frame_ctx.next_source_frame = buffer->next_frame.get();
                                // Smear + edge fade applied INSIDE the effect (after compositing,
                                // under the stripe): fixes both the left-edge flicker and the smear
                                // being wiped by slow-mo compositing.
                                frame_ctx.smear     = m_smear_enabled;
                                frame_ctx.edge_fade = m_edgefade_enabled;

                                fx_applied = m_betacam_effect.ApplyPixelFX(player_id, frame_ctx);
                                m_last_effect_frame[player_id] = buffer->frame_number;

                                // Post-effect SD softness: gently blur the hard horizontal
                                // edges of the grey Betacam bands (drawn crisp inside the
                                // effect). Only on effect frames (slow/shuttle/pause) — clean
                                // 1× playback stays sharp for frame-accurate analysis.
                                if (fx_applied) {
                                    m_betacam_effect.ApplySoftEdges(scratch.plane0.data(), y_pitch,
                                                                    buffer->width, buffer->height, buffer->format);
                                }
                            } else {
                                // No effect this frame (e.g. 1×) → apply smear + soft border here.
                                if (m_smear_enabled) {
                                    m_betacam_effect.ApplyAnalogSmear(scratch.plane0.data(), y_pitch,
                                                                      su0, sv0, u_pitch, v_pitch,
                                                                      buffer->width, buffer->height, buffer->format);
                                }
                                if (m_edgefade_enabled) {
                                    m_betacam_effect.ApplyEdgeFade(scratch.plane0.data(), y_pitch,
                                                                   buffer->width, buffer->height, buffer->format);
                                }
                            }

                            // FINAL LAYER: subtle film grain over EVERYTHING (image + effect).
                            // Soft chroma noise + lighter luma noise, grain sized from a 576p base.
                            if (m_grain_enabled) {
                                EnsureGrainTile(player_id, buffer->width, buffer->height);
                                ApplyFilmGrain(player_id, scratch.plane0.data(), y_pitch,
                                               su0, sv0, u_pitch, v_pitch,
                                               buffer->width, buffer->height, buffer->format);
                            }

                            // Point output at the scratch when it was actually modified (effect,
                            // smear, edge fade and/or grain); otherwise keep the zero-copy source.
                            if (fx_applied || m_grain_enabled || m_edgefade_enabled || m_smear_enabled) {
                                y_plane = scratch.plane0.data();
                                if (buffer->format == SDL_PIXELFORMAT_IYUV ||
                                    buffer->format == SDL_PIXELFORMAT_YV12) {
                                    u_plane = scratch.plane1.data();
                                    v_plane = scratch.plane2.data();
                                } else if (buffer->format == SDL_PIXELFORMAT_NV12) {
                                    u_plane = scratch.plane1.data();
                                    v_plane = nullptr;
                                }
                            }
                        }
                    }
                }

                planes.y_plane = y_plane;
                planes.u_plane = u_plane;
                planes.v_plane = v_plane;
                planes.y_pitch = y_pitch;
                planes.u_pitch = u_pitch;
                planes.v_pitch = v_pitch;

                // CRITICAL: Pass color_range for correct YUV→RGB conversion
                // Proxy: AVCOL_RANGE_JPEG (full range 0-255)
                // Full-res: usually AVCOL_RANGE_MPEG (limited range 16-235)
                planes.is_full_range = (buffer->av_frame->color_range == AVCOL_RANGE_JPEG);

                // CRITICAL: Pass format so renderer knows if it's NV12 (2 planes) or YUV420P (3 planes)
                planes.format = buffer->av_frame->format;

                // Logging color_range and linesize for diagnostics
                static int render_log = 0;
                if (++render_log <= 10) {
                    std::cout << "🖼️  [RENDER] Player " << player_id
                             << ": format=" << buffer->av_frame->format
                             << ", range=" << buffer->av_frame->color_range
                             << ", Y_pitch=" << planes.y_pitch
                             << ", U_pitch=" << planes.u_pitch
                             << ", V_pitch=" << planes.v_pitch
                             << " (width=" << buffer->width << ")" << std::endl;

                    // DIAGNOSTICS: Check alignment
                    bool y_misaligned = (planes.y_pitch % 64 != 0);
                    bool u_misaligned = (planes.u_pitch % 64 != 0);
                    bool v_misaligned = (planes.v_pitch > 0 && planes.v_pitch % 64 != 0);
                    if (y_misaligned || u_misaligned || v_misaligned) {
                        std::cerr << "⚠️  [RENDER] MISALIGNED PITCH DETECTED - may cause AGX errors!" << std::endl;
                    }
                }

                if (ENABLE_PIXEL_BUFFER_DEBUG) {
                    static int zero_copy_counter = 0;
                    if (++zero_copy_counter % 100 == 1) {
                        std::cout << "🚀 [ZERO-COPY PATH] AVFrame→SDL direct: "
                                  << buffer->width << "x" << buffer->height
                                  << ", Y_pitch=" << planes.y_pitch
                                  << ", U_pitch=" << planes.u_pitch
                                  << ", Y_plane=" << (void*)planes.y_plane
                                  << ", texture=" << (void*)m_yuv_renderers[player_id]->GetTexture() << std::endl;
                    }
                }
            } else {
                // LEGACY: No data (old path with pixel_data was removed)
                std::cerr << "❌ [PIXEL BUFFER] No av_frame in buffer (legacy path removed)"
                          << ", player=" << player_id << std::endl;
                return existing_texture;
            }

            // Update YUV texture (SDL copies to GPU)
            if (m_yuv_renderers[player_id]->UpdateYUVTexture(planes)) {
                SDL_Texture* result_texture = m_yuv_renderers[player_id]->GetTexture();
                if (ENABLE_PIXEL_BUFFER_DEBUG) {
                    static int success_counter = 0;
                    if (++success_counter % 100 == 1) {
                        std::cout << "✅ [ZERO-COPY] UpdateYUVTexture success, texture="
                                  << (void*)result_texture << std::endl;
                    }
                }
                return result_texture;
            } else {
                std::cerr << "❌ [ZERO-COPY] UpdateYUVTexture failed for player " << player_id << std::endl;
            }
        }
    }

    // FALLBACK: old RGB conversion method (REMOVED in zero-copy architecture)
    // We always use YUV NATIVE path - if it doesn't work, return existing_texture
    std::cerr << "❌ [PIXEL BUFFER] YUV renderer failed, no fallback available in zero-copy mode" << std::endl;
    return existing_texture;
}
bool FSTPPixelBufferManager::IsDataReady(int player_id) const {
    return ValidatePlayerID(player_id) && m_buffer_ready[player_id].load();
}

void FSTPPixelBufferManager::ClearPlayerBuffers(int player_id) {
    if (!ValidatePlayerID(player_id)) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_buffer_mutex[player_id]);

    // CRITICAL: Set buffer_ready to false FIRST (inside lock)
    // This prevents race condition where GetPixelBuffer returns
    // a pointer to a buffer that's being cleared
    m_buffer_ready[player_id].store(false);

    for (int i = 0; i < 2; ++i) {
        m_pixel_buffers[player_id][i] = PixelBuffer(); // Reset to default state
    }

    // Clear effect scratch buffers safely
    m_effect_scratch[player_id].plane0.clear();
    m_effect_scratch[player_id].plane1.clear();
    m_effect_scratch[player_id].plane2.clear();
    m_effect_scratch[player_id].width = 0;
    m_effect_scratch[player_id].height = 0;

    // Reset previous frame tracking
    m_prev_frame_number[player_id] = -1;

    m_last_effect_frame[player_id] = -1;
    m_playback_metrics[player_id] = {};
    m_betacam_effect.ResetPlayer(player_id);
}

size_t FSTPPixelBufferManager::CalculatePixelDataSize(int width, int height, Uint32 format) const {
    switch (format) {
        case SDL_PIXELFORMAT_YV12:
        case SDL_PIXELFORMAT_IYUV:  // Add support for IYUV (analog of YUV420P)
            return width * height * 3 / 2; // Y + U/2 + V/2
        case SDL_PIXELFORMAT_RGB24:
            return width * height * 3;
        case SDL_PIXELFORMAT_RGBA32:
            return width * height * 4;
        default:
            std::cerr << "❌ [PIXEL BUFFER] Unknown format: " << format << std::endl;
            return 0;
    }
}

bool FSTPPixelBufferManager::ValidatePlayerID(int player_id) const {
    return player_id >= 0 && player_id < MAX_PLAYERS;
}
