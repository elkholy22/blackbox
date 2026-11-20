#include "options.hpp"

#include <charconv>
#include <format>
#include <optional>

namespace blackbox {

const std::string_view kUsage = R"(Usage: blackbox [OPTIONS] [--] COMMAND [ARGS...]

Run COMMAND, pass its output through unchanged, and keep the last lines
in memory. Write them to a dump file when COMMAND fails or on SIGUSR1.

Options:
  -n, --keep N     number of lines to keep (0 to 10000, default 100)
  -o, --out FILE   append dumps to FILE (default blackbox.dump)
  -a, --always     also write a dump when COMMAND exits with 0
  -q, --quiet      do not print the "dump written" message
  -h, --help       show this help and exit
      --version    show the version and exit

Exit status:
  the exit code of COMMAND. If COMMAND was killed by signal N, blackbox
  ends itself with signal N after the dump, so a shell reports 128+N.
  125  blackbox itself failed (usage error, unwritable dump directory)
  126  COMMAND was found but could not be executed
  127  COMMAND was not found
)";

namespace {

std::size_t parse_keep(std::string_view text)
{
    std::size_t value = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || ec != std::errc{} || end != text.data() + text.size() || value > kMaxKeep) {
        throw UsageError(
            std::format("invalid value '{}' for --keep (expected 0 to {})", text, kMaxKeep));
    }
    return value;
}

} // namespace

Options parse_args(std::span<const char* const> args)
{
    Options options;
    std::size_t i = 1;

    for (; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        if (arg == "--") {
            ++i;
            break;
        }
        // "-" alone or anything without a leading dash is the command.
        if (arg.size() < 2 || arg[0] != '-') break;

        std::string_view name = arg;
        std::optional<std::string_view> attached; // from --name=value
        if (arg.starts_with("--")) {
            if (const auto eq = arg.find('='); eq != std::string_view::npos) {
                name = arg.substr(0, eq);
                attached = arg.substr(eq + 1);
            }
        }

        auto value = [&]() -> std::string_view {
            if (attached) return *attached;
            if (i + 1 >= args.size())
                throw UsageError(std::format("option '{}' requires an argument", name));
            return args[++i];
        };
        auto flag = [&](bool& target) {
            if (attached)
                throw UsageError(std::format("option '{}' does not take an argument", name));
            target = true;
        };

        if (name == "-n" || name == "--keep") {
            options.keep = parse_keep(value());
        } else if (name == "-o" || name == "--out") {
            options.out = value();
            if (options.out.empty()) throw UsageError("the dump file name must not be empty");
        } else if (name == "-a" || name == "--always") {
            flag(options.always);
        } else if (name == "-q" || name == "--quiet") {
            flag(options.quiet);
        } else if (name == "-h" || name == "--help") {
            flag(options.help);
        } else if (name == "--version") {
            flag(options.version);
        } else {
            throw UsageError(std::format("unknown option '{}'", name));
        }
    }

    options.command.assign(args.begin() + static_cast<std::ptrdiff_t>(i), args.end());
    if (options.command.empty() && !options.help && !options.version)
        throw UsageError("missing command");
    return options;
}

} // namespace blackbox
