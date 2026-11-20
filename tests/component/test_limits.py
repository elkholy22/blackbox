import os
import time

from conftest import SANITIZED, child

FLOOD_MB = 50


def test_flood_on_both_streams(dump_path, tmp_path, start_blackbox):
    out_file = tmp_path / "stdout"
    err_file = tmp_path / "stderr"

    started = time.monotonic()
    with open(out_file, "wb") as out, open(err_file, "wb") as err:
        command = child(f"flood:{FLOOD_MB}")
        proc = start_blackbox(["-o", str(dump_path), *command], stdout=out, stderr=err)
        # wait4 instead of wait, for the peak RSS of blackbox and its child.
        _, status, usage = os.wait4(proc.pid, 0)
        proc.returncode = os.waitstatus_to_exitcode(status)
    elapsed = time.monotonic() - started

    assert proc.returncode == 0
    total = out_file.stat().st_size + err_file.stat().st_size
    assert total == FLOOD_MB * 1024 * 1024
    assert out_file.stat().st_size > 0 and err_file.stat().st_size > 0
    assert elapsed < 10

    if not SANITIZED:
        peak_rss_mb = usage.ru_maxrss / 1024  # ru_maxrss is in KiB on Linux
        assert peak_rss_mb < 32, f"peak RSS {peak_rss_mb:.1f} MB"
