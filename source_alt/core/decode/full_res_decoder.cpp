#include "full_res_decoder.h"
#include "decode.h" // Includes FrameInfo definition
#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <cmath>
#include <algorithm>
#include <chrono> // Added for timing
#include <CoreVideo/CoreVideo.h> // Include necessary for CVPixelBufferRef type if used directly
#include <libavutil/pixdesc.h> // For av_get_pix_fmt_name if needed later

// --- ADDED: Initialization for static instance counter ---
std::atomic<int> FullResDecoder::instance_counter_(0);

// --- Implementation of get_hw_format (copied from test) ---
enum AVPixelFormat FullResDecoder::get_hw_format(AVCodecContext *ctx,
                                                 const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_VIDEOTOOLBOX) {
            // Log removed for cleaner output during problematic decodes
            // std::cout << "FullResDecoder: Found supported hardware pixel format: AV_PIX_FMT_VIDEOTOOLBOX" << std::endl;
            return *p;
        }
    }
    // Log removed for cleaner output
    // std::cerr << "FullResDecoder: Failed to get HW surface format." << std::endl;
    return AV_PIX_FMT_NONE;
}

FullResDecoder::FullResDecoder(const std::string& sourceFilename)
    : sourceFilename_(sourceFilename),
      initialized_(false),
      width_(0),
      height_(0),
      pixFmt_(AV_PIX_FMT_NONE),
      formatCtx_(nullptr),
      codecCtx_(nullptr),
      codecParams_(nullptr),
      videoStream_(nullptr),
      swsCtx_(nullptr),
      videoStreamIndex_(-1),
      hw_accel_enabled_(false), // Initialize new members
      hw_device_ctx_(nullptr),
      hw_pix_fmt_(AV_PIX_FMT_NONE),
      stop_requested_(false), // Initialize stop flag
      hw_irrecoverably_failed_(false) // Initialize new flag
{
    // --- ADDED: Instance counter log ---
    int current_instance_num = ++instance_counter_;
    std::cout << "[FullResDecoder CONSTRUCTOR TID:" << std::this_thread::get_id() << "] Instance # " << current_instance_num << " created for: " << sourceFilename_ << std::endl;
    std::cerr << "[FullResDecoder CONSTRUCTOR TID:" << std::this_thread::get_id() << "] Instance # " << current_instance_num << " created for: " << sourceFilename_ << std::endl;

    std::cout << "FullResDecoder: Initializing for " << sourceFilename_ << "..." << std::endl;
    initialized_ = initialize();
}

FullResDecoder::~FullResDecoder() {
    // --- ADDED: Instance counter log for destructor ---
    // int current_instance_num = instance_counter_.load(); // or just use a member if you store it
    std::cout << "[FullResDecoder DESTRUCTOR TID:" << std::this_thread::get_id() << "] Destroying decoder for: " << sourceFilename_ << " (Instance count might be misleading if not decremented)" << std::endl;
    std::cerr << "[FullResDecoder DESTRUCTOR TID:" << std::this_thread::get_id() << "] Destroying decoder for: " << sourceFilename_ << " (Instance count might be misleading if not decremented)" << std::endl;
    cleanup();
    // std::cout << "FullResDecoder: Destroyed." << std::endl; // Original log
}

bool FullResDecoder::initialize() {
    if (initialized_) return true;

    const AVCodec* codec = nullptr;
    int ret = 0;
    bool hw_init_success = false; // <--- ОБЪЯВЛЕНИЕ ПЕРЕМЕННОЙ

    // Сбрасываем флаг успеха HW инициализации в начале - теперь это делается выше
    // hw_init_success = false; 

    // Лог входа в функцию initialize
    std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize ENTERED for " << sourceFilename_ << std::endl;

    if (avformat_open_input(&formatCtx_, sourceFilename_.c_str(), nullptr, nullptr) != 0) {
        std::cerr << "FullResDecoder Error: Could not open input file: " << sourceFilename_ << std::endl;
        return false;
    }

    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::cerr << "FullResDecoder Error: Could not find stream info for: " << sourceFilename_ << std::endl;
        cleanup();
        return false;
    }

    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIndex_ < 0) {
         std::cerr << "FullResDecoder Error: Could not find video stream in: " << sourceFilename_ << std::endl;
         cleanup();
         return false;
    }
    videoStream_ = formatCtx_->streams[videoStreamIndex_];
    codecParams_ = videoStream_->codecpar;

    codec = avcodec_find_decoder(codecParams_->codec_id);
    if (!codec) {
        std::cerr << "FullResDecoder Error: Could not find decoder for codec id " << codecParams_->codec_id << std::endl;
        cleanup();
        return false;
    }
    std::cout << "FullResDecoder: Found decoder: " << codec->name << " for " << sourceFilename_ << std::endl;

