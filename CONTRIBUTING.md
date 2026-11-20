# Contributing to blackbox

Thanks for taking the time. This guide covers how the code is organized, how to build and test it, and the conventions the project follows. The requirements and the design are in [docs/SPEC.md](docs/SPEC.md); read its architecture section before larger changes.

## Development setup

You need Linux, a C++20 compiler (g++ 13 or clang 16 and newer), CMake 3.25 or newer, git and Python 3.10 or newer. On Ubuntu 24.04:

```
sudo apt install build-essential cmake git python3 python3-venv clang-format
```

Clone the repository and create a virtualenv for the component tests. Activate it before you configure CMake, so CTest picks up the Python with pytest installed:

```
git clone <repository url> blackbox
cd blackbox
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements-dev.txt
```

## Build and test

The presets keep the build configurations in the repository:

* `debug`: Debug build, warnings are errors, in `build/debug`
* `release`: optimized build, in `build/release`
* `asan`: Debug build with AddressSanitizer and UndefinedBehaviorSanitizer, warnings are errors, in `build/asan`

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

`ctest` runs both suites: every GoogleTest case as its own test, and the whole pytest suite as one test called `component_tests`.

Run a suite on its own while working on it:

```
./build/debug/tests/unit/unit_tests --gtest_filter='LineAssembler.*'
python -m pytest tests/component -k signals
```

The component tests use `build/debug/blackbox` unless `BLACKBOX_BIN` points to another binary:

```
BLACKBOX_BIN=build/release/blackbox python -m pytest
```

Before you open a pull request, run the sanitizer build too. CI runs it on every push.

```
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
```

## Code structure

```
src/
  main.cpp             parses arguments, checks the dump path, calls run()
  options.*            argv parsing into Options, UsageError, help text
  record.hpp           Record: one kept line with stream, timestamp and flags
  ring_buffer.hpp      RingBuffer<T>, fixed capacity, overwrites the oldest
  line_assembler.*     chunks of bytes in, complete lines out, 4096 byte cap
  exit_status.*        decodes the waitpid status, cause text, shell exit code
  dump.*               formats a dump (pure) and appends it to the file
  unique_fd.hpp        RAII owner of a file descriptor
  supervisor.*         fork/exec, the poll loop, signals, when to dump
tests/
  unit/                GoogleTest, one file per module
  component/           pytest, drives the built binary
    child.py           the test command: prints, crashes, floods, waits
    conftest.py        finds the binary, helpers to run it and parse dumps
docs/SPEC.md           scope, requirements, architecture, test plan
```

Everything except `main.cpp` and `supervisor.cpp` builds into the static library `blackbox_core`, which the unit tests link. That split is the main design rule of the project: logic with many edge cases goes into the core, where it takes plain values and can be tested without processes. `supervisor.cpp` holds the system calls and stays as thin as possible. It is covered by the component tests, because faking `fork`, `poll` and signals would mostly test the fakes.

### How a run works

1. Block the handled signals and read them from a `signalfd`. Ignore SIGPIPE, so a closed reader shows up as `EPIPE` from `write`. Reset SIGCHLD to its default, because an inherited SIG_IGN would make the kernel reap the child behind our back.
2. Create the stdout pipe, the stderr pipe and the exec error pipe, all `O_CLOEXEC`.
3. Fork. The child restores the signal mask, resets SIGPIPE, moves the pipes to fd 1 and 2 and calls `execvp`. If that fails it writes `errno` into the exec error pipe.
4. The parent reads the exec error pipe. EOF means the exec worked.
5. One `poll` loop serves both pipes and the signalfd until the child is reaped and both pipes are closed.
6. Decide whether to write the final dump. If the child was killed by a signal, raise the same signal in `blackbox`, otherwise return the exit code.

Between `fork` and `exec` only async signal safe calls are allowed. Anything that allocates (building `argv`, for example) happens before `fork`.

## Conventions

* C++20, no compiler extensions. The code builds without warnings under `-Wall -Wextra -Wpedantic`; the `debug` and `asan` presets add `-Werror`.
* Formatting follows `.clang-format`. Run `clang-format -i src/*.cpp src/*.hpp tests/unit/*.cpp` before committing.
* Names: `snake_case` for functions and variables, `PascalCase` for types, `kName` for constants, a trailing `_` for private members.
* Resources are owned by RAII types (`UniqueFd`, standard containers). No `new` or `delete`.
* System calls that can return `EINTR` are retried. A failed system call becomes a `std::system_error` with the call's name, and `main` turns it into exit code 125.
* Every message `blackbox` prints starts with `blackbox: ` and goes to stderr. Only `--help` and `--version` print to stdout.
* Comments explain why, not what. Most functions need none.
* Python tests use only the standard library plus pytest. Follow PEP 8 with lines up to 100 columns.
* The implementation stays below 1000 lines of C++, not counting tests, blank lines and comments (NFR-10). If a feature does not fit, it probably belongs in the roadmap first.

## Adding a test

**Unit test.** Add a `TEST` to the matching file in `tests/unit/`, or create `<module>_test.cpp` and add it to `tests/unit/CMakeLists.txt`. Unit tests must not start processes or depend on timing. Pass timestamps in explicitly, like the `LineAssembler` tests do.

**Component test.** Add a function to the matching `tests/component/test_*.py`. Use `run_blackbox` for runs that finish on their own and the `start_blackbox` fixture when the test needs to signal or talk to the process while it runs. The fixture kills whatever is left of the process group after the test. Describe what the test command should do with `child(...)` actions, for example `child("lines:1000", "abort")`. If you need a new behavior, add an action to `child.py` and document it in its docstring.

A few rules keep the component tests reliable:

* Wait for an explicit `ready` line before sending a signal, never for a fixed time.
* Use the `dump_path` fixture or `tmp_path`, never the working directory of the test run.
* Time limits are generous (seconds, not milliseconds), so a busy CI runner does not cause failures.
* Tools that must die from SIGPIPE cannot be Python scripts, because Python ignores SIGPIPE. Use `yes` or `cat` there.

## Commits and pull requests

* One logical change per commit. The build and all tests pass after every commit.
* Commit messages: a short subject in imperative mood ("Add trigger option", "Fix gap check at file end"), no trailing period, at most about 60 characters. Add a body when the reason for the change is not obvious from the diff.
* When a change affects behavior, update `docs/SPEC.md`, the README and the tests in the same pull request. If you decide not to do something, add it to `ROADMAP.md`.
