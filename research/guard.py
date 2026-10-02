#!/usr/bin/env python3
"""A command run so that it cannot take the machine down with it: in a
session of its own, stopped whole (SIGKILL) when the session's processes
hold more than MOST MiB, or when the machine has less than FLOOR MiB
available (what it writes to a tmpfs such as /tmp counts there). The Pi has
8 GiB, swap in zram and no OOM daemon: a run that outgrows its memory has
every program's code evicted before the kernel kills anything, and the
machine stops answering for minutes (it was reset twice so, 2026-10-01: a
run held 13 GiB of output it should have streamed).

guard.py [--most MiB] [--floor MiB] [--peak] -- COMMAND [ARG ...]
    --most   the session's resident memory at most (default 3072)
    --floor  the machine's available memory at least (default 1024)
    --peak   a JSON line on stderr at the end: the session's peak
  The command's exit, or 137 when stopped (a JSON line on stderr says why).
  What the session leaves running when the command ends is stopped too.
  Stopped itself (SIGTERM, SIGINT, SIGHUP), it stops the session first.
"""
import ctypes
import json
import os
import signal
import subprocess
import sys
import time

PAGE = os.sysconf('SC_PAGE_SIZE')
MIB = 1 << 20


def available():
    with open('/proc/meminfo') as info:
        for line in info:
            if line.startswith('MemAvailable:'):
                return int(line.split()[1]) * 1024
    return 0


def members(session):
    """The session's processes and their resident bytes."""
    found = {}
    for entry in os.scandir('/proc'):
        if not entry.name.isdigit():
            continue
        try:
            with open(f'/proc/{entry.name}/stat', 'rb') as stat:
                fields = stat.read().rsplit(b')', 1)[1].split()
        except OSError:  # gone meanwhile
            continue
        if int(fields[3]) == session:
            found[int(entry.name)] = int(fields[21]) * PAGE
    return found


def stop(session):
    for pid in members(session):
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def main():
    args, most, floor, peak_wanted = sys.argv[1:], 3072, 1024, False
    while args and args[0] != '--':
        option = args.pop(0)
        if option == '--peak':
            peak_wanted = True
        elif option in ('--most', '--floor') and args:
            value = int(args.pop(0))
            most, floor = (value, floor) if option == '--most' else (most, value)
        else:
            sys.exit(__doc__)
    command = args[1:]
    if not command:
        sys.exit(__doc__)
    libc = ctypes.CDLL(None, use_errno=True)
    # The command dies with the guard, however the guard ends (its own
    # children are stopped through the session).
    child = subprocess.Popen(command, start_new_session=True, preexec_fn=lambda: libc.prctl(1, signal.SIGKILL))
    session = child.pid

    def stopped_from_outside(number, frame):
        stop(session)
        sys.exit(128 + number)

    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, stopped_from_outside)
    peak, began = 0, time.monotonic()
    while child.poll() is None:
        held, free = sum(members(session).values()), available()
        peak = max(peak, held)
        if held > most * MIB or free < floor * MIB:
            stop(session)
            child.wait()
            print(json.dumps({'guard': 'stopped', 'why': 'its memory' if held > most * MIB else "the machine's memory",
                              'held_mib': held // MIB, 'most_mib': most, 'available_mib': free // MIB,
                              'floor_mib': floor, 'after_s': round(time.monotonic() - began, 1),
                              'command': ' '.join(command)[:200]}), file=sys.stderr, flush=True)
            return 137
        time.sleep(0.25)
    stop(session)  # what it left running
    if peak_wanted:
        print(json.dumps({'guard': 'done', 'peak_mib': peak // MIB, 'exit': child.returncode,
                          'seconds': round(time.monotonic() - began, 1)}), file=sys.stderr, flush=True)
    return child.returncode


if __name__ == '__main__':
    sys.exit(main())