#ifdef __APPLE__
    enum AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
    AVCodecContext* tempHwCodecCtx = nullptr;
    AVBufferRef* tempHwDeviceCtxRef = nullptr; // Для av_hwdevice_ctx_create

    std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: Starting HW config loop for " << sourceFilename_ << std::endl;
    for (int i = 0; ; i++) {
        const AVCodecHWConfig *hw_config = avcodec_get_hw_config(codec, i);
        if (!hw_config) {
            std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: No more HW configs for " << codec->name << " for " << sourceFilename_ << std::endl;
            break;
        }

        if (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX && hw_config->device_type == hw_type) {
            std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: Found VideoToolbox HW config method for " << sourceFilename_ << std::endl;

            // 1. Создаем HW девайс контекст
            ret = av_hwdevice_ctx_create(&tempHwDeviceCtxRef, hw_type, nullptr, nullptr, 0);
            if (ret < 0) {
                std::cerr << "FullResDecoder Warning: Failed to create HW device context: " << av_err2str(ret) << " for " << sourceFilename_ << std::endl;
                tempHwDeviceCtxRef = nullptr; // Убедимся, что NULL
                continue; // Следующая HW конфигурация
            }

            // 2. Создаем и настраиваем кодек контекст
            tempHwCodecCtx = avcodec_alloc_context3(codec);
            if (!tempHwCodecCtx) {
                std::cerr << "FullResDecoder Warning: Failed to allocate HW codec context attempt for " << sourceFilename_ << std::endl;
                av_buffer_unref(&tempHwDeviceCtxRef); // Очищаем созданный девайс контекст
                continue;
            }

            tempHwCodecCtx->hw_device_ctx = av_buffer_ref(tempHwDeviceCtxRef); // Передаем владение ссылкой
            if (!tempHwCodecCtx->hw_device_ctx) {
                std::cerr << "FullResDecoder Warning: Failed to ref HW device context for " << sourceFilename_ << std::endl;
                av_buffer_unref(&tempHwDeviceCtxRef); // Наша ссылка
                avcodec_free_context(&tempHwCodecCtx); // Контекст кодека (он бы свою ссылку очистил, если бы получил)
                continue;
            }
            // Важно: После успешного av_buffer_ref(tempHwDeviceCtxRef) в tempHwCodecCtx->hw_device_ctx,
            // сам tempHwDeviceCtxRef (наша исходная ссылка) все еще действителен и должен быть unref'нут позже,
            // если этот tempHwCodecCtx станет основным codecCtx_, или если мы от него отказываемся.

            tempHwCodecCtx->get_format = get_hw_format;
            if (avcodec_parameters_to_context(tempHwCodecCtx, codecParams_) < 0) {
                std::cerr << "FullResDecoder Warning: Failed to copy params to HW context attempt for " << sourceFilename_ << std::endl;
                avcodec_free_context(&tempHwCodecCtx); // Очистит свою ссылку на tempHwDeviceCtxRef
                av_buffer_unref(&tempHwDeviceCtxRef);    // Очищаем нашу исходную ссылку
                continue;
            }

            if (avcodec_open2(tempHwCodecCtx, codec, nullptr) < 0) {
                std::cerr << "FullResDecoder Warning: Failed to open codec with HW acceleration for " << sourceFilename_ << std::endl;
                avcodec_free_context(&tempHwCodecCtx);
                av_buffer_unref(&tempHwDeviceCtxRef);
                continue;
            }
            std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: HW codec opened, attempting test decode for " << sourceFilename_ << std::endl;

            // 3. ТЕСТОВОЕ ДЕКОДИРОВАНИЕ
            bool test_decode_successful = false;
            AVPacket* test_packet = av_packet_alloc();
            AVFrame* test_frame = av_frame_alloc();
            int frames_decoded_count = 0;
            const int REQUIRED_TEST_FRAMES = 1;

            if (test_packet && test_frame) {
                // Перемотка на начало видеопотока для теста
                av_seek_frame(formatCtx_, videoStreamIndex_, videoStream_->start_time != AV_NOPTS_VALUE ? videoStream_->start_time : 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
                avcodec_flush_buffers(tempHwCodecCtx); // Сброс буферов перед тестом

                int packets_read_for_test = 0;
                while (frames_decoded_count < REQUIRED_TEST_FRAMES && packets_read_for_test < 20) { // Ограничение на кол-во пакетов для теста
                    if (av_read_frame(formatCtx_, test_packet) < 0) {
                        std::cerr << "FullResDecoder HW Test: Failed to read packet for test decode for " << sourceFilename_ << std::endl;
                        break; 
                    }
                    packets_read_for_test++;
                    if (test_packet->stream_index == videoStreamIndex_) {
                        ret = avcodec_send_packet(tempHwCodecCtx, test_packet);
                        if (ret < 0 && ret != AVERROR(EAGAIN)) {
                            std::cerr << "FullResDecoder HW Test: Failed to send packet: " << av_err2str(ret) << " for " << sourceFilename_ << std::endl;
                            av_packet_unref(test_packet);
                            break; 
                        }
                        if (ret == 0) { // Пакет успешно отправлен (или EAGAIN)
                           int receive_ret = avcodec_receive_frame(tempHwCodecCtx, test_frame);
                           if (receive_ret == 0) {
                               std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: HW Test Decode successful for 1 frame for " << sourceFilename_ << std::endl;
                               frames_decoded_count++;
                               av_frame_unref(test_frame);
                           } else if (receive_ret != AVERROR(EAGAIN) && receive_ret != AVERROR_EOF) {
                               std::cerr << "FullResDecoder HW Test: Failed to receive frame: " << av_err2str(receive_ret) << " for " << sourceFilename_ << std::endl;
                               av_packet_unref(test_packet);
                break;
            }
                        }
                    }
                    av_packet_unref(test_packet);
                }
                test_decode_successful = (frames_decoded_count >= REQUIRED_TEST_FRAMES);
            }
            av_packet_free(&test_packet);
            av_frame_free(&test_frame);

            if (test_decode_successful) {
                std::cout << "FullResDecoder: Successfully initialized with VideoToolbox HW Acceleration (passed test decode) for " << sourceFilename_ << std::endl;
                codecCtx_ = tempHwCodecCtx; // Присваиваем успешно протестированный контекст
            hw_accel_enabled_ = true;
            hw_init_success = true;
                hw_pix_fmt_ = hw_config->pix_fmt; // Используем формат из hw_config

                // tempHwDeviceCtxRef теперь принадлежит codecCtx_, так что нашу ссылку на него можно освободить.
                av_buffer_unref(&tempHwDeviceCtxRef);
                tempHwDeviceCtxRef = nullptr;

                // **Критически важно**: Перемотать и сбросить для основного декодирования
                av_seek_frame(formatCtx_, videoStreamIndex_, videoStream_->start_time != AV_NOPTS_VALUE ? videoStream_->start_time : 0, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
                avcodec_flush_buffers(codecCtx_);
                                
                codecCtx_->thread_count = 0; // Let FFmpeg/VideoToolbox decide thread count (was 1)
                // codecCtx_->thread_type = FF_THREAD_FRAME; // Example, if frame-level threading is desired

                break; // Выходим из цикла поиска HW конфигураций
            } else {
                std::cerr << "FullResDecoder: HW Test Decode FAILED. Cleaning up this HW attempt for " << sourceFilename_ << std::endl;
                avcodec_free_context(&tempHwCodecCtx); // Очищаем неудачный кодек-контекст
                av_buffer_unref(&tempHwDeviceCtxRef);    // Очищаем наш изначальный ref на девайс-контекст
                tempHwDeviceCtxRef = nullptr;
            }
        } // if (config matches)
    } // for (HW configs)

    // Если tempHwDeviceCtxRef все еще существует (например, последняя итерация не вошла в if или вышла раньше), очистим
    if (tempHwDeviceCtxRef) {
        av_buffer_unref(&tempHwDeviceCtxRef);
    }
#endif // __APPLE__

    // --- Fallback to Software Decoder if HW failed (hw_init_success все еще false) ---
    if (!hw_init_success) {
        std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] initialize: Initializing Software Decoder for " << sourceFilename_ << std::endl;
        // hw_device_ctx_ должен быть nullptr здесь, если логика выше верна
        if (hw_device_ctx_) { // Дополнительная проверка
            av_buffer_unref(&hw_device_ctx_);
            hw_device_ctx_ = nullptr;
         }
         hw_accel_enabled_ = false;
         hw_pix_fmt_ = AV_PIX_FMT_NONE;
        hw_irrecoverably_failed_ = false; // Явный сброс для SW пути

        codecCtx_ = avcodec_alloc_context3(codec);
        if (!codecCtx_) {
            std::cerr << "FullResDecoder Error: Could not alloc SW codec context for " << sourceFilename_ << std::endl;
            cleanup(); return false;
        }
        if (avcodec_parameters_to_context(codecCtx_, codecParams_) < 0) {
            std::cerr << "FullResDecoder Error: Could not copy codec params to SW context for " << sourceFilename_ << std::endl;
            cleanup(); return false;
        }
        if (avcodec_open2(codecCtx_, codec, nullptr) < 0) {
            std::cerr << "FullResDecoder Error: Could not open SW codec for " << sourceFilename_ << std::endl;
            cleanup(); return false;
        }
        std::cout << "FullResDecoder: Initialized with Software Decoder for " << sourceFilename_ << std::endl;
    }

    if (!codecCtx_) { // Если ни HW, ни SW не инициализировались
        std::cerr << "FullResDecoder Error: Codec context is null after all initialization attempts for " << sourceFilename_ << std::endl;
        cleanup(); return false;
    }

    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    pixFmt_ = codecCtx_->pix_fmt;
    sampleAspectRatio_ = videoStream_->sample_aspect_ratio;
    if (height_ > 0 && sampleAspectRatio_.den != 0 && sampleAspectRatio_.num != 0) {
         displayAspectRatio_ = (static_cast<float>(width_) / height_) * (static_cast<float>(sampleAspectRatio_.num) / sampleAspectRatio_.den);
    } else if (height_ > 0) {
         displayAspectRatio_ = static_cast<float>(width_) / height_;
    } else {
         displayAspectRatio_ = 16.0f / 9.0f; // Fallback
    }

    initialized_ = true;
    hw_irrecoverably_failed_ = false; // Убедимся, что сброшен при успешной инициализации (HW или SW)

    std::cout << "FullResDecoder: Final Initialization successful for " << sourceFilename_ << "." << std::endl;
    std::cout << "  Mode: " << (hw_accel_enabled_ ? "Hardware (VideoToolbox)" : "Software") << std::endl;
    std::cout << "  Resolution: " << width_ << "x" << height_ << std::endl;
    const char* fmt_name = av_get_pix_fmt_name(pixFmt_);
    std::cout << "  Context Pixel Format: " << (fmt_name ? fmt_name : "N/A") << std::endl;
    std::cout << "  SAR: " << sampleAspectRatio_.num << "/" << sampleAspectRatio_.den << std::endl;
    std::cout << "  Calculated Display Aspect Ratio: " << displayAspectRatio_ << std::endl;
    std::cout << "  Time Base: " << videoStream_->time_base.num << "/" << videoStream_->time_base.den << std::endl;

    return true;
}


