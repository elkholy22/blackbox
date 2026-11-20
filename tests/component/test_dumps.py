import signal

from conftest import child, parse_dumps, run_blackbox


def test_crash_dump_has_the_last_lines(dump_path):
    result = run_blackbox(["--keep", "50", "-o", str(dump_path), *child("lines:1000", "abort")])

    assert result.returncode == -signal.SIGABRT
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"].startswith("signal 6 (SIGABRT)")
    assert dump.header["exit"] == "134"
    assert dump.header["lines"] == "50 kept, 1000 seen"
    assert dump.lines == [("out", f"line {i}") for i in range(951, 1001)]


def test_dump_header(dump_path):
    run_blackbox(["-o", str(dump_path), *child("out:it's here", "exit:7")])

    [dump] = parse_dumps(dump_path)
    assert dump.header["command"].endswith("child.py 'out:it'\\''s here' exit:7")
    assert dump.header["started"].endswith("Z")
    assert dump.header["runtime"].endswith("s")


def test_segfault(dump_path):
    result = run_blackbox(["-o", str(dump_path), *child("out:before crash", "kill:SIGSEGV")])

    assert result.returncode == -signal.SIGSEGV
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"].startswith("signal 11 (SIGSEGV)")
    assert dump.lines == [("out", "before crash")]


def test_nonzero_exit(dump_path):
    result = run_blackbox(["-o", str(dump_path), *child("err:something failed", "exit:3")])

    assert result.returncode == 3
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"] == "exit 3"
    assert dump.header["exit"] == "3"
    assert dump.lines == [("err", "something failed")]


def test_clean_exit_with_always(dump_path):
    result = run_blackbox(["--always", "-o", str(dump_path), *child("out:all good")])

    assert result.returncode == 0
    [dump] = parse_dumps(dump_path)
    assert dump.header["cause"] == "exit 0"
    assert dump.lines == [("out", "all good")]


def test_long_line_and_missing_newline(dump_path):
    result = run_blackbox(["-o", str(dump_path), *child("long:10000", "partial:tail", "exit:1")])

    assert result.stdout == b"x" * 10000 + b"\n" + b"tail"
    [dump] = parse_dumps(dump_path)
    assert dump.lines == [
        ("out", "x" * 4096 + " [truncated]"),
        ("out", "tail [no newline]"),
    ]


def test_control_bytes_are_escaped(dump_path):
    run_blackbox(["-o", str(dump_path), *child("out:\x1b[31mred\x1b[0m\r", "exit:1")])

    [dump] = parse_dumps(dump_path)
    assert dump.lines == [("out", "\\x1b[31mred\\x1b[0m\\x0d")]


def test_keep_zero_writes_only_the_header(dump_path):
    run_blackbox(["-n", "0", "-o", str(dump_path), *child("lines:5", "exit:1")])

    [dump] = parse_dumps(dump_path)
    assert dump.header["lines"] == "0 kept, 5 seen"
    assert dump.lines == []


def test_dumps_are_appended(dump_path):
    run_blackbox(["-o", str(dump_path), *child("out:first", "exit:1")])
    run_blackbox(["-o", str(dump_path), *child("out:second", "exit:2")])

    first, second = parse_dumps(dump_path)
    assert first.lines == [("out", "first")]
    assert second.lines == [("out", "second")]
    assert second.header["exit"] == "2"


def test_default_dump_file_in_working_directory(tmp_path):
    result = run_blackbox(child("exit:1"), cwd=tmp_path)

    assert result.returncode == 1
    assert (tmp_path / "blackbox.dump").exists()


def test_dump_message_on_stderr(dump_path):
    result = run_blackbox(["-o", str(dump_path), *child("err:child output", "exit:5")])

    assert result.stderr == (
        b"child output\n" + f"blackbox: dump written to {dump_path} (exit 5)\n".encode()
    )


def test_quiet_suppresses_the_message(dump_path):
    result = run_blackbox(["-q", "-o", str(dump_path), *child("exit:5")])

    assert result.stderr == b""
    assert dump_path.exists()


def test_dump_failure_keeps_the_exit_code(tmp_path):
    # A directory passes the startup check but cannot be opened as a file.
    target = tmp_path / "a_directory"
    target.mkdir()
    result = run_blackbox(["-o", str(target), *child("exit:9")])

    assert result.returncode == 9
    assert b"blackbox: cannot write dump" in result.stderr
