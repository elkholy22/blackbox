#include "supervisor.hpp"

#include "dump.hpp"
#include "exit_status.hpp"
#include "line_assembler.hpp"
#include "ring_buffer.hpp"
#include "unique_fd.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace blackbox {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t kReadSize = 64 * 1024;

// Once the child has exited, a grandchild may still hold the pipes open. We keep reading
// only until they have been quiet this long, so a leftover daemon cannot keep us alive.
constexpr int kIdleAfterExitMs = 100;

[[noreturn]] void throw_errno(const char* what)
{
    throw std::system_error(errno, std::generic_category(), what);
}

template <typename Call>
auto retry_on_eintr(Call call)
{
    decltype(call()) result;
    do {
        result = call();
    } while (result == -1 && errno == EINTR);
    return result;
}

void report(std::string_view message)
{
    std::cerr << std::format("blackbox: {}\n", message);
}

/// Returns 0 on success or the errno of the failed write.
int write_all(int fd, std::string_view data)
{
    while (!data.empty()) {
        const ssize_t n = retry_on_eintr([&] { return ::write(fd, data.data(), data.size()); });
        if (n < 0) return errno;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return 0;
}

struct Pipe {
    UniqueFd read;
    UniqueFd write;
};

Pipe make_pipe()
{
    std::array<int, 2> fds{};
    if (::pipe2(fds.data(), O_CLOEXEC) != 0) throw_errno("pipe2");
    return {UniqueFd(fds[0]), UniqueFd(fds[1])};
}

// If we were started with fd 0, 1 or 2 closed, pipe2 would hand out that number and
// dup2 in the child would mix up the streams.
void ensure_standard_fds_open()
{
    for (int fd = 0; fd <= 2; ++fd) {
        if (::fcntl(fd, F_GETFD) == -1 && errno == EBADF) {
            // open returns the lowest free number, which is fd. It stays open on purpose.
            if (::open("/dev/null", O_RDWR) != fd) throw_errno("open /dev/null");
        }
    }
}

constexpr std::array kForwardedSignals{SIGINT, SIGTERM, SIGHUP, SIGQUIT};

sigset_t handled_signals()
{
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, SIGCHLD);
    ::sigaddset(&set, SIGUSR1);
    for (int sig : kForwardedSignals) ::sigaddset(&set, sig);
    return set;
}

bool sent_by_process(const signalfd_siginfo& info)
{
    // Ctrl+C and Ctrl+\ arrive as SI_KERNEL. The terminal sends those to the whole
    // foreground process group, so the child has its own copy already.
    const int code = info.ssi_code;
    return code == SI_USER || code == SI_QUEUE || code == SI_TKILL;
}

// A shell can tell "killed by SIGINT" apart from "exited with 130". Bash, for example, only
// stops a script loop on Ctrl+C when the job died from SIGINT. GNU timeout does the same.
void raise_same_signal(int sig)
{
    // No core dump or crash report for blackbox, the crash was the command's. Unlike
    // RLIMIT_CORE, this also holds when core_pattern pipes to a crash handler.
    ::prctl(PR_SET_DUMPABLE, 0);

    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    ::sigaction(sig, &default_action, nullptr);

    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, sig);
    ::sigprocmask(SIG_UNBLOCK, &set, nullptr);
    ::raise(sig);
}

[[noreturn]] void exec_child(std::vector<char*>& argv, const sigset_t& original_mask, int out_fd,
                             int err_fd, int exec_error_fd)
{
    // Between fork and exec only async signal safe calls are allowed.
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    ::sigaction(SIGPIPE, &default_action, nullptr);
    ::sigprocmask(SIG_SETMASK, &original_mask, nullptr);

    if (::dup2(out_fd, STDOUT_FILENO) != -1 && ::dup2(err_fd, STDERR_FILENO) != -1)
        ::execvp(argv[0], argv.data());

    const int error = errno;
    // If this write fails too, the parent sees a normal exit 127 and reports that.
    [[maybe_unused]] const ssize_t ignored = ::write(exec_error_fd, &error, sizeof error);
    ::_exit(127);
}

class Supervisor {
public:
    explicit Supervisor(const Options& options) : options_(options), lines_(options.keep) {}

