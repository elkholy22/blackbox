import os
import pty
import signal
import subprocess
import time

import pytest

from conftest import BLACKBOX, child, parse_dumps, wait_for_line


def read_all(fd):
    data = b""
    while True:
        try:
            chunk = os.read(fd, 1024)
        except OSError:  # EIO once no process has the terminal open any more
            return data
        if not chunk:
            return data
        data += chunk


def press_ctrl_c(command):
    """Runs command on a new pseudo terminal and presses Ctrl+C after it printed "ready".

    Returns the terminal output and the exit code of command.
    """
    pid, fd = pty.fork()
    if pid == 0:
        try:
            os.execvp(command[0], command)
        finally:
            os._exit(127)
    try:
        output = b""
        while b"ready" not in output:
            output += os.read(fd, 1024)
        os.write(fd, b"\x03")
        output += read_all(fd)
    finally:
        os.close(fd)
    _, status = os.waitpid(pid, 0)
    return output, os.waitstatus_to_exitcode(status)


@pytest.mark.parametrize(
    "sig", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP, signal.SIGQUIT], ids=lambda s: s.name
)
def test_signal_is_forwarded(dump_path, start_blackbox, sig):
    # sh instead of child.py, because Python turns SIGINT into an exception.
    proc = start_blackbox(["-o", str(dump_path), "sh", "-c", "echo ready; exec sleep 30"])
    wait_for_line(proc.stdout, b"ready")

    proc.send_signal(sig)
    proc.communicate(timeout=10)

    assert proc.returncode == -sig
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"].startswith(f"signal {sig.value} ({sig.name})")


def test_ctrl_c_reaches_the_child_once(dump_path):
    # The terminal sends Ctrl+C to the whole foreground process group, blackbox included.
    # blackbox must not forward its copy as well.
    output, code = press_ctrl_c([str(BLACKBOX), "-q", "-o", str(dump_path), *child("count-int")])

    assert b"sigint 1" in output
    assert code == 0


def test_child_can_handle_sigterm(dump_path, start_blackbox):
    proc = start_blackbox(["-o", str(dump_path), *child("trap-term", "ready", "sleep:30")])
    wait_for_line(proc.stdout, b"ready")

    proc.send_signal(signal.SIGTERM)
    stdout, _ = proc.communicate(timeout=10)

    assert stdout == b"goodbye\n"
    assert proc.returncode == 0
    assert not dump_path.exists()


def test_sigusr1_writes_snapshot_and_child_keeps_running(dump_path, start_blackbox):
    proc = start_blackbox(["-o", str(dump_path), *child("lines:3", "ready", "readline")])
    wait_for_line(proc.stdout, b"ready")

    proc.send_signal(signal.SIGUSR1)
    message = f"blackbox: dump written to {dump_path} (snapshot (SIGUSR1))"
    wait_for_line(proc.stderr, message.encode())
    assert proc.poll() is None, "the child should still be running"

    proc.communicate(b"\n", timeout=10)
    assert proc.returncode == 0
    [snapshot] = parse_dumps(dump_path)
    assert snapshot.header["cause"] == "snapshot (SIGUSR1)"
    assert "exit" not in snapshot.header
    assert snapshot.lines == [("out", "line 1"), ("out", "line 2"), ("out", "line 3"),
                              ("out", "ready")]


def test_closed_reader_ends_the_child_with_sigpipe(dump_path, start_blackbox):
    # yes, not child.py: Python ignores SIGPIPE, yes dies from it like most tools.
    proc = start_blackbox(["-q", "-o", str(dump_path), "yes"])
    assert proc.stdout.readline() == b"y\n"
    proc.stdout.close()

    assert proc.wait(timeout=5) == -signal.SIGPIPE
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"] == "signal 13 (SIGPIPE)"


def test_write_error_on_stdout_ends_the_child_with_sigpipe(dump_path, start_blackbox):
    with open("/dev/full", "wb") as full:
        proc = start_blackbox(["-o", str(dump_path), "yes"], stdout=full)
        _, stderr = proc.communicate(timeout=10)

    assert proc.returncode == -signal.SIGPIPE
    assert b"blackbox: cannot forward stdout: No space left on device" in stderr
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"] == "signal 13 (SIGPIPE)"


def test_background_grandchild_does_not_keep_blackbox_alive(dump_path, start_blackbox):
    command = child("grandchild", "out:parent done", "partial:last words", "exit:4")
    started = time.monotonic()
    proc = start_blackbox(["-o", str(dump_path), *command])
    stdout, _ = proc.communicate(timeout=10)
    elapsed = time.monotonic() - started

    assert proc.returncode == 4
    assert stdout == b"parent done\nlast words"
    assert elapsed < 5
    [dump] = parse_dumps(dump_path)
    assert dump.lines[-1] == ("out", "last words [no newline]")


def test_sigterm_stops_waiting_for_a_busy_background_writer(dump_path, start_blackbox):
    # The writer starts once the shell is gone, which means blackbox has reaped it.
    writer = (
        "while kill -0 $$ 2>/dev/null; do sleep 0.01; done; "
        "while true; do echo tick; sleep 0.02; done"
    )
    proc = start_blackbox(["-o", str(dump_path), "sh", "-c", f"({writer}) & exit 0"])
    wait_for_line(proc.stdout, b"tick")

    proc.send_signal(signal.SIGTERM)

    assert proc.wait(timeout=5) == 0
    assert not dump_path.exists()


@pytest.mark.parametrize(
    "command, code", [(child("exit:3"), 3), (["no-such-command"], 127)], ids=["exit", "not-found"]
)
def test_inherited_sigchld_ignore(dump_path, command, code):
    # A Python service that ignores SIGCHLD passes SIG_IGN on through exec.
    result = subprocess.run(
        [str(BLACKBOX), "-q", "-o", str(dump_path), *command],
        capture_output=True,
        preexec_fn=lambda: signal.signal(signal.SIGCHLD, signal.SIG_IGN),
        timeout=10,
    )
    assert result.returncode == code


def test_ctrl_c_stops_a_shell_loop(dump_path):
    # bash only ends the loop if the job died from SIGINT, not if it exited with 130.
    run = f"{BLACKBOX} -q -o {dump_path} -- sh -c 'echo ready; exec sleep 30'"
    output, code = press_ctrl_c(["bash", "-c", f"for i in 1 2; do {run}; echo next; done"])

    assert b"next" not in output
    assert code == -signal.SIGINT
