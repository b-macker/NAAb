#!/usr/bin/env bash
# ============================================================
# stub_platform.sh — platform gate for suites that launch agent_stub.py
#
# Stub-backed HTTP tests hang on Windows/MSYS2, from signal propagation and
# process cleanup. That was diagnosed once, for test_absorption_degenerate.sh,
# and the guard applied to that file alone — while 28 other suites launched the
# same stub and kept running there. build-windows then stalled four times inside
# "CLI tests — shell suites": the step sat in_progress ~47 minutes, the runner
# was killed service-side, and the log archive 404'd. `timeout-minutes` could not
# fire either, so the cause could not be read out of a log — only excluded.
#
# Excluding all 29 took that step from a 47-minute hang to 2m04s (#120), which is
# what confirms the original one-file diagnosis generalises.
#
# This is a WORKAROUND, not a fix. The launcher's signal and cleanup behaviour
# under MSYS2 is the actual defect and is still open. Coverage is not lost in the
# meantime: build-linux and Build & Test run every one of these suites in full,
# and what they test — agent governance semantics — is platform-neutral.
#
# Why this lives in one file: the guard previously existed as 29 near-copies, and
# the reason it took four stalls to apply was that the finding sat in a comment in
# one of them. A single definition is the thing that makes the next such finding
# reach every caller.
#
# Usage — before any stub is launched:
#     source "$SCRIPT_DIR/../helpers/stub_platform.sh"
#     skip_if_no_stub_support
#
# The file is sourced, so `exit 0` here ends the CALLING script. The suite name is
# printed so the skip is visible in the run log rather than silently absent.
# ============================================================

skip_if_no_stub_support() {
    local name="${1:-}"
    if [ -z "$name" ]; then
        name="$(basename "${BASH_SOURCE[1]:-unknown}")"
    fi

    local is_windows=0
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*) is_windows=1 ;;
    esac
    # Second signal: a native shell where uname is absent or lies.
    if [ -n "${WINDIR:-}" ]; then
        is_windows=1
    fi

    if [ "$is_windows" -eq 0 ]; then
        return 0
    fi

    echo "  ${name}: SKIPPED — stub-backed tests unsupported on Windows/MSYS2"
    echo "    (signal propagation / process cleanup; covered by build-linux)"
    exit 0
}
