#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace blackbox {

/// Fixed capacity circular buffer. Pushing into a full buffer overwrites the oldest element.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : capacity_(capacity) { items_.reserve(capacity); }

    void push(T value)
    {
        ++total_pushed_;
        if (capacity_ == 0) return;
        if (items_.size() < capacity_) {
            items_.push_back(std::move(value));
            return;
        }
        items_[oldest_] = std::move(value);
        oldest_ = (oldest_ + 1) % capacity_;
    }

    std::size_t size() const { return items_.size(); }
    std::size_t capacity() const { return capacity_; }

    /// Number of push calls so far, including elements that were overwritten.
    std::uint64_t total_pushed() const { return total_pushed_; }

    /// Index 0 is the oldest element. i must be below size().
    const T& operator[](std::size_t i) const { return items_[(oldest_ + i) % items_.size()]; }

private:
    std::vector<T> items_;
    std::size_t capacity_;
    std::size_t oldest_ = 0; // stays 0 until the buffer is full
    std::uint64_t total_pushed_ = 0;
};

} // namespace blackbox
