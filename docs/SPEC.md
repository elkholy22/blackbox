# blackbox specification

Version 1.0 scope. This document is the contract for the first release. Anything not listed under "In scope" is out of scope until it moves here from the roadmap.

## Pitch

`blackbox` is a dashcam for Linux processes. You put it in front of any command, and the command runs exactly as before: its stdout and stderr reach the terminal or the CI log byte for byte, and blackbox exits with the same code. Meanwhile blackbox keeps the last N lines of output in memory with timestamps. Nothing is written while things go well. When the command crashes or exits with a nonzero code, blackbox writes a short dump: the last N lines, the exit cause (for example `signal 11 (SIGSEGV), core dumped` or `exit 3`), the runtime and the full command line. Sending SIGUSR1 to blackbox writes a snapshot at any time without stopping the command. The use case is any process whose last lines matter and get lost: CI jobs with huge logs, robot nodes, long running services on a target without a debugger attached.

```
$ blackbox --keep 200 -o crash.txt -- ./planner --map track.yaml
... normal output ...
blackbox: dump written to crash.txt (signal 6 (SIGABRT), core dumped)
$ echo $?
134
```

## Scope

### In scope

* A command line tool `blackbox [OPTIONS] [--] COMMAND [ARGS...]` that starts COMMAND as a child process.
* Passthrough of the child's stdout to blackbox's stdout and the child's stderr to blackbox's stderr, unchanged and without waiting for full lines.
* A ring buffer of the last N complete lines from both streams, each tagged with its stream and a timestamp relative to the start.
* Correct handling of lines split across reads, a last line without a newline, and very long lines (stored up to a cap and marked as truncated).
* A dump when the child exits nonzero or is killed by a signal, and on every exit with `--always`.
* A snapshot dump on SIGUSR1 while the child keeps running.
* Forwarding of SIGINT, SIGTERM, SIGHUP and SIGQUIT to the child.
* Exit status transparency: the child's exit code, or death by the same signal when the child was killed by a signal.
* Own exit codes in the style of `env` and `timeout`: 125 for blackbox errors, 126 for a command that cannot be executed, 127 for a command that was not found.
* User docs (README), contributor docs (CONTRIBUTING), ROADMAP, ESSAY, CMake build with CTest, C++ unit tests, Python component tests.

### Out of scope (see the roadmap)

* Pseudo terminal mode. The child writes into pipes, so C stdio and Python switch to block buffering. The README documents `stdbuf -oL` and `python -u` as the workaround.
* Supervising more than one process, restarts, daemon mode. That is the job of systemd.
* Log rotation, compression, uploading dumps, journald integration.
* A strict global order between stdout and stderr lines. The order in the dump is the order in which blackbox read the data, which is best effort.
* Capturing core files or stack traces.
* Machine readable dump formats (JSON), configuration files.
* Ports to anything other than Linux with glibc.

## Functional requirements

FR-1. `blackbox [OPTIONS] [--] COMMAND [ARGS...]` runs COMMAND with ARGS, searching `PATH` like `execvp`. Option parsing stops at `--` or at the first argument that is not an option, so `blackbox ls -l` runs `ls -l`.

FR-2. The child inherits blackbox's stdin, environment and working directory unchanged.

FR-3. Every byte the child writes to stdout appears on blackbox's stdout, and every byte it writes to stderr appears on blackbox's stderr, in the same order per stream. blackbox forwards each chunk as soon as it has read it and does not wait for a newline.

FR-4. blackbox keeps the last N lines in a ring buffer (`--keep N`, default 100, allowed range 0 to 10000). Lines from stdout and stderr share one buffer. Each record stores the stream, the time since the child was started (monotonic clock) and the line text without the newline.

FR-5. A line longer than 4096 bytes is stored with its first 4096 bytes and marked as truncated. The passthrough of that line is not affected.

FR-6. When a stream ends with bytes after the last newline, those bytes are stored as a final record marked as having no newline. The same applies when blackbox stops reading a stream early (FR-20, or a background process that keeps the pipe open after the child exited).

FR-7. After the child ends, blackbox writes a dump if the child exited with a nonzero code or was killed by a signal. With `--always` it also writes a dump after exit code 0. A clean run without `--always` creates no file.

