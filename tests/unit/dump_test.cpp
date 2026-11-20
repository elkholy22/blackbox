#include "dump.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <unistd.h>

using namespace std::chrono_literals;
using namespace std::string_literals;
using namespace blackbox;

namespace {

Record line(Stream stream, std::chrono::nanoseconds at, std::string text)
{
    return Record{.stream = stream, .at = at, .text = std::move(text)};
}

DumpInfo crash_info()
{
    using namespace std::chrono;
    return DumpInfo{
        .command = {"./planner", "--map", "track 2.yaml"},
        // The 700 ms must not show up, the start time has whole seconds.
        .started = sys_days{2026y / November / 20} + 14h + 3min + 12s + 700ms,
        .runtime = 12345678901ns,
        .cause = "signal 6 (SIGABRT), core dumped",
        .exit_code = 134,
    };
}

} // namespace

TEST(Dump, GoldenCrashDump)
{
    RingBuffer<Record> lines(3);
    lines.push(line(Stream::Out, 12'000ms, "dropped"));
    lines.push(line(Stream::Out, 12'101ms, "loop 1041"));
    lines.push(line(Stream::Err, 12'214ms, "planner: no path found"));
    lines.push(line(Stream::Err, 12'330ms, "terminate called"));

    EXPECT_EQ(format_dump(crash_info(), lines), "blackbox dump\n"
                                                "command: ./planner --map 'track 2.yaml'\n"
                                                "started: 2026-11-20T14:03:12Z\n"
                                                "runtime: 12.345s\n"
                                                "cause:   signal 6 (SIGABRT), core dumped\n"
                                                "exit:    134\n"
                                                "lines:   3 kept, 4 seen\n"
                                                "\n"
                                                "    12.101 out loop 1041\n"
                                                "    12.214 err planner: no path found\n"
                                                "    12.330 err terminate called\n"
                                                "\n");
}

TEST(Dump, SnapshotHasNoExitLine)
{
    auto info = crash_info();
    info.cause = "snapshot (SIGUSR1)";
    info.exit_code.reset();
    const auto text = format_dump(info, RingBuffer<Record>(5));

    EXPECT_NE(text.find("cause:   snapshot (SIGUSR1)\n"), std::string::npos);
    EXPECT_EQ(text.find("exit:"), std::string::npos);
}

TEST(Dump, EmptyBufferHasOnlyHeader)
{
    const auto text = format_dump(crash_info(), RingBuffer<Record>(0));
    EXPECT_TRUE(text.ends_with("lines:   0 kept, 0 seen\n\n")) << text;
}

TEST(Dump, MarksTruncatedAndUnterminatedLines)
{
    RingBuffer<Record> lines(2);
    auto long_line = line(Stream::Out, 1ms, "xxxx");
    long_line.truncated = true;
    auto last = line(Stream::Err, 2ms, "partial");
    last.no_newline = true;
    lines.push(long_line);
    lines.push(last);

    const auto text = format_dump(crash_info(), lines);
    EXPECT_NE(text.find("     0.001 out xxxx [truncated]\n"), std::string::npos) << text;
    EXPECT_NE(text.find("     0.002 err partial [no newline]\n"), std::string::npos) << text;
}

TEST(Dump, EscapesControlBytes)
{
    EXPECT_EQ(escape_control("plain text"), "plain text");
    EXPECT_EQ(escape_control("a\tb"), "a\tb");
    EXPECT_EQ(escape_control("dos\r"), "dos\\x0d");
    EXPECT_EQ(escape_control("nul\0end"s), "nul\\x00end");
    EXPECT_EQ(escape_control("\x1b[31mred"), "\\x1b[31mred");
    EXPECT_EQ(escape_control("del\x7f"), "del\\x7f");
    EXPECT_EQ(escape_control("\xc3\xa4\xff"), "\xc3\xa4\xff");
}

TEST(Dump, ShellQuote)
{
    EXPECT_EQ(shell_quote("make"), "make");
    EXPECT_EQ(shell_quote("--out=a/b.txt"), "--out=a/b.txt");
    EXPECT_EQ(shell_quote("two words"), "'two words'");
    EXPECT_EQ(shell_quote("it's"), "'it'\\''s'");
    EXPECT_EQ(shell_quote(""), "''");
    EXPECT_EQ(shell_quote("$HOME"), "'$HOME'");
}

TEST(Dump, FormatSeconds)
{
    EXPECT_EQ(format_seconds(0ns), "0.000");
    EXPECT_EQ(format_seconds(999999ns), "0.000");
    EXPECT_EQ(format_seconds(1ms), "0.001");
    EXPECT_EQ(format_seconds(61'250ms), "61.250");
}

TEST(Dump, AppendDumpAppends)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("blackbox_dump_test_" + std::to_string(::getpid()));
    std::filesystem::remove(path);

    append_dump(path, "first\n");
    append_dump(path, "second\n");

    std::stringstream content;
    content << std::ifstream(path).rdbuf();
    EXPECT_EQ(content.str(), "first\nsecond\n");
    std::filesystem::remove(path);
}

TEST(Dump, AppendDumpReportsErrors)
{
    EXPECT_THROW(append_dump("/nonexistent-dir/x.dump", "text"), std::system_error);
}