void FullResDecoder::cleanup() {
    if (swsCtx_) {
        sws_freeContext(swsCtx_);
        swsCtx_ = nullptr;
    }
    if (codecCtx_) {
        avcodec_free_context(&codecCtx_); // This also unrefs codecCtx_->hw_device_ctx if set
        codecCtx_ = nullptr;
    }
    // Our direct pointer hw_device_ctx_ should be null by now if initialization logic is correct.
    // No need to explicitly unref hw_device_ctx_ here as its lifetime is tied to codecCtx_->hw_device_ctx
    hw_device_ctx_ = nullptr;

    codecParams_ = nullptr; // Belongs to formatCtx_
    if (formatCtx_) {
        avformat_close_input(&formatCtx_);
        formatCtx_ = nullptr;
    }
    videoStream_ = nullptr; // Belongs to formatCtx_
    videoStreamIndex_ = -1;
    initialized_ = false;
    hw_accel_enabled_ = false;
    hw_pix_fmt_ = AV_PIX_FMT_NONE;
    stop_requested_ = false; // Reset stop flag on cleanup
    hw_irrecoverably_failed_ = false; // Reset on cleanup
    // std::cout << "FullResDecoder cleaned up." << std::endl; // Optional log
}


bool FullResDecoder::isHardwareAccelerated() const {
    return hw_accel_enabled_;
}