FR-8. A dump contains, in this order: the command line (shell quoted so it can be pasted back), the start time in UTC, the runtime, the cause, the exit status a shell reports for blackbox (final dumps only), the number of lines kept and seen, and then the kept lines from oldest to newest with timestamp, stream and text. Control bytes in the command line are escaped as in FR-10, so an argument with a newline cannot break the layout.

FR-9. The cause reads `exit N` for a normal exit, `signal N (NAME)` for a signal, with `, core dumped` appended when the kernel reports a core dump, and `snapshot (SIGUSR1)` for a snapshot.

FR-10. Control bytes in stored lines (0x00 to 0x1F except tab, and 0x7F) are written to the dump as `\xNN`, so a dump is always a readable text file. Other bytes, including invalid UTF-8, are written unchanged.

FR-11. Dumps are appended to the output file (`-o FILE`, `--out FILE`, default `blackbox.dump` in the current directory). The file is created with mode 0644 on the first dump. Each dump is formatted in memory and written with one `write` call on an `O_APPEND` descriptor, so two blackbox processes sharing a file on a local filesystem do not interleave their dumps. A short write is reported as an error (FR-19) instead of being completed with a second call.

FR-12. SIGUSR1 sent to blackbox writes a snapshot dump of the complete lines kept so far. The child is not signalled and keeps running.

FR-13. SIGINT, SIGTERM, SIGHUP and SIGQUIT sent to blackbox by another process are forwarded to the child. A signal generated by the terminal (Ctrl+C, Ctrl+\\) is not forwarded, because the terminal already delivered it to the whole foreground process group, which includes the child. blackbox keeps running until the child has ended and then follows FR-7. Once the child has ended, these signals are no longer forwarded. They stop blackbox from waiting for background output (see step 6 of the process handling below).

FR-14. blackbox exits with the child's exit code after a normal exit. After the child was killed by signal N, blackbox writes its dump and then kills itself with signal N, so its parent sees the same kind of wait status (a shell reports 128 + N). This is what GNU `timeout` does, and it matters for shells: `bash` only stops a script loop on Ctrl+C when the foreground job died from SIGINT. blackbox disables its own core dump first. If the signal does not end it, blackbox exits with 128 + N.

FR-15. If COMMAND is not found, blackbox prints `blackbox: COMMAND: No such file or directory` to stderr and exits 127. If it exists but cannot be executed, blackbox prints the reason and exits 126. No dump is written in either case.

FR-16. Usage errors (unknown option, missing command, invalid or out of range `--keep` value, missing option argument) print a one line error plus a hint to `--help` on stderr and exit 125 without starting the command.

FR-17. Before starting the command, blackbox checks that the directory of the dump file exists and is writable, and that the dump file is writable if it already exists. If not, it exits 125 without starting the command.

FR-18. When a dump is written, blackbox prints one line `blackbox: dump written to FILE (CAUSE)` to stderr after the child's output. `-q` / `--quiet` suppresses it.

FR-19. If writing a dump fails, blackbox reports it on stderr and still exits with the child's code (FR-14). The child's result is the information a CI job needs most, and the error is visible in the log.

FR-20. When blackbox's stdout or stderr is closed by the reader (for example `blackbox -- cmd | head -1`), blackbox stops forwarding that stream and closes its read end of the child's pipe. The child then gets SIGPIPE on its next write, the same as without blackbox in between.

FR-21. `-h` / `--help` prints usage to stdout and exits 0. `--version` prints `blackbox VERSION` and exits 0.

## Non functional requirements

NFR-1. Transparency. For any child output, the bytes on blackbox's stdout and stderr are identical to running the child directly. Checked by a component test that compares SHA-256 hashes of binary output including NUL bytes and invalid UTF-8.

NFR-2. Bounded memory. Memory use does not grow with the amount of output. The ring buffer holds at most N × 4096 bytes of text plus a small per record overhead, and both pipes share one 64 KiB read buffer. Checked by a component test: 50 MB of output with the default `--keep` stays below 32 MB peak RSS (`ru_maxrss` from `os.wait4`).

