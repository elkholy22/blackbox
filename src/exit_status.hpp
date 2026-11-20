#pragma once

#include <string>

namespace blackbox {

/// How the child ended, decoded from a raw waitpid status.
struct ExitStatus {
    bool signaled = false;
    int code = 0;   // exit code when !signaled
    int signal = 0; // signal number when signaled
    bool core_dumped = false;
};

ExitStatus decode_wait_status(int raw);

/// "exit 3", "signal 11 (SIGSEGV)", "signal 6 (SIGABRT), core dumped"
std::string describe(const ExitStatus& status);

/// The exit code a shell would report for the child: the code itself, or 128 + signal.
int shell_exit_code(const ExitStatus& status);

} // namespace blackbox