bool FullResDecoder::decodeFrameRange(std::vector<FrameInfo>& frameIndex, int startFrame, int endFrame) {
    std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange ENTERED. File: " << sourceFilename_ << " Range: [" << startFrame << "-" << endFrame <<"] hw_failed_flag is: " << hw_irrecoverably_failed_.load() << std::endl;
    stop_requested_ = false;

    if (hw_irrecoverably_failed_.load()) {
        std::cerr << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange Error: HW irrecoverably failed previously. FLAG IS TRUE. ABORTING for " << sourceFilename_ << std::endl;
        return false;
    }

    auto function_start_time = std::chrono::high_resolution_clock::now();
    if (!initialized_ || !formatCtx_ || !codecCtx_ || videoStreamIndex_ < 0) {
        std::cerr << "FullResDecoder::decodeFrameRange Error: Decoder not initialized." << std::endl;
        return false;
    }

    if (frameIndex.empty()) {
        std::cerr << "FullResDecoder::decodeFrameRange Warning: Frame index is empty." << std::endl;
        return true; // Nothing to do
    }

    // Clamp range to valid bounds
    startFrame = std::max(0, startFrame);
    endFrame = std::min(static_cast<int>(frameIndex.size()) - 1, endFrame);

    if (startFrame > endFrame) {
         std::cerr << "FullResDecoder::decodeFrameRange Error: Invalid frame range requested after clamping (" << startFrame << " - " << endFrame << ")" << std::endl;
         return false;
    }

    // Comment out debug log for range
    // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Decoding range [" << startFrame << " - " << endFrame << "] (HW: " << hw_accel_enabled_ << ")" << std::endl;

    // --- Seeking (using member contexts) ---
    AVRational timeBase = videoStream_->time_base;
    int64_t seek_target_ts = -1;
    int64_t startTimeMs = -1; // Store start time of the segment

    // Attempt to seek to the startFrame's time_ms if valid
    if (startFrame >= 0 && startFrame < frameIndex.size() && frameIndex[startFrame].time_ms >= 0) {
        startTimeMs = frameIndex[startFrame].time_ms;
        seek_target_ts = av_rescale_q(startTimeMs, {1, 1000}, timeBase);

        auto seek_start_time = std::chrono::high_resolution_clock::now(); // Timing: Seek start
        // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Seeking to frame " << startFrame << " (target_ms=" << startTimeMs << ", target_ts=" << seek_target_ts << ")" << std::endl;

        int seek_flags = AVSEEK_FLAG_BACKWARD; // Seek to the keyframe <= target_ts
        int seek_ret = av_seek_frame(formatCtx_, videoStreamIndex_, seek_target_ts, seek_flags);

        auto seek_end_time = std::chrono::high_resolution_clock::now(); // Timing: Seek end
        std::chrono::duration<double, std::milli> seek_duration = seek_end_time - seek_start_time;

        if (seek_ret < 0) {
            std::cerr << "[Timing] FullResDecoder::decodeFrameRange Warning: Seek failed in " << seek_duration.count() << " ms (Error: " << av_err2str(seek_ret) << "). Will attempt decode sequentially." << std::endl;
            startTimeMs = -1; // Reset startTimeMs if seek failed, rely only on counter
        } else {
            avcodec_flush_buffers(codecCtx_);
            // Comment out timing log
            // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Seek successful in " << seek_duration.count() << " ms. Buffers flushed." << std::endl;
            
            // Repair timestamp synchronization after seek to prevent frame jumping
            double fps = 25.0; // Default fallback
            if (videoStream_->avg_frame_rate.den != 0) {
                fps = av_q2d(videoStream_->avg_frame_rate);
            } else if (videoStream_->r_frame_rate.den != 0) {
                fps = av_q2d(videoStream_->r_frame_rate);
            }
            // REMOVED: Timestamp repair - let original timestamps work naturally
        }
    } else {
        std::cerr << "[Timing] FullResDecoder::decodeFrameRange Warning: Invalid time_ms for startFrame " << startFrame << ". Will attempt decode sequentially." << std::endl;
        startTimeMs = -1; // No valid start time, rely only on counter
    }

    // --- Decoding Loop (Single-threaded, using member contexts) ---
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc(); // This frame will receive the final data (HW or SW)
    if (!packet || !frame) {
        std::cerr << "FullResDecoder::decodeFrameRange Error: Failed to allocate packet or frame." << std::endl;
        av_packet_free(&packet);
        av_frame_free(&frame);
        return false;
    }

    int currentOutputFrameIndex = startFrame; // Index where the next decoded frame should be stored
    int decoded_frame_count = 0; // Counter for timing log
    bool success = true; // Flag to track overall success

    // Comment out timing log
    // auto loop_start_time = std::chrono::high_resolution_clock::now(); // Timing: Loop start
    while (!stop_requested_.load() && av_read_frame(formatCtx_, packet) >= 0) {
        if (packet->stream_index == videoStreamIndex_) {
            int ret = avcodec_send_packet(codecCtx_, packet);
                if (ret < 0) {
                // if (ret != AVERROR(EAGAIN)) { // OLD CHECK
                //     std::cerr << "FullResDecoder::decodeFrameRange Warning: Error sending packet: " << av_err2str(ret) << std::endl;
                //     if (hw_accel_enabled_ && (ret == AVERROR_INVALIDDATA || ret == AVERROR_EXTERNAL)) { 
                //         std::cerr << "FullResDecoder::decodeFrameRange: Marking HW as irrecoverably failed due to send_packet error." << std::endl;
                //         hw_irrecoverably_failed_ = true;
                //     }
                // }
                // --- NEW AGGRESSIVE CHECK ---
                if (ret != AVERROR(EAGAIN)) { // Still ignore EAGAIN
                    std::cerr << "FullResDecoder::decodeFrameRange Warning: Error sending packet: " << av_err2str(ret) << " (code: " << ret << ") for " << sourceFilename_ << std::endl;
                    if (hw_accel_enabled_) {
                        std::cerr << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange: AGGRESSIVE CHECK - Marking HW as irrecoverably failed due to ANY critical send_packet error." << std::endl;
                        hw_irrecoverably_failed_ = true;
                        std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange hw_failed_flag SET TO TRUE after send_packet error." << std::endl;
                    }
                }
                // --- END NEW AGGRESSIVE CHECK ---
                    av_packet_unref(packet);
                    if (hw_irrecoverably_failed_.load()) { success = false; goto decode_loop_end; } // Exit if HW failed
                    continue;
                }

                while (!stop_requested_.load() && ret >= 0) {
                ret = avcodec_receive_frame(codecCtx_, frame); 
                if (ret == AVERROR(EAGAIN)) {
                        break;
                } else if (ret == AVERROR_EOF) {
                    av_packet_unref(packet);
                    goto decode_loop_end;
                    } else if (ret < 0) {
                    // std::cerr << "FullResDecoder::decodeFrameRange Error: Error receiving frame: " << av_err2str(ret) << std::endl; // OLD LOG
                    //     if (hw_accel_enabled_ && (ret == AVERROR_INVALIDDATA || ret == AVERROR_EXTERNAL)) { 
                    //         std::cerr << "FullResDecoder::decodeFrameRange: Marking HW as irrecoverably failed due to receive_frame error." << std::endl;
                    //         hw_irrecoverably_failed_ = true;
                    //     }
                    // success = false; 
                    // goto decode_loop_end;
                    // --- NEW AGGRESSIVE CHECK ---
                    std::cerr << "FullResDecoder::decodeFrameRange Error: Error receiving frame: " << av_err2str(ret) << " (code: " << ret << ") for " << sourceFilename_ << std::endl;
                    if (hw_accel_enabled_) {
                         std::cerr << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange: AGGRESSIVE CHECK - Marking HW as irrecoverably failed due to ANY critical receive_frame error." << std::endl;
                         hw_irrecoverably_failed_ = true;
                         std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange hw_failed_flag SET TO TRUE after receive_frame error." << std::endl;
                    }
                    success = false; 
                        goto decode_loop_end;
                    // --- END NEW AGGRESSIVE CHECK ---
                }

                // --- Frame Identification & Storage ---
                int64_t framePts = frame->best_effort_timestamp;
                if (framePts == AV_NOPTS_VALUE) framePts = frame->pts;

                // Enhanced frame timing calculation with microsecond precision
                int64_t frameTimeMs = -1;
                if (framePts != AV_NOPTS_VALUE) {
                    // Convert to microseconds for maximum precision
                    int64_t pts_us = av_rescale_q(framePts, timeBase, {1, 1000000});
                    int64_t start_us = (videoStream_->start_time != AV_NOPTS_VALUE) ?
                        av_rescale_q(videoStream_->start_time, timeBase, {1, 1000000}) : 0;
                    
                    // Calculate relative time in microseconds
                    int64_t relative_us = pts_us - start_us;
                    
                    // Convert to milliseconds with proper rounding
                    frameTimeMs = (relative_us + 500) / 1000;
                }

                // Check if frame time is reasonable and index is within range
                if (currentOutputFrameIndex <= endFrame &&
                    (startTimeMs < 0 || frameTimeMs >= startTimeMs)) // Check time only if seek was attempted/successful
                {
                    // Store the frame at the currentOutputFrameIndex
                    std::lock_guard<std::mutex> lock(frameIndex[currentOutputFrameIndex].mutex);

                    // frameIndex[currentOutputFrameIndex].is_decoding = true; // Optional mark

                    // Clone the received frame (could be HW surface or SW data)
                    frameIndex[currentOutputFrameIndex].frame = std::shared_ptr<AVFrame>(av_frame_clone(frame), [](AVFrame* f) { av_frame_free(&f); });
                    if (!frameIndex[currentOutputFrameIndex].frame) {
                        std::cerr << "FullResDecoder::decodeFrameRange Error: Failed to clone frame for index " << currentOutputFrameIndex << std::endl;
                        success = false;
                        goto decode_loop_end; // Stop processing on critical error
                    } else {
                        // Store frame metadata
                        frameIndex[currentOutputFrameIndex].pts = framePts;
                        frameIndex[currentOutputFrameIndex].relative_pts = framePts - (videoStream_->start_time != AV_NOPTS_VALUE ? videoStream_->start_time : 0);
                        frameIndex[currentOutputFrameIndex].time_ms = frameTimeMs; // FIXED: Update time_ms consistently
                        frameIndex[currentOutputFrameIndex].type = FrameInfo::FULL_RES; // Mark as full-res attempt
                        frameIndex[currentOutputFrameIndex].time_base = timeBase;
                        frameIndex[currentOutputFrameIndex].format = (AVPixelFormat)frame->format; // <<< STORE THE ACTUAL FORMAT

                        // Debug log for HW frame
                        if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
                             CVPixelBufferRef cv_pix_buf = (CVPixelBufferRef)frame->data[3];
                             // std::cout << "[Decoded HW Frame " << currentOutputFrameIndex << "] Format: VT, CVPixelBufferRef: " << (void*)cv_pix_buf << std::endl;
                        } else {
                             // const char* sw_fmt_name = av_get_pix_fmt_name((AVPixelFormat)frame->format);
                             // std::cout << "[Decoded SW Frame " << currentOutputFrameIndex << "] Format: " << (sw_fmt_name ? sw_fmt_name : "Unknown") << std::endl;
                        }
                        decoded_frame_count++; // Increment counter only when frame is stored
                    }

                    // frameIndex[currentOutputFrameIndex].is_decoding = false;

                    // Move to the next output index
                    currentOutputFrameIndex++;

                    // Check if we have processed beyond the end frame
                     if (currentOutputFrameIndex > endFrame) {
                        // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Reached end frame index " << endFrame << ". Stopping decode loop." << std::endl;
                        av_frame_unref(frame); // Unref the last received frame before breaking
                        av_packet_unref(packet);
                        goto decode_loop_end;
                     }
                }

                av_frame_unref(frame); // Unref frame inside receive loop
                 if (hw_irrecoverably_failed_.load()) { success = false; goto decode_loop_end; } // Check after processing frame too
            } // end while(receive_frame)
        } // end if(packet->stream_index == videoStreamIndex_)
            av_packet_unref(packet);
        if (hw_irrecoverably_failed_.load()) { success = false; goto decode_loop_end; } // Check in outer loop
    } // end while(av_read_frame)

