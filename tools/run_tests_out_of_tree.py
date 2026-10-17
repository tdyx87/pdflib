#!/usr/bin/env python3
"""Run the ctest suite from a COPY of the build output, in another directory.

Why this exists
---------------
Some environments refuse file writes to a process whose executable lives
inside the checked-out workspace, while the shell itself can still write
there. The symptom is unmistakable and misleading: every test that saves a
document fails with a permission error (``fopen`` returning EACCES, or
``HPDF_FILE_IO_ERROR`` / ``PDF_ERR_IO`` out of the library), while
``cmake --build`` in the same directory succeeds. The same binaries copied
to a temp directory work perfectly.

Rather than special-case the tests, this script asks ctest which commands
it would run, copies the executables and DLLs somewhere writable, and runs
those same commands there. It is also a useful check in its own right: it
proves the binaries run from the installed layout, with no dependency on
the build tree.

    python tools/run_tests_out_of_tree.py --build-dir build/x64 --dest DIR

Exit status is non-zero if any test failed. Test output is only shown for
the tests that failed, so a green run stays short.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

COPY_PATTERNS = (".exe", ".dll")


def ctest_tests(build_dir):
    """Ask ctest what it would run, without running it."""
    out = subprocess.run(
        ["ctest", "--test-dir", build_dir, "--show-only=json-v1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if out.returncode != 0:
        raise RuntimeError("ctest --show-only failed: %s" %
                           out.stderr.decode("utf-8", "replace").strip())
    return json.loads(out.stdout.decode("utf-8", "replace"))["tests"]


def working_directory(test, build_dir):
    for prop in test.get("properties", []):
        if prop.get("name") == "WORKING_DIRECTORY":
            return prop.get("value") or build_dir
    return build_dir


def stage(build_dir, dest):
    """Copy the executables and DLLs into `dest`.

    Only the top level of the build directory matters: CMake puts every
    runtime artefact next to the others, and the DLLs have to sit beside
    the .exe files because that is how Windows resolves them.
    """
    os.makedirs(dest, exist_ok=True)
    copied = 0
    for entry in sorted(os.listdir(build_dir)):
        if not entry.lower().endswith(COPY_PATTERNS):
            continue
        src = os.path.join(build_dir, entry)
        if not os.path.isfile(src):
            continue
        shutil.copy2(src, os.path.join(dest, entry))
        copied += 1
    return copied


def run_one(test, dest, build_dir):
    command = list(test["command"])
    # The recorded command holds an absolute path inside the build tree;
    # the staged copy is the same file by basename. Run it by absolute
    # path - Windows does not search the child's working directory for
    # the executable, only the parent's, and the parent's is the
    # repository root.
    base = os.path.basename(command[0])
    if not os.path.exists(os.path.join(build_dir, base)):
        # Not a staged binary: this is a script run by an interpreter
        # (the API-coverage check). It does not write files, so the
        # staging trick is irrelevant - run it exactly as ctest would.
        proc = subprocess.run(command, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False)
    else:
        command[0] = os.path.join(dest, base)
        if not os.path.exists(command[0]):
            return False, "%s is not in %s" % (base, dest)
        proc = subprocess.run(command, cwd=dest, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, check=False)
    text = proc.stdout.decode("utf-8", "replace")
    if proc.returncode == 0:
        return True, text
    return False, text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--build-dir", required=True,
                    help="CMake build directory that ctest would use")
    ap.add_argument("--dest", required=True,
                    help="directory to copy the binaries to and run from")
    ap.add_argument("--list", action="store_true",
                    help="only list the tests that would run")
    ap.add_argument("--keep", action="store_true",
                    help="do not delete --dest first")
    args = ap.parse_args()

    if not os.path.isdir(args.build_dir):
        print("no such build directory: %s" % args.build_dir)
        return 2

    # Everything below works with absolute paths: the child process is
    # started with `dest` as its working directory, and a relative
    # executable path would then be resolved against the wrong base.
    args.dest = os.path.abspath(args.dest)

    try:
        tests = ctest_tests(args.build_dir)
    except (RuntimeError, ValueError) as exc:
        print("could not enumerate the tests: %s" % exc)
        return 2

    if args.list:
        for test in tests:
            print("%-24s %s" % (test["name"], " ".join(test["command"])))
        return 0

    if not args.keep and os.path.isdir(args.dest):
        shutil.rmtree(args.dest)
    copied = stage(args.build_dir, args.dest)
    if copied == 0:
        print("nothing to run: no .exe/.dll in %s" % args.build_dir)
        return 2
    print("staged %d file(s) into %s" % (copied, args.dest))
    print("(the build tree's own working directory is not used)")

    failed = []
    for test in tests:
        ok, text = run_one(test, args.dest, args.build_dir)
        name = test["name"]
        if ok:
            # Show the suite's own summary line - it carries the check
            # count, which is the number worth watching over time.
            summary = [ln for ln in text.splitlines()
                       if "PASSED" in ln or "failure(s)" in ln]
            print("  [ok]   %-22s %s" %
                  (name, summary[-1].strip() if summary else ""))
        else:
            print("  [FAIL] %-22s" % name)
            failed.append(name)
            for line in text.splitlines()[-25:]:
                print("         %s" % line)

    print()
    if failed:
        print("=== FAILED: %d of %d test(s): %s ===" %
              (len(failed), len(tests), ", ".join(failed)))
        return 1
    print("=== all %d test(s) passed ===" % len(tests))
    return 0


if __name__ == "__main__":
    sys.exit(main())