    int run();

private:
    struct Channel {
        Stream stream;
        int target; // our own stdout or stderr
        UniqueFd pipe;
        LineAssembler assembler;
    };

    UniqueFd start_child(const sigset_t& original_mask);
    std::optional<int> check_exec(UniqueFd exec_error);
    void loop();
    void read_from(Channel& channel);
    void close_channel(Channel& channel);
    void handle_signals();
    void reap(int flags);
    void kill_and_reap() noexcept;
    void write_dump(const std::string& cause, std::optional<int> exit_code);
    std::chrono::nanoseconds elapsed() const { return Clock::now() - started_; }

    const Options& options_;
    RingBuffer<Record> lines_;
    std::array<Channel, 2> channels_{
        Channel{Stream::Out, STDOUT_FILENO, UniqueFd(), LineAssembler(Stream::Out)},
        Channel{Stream::Err, STDERR_FILENO, UniqueFd(), LineAssembler(Stream::Err)},
    };
    std::vector<char> buffer_ = std::vector<char>(kReadSize);
    UniqueFd signal_fd_;
    pid_t child_ = -1;
    std::optional<ExitStatus> status_;
    bool stop_draining_ = false;
    Clock::time_point started_;
    std::chrono::system_clock::time_point started_wall_;
    std::chrono::nanoseconds runtime_{};
};

int Supervisor::run()
{
    ensure_standard_fds_open();

    // Signals are blocked and read from a signalfd in the poll loop, so no work happens
    // in an asynchronous handler.
    const sigset_t handled = handled_signals();
    sigset_t original_mask;
    if (::sigprocmask(SIG_BLOCK, &handled, &original_mask) != 0) throw_errno("sigprocmask");
    signal_fd_ = UniqueFd(::signalfd(-1, &handled, SFD_CLOEXEC | SFD_NONBLOCK));
    if (!signal_fd_) throw_errno("signalfd");

    // A reader that goes away must show up as EPIPE from write instead of killing us.
    struct sigaction ignore {};
    ignore.sa_handler = SIG_IGN;
    if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) throw_errno("sigaction");

    // An inherited SIG_IGN would make the kernel reap the child and we would never see SIGCHLD.
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    if (::sigaction(SIGCHLD, &default_action, nullptr) != 0) throw_errno("sigaction");

    UniqueFd exec_error = start_child(original_mask);
    try {
        if (const auto code = check_exec(std::move(exec_error))) return *code;
        loop();
        // loop() can return while a grandchild still holds a pipe. Keep the partial line it has.
        for (auto& channel : channels_) {
            if (channel.pipe) close_channel(channel);
        }
    } catch (...) {
        kill_and_reap();
        throw;
    }

    const int code = shell_exit_code(*status_);
    if (code != 0 || options_.always) write_dump(describe(*status_), code);
    if (status_->signaled) raise_same_signal(status_->signal);
    return code; // only reached if the signal did not end us
}

UniqueFd Supervisor::start_child(const sigset_t& original_mask)
{
    Pipe out = make_pipe();
    Pipe err = make_pipe();
    Pipe exec_error = make_pipe();

    std::vector<std::string> words = options_.command;
    std::vector<char*> argv;
    for (auto& word : words) argv.push_back(word.data());
    argv.push_back(nullptr);

    started_wall_ = std::chrono::system_clock::now();
    started_ = Clock::now();
    child_ = ::fork();
    if (child_ < 0) throw_errno("fork");
    if (child_ == 0)
        exec_child(argv, original_mask, out.write.get(), err.write.get(), exec_error.write.get());

    // The write ends are closed in the parent when they go out of scope here. Without
    // that, our reads would never see EOF after the child exits.
    channels_[0].pipe = std::move(out.read);
    channels_[1].pipe = std::move(err.read);
    return std::move(exec_error.read);
}

std::optional<int> Supervisor::check_exec(UniqueFd exec_error)
{
    // The write end is O_CLOEXEC, so a successful exec closes it and we read EOF.
    int error = 0;
    const ssize_t n =
        retry_on_eintr([&] { return ::read(exec_error.get(), &error, sizeof error); });
    if (n < 0) throw_errno("read");
    if (n == 0) return std::nullopt;

    reap(0);
    report(std::format("{}: {}", options_.command[0], std::strerror(error)));
    return error == ENOENT ? 127 : 126;
}

