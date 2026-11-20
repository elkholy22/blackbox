#include "line_assembler.hpp"

#include <utility>

namespace blackbox {

LineAssembler::LineAssembler(Stream stream, std::size_t max_line)
    : stream_(stream), max_line_(max_line)
{
}

std::vector<Record> LineAssembler::feed(std::string_view chunk, std::chrono::nanoseconds now)
{
    std::vector<Record> lines;
    while (!chunk.empty()) {
        const auto newline = chunk.find('\n');
        append(chunk.substr(0, newline));
        if (newline == std::string_view::npos) break;
        lines.push_back(take(now, false));
        chunk.remove_prefix(newline + 1);
    }
    return lines;
}

std::optional<Record> LineAssembler::finish(std::chrono::nanoseconds now)
{
    if (pending_.empty() && !truncated_) return std::nullopt;
    return take(now, true);
}

void LineAssembler::append(std::string_view bytes)
{
    // Everything past the cap is dropped until the next newline, so one huge line
    // cannot grow memory without bound.
    const std::size_t room = max_line_ - pending_.size();
    if (bytes.size() > room) {
        bytes = bytes.substr(0, room);
        truncated_ = true;
    }
    pending_.append(bytes);
}

Record LineAssembler::take(std::chrono::nanoseconds now, bool no_newline)
{
    Record record{.stream = stream_,
                  .at = now,
                  .text = std::exchange(pending_, {}),
                  .truncated = truncated_,
                  .no_newline = no_newline};
    truncated_ = false;
    return record;
}

} // namespace blackbox
