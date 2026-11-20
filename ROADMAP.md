# Roadmap

Version 1.0 does one thing: run a command transparently and dump its last lines when it fails. Everything below was left out on purpose to keep that core small and finished. The order inside each section is a rough priority.

## Next features

1. **Pseudo terminal mode (`--pty`).** Run the command on a pseudo terminal so C stdio and Python keep line buffering, and lines that are still buffered at a crash are not lost. This is the most common surprise for new users today. It needs `posix_openpt`, window size forwarding and a decision on how to separate stdout from stderr (a pty merges them).
1. **Trigger lines (`--trigger REGEX`).** Write a dump when an output line matches, the software version of a dashcam's shock sensor. Useful for errors that a process logs but survives.
1. **`--kill-after DURATION`.** After a forwarded signal, send SIGKILL if the command has not ended in time, like `timeout -k`.
1. **Partial line in snapshots.** A snapshot only contains complete lines. The line that is currently being written could be included and marked.
1. **Dump to stderr or an inherited descriptor (`-o -`).** For CI systems that collect only the job log.
1. **`--format json`.** A machine readable dump for tools that collect dumps from many machines.
1. **Byte budget (`--keep-bytes`) and a configurable line cap** instead of the fixed 4096 bytes per line.
1. **Core dump pointer.** Mention the `coredumpctl` entry in the dump, or run `gdb` in batch mode for a backtrace.
1. **journald output** through `sd_journal_send`, as an alternative to a file.
1. **Packaging.** A Debian package through CPack, and a static build for targets without a matching glibc.

## Known limitations

* **Block buffering of the command.** See the README. Workarounds are `stdbuf -oL -eL` and `python3 -u` until `--pty` exists.
* **Order between streams.** The order of stdout and stderr lines in a dump is the order in which `blackbox` read them. When the command writes to both streams quickly, the dump can show them in a slightly different order than a terminal would.
* **Busy background processes.** After the command exits, `blackbox` keeps forwarding output from background processes that inherited its pipes until the pipes have been quiet for 100 ms. A background process that writes without pause keeps `blackbox` running until it stops, closes the pipes, or `blackbox` gets a termination signal.
* **Process group signals.** A signal sent to the whole process group with `kill -TERM -PGID` reaches the command directly and is also forwarded by `blackbox`, so the command gets it twice. Most programs do not care, but a program that counts signals would.
* **Slow readers delay signals.** Writes to `blackbox`'s own stdout and stderr block when the reader is slow. Signals that arrive meanwhile are handled after the write finishes.
* **Signals right after start.** A SIGUSR1 or SIGTERM that arrives in the first moments, before `blackbox` has blocked its signals, gets the default action and ends `blackbox` before the command starts. Blocking them as the first thing in `main` would make the window smaller, but the dynamic loader runs before that, so it cannot be closed completely.
* **If `blackbox` itself is killed with SIGKILL**, the command keeps running without a supervisor and no dump is written.
* **Short options cannot be combined.** `-aq` has to be written as `-a -q`, and `-n5` as `-n 5`.
* **Arguments with control characters** are escaped in the `command:` line of the dump, so that line can no longer be pasted back exactly.
* **glibc only.** `sigabbrev_np` needs glibc 2.32 or newer. musl would need a small signal name table.

## Hardening and refactoring ideas

* Set `PR_SET_PDEATHSIG` in the child so the command dies with `blackbox`, and `PR_SET_CHILD_SUBREAPER` to also reap orphaned grandchildren.
* Fuzz `LineAssembler` and `format_dump` with libFuzzer. Both take plain bytes and are cheap to fuzz.
* Avoid one allocation per line in the hot path by keeping the ring buffer's text in one preallocated byte buffer. The release build passes about 650 MB/s today, so this is only worth it if profiling on a slow target says so.
* Run the component tests on more environments in CI: clang with libc++, an older glibc, aarch64.
* A component test for the 125 path after the child has started (a failing `poll`), which today is only covered by reading the code. It would need fault injection, for example through an `LD_PRELOAD` shim.