decode_loop_end:
    // Comment out timing log
    // auto loop_end_time = std::chrono::high_resolution_clock::now(); // Timing: Loop end
    // std::chrono::duration<double, std::milli> loop_duration = loop_end_time - loop_start_time;
    // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Decode loop finished in " << loop_duration.count() << " ms. Decoded frames: " << decoded_frame_count << std::endl;

    if (stop_requested_.load()) { // Optional: Log if stopped due to request
        // Keep this log? It might be useful.
        std::cout << "[FullResDecoder] Exiting decode loop due to stop request." << std::endl;
    }
    av_frame_free(&frame);
    av_packet_free(&packet);

    auto function_end_time = std::chrono::high_resolution_clock::now(); // Timing: Function end
    std::chrono::duration<double, std::milli> function_duration = function_end_time - function_start_time;
    // std::cout << "[Timing] FullResDecoder::decodeFrameRange: Finished processing range [" << startFrame << " - " << endFrame << "] in " << function_duration.count() << " ms." << std::endl;

    if (hw_irrecoverably_failed_.load()) {
         std::cerr << "[FullResDecoder TID:" << std::this_thread::get_id() << "] decodeFrameRange EXITING due to hw_failed_flag=true for " << sourceFilename_ << std::endl;
    }
    return success; // Indicate operation completed (or if errors occurred)
}


