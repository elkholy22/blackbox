#include "line_assembler.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace std::chrono_literals;
using namespace std::string_literals;
using blackbox::LineAssembler;
using blackbox::Record;
using blackbox::Stream;

namespace {

std::vector<std::string> texts(const std::vector<Record>& records)
{
    std::vector<std::string> out;
    for (const auto& r : records) out.push_back(r.text);
    return out;
}

} // namespace

TEST(LineAssembler, OneChunkOneLine)
{
    LineAssembler assembler(Stream::Err);
    const auto lines = assembler.feed("hello\n", 5ms);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, "hello");
    EXPECT_EQ(lines[0].stream, Stream::Err);
    EXPECT_EQ(lines[0].at, 5ms);
    EXPECT_FALSE(lines[0].truncated);
    EXPECT_FALSE(lines[0].no_newline);
}

TEST(LineAssembler, SeveralLinesIncludingEmptyOnes)
{
    LineAssembler assembler(Stream::Out);
    const auto lines = assembler.feed("a\n\nb\n\n", 0ns);
    EXPECT_EQ(texts(lines), (std::vector<std::string>{"a", "", "b", ""}));
}

TEST(LineAssembler, LineSplitAcrossTwoChunks)
{
    LineAssembler assembler(Stream::Out);
    EXPECT_TRUE(assembler.feed("hel", 0ns).empty());
    EXPECT_EQ(texts(assembler.feed("lo\nwor", 0ns)), (std::vector<std::string>{"hello"}));
}

TEST(LineAssembler, LineSplitAcrossThreeChunks)
{
    LineAssembler assembler(Stream::Out);
    EXPECT_TRUE(assembler.feed("a", 0ns).empty());
    EXPECT_TRUE(assembler.feed("b", 0ns).empty());
    EXPECT_EQ(texts(assembler.feed("c\n", 0ns)), (std::vector<std::string>{"abc"}));
}

TEST(LineAssembler, ChunkEndingWithNewlineLeavesNothingPending)
{
    LineAssembler assembler(Stream::Out);
    assembler.feed("done\n", 0ns);
    EXPECT_FALSE(assembler.finish(0ns).has_value());
}

TEST(LineAssembler, KeepsCarriageReturn)
{
    LineAssembler assembler(Stream::Out);
    EXPECT_EQ(texts(assembler.feed("dos\r\n", 0ns)), (std::vector<std::string>{"dos\r"}));
}

TEST(LineAssembler, KeepsNulAndInvalidUtf8)
{
    LineAssembler assembler(Stream::Out);
    const std::string line = "a\0b\xff\xfe\xc3("s;
    EXPECT_EQ(texts(assembler.feed(line + "\n", 0ns)), (std::vector<std::string>{line}));
}

TEST(LineAssembler, LineAtCapIsNotTruncated)
{
    LineAssembler assembler(Stream::Out);
    const auto lines = assembler.feed(std::string(4096, 'x') + "\n", 0ns);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text.size(), 4096u);
    EXPECT_FALSE(lines[0].truncated);
}

TEST(LineAssembler, LineOverCapIsTruncated)
{
    LineAssembler assembler(Stream::Out);
    const auto lines = assembler.feed(std::string(4097, 'x') + "\n", 0ns);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].text, std::string(4096, 'x'));
    EXPECT_TRUE(lines[0].truncated);
}

TEST(LineAssembler, DropsBytesPastCapUntilNextNewline)
{
    LineAssembler assembler(Stream::Out, 4);
    EXPECT_TRUE(assembler.feed("abcdef", 0ns).empty());
    EXPECT_TRUE(assembler.feed("ghij", 0ns).empty());
    const auto lines = assembler.feed("kl\nnext\n", 0ns);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0].text, "abcd");
    EXPECT_TRUE(lines[0].truncated);
    EXPECT_EQ(lines[1].text, "next");
    EXPECT_FALSE(lines[1].truncated);
}

TEST(LineAssembler, FinishReturnsPartialLine)
{
    LineAssembler assembler(Stream::Err);
    assembler.feed("full\npart", 1ms);
    const auto last = assembler.finish(2ms);
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->text, "part");
    EXPECT_TRUE(last->no_newline);
    EXPECT_EQ(last->at, 2ms);
    EXPECT_FALSE(assembler.finish(3ms).has_value());
}

TEST(LineAssembler, FinishWithoutInputReturnsNothing)
{
    LineAssembler assembler(Stream::Out);
    EXPECT_FALSE(assembler.finish(0ns).has_value());
}

TEST(LineAssembler, TimestampIsTimeOfCompletingChunk)
{
    LineAssembler assembler(Stream::Out);
    assembler.feed("slow ", 10ms);
    const auto lines = assembler.feed("line\n", 250ms);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0].at, 250ms);
}
