#include "options.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

using blackbox::Options;
using blackbox::UsageError;

namespace {

Options parse(std::initializer_list<const char*> args)
{
    std::vector<const char*> argv{"blackbox"};
    argv.insert(argv.end(), args);
    return blackbox::parse_args(argv);
}

using Command = std::vector<std::string>;

} // namespace

TEST(Options, DefaultsWithOnlyACommand)
{
    const auto options = parse({"make"});
    EXPECT_EQ(options.keep, 100u);
    EXPECT_EQ(options.out, "blackbox.dump");
    EXPECT_FALSE(options.always);
    EXPECT_FALSE(options.quiet);
    EXPECT_FALSE(options.help);
    EXPECT_FALSE(options.version);
    EXPECT_EQ(options.command, Command{"make"});
}

TEST(Options, ShortForms)
{
    const auto options = parse({"-n", "5", "-o", "x.txt", "-a", "-q", "true"});
    EXPECT_EQ(options.keep, 5u);
    EXPECT_EQ(options.out, "x.txt");
    EXPECT_TRUE(options.always);
    EXPECT_TRUE(options.quiet);
    EXPECT_EQ(options.command, Command{"true"});
}

TEST(Options, LongFormsWithSeparateValue)
{
    const auto options = parse({"--keep", "5", "--out", "x.txt", "--always", "--quiet", "true"});
    EXPECT_EQ(options.keep, 5u);
    EXPECT_EQ(options.out, "x.txt");
    EXPECT_TRUE(options.always);
    EXPECT_TRUE(options.quiet);
}

TEST(Options, LongFormsWithEquals)
{
    const auto options = parse({"--keep=5", "--out=a=b.txt", "true"});
    EXPECT_EQ(options.keep, 5u);
    EXPECT_EQ(options.out, "a=b.txt");
}

TEST(Options, DoubleDashEndsOptions)
{
    const auto options = parse({"-q", "--", "-weird-name", "--keep", "1"});
    EXPECT_TRUE(options.quiet);
    EXPECT_EQ(options.keep, 100u);
    EXPECT_EQ(options.command, (Command{"-weird-name", "--keep", "1"}));
}

TEST(Options, StopsAtFirstNonOption)
{
    const auto options = parse({"ls", "-l", "--keep", "3"});
    EXPECT_EQ(options.keep, 100u);
    EXPECT_EQ(options.command, (Command{"ls", "-l", "--keep", "3"}));
}

TEST(Options, KeepBounds)
{
    EXPECT_EQ(parse({"-n", "0", "true"}).keep, 0u);
    EXPECT_EQ(parse({"-n", "10000", "true"}).keep, 10000u);
}

TEST(Options, HelpAndVersionNeedNoCommand)
{
    EXPECT_TRUE(parse({"--help"}).help);
    EXPECT_TRUE(parse({"-h"}).help);
    EXPECT_TRUE(parse({"--version"}).version);
}

TEST(Options, MissingCommand)
{
    EXPECT_THROW(parse({}), UsageError);
    EXPECT_THROW(parse({"-n", "5"}), UsageError);
    EXPECT_THROW(parse({"--"}), UsageError);
}

TEST(Options, UnknownOption)
{
    EXPECT_THROW(parse({"--frobnicate", "true"}), UsageError);
    EXPECT_THROW(parse({"-x", "true"}), UsageError);
}

TEST(Options, MissingValue)
{
    EXPECT_THROW(parse({"-n"}), UsageError);
    EXPECT_THROW(parse({"--out"}), UsageError);
    // After the command, -o belongs to the command.
    EXPECT_EQ(parse({"true", "-o"}).command, (Command{"true", "-o"}));
}

TEST(Options, FlagWithValueIsRejected)
{
    EXPECT_THROW(parse({"--always=yes", "true"}), UsageError);
}

TEST(Options, EmptyDumpFileName)
{
    EXPECT_THROW(parse({"-o", "", "true"}), UsageError);
}

class BadKeep : public ::testing::TestWithParam<const char*> {};

TEST_P(BadKeep, IsRejected)
{
    EXPECT_THROW(parse({"--keep", GetParam(), "true"}), UsageError);
}

INSTANTIATE_TEST_SUITE_P(Options, BadKeep,
                         ::testing::Values("-1", "abc", "10x", "10001", "", " 5", "+5",
                                           "99999999999999999999999"));

TEST(Options, ErrorMessageNamesTheValue)
{
    try {
        parse({"--keep", "abc", "true"});
        FAIL() << "expected UsageError";
    } catch (const UsageError& e) {
        EXPECT_STREQ(e.what(), "invalid value 'abc' for --keep (expected 0 to 10000)");
    }
}