void FullResDecoder::removeHighResFrames(std::vector<FrameInfo>& frameIndex,
                                       int start, int end,
                                       int highResStart, int highResEnd) {
    // Clamp indices
    start = std::max(0, start);
    end = std::min(static_cast<int>(frameIndex.size()) - 1, end);
    if (start > end) return;

    for (int i = start; i <= end; ++i) {
        // Lock only the frame being modified
        std::lock_guard<std::mutex> lock(frameIndex[i].mutex);

        // Check if it's outside the current high-res window
        if (i < highResStart || i > highResEnd) {
            // Check if it currently has a full-res frame (could be HW or SW)
            if (frameIndex[i].frame) {
                frameIndex[i].frame.reset(); // Release the shared_ptr

                // Update type based on whether low_res exists
                if (frameIndex[i].low_res_frame) {
                    frameIndex[i].type = FrameInfo::LOW_RES;
                    // Keep the format of the low_res frame (should be SW)
                    frameIndex[i].format = static_cast<AVPixelFormat>(frameIndex[i].low_res_frame->format);
                } else {
                    frameIndex[i].type = FrameInfo::EMPTY;
                    frameIndex[i].format = AV_PIX_FMT_NONE;
                }
                 // std::cout << "Removed full-res frame outside window at index " << i << std::endl;
            }
        }
    }
}

