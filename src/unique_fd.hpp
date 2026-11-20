#pragma once

#include <utility>

#include <unistd.h>

namespace blackbox {

/// Owns a file descriptor and closes it in the destructor. Move only.
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

    int release() { return std::exchange(fd_, -1); }

    void reset(int fd = -1)
    {
        // No retry on EINTR: Linux releases the descriptor even when close is interrupted,
        // and a retry could close a descriptor another call just reused.
        if (fd_ >= 0) ::close(fd_);
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

} // namespace blackbox
