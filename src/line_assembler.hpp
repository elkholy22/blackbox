#pragma once

#include "record.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blackbox {

inline constexpr std::size_t kMaxLineBytes = 4096;

/// Splits a byte stream that arrives in arbitrary chunks into line records.
class LineAssembler {
public:
    explicit LineAssembler(Stream stream, std::size_t max_line = kMaxLineBytes);

    /// Returns the lines completed by this chunk. A trailing partial line is kept for later.
    std::vector<Record> feed(std::string_view chunk, std::chrono::nanoseconds now);

    /// Call at end of stream. Returns the partial line left over, if any.
    std::optional<Record> finish(std::chrono::nanoseconds now);

private:
    void append(std::string_view bytes);
    Record take(std::chrono::nanoseconds now, bool no_newline);

    Stream stream_;
    std::size_t max_line_;
    std::string pending_;
    bool truncated_ = false;
};

} // namespace blackbox
