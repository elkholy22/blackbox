# Essay

## The project

`blackbox` is a dashcam for Linux processes. You put it in front of a command, the command runs as before, and `blackbox` remembers the last lines of its output. If the command crashes or exits with an error, `blackbox` writes those lines to a file together with the exit cause, the runtime and the command line. While things go well it writes nothing.

The idea comes from my work in Formula Student. At FaSTTUBe I am responsible for SLAM on our driverless car, and a large part of that work is test infrastructure: a Docker setup that replays real vehicle recordings through the software stack with rosbag2 and MCAP, fault injection tests for six failure modes, and a recording standard with 37 signals that every test drive is checked against automatically. Almost all of it serves one question: what happened just before something went wrong? `blackbox` is a small and general version of that idea for any process on Linux, such as a CI job with a log too long to scroll, a robot node, or a service on a target without a debugger.

I picked it because it is small enough to finish properly and still touches the parts of Linux that are easy to get wrong: `fork` and `exec`, pipes, signals, exit status and file descriptors. The scope has a clear boundary. The tool runs one command, it never changes that command's output, and it writes a dump at the right moments. Supervising several processes, restarts and log rotation are the job of systemd and stay out.

## The organization

The most important decision in the code is a split into two parts. Everything with many edge cases but no process or signal handling lives in a static library called `blackbox_core`: argument parsing, the ring buffer, the code that turns chunks of bytes into lines, the decoding of the wait status, and the dump format. These parts take plain values and return plain values, so the unit tests need no processes and no timing. The second part is `supervisor.cpp`, which starts the child, runs the event loop and handles signals. It stays thin and is tested from the outside.

That split also decided the test strategy. The C++ unit tests use GoogleTest and cover the core, where the edge cases are: lines split across reads, a line of 4096 bytes and one of 4097, `\r\n`, NUL bytes, the ring buffer after many wraparounds, and a golden test for the dump format. The Python component tests start the real binary and check what a user sees: exit codes, bytes on stdout and stderr, and the dump file. A small helper script, `child.py`, plays the command and can print, crash, flood both streams or wait for a signal. I did not mock `fork`, `poll` or signals. Such tests would mostly test the mocks, while a real process on a real pipe shows the real behavior. The component tests compare SHA-256 hashes of binary output with and without `blackbox`, push 50 MB through both streams to check that memory stays bounded and nothing deadlocks, and drive Ctrl+C through a pseudo terminal.

The specification came first (`docs/SPEC.md`), with numbered functional and non functional requirements and a test plan that maps the tests to them. The implementation followed in small commits that each keep the build green. The README is for users, CONTRIBUTING explains the structure, the setup and the conventions, and the ROADMAP records what I left out on purpose. A GitHub Actions workflow builds with g++ and clang, in debug and release mode and with AddressSanitizer and UndefinedBehaviorSanitizer, and runs both test suites each time. I had set up a GitLab CI/CD pipeline with build, verify and deploy stages for a robotics project at university and wanted the same safety net here.

## Technological decisions

**C++20.** It gives me `std::format`, `std::span` and designated initializers, and g++ 13 and clang 16 support it fully. C++23 would add `std::expected`, but toolchains for embedded Linux targets often lag behind, and one exception type for usage errors is enough here.

**One thread, one `poll` loop, `signalfd`.** The child writes into two pipes. A single `poll` waits on both pipes and on a `signalfd`, so a child that floods stdout and stderr at the same time cannot deadlock `blackbox`, and no work happens inside an asynchronous signal handler. Threads would have made the shared ring buffer and the signal handling harder to reason about, for no gain at this size.

**Pipes, not a pseudo terminal.** With pipes, stdout and stderr stay separate, which is half of what a dump is for. The price is that C stdio and Python switch to block buffering when they write into a pipe. The README documents `stdbuf` and `python3 -u` as the workaround, and a `--pty` mode is the first item on the roadmap.

**Exit status like `env` and `timeout`.** `blackbox` returns the child's code, and 125, 126 and 127 for its own errors. When the child is killed by a signal, `blackbox` writes the dump and then kills itself with the same signal, as GNU `timeout` does. That way `bash` sees a real SIGINT death and stops a script loop on Ctrl+C.

**Bounded memory, one write per dump.** Lines are capped at 4096 bytes and the ring buffer holds at most N of them, so memory does not grow with the amount of output. Each dump is formatted in memory and written with one `write` call on an `O_APPEND` file, so two processes can share a dump file without mixing their dumps.

**CMake with presets, GoogleTest pinned by hash.** CMake is what C++ developers on Linux expect, and CTest runs both suites with one command. The presets keep the debug, release and sanitizer builds in the repository. GoogleTest is the only third party dependency and is only used for tests. The tool itself needs nothing but the C++ standard library and glibc, which matters on targets where installing libraries is a hassle. The cost is a hand written option parser, which is why short options cannot be combined yet.

## Looking back

The data structures were the easy part. The ring buffer and the line assembler worked almost from the start. The real bugs were in how `blackbox` behaves as a process among other processes, and a review of the first finished version turned up four of them. If `blackbox` was started with SIGCHLD ignored, the kernel reaped the child on its own and `blackbox` waited forever. A last line without a newline was lost when a background process kept the pipe open. During the wait for such a background process, termination signals were ignored. And `blackbox` returned 130 after Ctrl+C instead of dying from SIGINT, so shell loops kept running. Each of them now has a component test.

With hindsight I would write down early which process state `blackbox` inherits and passes on (signal dispositions, the signal mask, open descriptors) and test each item from the start. I did that for the child, which starts with a clean signal state and only file descriptors 0, 1 and 2, but not for `blackbox` itself. I would also write the component tests alongside the supervisor instead of after it. They came several commits later, and most of the fixes above followed them.

Some choices are compromises that I would still make. After the child exits, `blackbox` waits until the pipes have been quiet for 100 ms, because a daemon the child left behind could otherwise keep it alive forever. That number is a guess that works in practice. The order between stdout and stderr lines in a dump is the order in which `blackbox` read them, which is close but not guaranteed. And a signal that arrives in the first moments after start, before the signals are blocked, still ends `blackbox`. All three are documented in the README and the ROADMAP.
