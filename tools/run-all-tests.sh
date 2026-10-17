#!/usr/bin/env bash
# =====================================================================
# run-all-tests.sh - build pdflib.dll and run the whole verification
# chain for one or both architectures, from a bash shell.
#
#   tools/run-all-tests.sh                 # x64 and x86
#   tools/run-all-tests.sh x64             # one architecture
#   tools/run-all-tests.sh x86 --clean     # wipe build/<arch> first
#   tools/run-all-tests.sh --out-of-tree   # run the tests from a temp copy
#
# Steps per architecture:
#   1. configure (CMake + Ninja, conan toolchain from build/<arch>-deps)
#   2. build every target (pdflib.dll, pdfx.dll, smoke, apitest, examples)
#   3. install into dist/<arch>
#   4. verify the export tables against src/pdflib.def and pdfx/pdfx.def
#   5. verify the API test references every export
#   6. run the test suite - ctest, or the out-of-tree runner
#
# --out-of-tree copies the executables and DLLs to a temporary directory
# and runs the same commands there. Use it when the test binaries cannot
# write files but the shell can: some environments deny writes to a
# process whose image lives inside the checked-out workspace, which turns
# every save into a bogus permission error. See
# tools/run_tests_out_of_tree.py.
#
# Exits non-zero if any step of any architecture failed.
#
# Prerequisites: conan dependencies already installed for each arch
# (build-deps.cmd <arch>), i.e. build/<arch>-deps/build/Release/generators
# /conan_toolchain.cmake must exist.
# =====================================================================
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1

LOGDIR="$ROOT/build/logs"
# Native (Windows) tools - cmake, dumpbin, python - cannot resolve MSYS
# paths such as /f/project/..., so anything handed to them must be
# relative to the repository root, which is the working directory.
LOGDIR_REL="build/logs"
mkdir -p "$LOGDIR"

# The same rule applies to the paths given to cmake on the configure
# line: a prefix of /f/project/... is silently taken as "F:\f\project\..."
# (absolute on the current drive), so the DLLs get installed into a
# directory that mirrors the whole path under the drive root instead of
# the one that was asked for. cygpath -m gives the F:/... form that both
# cmake and MSVC understand.
ROOT_WIN="$(cygpath -m "$ROOT" 2>/dev/null || echo "$ROOT")"

# --- arguments -------------------------------------------------------
ARCHES=()
CLEAN=0
OUT_OF_TREE=0
for arg in "$@"; do
    case "$arg" in
        x64|x86) ARCHES+=("$arg") ;;
        --clean) CLEAN=1 ;;
        --out-of-tree) OUT_OF_TREE=1 ;;
        -h|--help) awk 'NR>1 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "$0"; exit 0 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done