NFR-3. Throughput. On a current laptop blackbox passes at least 200 MB/s of stdout to `/dev/null`. The component test uses a looser bound (50 MB in under 10 s) so it does not fail on a loaded CI runner.

NFR-4. No deadlock. A child that floods stdout and stderr at the same time never blocks forever, because both pipes are serviced by one `poll` loop. Covered by the flood test in NFR-2.

NFR-5. No leaks into the child. The child starts with only file descriptors 0, 1 and 2 open (everything blackbox opens is `O_CLOEXEC`), with the signal mask blackbox itself was started with (normally empty) and the default disposition for SIGPIPE. Checked by component tests: a Python child probes every descriptor number with `fstat`, and `cat /proc/self/status` shows the signal mask and the ignored signals (Python cannot be used for that check, because it ignores SIGPIPE at startup).

NFR-6. No zombies, no lost signals. blackbox always reaps the child before it exits. Signals are received through `signalfd`, so no work happens in an asynchronous signal handler. All system calls are retried on `EINTR`.

NFR-7. Clean code. Builds without warnings with `-Wall -Wextra -Wpedantic -Werror` on g++ 13 and clang 16 or newer. All unit and component tests pass under AddressSanitizer and UndefinedBehaviorSanitizer (CMake preset `asan`).

NFR-8. Platform. Linux with glibc 2.32 or newer (`sigabbrev_np`), any kernel since 2.6.27 (`signalfd4`, `pipe2`). Developed and tested on Ubuntu 24.04 (g++ 13.3, CMake 3.28, glibc 2.39, x86_64).

NFR-9. Usability. `--help` fits on one screen (under 25 lines). Every message blackbox prints itself starts with `blackbox: `. The dump format is plain text that a person can read without a tool.

NFR-10. Size. The implementation stays below 1000 lines of C++ without tests, blank lines and comments (about 790 in version 1.0), so a new contributor can read all of it in one sitting.

## CLI

```
Usage: blackbox [OPTIONS] [--] COMMAND [ARGS...]

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
```

Examples:

```
# keep the last 500 lines of a test run, dump only on failure
blackbox -n 500 -o test-failure.txt -- ctest --output-on-failure

# take a snapshot of a running node without stopping it
blackbox -o node.dump -- python3 -u my_node.py &
sleep 1     # signals sent before blackbox has started its loop end it
kill -USR1 $!

# the exit code passes through, so scripts keep working
blackbox -- make -j8 || echo "build failed, see blackbox.dump"
```

Example dump:

```
blackbox dump
command: ./planner --map 'track 2.yaml'
started: 2026-11-20T14:03:12Z
runtime: 12.345s
cause:   signal 6 (SIGABRT), core dumped
exit:    134
lines:   3 kept, 1043 seen

    12.101 out loop 1041
    12.214 err planner: no path found
    12.330 err terminate called after throwing an instance of 'std::runtime_error'
```

Timestamps are seconds since the child was started, with millisecond precision. A truncated line ends with ` [truncated]`, a last line without newline ends with ` [no newline]`. Dumps in the same file are separated by one empty line.

## Architecture

### Overview

One process, one thread, one event loop. The design splits the code into a pure core that does no process or signal system calls and can be unit tested with plain values, and a thin Linux layer that starts the child and runs the loop.

Data flow:

1. The child writes into two pipes, one for stdout and one for stderr.
2. A single `poll` loop waits on both pipes and on a `signalfd`.
3. Each chunk read from a pipe goes to our matching output unchanged, and to that stream's `LineAssembler`.
4. The assembler emits a `Record` for every finished line, and the record goes into one shared `RingBuffer<Record>`.
5. On failure, with `--always` or on SIGUSR1, `format_dump` renders the buffer into text, which is appended to the dump file.

### Modules

