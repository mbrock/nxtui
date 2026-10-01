#!/usr/bin/env python3
"""Process boundaries, real pipes/timers, and host checkpoint policy."""
import pathlib
import re
import stat
import subprocess
import sys
import tempfile
import time

binary = str(pathlib.Path(sys.argv[1]).resolve())


def command(*args, input=b"", ok=True, timeout=20):
    result = subprocess.run(
        [binary, *map(str, args)], input=input, capture_output=True, timeout=timeout
    )
    assert (result.returncode == 0) == ok, (args, result.returncode, result.stderr)
    return result


with tempfile.TemporaryDirectory(prefix="wisp-host-") as directory:
    directory = pathlib.Path(directory)
    source = directory / "job.wisp"
    tape = directory / "job.tape"

    repl = command("repl", input=b"(+ 17 25)\nunbound\n(+ 19 4)\nnil\n")
    assert repl.stdout == b"42\n23\nNIL\n", repl.stdout
    assert b"UNBOUND-VARIABLE" in repl.stderr, repl.stderr

    source.write_text('''
      (write "before\\n")
      (let ((x 35)) (sleep-ms 4000) (print (+ x 7)))
      (defpackage after (:use wisp))
      (in-package after)
      (print 'finished)
    ''')
    stopped = command("run", source, "--checkpoint", tape)
    assert stopped.stdout == b"before\n" and stopped.stderr == b"", stopped
    assert stat.S_IMODE(tape.stat().st_mode) == 0o600
    original = tape.read_bytes()
    info = command("inspect", tape)
    assert b"request: #<:TIMER 4000>" in info.stdout, info.stdout
    assert b"request-id: ~20220101." in info.stdout, info.stdout
    deadline = int(re.search(rb'deadline-unix-ms: "([0-9]+)"', info.stdout)[1])
    assert deadline > time.time() * 1000, "fixture timer already expired"
    disabled = command("restore", tape, ok=False)
    assert disabled.stdout == b"" and b"effects disabled" in disabled.stderr
    assert tape.read_bytes() == original

    # Restore never reloads this file. Both the source and its byte cursor
    # must be in the entry, not in a C++ reader or an external path.
    source.unlink()
    resumed = command("restore", tape, "--effects")
    assert resumed.stdout == b"42\nFINISHED\n", resumed.stdout
    assert resumed.stderr == b"", resumed.stderr
    assert time.time() * 1000 >= deadline - 25, "timer fired before deadline"
    # The same image is now an expired fork. A mistaken relative-delay
    # restore would wait four seconds and exceed this deadline.
    expired = command("restore", tape, "--effects", timeout=2)
    assert expired.stdout == resumed.stdout
    assert tape.read_bytes() == original, "restores must not rewrite input"

    source.write_text('''
      (print (try (do (sleep-ms 10000) 'wrong)
                  (error (reason continuation) 'cancelled)))
      (print 19)
    ''')
    command("run", source, "--checkpoint", tape)
    cancelled = command("restore", tape, "--effects", "--cancel", timeout=2)
    assert cancelled.stdout == b"CANCELLED\n19\n", cancelled.stdout

    source.write_text('''
      (write-error "diagnostic\\n")
      (print (read-line))
      (write (read-bytes 3))
      (print (read-line))
      (print (try (sleep-ms (- 0 1)) (error (why k) 'bad-delay)))
      (print (try (send! :host (vector :unknown nil))
                  (error (why k) 'unknown-effect)))
      (print (try (write "prefix" 17) (error (why k) 'bad-write)))
      (sleep-ms 1)
      (gc)
      (print 73)
    ''')
    console = command("run", source, input=b"line\na\0b")
    assert console.stdout == b'"line"\na\0bNIL\nBAD-DELAY\nUNKNOWN-EFFECT\nBAD-WRITE\n73\n', console.stdout
    assert console.stderr == b"diagnostic\n", console.stderr

    source.write_text('(sleep-ms 1) (write "must not run")')
    destination = directory / "existing-directory"
    destination.mkdir()
    failed = command("run", source, "--checkpoint", destination, ok=False)
    assert b"checkpoint rename" in failed.stderr and failed.stdout == b"", failed
    assert destination.is_dir()
    assert not list(directory.glob("existing-directory.*")), "temporary tape leaked"

    source.write_text('(print 7)')
    missing = directory / "missing.tape"
    no_timer = command("run", source, "--checkpoint", missing, ok=False)
    assert b"no checkpoint written" in no_timer.stderr and not missing.exists()
    assert command("inspect", source, ok=False).stdout == b""

print("Wisp host: REPL, console, timers, restart, effect gating, cancellation, and file failures passed")
