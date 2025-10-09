#ifndef FSTP_PERFORMANCE_PROFILER_H
#define FSTP_PERFORMANCE_PROFILER_H

#ifdef __APPLE__
#include <os/signpost.h>
#include <os/log.h>

// Create logging categories for different components
#define FSTP_LOG_SUBSYSTEM "com.tapex.player"

// Main logging categories
static os_log_t fstp_log_decoder = os_log_create(FSTP_LOG_SUBSYSTEM, "Decoder");
static os_log_t fstp_log_texture = os_log_create(FSTP_LOG_SUBSYSTEM, "Texture");
static os_log_t fstp_log_render = os_log_create(FSTP_LOG_SUBSYSTEM, "Render");
static os_log_t fstp_log_swscale = os_log_create(FSTP_LOG_SUBSYSTEM, "SwScale");

// Macros for convenient profiling
#define FSTP_SIGNPOST_BEGIN(log, name, ...) \
    os_signpost_interval_begin(log, OS_SIGNPOST_ID_EXCLUSIVE, name, ##__VA_ARGS__)

#define FSTP_SIGNPOST_END(log, name, ...) \
    os_signpost_interval_end(log, OS_SIGNPOST_ID_EXCLUSIVE, name, ##__VA_ARGS__)

#define FSTP_SIGNPOST_EVENT(log, name, ...) \
    os_signpost_event_emit(log, OS_SIGNPOST_ID_EXCLUSIVE, name, ##__VA_ARGS__)

// Specialized macros for frequently used operations
#define FSTP_PROFILE_DECODE_BEGIN(decoder_name) \
    FSTP_SIGNPOST_BEGIN(fstp_log_decoder, "decode_frame", "decoder=%{public}s", decoder_name)

#define FSTP_PROFILE_DECODE_END(decoder_name) \
    FSTP_SIGNPOST_END(fstp_log_decoder, "decode_frame", "decoder=%{public}s", decoder_name)

#define FSTP_PROFILE_SWSCALE_BEGIN(width, height) \
    FSTP_SIGNPOST_BEGIN(fstp_log_swscale, "swscale_convert", "size=%dx%d", width, height)

#define FSTP_PROFILE_SWSCALE_END() \
    FSTP_SIGNPOST_END(fstp_log_swscale, "swscale_convert")

#define FSTP_PROFILE_TEXTURE_BEGIN(operation) \
    FSTP_SIGNPOST_BEGIN(fstp_log_texture, "texture_operation", "op=%{public}s", operation)

#define FSTP_PROFILE_TEXTURE_END(operation) \
    FSTP_SIGNPOST_END(fstp_log_texture, "texture_operation", "op=%{public}s", operation)

#define FSTP_PROFILE_RENDER_BEGIN() \
    FSTP_SIGNPOST_BEGIN(fstp_log_render, "render_frame")

#define FSTP_PROFILE_RENDER_END() \
    FSTP_SIGNPOST_END(fstp_log_render, "render_frame")

// Simplified class for RAII profiling
class FSTPSignpostInterval {
public:
    FSTPSignpostInterval(os_log_t log, os_signpost_id_t id)
        : m_log(log), m_id(id) {
    }

    ~FSTPSignpostInterval() {
        os_signpost_interval_end(m_log, m_id, "");
    }

private:
    os_log_t m_log;
    os_signpost_id_t m_id;
};

// Macros for scope profiling - removed due to API limitations

#else // not macOS

// Stubs for other platforms
#define FSTP_SIGNPOST_BEGIN(log, name, ...)
#define FSTP_SIGNPOST_END(log, name, ...)
#define FSTP_SIGNPOST_EVENT(log, name, ...)
#define FSTP_PROFILE_DECODE_BEGIN(decoder_name)
#define FSTP_PROFILE_DECODE_END(decoder_name)
#define FSTP_PROFILE_SWSCALE_BEGIN(width, height)
#define FSTP_PROFILE_SWSCALE_END()
#define FSTP_PROFILE_TEXTURE_BEGIN(operation)
#define FSTP_PROFILE_TEXTURE_END(operation)
#define FSTP_PROFILE_RENDER_BEGIN()
#define FSTP_PROFILE_RENDER_END()

#endif // __APPLE__

#endif // FSTP_PERFORMANCE_PROFILER_H