* `options` parses argv into `Options`. Throws `UsageError` with a message. No I/O.
* `ring_buffer` is a fixed capacity circular buffer template. Pushing into a full buffer overwrites the oldest element. Counts every push, so the dump can say how many lines were seen in total.
* `line_assembler` turns a stream of byte chunks into `Record`s. Keeps the pending partial line between calls, applies the 4096 byte cap and the truncation flag, and produces the final record at end of stream.
* `exit_status` decodes a raw `waitpid` status into `ExitStatus`, turns it into the cause text of FR-9, and maps it to the shell exit code of FR-14.
* `dump` formats a dump into a string (pure, golden tested) and contains the helpers `shell_quote`, `escape_control` and the runtime formatting. Writing the string to the file is a separate small function.
* `unique_fd` is an RAII owner of a file descriptor (move only, closes in the destructor).
* `supervisor` is the Linux layer: blocks signals, creates the signalfd and the pipes, forks and execs the child, runs the poll loop, forwards signals, reaps the child and decides when to dump. The only module that is not unit tested; the component tests cover it.
* `main` calls `parse_args`, handles `--help` and `--version`, checks the dump directory, calls `run` and returns its exit code. Catches `UsageError` and system errors and maps them to 125.

### Key types

```cpp
enum class Stream { Out, Err };

struct Record {
    Stream stream;
    std::chrono::nanoseconds at;   // since the child was started
    std::string text;              // without the newline, at most 4096 bytes
    bool truncated = false;
    bool no_newline = false;
};

template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity);
    void push(T value);
    std::size_t size() const;
    std::size_t capacity() const;
    std::uint64_t total_pushed() const;
    const T& operator[](std::size_t i) const;   // 0 is the oldest
};

class LineAssembler {
public:
    LineAssembler(Stream stream, std::size_t max_line = 4096);
    std::vector<Record> feed(std::string_view chunk, std::chrono::nanoseconds now);
    std::optional<Record> finish(std::chrono::nanoseconds now);
};

struct ExitStatus {
    bool signaled = false;
    int code = 0;        // exit code when !signaled
    int signal = 0;      // signal number when signaled
    bool core_dumped = false;
};
ExitStatus decode_wait_status(int raw);
std::string describe(const ExitStatus& status);
int shell_exit_code(const ExitStatus& status);

struct Options {
    std::size_t keep = 100;
    std::string out = "blackbox.dump";
    bool always = false;
    bool quiet = false;
    bool help = false;
    bool version = false;
    std::vector<std::string> command;
};
Options parse_args(std::span<const char* const> args);   // throws UsageError

struct DumpInfo {
    std::vector<std::string> command;
    std::chrono::system_clock::time_point started;
    std::chrono::nanoseconds runtime;
    std::string cause;
    std::optional<int> exit_code;   // empty for a snapshot
};
std::string format_dump(const DumpInfo& info, const RingBuffer<Record>& lines);

int run(const Options& options);   // returns blackbox's exit code
```

### Process and signal handling

1. If blackbox was started with descriptor 0, 1 or 2 closed, open `/dev/null` on it. Otherwise the next pipe would get that number and the child's streams would get mixed up. Block SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT and SIGUSR1 with `sigprocmask` and create a `signalfd` for them. Ignore SIGPIPE in blackbox, so a closed reader shows up as `EPIPE` from `write`. Reset SIGCHLD to the default disposition, because a SIGCHLD inherited as ignored makes the kernel reap the child without telling us.
2. Create the stdout pipe, the stderr pipe and an exec error pipe with `pipe2(O_CLOEXEC)`.
3. `fork`. In the child: restore the original signal mask, reset SIGPIPE to the default disposition, `dup2` the pipe write ends onto 1 and 2, and call `execvp`. If `execvp` returns, write `errno` into the exec error pipe and `_exit(127)`. Only async signal safe calls are used between `fork` and `exec`.
4. In the parent: close the write ends, then read the exec error pipe. EOF with no data means `exec` succeeded, because the `O_CLOEXEC` end closed during `exec`. Otherwise reap the child and exit 127 for `ENOENT` and 126 for anything else.
5. Loop on `poll` over the two pipes and the signalfd:
   * Pipe readable: `read` up to 64 KiB, write the chunk to our stdout or stderr, feed it to that stream's `LineAssembler`, push the records into the ring buffer. On EOF, flush the assembler and stop polling that pipe.
   * SIGUSR1: write a snapshot dump.
   * SIGINT, SIGTERM, SIGHUP, SIGQUIT: forward with `kill(child, sig)` if `ssi_code` is `SI_USER` or `SI_QUEUE` (sent by a process). Terminal generated signals (`SI_KERNEL`) already reached the child.
   * SIGCHLD: `waitpid(child, WNOHANG)`. If the child has ended, remember the status.
