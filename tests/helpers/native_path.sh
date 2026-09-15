#!/usr/bin/env bash
# ============================================================
# native_path.sh — hand the binary a path IT can open
#
# WHY THIS IS SHARED
#
# naab-lang under MSYS2 is a NATIVE Windows build. The shell says /tmp/xxx; the
# binary cannot open that. So any test that writes an absolute path INTO a .naab
# program, or passes one as argv, is handing over a name in the wrong vocabulary
# — and the failure is silent in the worst way: the read fails, the test's
# "was it refused?" logic says yes, and an arm expecting a refusal PASSES for a
# reason that has nothing to do with what it measures.
#
# This has now cost four separate diagnoses (test_hivemind_governed.sh,
# test_coverage_visibility.sh, test_state_field_screen.sh twice), then
# test_relative_path_base.sh reddened build-windows across three master
# commits, and then test_inert_capability_keys.sh reintroduced it the same day
# the previous one was fixed. Each time it was fixed locally and the fix reached
# exactly one file. Hence one helper.
#
# It is invisible on Linux, which is why it keeps coming back: the author's box
# and the reviewer's box both pass.
#
# USAGE
#   source "<repo>/tests/helpers/native_path.sh"
#   T=$(native_path "$W/secret.txt")     # C:/... under MSYS2, unchanged elsewhere
#   printf 'file.read("%s")' "$T"
#
# PREFER A RELATIVE PATH WHERE THE TEST ALLOWS IT. If the harness cd's into the
# fixture directory, "secret.txt" needs no conversion and goes through NAAb's own
# canonicaliser on both sides — that is what test_path_precedence.sh does, and it
# is strictly more robust. Use native_path() only where the test genuinely needs
# an ABSOLUTE path (e.g. asserting on absolute-path policy), because there a
# relative path would not test the thing named.
#
# -m and not -w: cygpath -m yields C:/… with FORWARD slashes, which survives
# being pasted into a NAAb string literal. -w yields backslashes, which the
# lexer reads as escapes.
# ============================================================

native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# True when the shell and the binary do NOT share a path vocabulary. Useful for
# a test that wants to skip (UNMEASURABLE) rather than convert.
native_path_differs() {
    command -v cygpath >/dev/null 2>&1
}
