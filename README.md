# blackbox

A dashcam for Linux processes.

Put `blackbox` in front of a command and the command runs as before: its stdout and stderr reach your terminal or CI log byte for byte, and `blackbox` exits with the same code. Meanwhile it keeps the last N lines of output in memory. Nothing is written while things go well. When the command crashes or exits with a nonzero code, `blackbox` appends a short dump to a file: the last lines with timestamps, the exit cause, the runtime and the command line. Sending SIGUSR1 writes a snapshot at any time without stopping the command.

It is meant for processes whose last lines matter and tend to get lost: CI jobs with huge logs, robot nodes, long running services on a target without a debugger attached.

```
$ blackbox -n 3 -o crash.txt -- python3 -u planner.py
loop 1
...
loop 1000
planner: no path found
blackbox: dump written to crash.txt (signal 6 (SIGABRT), core dumped)
$ echo $?
134
$ cat crash.txt
blackbox dump
command: python3 -u planner.py
started: 2026-11-20T21:46:11Z
runtime: 0.069s
cause:   signal 6 (SIGABRT), core dumped
exit:    134
lines:   3 kept, 1001 seen

     0.021 out loop 999
     0.021 out loop 1000
     0.021 err planner: no path found
```

## Requirements

* Linux with glibc 2.32 or newer
* A C++20 compiler (tested with g++ 13.3) and CMake 3.25 or newer
* For the component tests: Python 3.10 or newer

There are no runtime dependencies besides the C++ standard library and glibc. GoogleTest is downloaded by CMake for the unit tests.

## Build and install

```
cmake --preset release
cmake --build --preset release
sudo cmake --install build/release            # installs to /usr/local/bin
```

Without root, pick another prefix: `cmake --install build/release --prefix ~/.local`.

The binary is `build/release/blackbox`. It is a single executable, so copying it to a target machine also works.

Without the presets, and without the tests:

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBLACKBOX_TESTS=OFF
cmake --build build
```

## Usage

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

Option parsing stops at `--` or at the first argument that is not an option. `blackbox ls -l` runs `ls -l`, and the `-l` is never read as an option of `blackbox`. Use `--` when the command itself starts with a dash.

### Examples

Keep the last 500 lines of a test run and write them only if it fails:

```
blackbox -n 500 -o test-failure.txt -- ctest --output-on-failure
```

The exit code passes through, so scripts and CI keep working:

```
blackbox -- make -j8 || echo "build failed, see blackbox.dump"
```

Take a snapshot of a running process without stopping it. Send the signal once the command is running, see the note in [Signals](#signals):

```
$ blackbox -o snap.txt -- sh -c 'for i in 1 2 3; do echo tick $i; sleep 0.2; done' > /dev/null &
$ sleep 0.5
$ kill -USR1 $!
blackbox: dump written to snap.txt (snapshot (SIGUSR1))
$ cat snap.txt
blackbox dump
command: sh -c 'for i in 1 2 3; do echo tick $i; sleep 0.2; done'
started: 2026-11-20T21:46:11Z
runtime: 0.483s
cause:   snapshot (SIGUSR1)
lines:   3 kept, 3 seen

     0.001 out tick 1
     0.203 out tick 2
     0.404 out tick 3
```

Errors from `blackbox` itself are short and use the exit codes of `env` and `timeout`:

```
$ blackbox -- nonexistent-tool
blackbox: nonexistent-tool: No such file or directory
$ echo $?
127
$ blackbox --keep 99999 ls
blackbox: invalid value '99999' for --keep (expected 0 to 10000)
Try 'blackbox --help' for more information.
$ echo $?
125
```

## When is a dump written

* The command exits with a nonzero code, or is killed by a signal.
* The command exits with 0 and `--always` is given.
* `blackbox` receives SIGUSR1 (a snapshot, the command keeps running).

A clean run without `--always` creates no file. If the dump cannot be written (disk full, file removed), `blackbox` prints an error and still exits with the command's code, because that code is what a CI job needs most.

Before the command starts, `blackbox` checks that the dump file can be created. A typo in `-o` therefore fails right away with exit 125 instead of after a long run.

## Dump format

Each dump is plain text and is appended to the file, so several runs can share one file. Dumps are separated by an empty line.

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

* `command` is shell quoted, so it can be pasted back into a shell.
* `started` is the start time in UTC. Line timestamps are seconds since the start of the command, from a monotonic clock.
* `cause` is `exit N`, `signal N (NAME)` with `, core dumped` when the kernel reports a core dump, or `snapshot (SIGUSR1)`.
* `exit` is the exit status a shell reports for `blackbox`. Snapshots have no `exit` line.
* `lines` says how many lines are in the dump and how many the command printed in total.
* Each line is tagged `out` or `err`. Lines longer than 4096 bytes are cut and end with ` [truncated]`. A last line without a newline ends with ` [no newline]`.
* Control bytes (except tab) are written as `\xNN`, so a dump is always readable in a terminal or editor. Other bytes, including invalid UTF-8, are written as they are.

Each dump is written with a single `write` call on a file opened with `O_APPEND`, so two `blackbox` processes writing to the same file on a local filesystem do not mix their dumps.

## Signals

* SIGINT, SIGTERM, SIGHUP and SIGQUIT sent to `blackbox` by another process (`kill`, systemd, a CI runner) are forwarded to the command. `blackbox` then waits for the command to end and writes the dump as usual.
* Ctrl+C and Ctrl+\\ in a terminal are not forwarded, because the terminal already sends them to the command directly. The command gets the signal exactly once.
* SIGUSR1 writes a snapshot.
* A signal that arrives in the first moments after start, before `blackbox` has set up its signal handling, gets the default action. For SIGUSR1 and the forwarded signals that ends `blackbox` before the command starts. Scripts should wait until the command is running, for example `sleep 1` before `kill -USR1`.
* When the command is killed by a signal, `blackbox` writes the dump and then ends itself with the same signal, as GNU `timeout` does. A shell reports 128+N either way, but only a real signal death makes `bash` stop a script loop on Ctrl+C.

## Things to know

* **Buffering.** The command writes into pipes instead of a terminal, so C stdio and Python switch from line buffering to block buffering. Output then arrives in larger chunks, and lines that were still in the command's buffer when it crashed are lost (with or without `blackbox`). Use `stdbuf -oL -eL COMMAND` for C programs and `python3 -u` for Python. A pseudo terminal mode is on the [roadmap](ROADMAP.md).
* **Order between stdout and stderr.** Each stream keeps its order. The order between the two streams in the dump is the order in which `blackbox` read them, which can differ slightly from the order in which they were written.
* **Background processes.** If the command starts a background process that inherits its stdout or stderr, `blackbox` keeps forwarding that output after the command has exited, and stops once the pipes have been quiet for 100 ms. SIGINT, SIGTERM, SIGHUP or SIGQUIT sent to `blackbox` in that phase stops it right away, and the final dump is written as usual.
* **Closed output.** If the reader of `blackbox`'s output goes away (for example `blackbox -- cmd | head -1`), `blackbox` stops forwarding that stream and the command gets SIGPIPE on its next write, the same as without `blackbox`.

## Documentation

* [docs/SPEC.md](docs/SPEC.md): scope, functional and non functional requirements, architecture
* [CONTRIBUTING.md](CONTRIBUTING.md): code structure, development setup, tests, conventions
* [ROADMAP.md](ROADMAP.md): what is not in this release and known limitations
* [ESSAY.md](ESSAY.md): background and design decisions

## License

MIT, see [LICENSE](LICENSE).
