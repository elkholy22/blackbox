#include "dump.hpp"

#include "unique_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <format>
#include <stdexcept>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace blackbox {

namespace {

bool needs_quoting(std::string_view word)
{
    constexpr std::string_view safe_punctuation = "@%+=:,./_-";
    if (word.empty()) return true;
    return std::ranges::any_of(word, [&](unsigned char c) {
        const bool alnum =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        return !alnum && safe_punctuation.find(static_cast<char>(c)) == std::string_view::npos;
    });
}

std::string quote_command(const std::vector<std::string>& command)
{
    std::string out;
    for (const auto& word : command) {
        if (!out.empty()) out += ' ';
        out += shell_quote(word);
    }
    return out;
}

} // namespace

std::string shell_quote(std::string_view word)
{
    if (!needs_quoting(word)) return std::string(word);

    std::string out = "'";
    for (char c : word) {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += '\'';
    return out;
}

std::string escape_control(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte < 0x20 && byte != '\t') || byte == 0x7f)
            out += std::format("\\x{:02x}", byte);
        else
            out += c;
    }
    return out;
}

std::string format_seconds(std::chrono::nanoseconds duration)
{
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return std::format("{}.{:03}", ms / 1000, ms % 1000);
}

std::string format_dump(const DumpInfo& info, const RingBuffer<Record>& lines)
{
    std::string out = "blackbox dump\n";
    // Escaped as well, so an argument with a newline cannot break the layout.
    out += std::format("command: {}\n", escape_control(quote_command(info.command)));
    out += std::format("started: {:%Y-%m-%dT%H:%M:%SZ}\n",
                       std::chrono::floor<std::chrono::seconds>(info.started));
    out += std::format("runtime: {}s\n", format_seconds(info.runtime));
    out += std::format("cause:   {}\n", info.cause);
    if (info.exit_code) out += std::format("exit:    {}\n", *info.exit_code);
    out += std::format("lines:   {} kept, {} seen\n", lines.size(), lines.total_pushed());

    if (lines.size() > 0) out += '\n';
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Record& r = lines[i];
        out += std::format("{:>10} {} {}", format_seconds(r.at),
                           r.stream == Stream::Out ? "out" : "err", escape_control(r.text));
        if (r.truncated) out += " [truncated]";
        if (r.no_newline) out += " [no newline]";
        out += '\n';
    }
    out += '\n';
    return out;
}

void append_dump(const std::string& path, std::string_view text)
{
    UniqueFd fd(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644));
    if (!fd) throw std::system_error(errno, std::generic_category(), path);

    ssize_t written = 0;
    do {
        written = ::write(fd.get(), text.data(), text.size());
    } while (written < 0 && errno == EINTR);

    if (written < 0) throw std::system_error(errno, std::generic_category(), path);
    // Finishing a short write with a second call could interleave with another writer,
    // so it is reported instead.
    if (static_cast<std::size_t>(written) != text.size())
        throw std::runtime_error(
            std::format("{}: short write ({} of {} bytes)", path, written, text.size()));
}

} // namespace blackbox
