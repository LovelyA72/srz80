#include "pcm_queue.hpp"
#include <algorithm>

namespace srz80::ui {
PcmQueue::PcmQueue() : samples_(44100 * 2), history_(44100 * 2 * 2) {}
void PcmQueue::set_capacity(uint64_t frames) {
    const auto capacity = static_cast<size_t>(std::clamp<uint64_t>(frames, 256, 44100 * 60));
    std::lock_guard lock(mutex_);
    if (capacity == samples_.size() / 2)
        return;
    const auto keep = std::min(size_, capacity);
    const auto skip = size_ - keep;
    std::vector<int16_t> resized(capacity * 2);
    for (size_t i = 0; i < keep; ++i) {
        const auto offset = ((head_ + skip + i) % (samples_.size() / 2)) * 2;
        resized[i * 2] = samples_[offset];
        resized[i * 2 + 1] = samples_[offset + 1];
    }
    dropped_frames_ += skip;
    samples_.swap(resized);
    head_ = 0;
    size_ = keep;
}
void PcmQueue::push(const int16_t *interleaved, uint32_t frames) {
    if (!interleaved || !frames)
        return;
    std::lock_guard lock(mutex_);
    const int16_t *input = interleaved;
    const size_t capacity = samples_.size() / 2;
    const size_t keep = std::min<size_t>(frames, capacity);
    const size_t dropped = size_ + frames > capacity ? size_ + frames - capacity : 0;
    dropped_frames_ += dropped;
    const auto old_drop = std::min(dropped, size_);
    head_ = (head_ + old_drop) % capacity;
    size_ -= old_drop;
    interleaved += (frames - keep) * 2;
    const size_t tail = (head_ + size_) % capacity;
    const size_t first = std::min(keep, capacity - tail);
    std::copy_n(interleaved, first * 2, samples_.data() + tail * 2);
    std::copy_n(interleaved + first * 2, (keep - first) * 2, samples_.data());
    size_ += keep;

    // Keep a separate history: samples_ is consumed by SDL, while this tap is
    // deliberately retained for UI analysis.  Two seconds is enough for the
    // available timebase range without making publication depend on PCM data.
    const size_t history_capacity = history_.size() / 2;
    const size_t history_keep = std::min<size_t>(frames, history_capacity);
    const size_t history_drop = history_size_ + history_keep > history_capacity
                                    ? history_size_ + history_keep - history_capacity
                                    : 0;
    history_head_ = (history_head_ + history_drop) % history_capacity;
    history_size_ -= history_drop;
    const int16_t *history_input = input + (frames - history_keep) * 2;
    const size_t history_tail = (history_head_ + history_size_) % history_capacity;
    const size_t history_first = std::min(history_keep, history_capacity - history_tail);
    std::copy_n(history_input, history_first * 2, history_.data() + history_tail * 2);
    std::copy_n(history_input + history_first * 2, (history_keep - history_first) * 2,
                history_.data());
    history_size_ += history_keep;
}
uint32_t PcmQueue::pop(int16_t *interleaved, uint32_t frames, uint64_t &epoch) {
    std::lock_guard lock(mutex_);
    epoch = epoch_;
    if (!interleaved || !frames)
        return 0;
    const auto count = static_cast<uint32_t>(std::min<size_t>(frames, size_));
    const size_t capacity = samples_.size() / 2;
    const size_t first = std::min<size_t>(count, capacity - head_);
    std::copy_n(samples_.data() + head_ * 2, first * 2, interleaved);
    std::copy_n(samples_.data(), (count - first) * 2, interleaved + first * 2);
    head_ = (head_ + count) % capacity;
    size_ -= count;
    underflow_frames_ += frames - count;
    return count;
}
uint32_t PcmQueue::capacity() const {
    std::lock_guard lock(mutex_);
    return static_cast<uint32_t>(samples_.size() / 2);
}
uint32_t PcmQueue::available() const {
    std::lock_guard lock(mutex_);
    return static_cast<uint32_t>(size_);
}
void PcmQueue::recent(std::vector<int16_t> &interleaved, uint32_t frames) const {
    std::lock_guard lock(mutex_);
    const size_t count = std::min<size_t>(frames, history_size_);
    interleaved.resize(count * 2);
    if (!count)
        return;
    const size_t capacity = history_.size() / 2;
    const size_t start = (history_head_ + history_size_ - count) % capacity;
    const size_t first = std::min(count, capacity - start);
    std::copy_n(history_.data() + start * 2, first * 2, interleaved.data());
    std::copy_n(history_.data(), (count - first) * 2, interleaved.data() + first * 2);
}
uint64_t PcmQueue::epoch() const { std::lock_guard lock(mutex_); return epoch_; }
void PcmQueue::clear() {
    std::lock_guard lock(mutex_);
    head_ = size_ = 0;
    history_head_ = history_size_ = 0;
    ++epoch_;
    underflow_frames_ = 0;
}
uint64_t PcmQueue::dropped_frames() const { std::lock_guard lock(mutex_); return dropped_frames_; }
uint64_t PcmQueue::underflow_frames() const { std::lock_guard lock(mutex_); return underflow_frames_; }
}
