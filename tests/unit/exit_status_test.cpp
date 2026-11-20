#include "exit_status.hpp"

#include <gtest/gtest.h>

#include <csignal>

using namespace blackbox;

namespace {

// Raw wait status layout on Linux: exit code in bits 8..15, terminating signal in
// bits 0..6, core dump flag in bit 7.
constexpr int exited(int code)
{
    return code << 8;
}

constexpr int killed(int sig, bool core = false)
{
    return sig | (core ? 0x80 : 0);
}

} // namespace

TEST(ExitStatus, DecodesNormalExits)
{
    for (int code : {0, 3, 255}) {
        const auto status = decode_wait_status(exited(code));
        EXPECT_FALSE(status.signaled);
        EXPECT_EQ(status.code, code);
        EXPECT_FALSE(status.core_dumped);
    }
}

TEST(ExitStatus, DecodesSignalWithoutCore)
{
    const auto status = decode_wait_status(killed(SIGSEGV));
    EXPECT_TRUE(status.signaled);
    EXPECT_EQ(status.signal, SIGSEGV);
    EXPECT_FALSE(status.core_dumped);
}

TEST(ExitStatus, DecodesSignalWithCore)
{
    const auto status = decode_wait_status(killed(SIGABRT, true));
    EXPECT_TRUE(status.signaled);
    EXPECT_EQ(status.signal, SIGABRT);
    EXPECT_TRUE(status.core_dumped);
}

TEST(ExitStatus, Describe)
{
    EXPECT_EQ(describe(decode_wait_status(exited(0))), "exit 0");
    EXPECT_EQ(describe(decode_wait_status(exited(3))), "exit 3");
    EXPECT_EQ(describe(decode_wait_status(killed(SIGSEGV))), "signal 11 (SIGSEGV)");
    EXPECT_EQ(describe(decode_wait_status(killed(SIGABRT, true))),
              "signal 6 (SIGABRT), core dumped");
}

TEST(ExitStatus, DescribeUnknownSignal)
{
    EXPECT_EQ(describe(ExitStatus{.signaled = true, .signal = 77}), "signal 77");
}

TEST(ExitStatus, ShellExitCode)
{
    EXPECT_EQ(shell_exit_code(decode_wait_status(exited(0))), 0);
    EXPECT_EQ(shell_exit_code(decode_wait_status(exited(42))), 42);
    EXPECT_EQ(shell_exit_code(decode_wait_status(killed(SIGKILL))), 137);
    EXPECT_EQ(shell_exit_code(decode_wait_status(killed(SIGABRT, true))), 134);
}
