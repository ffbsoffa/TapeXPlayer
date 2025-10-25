#include "FSTPVideoFrame.h"
#include <algorithm>

namespace FSTP {

// Implementation of FrameBuffer
void FrameBuffer::Resize(size_t size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.resize(size);
}

size_t FrameBuffer::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.size();
}

FrameInfo& FrameBuffer::GetFrame(size_t index) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.at(index);
}

const FrameInfo& FrameBuffer::GetFrame(size_t index) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_frames.at(index);
}

void FrameBuffer::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames.clear();
}

// Implementation of FrameCleaner
void FrameCleaner::CleanFrames(std::vector<FrameInfo>& frames, int start, int end) {
    start = std::max(0, start);
    end = std::min(static_cast<int>(frames.size()) - 1, end);
    
    if (start > end) {
        return;
    }
    
    for (int i = start; i <= end; ++i) {
        std::lock_guard<std::mutex> lock(frames[i].mutex);
        frames[i].low_res_frame.reset();
        frames[i].cached_frame.reset();
        // Save full-res frame if it exists
        if (!frames[i].frame) {
            frames[i].type = FrameInfo::EMPTY;
        }
        frames[i].is_ready.store(false);
        frames[i].is_decoding.store(false);
    }
}

// Implementation of RingBuffer
RingBuffer::RingBuffer(size_t capacity)
    : m_buffer(capacity), m_capacity(capacity), m_start(0), m_size(0) {
}

bool RingBuffer::Push(const FrameInfo& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_size == m_capacity) {
        // Buffer is full, overwrite oldest element
        m_start = (m_start + 1) % m_capacity;
    } else {
        ++m_size;
    }
    
    size_t index = (m_start + m_size - 1) % m_capacity;
    m_buffer[index] = frame;
    
    return true;
}

bool RingBuffer::Pop(FrameInfo& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_size == 0) {
        return false;
    }
    
    frame = m_buffer[m_start];
    m_start = (m_start + 1) % m_capacity;
    --m_size;
    
    return true;
}

bool RingBuffer::IsEmpty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size == 0;
}

bool RingBuffer::IsFull() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size == m_capacity;
}

size_t RingBuffer::Size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_size;
}

void RingBuffer::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_size = 0;
    m_start = 0;
    // Clear all frames
    for (auto& frame : m_buffer) {
        frame = FrameInfo();
    }
}

} // namespace FSTP