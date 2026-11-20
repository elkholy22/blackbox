"""Test child for the component tests.

Runs the actions given as arguments in order, for example:

    child.py lines:1000 abort
    child.py ready readline exit:0

Actions:
    out:TEXT       print TEXT to stdout
    err:TEXT       print TEXT to stderr
    lines:N        print "line 1" to "line N" to stdout
    partial:TEXT   write TEXT to stdout without a newline
    long:N         print a line of N "x" characters to stdout
    binary         write BINARY_OUT to stdout and BINARY_ERR to stderr
    flood:MB       write MB megabytes of lines, alternating stdout and stderr
    cat            copy stdin to stdout
    ready          print "ready" to stdout (tests wait for it before signalling)
    readline       wait for one line on stdin
    sleep:SECONDS  sleep
    trap-term      on SIGTERM print "goodbye" and exit 0
    count-int      print "ready", wait for SIGINT, then print how many arrived ("sigint N")
    grandchild     start "sleep 30" in the background
    exit:N         exit with code N
    abort          call abort()
    kill:SIGNAME   send SIGNAME to itself
"""

import os
import signal
import subprocess
import sys
import time

BINARY_OUT = bytes(range(256)) * 16 + b"\x00\xff\xfe invalid \xc3\x28 utf-8\n" + b"\x00" * 10
BINARY_ERR = b"\xc0\xaf\r\n\x1b[31m" + bytes(range(255, -1, -1)) * 8


def write(fd, data):
    while data:
        written = os.write(fd, data)
        data = data[written:]


def flood(megabytes):
    line = b"x" * 99 + b"\n"
    block = line * 655  # about 64 KiB
    remaining = megabytes * 1024 * 1024
    fd = 1
    while remaining > 0:
        chunk = block[:remaining]
        write(fd, chunk)
        remaining -= len(chunk)
        fd = 3 - fd  # alternate between 1 and 2


def on_term(signum, frame):
    write(1, b"goodbye\n")
    sys.exit(0)


def count_sigint():
    received = []
    signal.signal(signal.SIGINT, lambda signum, frame: received.append(signum))
    write(1, b"ready\n")
    while not received:
        time.sleep(0.01)
    time.sleep(0.5)  # time for a second copy to arrive
    write(1, b"sigint %d\n" % len(received))


def run(action):
    name, _, arg = action.partition(":")
    if name == "out":
        write(1, arg.encode() + b"\n")
    elif name == "err":
        write(2, arg.encode() + b"\n")
    elif name == "lines":
        write(1, b"".join(b"line %d\n" % i for i in range(1, int(arg) + 1)))
    elif name == "partial":
        write(1, arg.encode())
    elif name == "long":
        write(1, b"x" * int(arg) + b"\n")
    elif name == "binary":
        write(1, BINARY_OUT)
        write(2, BINARY_ERR)
    elif name == "flood":
        flood(int(arg))
    elif name == "cat":
        while data := os.read(0, 65536):
            write(1, data)
    elif name == "ready":
        write(1, b"ready\n")
    elif name == "readline":
        sys.stdin.readline()
    elif name == "sleep":
        time.sleep(float(arg))
    elif name == "trap-term":
        signal.signal(signal.SIGTERM, on_term)
    elif name == "count-int":
        count_sigint()
    elif name == "grandchild":
        subprocess.Popen(["sleep", "30"])
    elif name == "exit":
        sys.exit(int(arg))
    elif name == "abort":
        os.abort()
    elif name == "kill":
        os.kill(os.getpid(), getattr(signal, arg))
        time.sleep(5)  # the signal should have ended us already
    else:
        sys.exit(f"child.py: unknown action {action!r}")


if __name__ == "__main__":
    for action in sys.argv[1:]:
        run(action)
