#include "exit_status.hpp"

#include <format>
#include <string.h>

#include <sys/wait.h>

namespace blackbox {

ExitStatus decode_wait_status(int raw)
{
    ExitStatus status;
    if (WIFSIGNALED(raw)) {
        status.signaled = true;
        status.signal = WTERMSIG(raw);
        status.core_dumped = WCOREDUMP(raw);
    } else {
        status.code = WEXITSTATUS(raw);
    }
    return status;
}

std::string describe(const ExitStatus& status)
{
    if (!status.signaled) return std::format("exit {}", status.code);

    std::string text = std::format("signal {}", status.signal);
    // glibc 2.32+. Returns null for numbers it does not know, e.g. realtime signals.
    if (const char* name = sigabbrev_np(status.signal)) text += std::format(" (SIG{})", name);
    if (status.core_dumped) text += ", core dumped";
    return text;
}

int shell_exit_code(const ExitStatus& status)
{
    return status.signaled ? 128 + status.signal : status.code;
}

} // namespace blackbox
