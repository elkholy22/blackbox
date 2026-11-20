import contextlib
import os
import re
import resource
import signal
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
CHILD = Path(__file__).resolve().parent / "child.py"


def find_binary():
    path = os.environ.get("BLACKBOX_BIN")
    if path:
        return Path(path).resolve()
    return REPO_ROOT / "build" / "debug" / "blackbox"


BLACKBOX = find_binary()

# ASan reserves a lot of virtual memory and adds per allocation overhead, so the memory
# limit only makes sense for normal builds. CMake sets this for the asan preset.
SANITIZED = os.environ.get("BLACKBOX_SANITIZED") == "1"


def pytest_configure(config):
    if not os.access(BLACKBOX, os.X_OK):
        raise pytest.UsageError(
            f"blackbox binary not found at {BLACKBOX}. "
            "Build it first (cmake --preset debug && cmake --build --preset debug) "
            "or set BLACKBOX_BIN."
        )


def child(*actions):
    """Command line that runs child.py with the given actions, unbuffered."""
    return [sys.executable, "-u", str(CHILD), *actions]


def no_core_files():
    # Crash tests should not litter the disk with core files.
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def run_blackbox(args, *, input=None, timeout=20, cwd=None, **kwargs):
    """Runs blackbox to completion and returns the CompletedProcess with bytes output."""
    return subprocess.run(
        [str(BLACKBOX), *args],
        input=input,
        capture_output=True,
        timeout=timeout,
        cwd=cwd,
        preexec_fn=no_core_files,
        **kwargs,
    )


def wait_for_line(stream, expected):
    """Reads lines until one equals expected. Fails on EOF."""
    while True:
        line = stream.readline()
        assert line, f"stream ended before {expected!r}"
        if line.rstrip(b"\n") == expected:
            return


@dataclass
class Dump:
    header: dict = field(default_factory=dict)
    lines: list = field(default_factory=list)  # (stream, text) pairs, oldest first


RECORD = re.compile(r"^ *\d+\.\d{3} (out|err) (.*)$")


def parse_dumps(path):
    """Parses every dump in a dump file."""
    dumps = []
    for block in Path(path).read_text(errors="surrogateescape").split("blackbox dump\n")[1:]:
        dump = Dump()
        for line in block.splitlines():
            if not line:
                continue
            record = RECORD.match(line)
            if record:
                dump.lines.append((record.group(1), record.group(2)))
            else:
                key, _, value = line.partition(":")
                dump.header[key] = value.strip()
        dumps.append(dump)
    return dumps


@pytest.fixture
def dump_path(tmp_path):
    return tmp_path / "test.dump"


@pytest.fixture
def start_blackbox():
    """Starts blackbox in the background, by default with pipes for stdin, stdout and stderr.

    Each run gets its own process group. Teardown kills the whole group, so a failed test
    does not leave blackbox, the command or a background grandchild running.
    """
    started = []

    def start(args, **kwargs):
        options = {
            "stdin": subprocess.PIPE,
            "stdout": subprocess.PIPE,
            "stderr": subprocess.PIPE,
            **kwargs,
        }
        proc = subprocess.Popen(
            [str(BLACKBOX), *args], preexec_fn=no_core_files, start_new_session=True, **options
        )
        started.append(proc)
        return proc

    yield start

    for proc in started:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            if stream:
                stream.close()