void Supervisor::loop()
{
    std::array<pollfd, 3> fds{};
    std::array<Channel*, 3> channel_at{};

    while (true) {
        const bool pipes_open = channels_[0].pipe || channels_[1].pipe;
        if (status_ && (!pipes_open || stop_draining_)) return;

        std::size_t count = 0;
        fds[count++] = {signal_fd_.get(), POLLIN, 0};
        for (auto& channel : channels_) {
            if (!channel.pipe) continue;
            channel_at[count] = &channel;
            fds[count++] = {channel.pipe.get(), POLLIN, 0};
        }

        const int timeout = status_ ? kIdleAfterExitMs : -1;
        const int ready = retry_on_eintr([&] { return ::poll(fds.data(), count, timeout); });
        if (ready < 0) throw_errno("poll");
        if (ready == 0) return; // the child is gone and nobody wrote for a while

        if (fds[0].revents != 0) handle_signals();
        for (std::size_t i = 1; i < count; ++i) {
            if (fds[i].revents != 0) read_from(*channel_at[i]);
        }
    }
}

void Supervisor::read_from(Channel& channel)
{
    const ssize_t n =
        retry_on_eintr([&] { return ::read(channel.pipe.get(), buffer_.data(), buffer_.size()); });
    if (n < 0) throw_errno("read");

    if (n == 0) {
        close_channel(channel);
        return;
    }

    const std::string_view chunk(buffer_.data(), static_cast<std::size_t>(n));
    for (auto& record : channel.assembler.feed(chunk, elapsed())) lines_.push(std::move(record));

    if (const int error = write_all(channel.target, chunk); error != 0) {
        // Closing our read end gives the child SIGPIPE on its next write, the same as it
        // would get without us in between.
        if (error != EPIPE) {
            report(std::format("cannot forward {}: {}",
                               channel.stream == Stream::Out ? "stdout" : "stderr",
                               std::strerror(error)));
        }
        close_channel(channel);
    }
}

void Supervisor::close_channel(Channel& channel)
{
    if (auto last = channel.assembler.finish(elapsed())) lines_.push(std::move(*last));
    channel.pipe.reset();
}

void Supervisor::handle_signals()
{
    signalfd_siginfo info{};
    while (true) {
        const ssize_t n = ::read(signal_fd_.get(), &info, sizeof info);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) return;
            throw_errno("read signalfd");
        }

        const int sig = static_cast<int>(info.ssi_signo);
        if (sig == SIGCHLD) {
            if (!status_) reap(WNOHANG);
        } else if (sig == SIGUSR1) {
            write_dump("snapshot (SIGUSR1)", std::nullopt);
        } else if (status_) {
            // The child is gone, so this is meant for us. Stop waiting for background writers.
            stop_draining_ = true;
        } else if (sent_by_process(info)) {
            ::kill(child_, sig);
        }
    }
}

void Supervisor::reap(int flags)
{
    int raw = 0;
    const pid_t pid = retry_on_eintr([&] { return ::waitpid(child_, &raw, flags); });
    if (pid < 0) throw_errno("waitpid");
    if (pid == child_) {
        status_ = decode_wait_status(raw);
        runtime_ = elapsed();
    }
}

void Supervisor::kill_and_reap() noexcept
{
    if (child_ <= 0 || status_) return;
    ::kill(child_, SIGKILL);
    int raw = 0;
    retry_on_eintr([&] { return ::waitpid(child_, &raw, 0); });
}

void Supervisor::write_dump(const std::string& cause, std::optional<int> exit_code)
{
    const DumpInfo info{
        .command = options_.command,
        .started = started_wall_,
        .runtime = status_ ? runtime_ : elapsed(),
        .cause = cause,
        .exit_code = exit_code,
    };
    try {
        append_dump(options_.out, format_dump(info, lines_));
    } catch (const std::exception& e) {
        report(std::format("cannot write dump: {}", e.what()));
        return;
    }
    if (!options_.quiet) report(std::format("dump written to {} ({})", options_.out, cause));
}

} // namespace

int run(const Options& options)
{
    Supervisor supervisor(options);
    return supervisor.run();
}

} // namespace blackbox