if [ ${#ARCHES[@]} -eq 0 ]; then
    ARCHES=(x64 x86)
fi

# --- python for the static checks -----------------------------------
# Prefer an explicit override, then whatever is on PATH, then the
# sandbox-managed interpreter.
find_python() {
    if [ -n "${PYTHON:-}" ] && command -v "$PYTHON" >/dev/null 2>&1; then
        echo "$PYTHON"; return 0
    fi
    for cand in python3 python; do
        if command -v "$cand" >/dev/null 2>&1; then echo "$cand"; return 0; fi
    done
    for cand in \
        "$HOME/.workbuddy-ai/binaries/python/versions/3.13.12/python.exe" \
        "$HOME/.workbuddy-ai/binaries/python/versions/3.13.5/python.exe"; do
        if [ -x "$cand" ]; then echo "$cand"; return 0; fi
    done
    return 1
}
PYTHON_BIN="$(find_python)" || PYTHON_BIN=""

# Where --out-of-tree stages the binaries. Outside the repository on
# purpose: the whole point is to run them where nothing restricts them.
TMPDIR_RUN="${TMPDIR_RUN:-${TEMP:-${TMPDIR:-/tmp}}/pdflib-run}"

# --- per-architecture driver ----------------------------------------
# Returns 0 only when every step of this architecture passed. The caller
# aggregates the exit status, because a subshell cannot hand back an
# array assignment.
run_arch() {
    local ARCH="$1"
    local BUILD="build/$ARCH"
    local DEPS="$BUILD-deps"
    local LOG="$LOGDIR/run-$ARCH.log"
    local STEP_FAIL=0

    : > "$LOG"

    echo
    echo "==================== $ARCH ===================="

    if [ "$CLEAN" = "1" ] && [ -d "$BUILD" ]; then
        echo "[$ARCH] cleaning $BUILD"
        rm -rf "$BUILD" 2>/dev/null
        # A sandbox or a read-only file can silently defeat the delete;
        # a half-removed build tree is worse than none, so say so.
        if [ -d "$BUILD" ]; then
            echo "[$ARCH] [warn] could not remove $BUILD (permissions?);"
            echo "               continuing with the existing build tree"
        fi
    fi

    # MSVC environment. env.sh exports INCLUDE/LIB/PATH plus CL_EXE and
    # DUMPBIN_EXE for the requested arch.
    # shellcheck disable=SC1091
    source "$ROOT/env.sh" "$ARCH" >> "$LOG" 2>&1 || {
        echo "[$ARCH] [FAIL] could not set up the MSVC environment"
        return 1
    }
    # Stop MSYS from rewriting /flags into paths.
    export MSYS2_ARG_CONV_EXCL="*"

    local TOOLCHAIN="$ROOT_WIN/$DEPS/build/Release/generators/conan_toolchain.cmake"
    if [ ! -f "$TOOLCHAIN" ]; then
        echo "[$ARCH] [FAIL] dependencies missing: $DEPS"
        echo "            run build-deps.cmd $ARCH first"
        return 1
    fi

    # 1) configure ----------------------------------------------------
    # Always run it. It is a couple of seconds, and it is the only thing
    # that keeps the cache in step with the flags above: a build tree
    # configured earlier with a different install prefix keeps that
    # prefix forever, and `cmake --install` then writes somewhere nobody
    # looks while reporting success.
    echo "[$ARCH] [1/5] configure"
    cmake -S standalone -B "$BUILD" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_PREFIX_PATH="$ROOT_WIN/$DEPS/build/Release/generators" \
        -DCMAKE_INSTALL_PREFIX="$ROOT_WIN/dist/$ARCH" \
        >> "$LOG" 2>&1 || {
        echo "[$ARCH] [FAIL] configure - see $LOG"
        return 1
    }

    # 2) build --------------------------------------------------------
    echo "[$ARCH] [2/5] build"
    if ! cmake --build "$BUILD" >> "$LOG" 2>&1; then
        echo "[$ARCH] [FAIL] build - see $LOG"
        grep -aiE "error" "$LOG" | head -20
        return 1
    fi

    # 3) install ------------------------------------------------------
    echo "[$ARCH] [3/5] install -> dist/$ARCH"
    if ! cmake --install "$BUILD" >> "$LOG" 2>&1; then
        echo "[$ARCH] [FAIL] install - see $LOG"
        STEP_FAIL=1
    else
        # Assert the files landed where the prefix says. `cmake --install`
        # reports success even when the prefix pointed somewhere else
        # entirely, so a silent mis-install is worth failing on.
        local MISSING=""
        for f in pdflib.dll lib/pdflib.lib include/pdflib.h; do
            [ -f "dist/$ARCH/$f" ] || MISSING="$MISSING $f"
        done
        if [ -f "$BUILD/pdfx.dll" ]; then
            for f in pdfx.dll lib/pdfx.lib include/pdfx/pdfx.h; do
                [ -f "dist/$ARCH/$f" ] || MISSING="$MISSING $f"
            done
        fi
        if [ -n "$MISSING" ]; then
            echo "[$ARCH] [FAIL] install prefix did not take: missing$MISSING"
            STEP_FAIL=1
        fi
    fi

    # 4) export tables vs the .def files -------------------------------
    # This is the guard against a decorated or missing export, which
    # would break every GetProcAddress / ctypes / P-Invoke caller. pdfx
    # matters most on x86, where __declspec(dllexport) would have added
    # the "@N" stdcall decoration; its exports come from the .def file
    # only, so this check is what proves the scheme works.
    echo "[$ARCH] [4/5] export tables"
    if [ -n "${DUMPBIN_EXE:-}" ] && [ -x "$DUMPBIN_EXE" ]; then
        local DLL="$BUILD/pdflib.dll"
        local EXPORTS="$LOGDIR_REL/exports-$ARCH.txt"
        "$DUMPBIN_EXE" /exports "$DLL" > "$EXPORTS" 2>&1
        if "$PYTHON_BIN" tools/check_exports.py "$EXPORTS" src/pdflib.def; then
            :
        else
            echo "[$ARCH] [FAIL] pdflib export table does not match src/pdflib.def"
            STEP_FAIL=1
        fi

        local XDLL="$BUILD/pdfx.dll"
        local XEXPORTS="$LOGDIR_REL/exports-pdfx-$ARCH.txt"
        if [ -f "$XDLL" ]; then
            "$DUMPBIN_EXE" /exports "$XDLL" > "$XEXPORTS" 2>&1
            if "$PYTHON_BIN" tools/check_exports.py "$XEXPORTS" pdfx/pdfx.def; then
                :
            else
                echo "[$ARCH] [FAIL] pdfx export table does not match pdfx/pdfx.def"
                STEP_FAIL=1
            fi
        else
            echo "[$ARCH] [skip] pdfx.dll not built"
        fi
    else
        echo "[$ARCH] [skip] dumpbin not available; export check skipped"
    fi

    # 5) api coverage + ctest ----------------------------------------
    if [ -n "$PYTHON_BIN" ]; then
        echo "[$ARCH] [5/5] api coverage"
        if ! "$PYTHON_BIN" tools/check_api_coverage.py; then
            echo "[$ARCH] [FAIL] the API test does not reference every export"
            STEP_FAIL=1
        fi
    else
        echo "[$ARCH] [5/5] api coverage (skipped: no python interpreter)"
    fi

    echo "[$ARCH] ctest"
    if [ "$OUT_OF_TREE" = "1" ] && [ -z "$PYTHON_BIN" ]; then
        echo "[$ARCH] [FAIL] --out-of-tree needs a python interpreter"
        STEP_FAIL=1
    elif [ "$OUT_OF_TREE" = "1" ]; then
        if ! "$PYTHON_BIN" tools/run_tests_out_of_tree.py \
                --build-dir "$BUILD" --dest "$TMPDIR_RUN/$ARCH" \
                >> "$LOG" 2>&1; then
            echo "[$ARCH] [FAIL] out-of-tree test run - see $LOG"
            grep -aE "\[FAIL\]|=== FAILED" "$LOG" | tail -20
            STEP_FAIL=1
        else
            grep -aE "=== all .* test" "$LOG" | tail -1
        fi
    elif ! ctest --test-dir "$BUILD" --output-on-failure >> "$LOG" 2>&1; then
        echo "[$ARCH] [FAIL] ctest - see $LOG"
        grep -aE "Failed|FAILED|\*\*\*" "$LOG" | head -20
        STEP_FAIL=1
    else
        grep -E "tests passed" "$LOG" | tail -1
    fi

    if [ "$STEP_FAIL" = "0" ]; then
        echo "[$ARCH] [ok] all steps passed"
        return 0
    fi
    return 1
}

# --- run every requested architecture --------------------------------
FAILED_ARCHES=()
for arch in "${ARCHES[@]}"; do
    if ! ( run_arch "$arch" ); then
        FAILED_ARCHES+=("$arch")
    fi
done

echo
if [ ${#FAILED_ARCHES[@]} -eq 0 ]; then
    echo "=== ALL PASSED (${ARCHES[*]}) ==="
    exit 0
fi
echo "=== FAILED: ${FAILED_ARCHES[*]} ==="
exit 1
