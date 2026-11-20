#pragma once

#include "options.hpp"

namespace blackbox {

/// Runs options.command under supervision and returns the exit code for blackbox.
/// Throws std::system_error when a system call fails before the child could be started.
/// When one fails afterwards, the child is killed and reaped before the exception leaves.
int run(const Options& options);

} // namespace blackbox
