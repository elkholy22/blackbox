#pragma once

#include "record.hpp"
#include "ring_buffer.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace blackbox {

struct DumpInfo {
    std::vector<std::string> command;
    std::chrono::system_clock::time_point started;
    std::chrono::nanoseconds runtime{};
    std::string cause;
    std::optional<int> exit_code; // empty for a snapshot
};

/// Renders one dump, ending with an empty line that separates it from the next one.
std::string format_dump(const DumpInfo& info, const RingBuffer<Record>& lines);

/// Appends text to path with a single write on an O_APPEND descriptor, so dumps from
/// several processes do not interleave. Throws std::runtime_error (std::system_error when a
/// system call fails).
void append_dump(const std::string& path, std::string_view text);

/// Quotes word for a POSIX shell. Words that need no quoting are returned unchanged.
std::string shell_quote(std::string_view word);

/// Replaces control bytes except tab with \xNN. Other bytes are kept as they are.
std::string escape_control(std::string_view text);

/// Seconds with millisecond precision, e.g. "12.345".
std::string format_seconds(std::chrono::nanoseconds duration);

} // namespace blackbox
