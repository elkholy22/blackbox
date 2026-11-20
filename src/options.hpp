#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace blackbox {

inline constexpr std::size_t kMaxKeep = 10000;

struct Options {
    std::size_t keep = 100;
    std::string out = "blackbox.dump";
    bool always = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
    std::vector<std::string> command;
};

/// Thrown for invalid command lines. what() is the message without the "blackbox: " prefix.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// args is the full argv, including the program name at index 0.
Options parse_args(std::span<const char* const> args);

extern const std::string_view kUsage;

} // namespace blackbox
