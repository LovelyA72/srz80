#pragma once
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <vector>

namespace srz80::ui {
// Simulation producer / presentation consumer. Bulk copies under a short
// mutex keep reset and overflow semantics explicit without per-sample allocation.
class PcmQueue {
  public:
    PcmQueue();
    void set_capacity(uint64_t frames);
    void push(const int16_t *interleaved, uint32_t frames);
    uint32_t pop(int16_t *interleaved, uint32_t frames, uint64_t &epoch);
    // Returns an owned tail of the mixed PCM stream.  Unlike pop(), this is a
    // non-destructive diagnostic tap for GUI analysis such as the scope.
    void recent(std::vector<int16_t> &interleaved, uint32_t frames) const;
    uint32_t available() const;
    uint32_t capacity() const;
    uint64_t epoch() const;
    void clear();
    uint64_t dropped_frames() const;
    uint64_t underflow_frames() const;

  private:
    mutable std::mutex mutex_;
    std::vector<int16_t> samples_;
    size_t head_ = 0, size_ = 0;
    std::vector<int16_t> history_;
    size_t history_head_ = 0, history_size_ = 0;
    uint64_t epoch_ = 1, dropped_frames_ = 0, underflow_frames_ = 0;
};
}