6. The loop ends when the child is reaped and both pipes reached EOF. If the child is reaped but a pipe stays open (a background grandchild inherited it), blackbox keeps reading until the pipe is idle for 100 ms, then stops. This avoids hanging forever on a daemon the child left behind. A background process that keeps writing without a pause keeps blackbox forwarding until it pauses or closes the pipe, which is documented as a known limitation. SIGINT, SIGTERM, SIGHUP or SIGQUIT in this phase end the loop at once.
7. Decide on the final dump (FR-7), write it, print the message (FR-18). If the child was killed by a signal, reset that signal to the default action, unblock it and raise it (FR-14). Otherwise return `shell_exit_code`.

Writes to our stdout and stderr are blocking. If the reader is slow, blackbox waits, and the child then blocks on its full pipe. That is the same back pressure the child would see without blackbox. Signals that arrive meanwhile wait in the signalfd and are handled on the next loop turn.

If a system call fails in a way blackbox cannot handle after the child has started (for example `poll` fails), blackbox prints the error, sends SIGKILL to the child, reaps it and exits 125, so it never leaves an orphan behind.

## File layout

```
blackbox/
  CMakeLists.txt
  CMakePresets.json          debug, release, asan
  README.md                  user docs: what, build, usage, dump format, limitations
  CONTRIBUTING.md            structure, dev setup, tests, conventions, design notes
  ROADMAP.md
  ESSAY.md
  LICENSE                    MIT
  .clang-format
  .gitattributes             text eol=lf
  .gitignore
  .github/workflows/ci.yml   build, unit tests, component tests, asan job
  requirements-dev.txt       pytest and pytest-timeout, pinned
  pytest.ini                 test path and a 60 s timeout per test
  docs/
    SPEC.md                  this file
  src/
    main.cpp
    options.hpp  options.cpp
    record.hpp               Stream and Record
    ring_buffer.hpp
    line_assembler.hpp  line_assembler.cpp
    exit_status.hpp  exit_status.cpp
    dump.hpp  dump.cpp
    unique_fd.hpp
    supervisor.hpp  supervisor.cpp
  tests/
    unit/
      CMakeLists.txt
      ring_buffer_test.cpp
      line_assembler_test.cpp
      exit_status_test.cpp
      options_test.cpp
      dump_test.cpp
      unique_fd_test.cpp
    component/
      conftest.py            finds the binary, helpers to run it and parse dumps
      child.py               test child with modes: print N lines, crash, flood, wait
      test_passthrough.py
      test_dumps.py
      test_signals.py
      test_errors.py
      test_limits.py
```

Everything except `main.cpp` and `supervisor.cpp` builds into a static library `blackbox_core`, which both the executable and the unit tests link. That keeps the unit tests free of process handling.

## Build system and dependencies

* C++20. It gives `std::span`, `std::format`, designated initializers and `<chrono>` calendar types, and it is fully supported by g++ 13 and clang 16. C++23 would add `std::expected`, but older toolchains for embedded Linux targets often lag behind, and the code does not need it. Usage errors use one exception type instead.
* CMake 3.25 or newer with presets. It is the standard build system for C++ on Linux, IDEs understand it, and CTest gives one command for both test suites. Presets keep the debug, release and sanitizer configurations in the repository instead of in someone's shell history.
* GoogleTest 1.15.2 through `FetchContent`, pinned by URL and SHA-256. The most common C++ test framework, so reviewers and contributors know it. Fetching it pinned avoids depending on a system package version. It is the only third party dependency, and only for tests.
* No runtime dependencies besides the C++ standard library and glibc. The tool is meant to run on build servers and targets where installing extra libraries is a hassle.
* Python 3.10 or newer with pytest for the component tests. The tests use only the standard library otherwise (`subprocess`, `signal`, `hashlib`, `os`, `resource`). pytest-timeout is a safety net, so a hanging test fails after 60 s instead of blocking CI. A venv with `requirements-dev.txt` is the documented setup.
* CTest registers the unit tests through `gtest_discover_tests` and the component suite as one test that calls `python3 -m pytest tests/component` with the path of the built binary in `BLACKBOX_BIN`. `ctest --preset debug` therefore runs everything.
* Compiler flags: `-Wall -Wextra -Wpedantic -Werror` for our targets only, not for GoogleTest. The `asan` preset adds `-fsanitize=address,undefined -fno-omit-frame-pointer`.
* `cmake --install` puts the binary into `PREFIX/bin`.