void FullResDecoder::clearHighResFrames(std::vector<FrameInfo>& frameIndex) {
    for (auto& frameInfo : frameIndex) {
        // Lock the frame being modified
        std::lock_guard<std::mutex> lock(frameInfo.mutex);

        if (frameInfo.frame) {
            frameInfo.frame.reset(); // Release the full-res frame

            // Update type based on low_res frame presence
            if (frameInfo.low_res_frame) {
                frameInfo.type = FrameInfo::LOW_RES;
                frameInfo.format = static_cast<AVPixelFormat>(frameInfo.low_res_frame->format);
            } else {
                frameInfo.type = FrameInfo::EMPTY;
                frameInfo.format = AV_PIX_FMT_NONE;
            }
        }
    }
    // std::cout << "Cleared all high-res frames." << std::endl;
}


bool FullResDecoder::shouldProcessFrame(const FrameInfo& frame) {
    // Reverted logic: Let's focus on processing if NO full-res exists yet
    // No need to lock here as we are just reading atomic/simple types
    return !frame.is_decoding && !frame.frame; // Process if not decoding and no full-res frame exists
}

// --- Add back the missing isInitialized implementation ---
bool FullResDecoder::isInitialized() const {
    return initialized_;
}

// --- Getter implementations --- (getWidth, getHeight, getPixelFormat) should be fine
int FullResDecoder::getWidth() const { return width_; }
int FullResDecoder::getHeight() const { return height_; }
AVPixelFormat FullResDecoder::getPixelFormat() const { return pixFmt_; }

// --- ADDED --- Implement the getter for Display Aspect Ratio
float FullResDecoder::getDisplayAspectRatio() const {
    return displayAspectRatio_;
}

// --- Public method to request stop --- 
void FullResDecoder::requestStop() {
    stop_requested_ = true;
}

// --- ADDED: Implementation for checking irrecoverable HW failure ---
bool FullResDecoder::hasHardwareFailedIrrecoverably() const {
    // Added log to see when this is called
    std::cout << "[FullResDecoder TID:" << std::this_thread::get_id() << "] hasHardwareFailedIrrecoverably() CALLED. Returning: " << hw_irrecoverably_failed_.load() << " for " << sourceFilename_ << std::endl;
    return hw_irrecoverably_failed_.load();
} 