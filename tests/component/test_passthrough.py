import hashlib
import os
import subprocess

import pytest

from conftest import BLACKBOX, child, run_blackbox


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def test_binary_output_is_passed_through_unchanged(dump_path):
    command = child("binary", "out:after binary", "err:after binary")
    direct = subprocess.run(command, capture_output=True, check=True)
    wrapped = run_blackbox(["-o", str(dump_path), "--", *command])

    assert wrapped.returncode == 0
    assert sha256(wrapped.stdout) == sha256(direct.stdout)
    assert sha256(wrapped.stderr) == sha256(direct.stderr)
    assert b"\x00" in wrapped.stdout and b"\xff" in wrapped.stderr
    assert not dump_path.exists()


@pytest.mark.parametrize("code", [0, 1, 42, 255])
def test_exit_code_is_passed_through(dump_path, code):
    result = run_blackbox(["-q", "-o", str(dump_path), *child(f"exit:{code}")])
    assert result.returncode == code


def test_stdin_reaches_the_child(dump_path):
    data = b"first line\n\x00binary\xff\nlast"
    result = run_blackbox(["-o", str(dump_path), *child("cat")], input=data)
    assert result.returncode == 0
    assert result.stdout == data


def test_output_is_forwarded_before_the_child_exits(dump_path, start_blackbox):
    proc = start_blackbox(["-o", str(dump_path), *child("out:early", "readline")])
    try:
        assert proc.stdout.readline() == b"early\n"
    finally:
        proc.communicate(b"\n", timeout=10)
    assert proc.returncode == 0


def test_child_gets_only_standard_fds(dump_path):
    script = (
        "import os\n"
        "fds = []\n"
        "for fd in range(1024):\n"
        "    try:\n"
        "        os.fstat(fd)\n"
        "        fds.append(fd)\n"
        "    except OSError:\n"
        "        pass\n"
        "print(fds)\n"
    )
    result = run_blackbox(["-o", str(dump_path), "python3", "-c", script])
    assert result.returncode == 0
    assert result.stdout == b"[0, 1, 2]\n"


def signal_masks(status_text):
    fields = dict(line.split(":\t", 1) for line in status_text.splitlines() if ":\t" in line)
    return int(fields["SigBlk"], 16), int(fields["SigIgn"], 16)


def test_child_gets_clean_signal_state(dump_path):
    # cat instead of child.py, because Python ignores SIGPIPE itself at startup.
    direct = subprocess.run(["cat", "/proc/self/status"], capture_output=True, check=True)
    wrapped = run_blackbox(["-o", str(dump_path), "cat", "/proc/self/status"])
    assert wrapped.returncode == 0

    blocked, ignored = signal_masks(wrapped.stdout.decode())
    sigpipe_bit = 1 << (13 - 1)
    assert blocked == 0
    assert ignored & sigpipe_bit == 0
    assert (blocked, ignored) == signal_masks(direct.stdout.decode())


def test_started_with_closed_stdout(dump_path):
    # Without care, the first pipe would get fd 1 and the streams would get mixed up.
    result = subprocess.run(
        [str(BLACKBOX), "-o", str(dump_path), *child("out:to nowhere", "err:visible")],
        stderr=subprocess.PIPE,
        preexec_fn=lambda: os.close(1),
        timeout=20,
    )
    assert result.returncode == 0
    assert result.stderr == b"visible\n"
