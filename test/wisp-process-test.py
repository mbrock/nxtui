#!/usr/bin/env python3
"""--run grants and run-command: results, output, limits and refusals."""
import os
import pathlib
import subprocess
import sys
import tempfile

binary = str(pathlib.Path(sys.argv[1]).resolve())


def run(source, *args, ok=True):
    result = subprocess.run([binary, "run", str(source), *args],
                            capture_output=True, timeout=30)
    assert (result.returncode == 0) == ok, result.stderr
    return result


with tempfile.TemporaryDirectory(prefix="wisp-process-") as directory:
    directory = pathlib.Path(directory)
    tool = directory / "tool.sh"
    tool.write_text("#!/bin/sh\nprintf 'tool:%s' \"$*\"\n")
    tool.chmod(0o755)
    source = directory / "commands.wisp"
    source.write_text(r'''
      (defun code (thunk)
        (try (call thunk) (catch (e k) (host-error-code e))))
      (print (run-command "sh" "-c" "echo out; echo err >&2; exit 3"))
      (print (run-command "sh" "-c" "kill -TERM $$"))
      (write (process-result-output
               (run-command "sh" "-c" "printf 'a\\000\\377'")) "|")
      (print (process-result-output (run-command "tool" "x y" "z")))
      (print (list (code (fn () (run-command "ls")))
                   (code (fn () (run-command "yes")))
                   (code (fn () (run-command "sh" 42)))
                   (code (fn () (run-command 'sh)))))
    ''')
    out = run(source, "--run", "sh", "--run", "yes",
              "--run", f"tool={tool}").stdout
    assert out == (b'#S(PROCESS-RESULT :EXIT-CODE 3 :SIGNAL NIL :OUTPUT "out\\nerr\\n")\n'
                   b'#S(PROCESS-RESULT :EXIT-CODE NIL :SIGNAL 15 :OUTPUT "")\n'
                   b'a\0\xff|"tool:x y z"\n'
                   b'(:NOT-CAPABLE :TOO-LARGE :INVALID-ARGUMENT :INVALID-ARGUMENT)\n'), out

    # Grants resolve PATH once, at startup; the guest never searches PATH.
    source.write_text('(print (process-result-output (run-command "here")))')
    env = dict(os.environ, PATH=f"{directory}:{os.environ['PATH']}")
    (directory / "here").write_text("#!/bin/sh\necho found\n")
    (directory / "here").chmod(0o755)
    found = subprocess.run([binary, "run", str(source), "--run", "here"],
                           capture_output=True, timeout=30, env=env)
    assert found.stdout == b'"found\\n"\n', found
    for bad in (["--run", "a/b"], ["--run", "missing-program-xyz"],
                ["--run", f"x={directory}"], ["--run", "sh", "--run", "sh"]):
        assert b"--run" in run(source, *bad, ok=False).stderr, bad

    # Like HTTP, commands are external effects a checkpoint cannot replay.
    source.write_text('(run-command "sh" "-c" "true")')
    rejected = run(source, "--run", "sh", "--checkpoint",
                   directory / "tape", ok=False)
    assert b"NOT-REPLAYABLE" in rejected.stderr, rejected.stderr

print("Wisp processes: grants, results, output, limits, refusals and checkpoint rejection passed")