## Test plan

The split follows the architecture. Logic with many edge cases lives in the pure core and gets unit tests with exact expectations. Behavior that only exists with a real process (signals, pipes, exit codes, file descriptors) is tested from the outside with the real binary, because mocking those system calls would mostly test the mocks.

### C++ unit tests (GoogleTest)

`ring_buffer_test`
* empty buffer has size 0
* fewer pushes than capacity keeps all, in order
* exactly full, then one more push drops the oldest
* many wraparounds still return oldest to newest
* capacity 0 ignores pushes but still counts them
* capacity 1 always holds the latest
* `total_pushed` counts beyond capacity

`line_assembler_test`
* one chunk with one line
* one chunk with several lines, including empty lines
* a line split across two and across three chunks
* a chunk that ends right after a newline leaves nothing pending
* `\r\n`: the `\r` stays in the text (it is escaped later in the dump)
* NUL bytes and invalid UTF-8 are kept unchanged
* a line of exactly 4096 bytes is not truncated, 4097 bytes is truncated
* the bytes after the cap are dropped until the next newline, and the next line is normal again
* `finish` returns the pending partial line with `no_newline`, and nothing when there is none
* the timestamp of a record is the time of the chunk that completed it

`exit_status_test`
* decoding exit 0, 3 and 255
* decoding SIGSEGV without and SIGABRT with the core flag (raw status values built with the documented Linux layout)
* `describe` for each of those, and for an unknown signal number (`signal 77`)
* `shell_exit_code`: exit code unchanged, 128 + N for signals

`options_test`
* defaults when only a command is given
* short and long forms of every option, `--keep=5` and `--keep 5`
* `--` ends option parsing, and a command that starts with `-` works after `--`
* parsing stops at the first non option, so `blackbox ls -l` keeps `-l` for `ls`
* errors: missing command, unknown option, missing value, `--keep` with `-1`, `abc`, `10x`, `10001`
* `--help` and `--version` work without a command

`dump_test`
* golden test of a full dump with fixed time values and a few records of both streams
* snapshot dump has no `exit:` line
* truncated and no newline markers
* `escape_control` for tab (kept), `\r`, `\x00`, `\x1b` and `\x7f`
* `shell_quote` for plain words, spaces, single quotes and the empty string
* runtime formatting with millisecond precision
* `append_dump` appends to an existing file and reports a missing directory

`unique_fd_test`
* closes on destruction, move construction and move assignment transfer ownership, `release` and `reset`

### Python component tests (pytest)

Each test starts the built binary with `subprocess` and uses `child.py` as the command, run with `python3 -u` so its output is not block buffered. The tests check what a user sees: exit codes, bytes on stdout and stderr, the dump file, timing only with generous limits.

`test_passthrough.py`
* binary output with NUL bytes and invalid UTF-8 on both streams: SHA-256 equal to running the child directly, exit 0, no dump file (FR-3, FR-7, NFR-1)
* exit codes 0, 1, 42 and 255 pass through unchanged (FR-14)
* stdin reaches the child (FR-2)
* output is forwarded while the child still runs, not at its end (FR-3)
* the child starts with only fds 0, 1, 2, an empty signal mask and default SIGPIPE (NFR-5)
* blackbox started with fd 1 closed still keeps the child's stdout and stderr apart

