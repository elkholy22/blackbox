import pytest

from conftest import run_blackbox


def test_command_not_found(dump_path):
    result = run_blackbox(["-o", str(dump_path), "no-such-command-for-blackbox"])

    assert result.returncode == 127
    assert result.stderr == b"blackbox: no-such-command-for-blackbox: No such file or directory\n"
    assert not dump_path.exists()


def test_command_not_executable(dump_path, tmp_path):
    script = tmp_path / "script.sh"
    script.write_text("#!/bin/sh\necho should not run\n")
    script.chmod(0o644)

    result = run_blackbox(["-o", str(dump_path), str(script)])

    assert result.returncode == 126
    assert result.stderr == f"blackbox: {script}: Permission denied\n".encode()
    assert result.stdout == b""
    assert not dump_path.exists()


TOUCH = ["touch", "started"]


@pytest.mark.parametrize(
    "args, message",
    [
        ([], b"missing command"),
        (["-n", "5"], b"missing command"),
        (["--bogus", *TOUCH], b"unknown option '--bogus'"),
        (["--keep"], b"option '--keep' requires an argument"),
        (["--keep", "-1", *TOUCH], b"invalid value '-1' for --keep"),
        (["--keep", "abc", *TOUCH], b"invalid value 'abc' for --keep"),
        (["--keep", "10x", *TOUCH], b"invalid value '10x' for --keep"),
        (["--keep", "10001", *TOUCH], b"invalid value '10001' for --keep"),
    ],
)
def test_usage_errors(tmp_path, args, message):
    result = run_blackbox(args, cwd=tmp_path)

    assert result.returncode == 125
    assert message in result.stderr
    assert b"Try 'blackbox --help'" in result.stderr
    assert not (tmp_path / "started").exists()


def test_missing_dump_directory(tmp_path):
    marker = tmp_path / "started"
    result = run_blackbox(["-o", str(tmp_path / "missing" / "x.dump"), "touch", str(marker)])

    assert result.returncode == 125
    assert b"cannot write dump to" in result.stderr
    assert not marker.exists()


def test_help():
    result = run_blackbox(["--help"])

    assert result.returncode == 0
    assert result.stdout.startswith(b"Usage: blackbox")
    assert len(result.stdout.splitlines()) < 25


def test_version():
    result = run_blackbox(["--version"])

    assert result.returncode == 0
    assert result.stdout.startswith(b"blackbox ")
