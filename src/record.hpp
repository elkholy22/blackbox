#pragma once

#include <chrono>
#include <string>

namespace blackbox {

enum class Stream { Out, Err };

/// One line of child output as it is kept in the ring buffer.
struct Record {
    Stream stream = Stream::Out;
    std::chrono::nanoseconds at{}; // since the child was started
    std::string text;              // without the newline
    bool truncated = false;
    bool no_newline = false;
};

} // namespace blackbox