`test_dumps.py`
* 1000 lines then `abort()` with `--keep 50`: blackbox dies from SIGABRT, the dump says `exit: 134`, the dump has exactly lines 951 to 1000 in order and names SIGABRT (FR-4, FR-8, FR-9)
* SIGSEGV through `os.kill` on itself: blackbox dies from SIGSEGV, cause names SIGSEGV
* exit 3: dump written, `exit 3`, blackbox exits 3
* exit 0 with `--always`: dump with `exit 0`
* a 10000 byte line and a last line without newline: markers present, passthrough unchanged (FR-5, FR-6)
* `--keep 0`: dump has the header and no lines
* control bytes in a line are escaped in the dump (FR-10)
* the command line in the header is shell quoted (FR-8)
* two failing runs into the same file: two dumps, appended (FR-11)
* without `-o` the dump goes to `blackbox.dump` in the working directory (FR-11)
* the "dump written" message on stderr, and none with `--quiet` (FR-18)
* a dump that cannot be written still leaves the child's exit code (FR-19)

`test_signals.py`
* SIGINT, SIGTERM, SIGHUP and SIGQUIT sent to blackbox reach the child: blackbox dies from the same signal, the cause names it (FR-13, FR-14)
* Ctrl+C on a pseudo terminal reaches a child that counts SIGINTs exactly once (FR-13)
* Ctrl+C on a pseudo terminal stops a `bash` loop that runs blackbox (FR-14)
* a child that handles SIGTERM, prints a goodbye line and exits 0: the goodbye line is passed through and blackbox exits 0
* SIGUSR1 while the child waits: the snapshot appears, the child keeps running, then exits 0 and no second dump is written (FR-12)
* reader closes stdout after one line while the child keeps writing: blackbox ends within 5 s and dies from SIGPIPE (FR-20)
* stdout is `/dev/full`: blackbox reports `cannot forward stdout`, the child dies from SIGPIPE (FR-20)
* a busy background writer is left behind: SIGTERM still ends blackbox (FR-13)
* blackbox started with SIGCHLD ignored still sees the child exit
* the child's own background grandchild keeps the pipe open: blackbox still ends within 5 s

`test_errors.py`
* command not found: exit 127, message on stderr, no dump (FR-15)
* file without execute permission: exit 126 (FR-15)
* usage errors, parametrized: no command, unknown option, bad `--keep` values: exit 125, child never started (FR-16)
* dump directory does not exist: exit 125 before the child runs (FR-17)
* `--help` and `--version` exit 0 (FR-21)

`test_limits.py`
* 50 MB interleaved on stdout and stderr: finishes in under 10 s, output length correct, peak RSS under 32 MB (NFR-2, NFR-3, NFR-4)

The same suites run in the `asan` preset, which catches memory errors in the paths that only the component tests reach.

## Implementation order

Each step is one or a few commits and leaves the build green.

1. CMake skeleton, presets, `.gitattributes`, GoogleTest wiring, empty `main`.
2. `RingBuffer` with tests.
3. `LineAssembler` with tests.
4. `ExitStatus` with tests.
5. `parse_args` with tests, `--help` and `--version`.
6. `format_dump` and helpers with tests.
7. `unique_fd` and `supervisor`: fork, exec, exec error pipe, passthrough loop.
8. Ring buffer and dump on failure wired into the loop.
9. Signal forwarding and SIGUSR1 snapshot.
10. Python component tests, CTest integration.
11. README, CONTRIBUTING, ROADMAP, ESSAY, CI file.

## Roadmap (first list)

* `--pty`: run the child on a pseudo terminal so it keeps line buffering.
* `--kill-after DURATION`: send SIGKILL when the child ignores a forwarded signal, like `timeout -k`.
* Trigger lines: `--trigger REGEX` writes a dump when a line matches, the software version of a dashcam's shock sensor.
* Kill the child when blackbox itself dies (`PR_SET_PDEATHSIG`), and adopt orphaned grandchildren with `PR_SET_CHILD_SUBREAPER`.
* Include the pending partial line in snapshots.
* `--format json` for dumps, for tools that collect them.
* Dump to stderr or an inherited file descriptor (`-o -`).
* A byte budget (`--keep-bytes`) and a configurable line cap instead of the fixed 4096.
* Point to the core file in the dump (`coredumpctl`) or run `gdb` in batch mode for a backtrace.
* journald output via `sd_journal_send`.
* musl support with an own signal name table instead of `sigabbrev_np`.
* Debian package through CPack.
