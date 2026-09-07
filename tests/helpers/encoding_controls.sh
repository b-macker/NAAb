#!/usr/bin/env bash
# ============================================================
# encoding_controls.sh -- controls for a test's OUTPUT CHANNEL
#
# THE CLASS. A test that runs a tool, captures its stdout, and compares the
# result against a baseline has TWO things that can be wrong: the tool's answer,
# and the path from that answer to the comparison. The second one is part of the
# instrument and needs its own control. It has none by default, and it breaks
# only on platforms nobody runs locally.
#
# Three occurrences in two days across two sessions, none retained from the
# previous fix:
#   tests/governance_v4/test_hivemind_governed.sh   cp1252 read, box-drawing anchor
#   tests/self-audit/test_coverage_visibility.sh    cp1252 read, run-all-tests.sh
#   tests/self-audit/test_state_field_screen.sh     cp1252 write, then CRLF write
#
# TWO FAILURE SHAPES, and the second is the dangerous one:
#
#   ENCODING  Python encodes stdout with the LOCALE's encoding. Under MSYS2 that
#             is cp1252, which writes U+2014 as the single byte 0x97; a UTF-8
#             reader then dies. Under a bare C locale the WRITER dies instead.
#             A traceback announces itself.
#
#   NEWLINE   print() goes through a text wrapper that translates "\n" to
#             "\r\n" on Windows. Command substitution strips the trailing
#             newline but not the embedded CRs, so the captured string compares
#             unequal to a baseline it renders IDENTICALLY to. Nothing announces
#             itself; the test reports a confident, specific, wrong finding.
#             This is why enc_escaped_diff exists -- making the difference
#             visible is the fix, more than any comparison change.
#
# REPRODUCING THE ENCODING HALF ON LINUX -- the detail that costs the most time:
#   LC_ALL=C does NOT reproduce it. PEP 538 coerces the C locale back to UTF-8.
#   The reproduction is:  PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 LC_ALL=C
# The newline half is reproduced by injecting into the python being tested:
#   sys.stdout.reconfigure(newline="\r\n")
#
# PREVENTION, in the tool being tested:
#   - print ASCII only, and assert it with enc_has_non_ascii
#   - write bytes for anything a test will parse: sys.stdout.buffer.write(...)
#   - open() every handle with explicit encoding= AND errors=
#
# USAGE
#   source "$REPO/tests/helpers/encoding_controls.sh"
#   if enc_has_non_ascii "$OUT"; then bad "X-01" "non-ASCII in tool output"; fi
#   ACTUAL="$(printf '%s' "$ACTUAL" | enc_strip_cr)"
#   enc_escaped_diff expected "$EXPECTED" actual "$ACTUAL"
#
# Self-test:  bash tests/helpers/encoding_controls.sh --self-test
# Callers should run enc_self_test as one of their own assertions; a helper
# nobody exercises is the failure register B10 documents.
# ============================================================

# enc_has_non_ascii FILE
# Exit 0 (true) when the file contains a non-ASCII byte, printing the offending
# lines; exit 1 when clean. Phrased as "has" so `if enc_has_non_ascii f; then`
# reads as the failure branch.
enc_has_non_ascii() {
    local f="$1"
    [ -f "$f" ] || return 1
    if LC_ALL=C grep -qP '[^\x00-\x7F]' "$f" 2>/dev/null; then
        LC_ALL=C grep -nP '[^\x00-\x7F]' "$f" 2>/dev/null | head -5
        return 0
    fi
    return 1
}

# enc_strip_cr  (filter: stdin -> stdout)
# Second line of defence behind writing bytes. Never the primary fix: a CR that
# reaches here means some handle is still in text mode.
enc_strip_cr() { tr -d '\r'; }

# enc_escaped_diff LABEL_A VAL_A LABEL_B VAL_B
# Render both values with control characters visible and "$" marking end of
# line. Call this on EVERY baseline mismatch, not just suspected ones -- the
# whole point is that you cannot tell in advance which mismatches are invisible.
# Renders IN-PROCESS, with no external text tool in the path. The first
# version piped through `sed -n l`, which reddened build-windows a third time:
# the assertion that the renderer surfaces a CR passed on Linux and failed under
# MSYS2. Two hypotheses fit that evidence and they demand DIFFERENT fixes —
# MSYS2's sed rendering CR as octal \015 instead of \r (widen the grep), or
# MSYS2's sed stripping a trailing CR on input as part of a CRLF line terminator
# (nothing renders at all, and widening the grep fixes nothing). Rather than pick
# one, the renderer stops asking a text tool. Bash parameter expansion sees the
# bytes in the variable and no line-ending convention gets a vote.
#
# Note POSIX does list \r among `l`'s named escapes, so the octal hypothesis
# needs sed to be non-conforming; that is why it is a hypothesis and not the
# explanation. enc_platform_probe below records which one it actually is.
enc_escaped_diff() {
    echo "--- escaped (control chars visible, \$ = end of line) ---"
    local lbl val
    for lbl in "$1" "$3"; do
        [ "$lbl" = "$1" ] && val="$2" || val="$4"
        echo "$lbl:"
        val="${val//\\/\\\\}"
        val="${val//$'\r'/\\r}"
        val="${val//$'\t'/\\t}"
        printf '%s$\n' "$val"
    done
}

# enc_platform_probe
# Prints what THIS platform's text tools do with a known CRLF byte sequence.
# Costs nothing, is never asserted on, and exists so a future failure here is
# diagnosable from the CI log alone instead of costing another round-trip to a
# runner nobody has locally. Three rounds were spent on this class already; two
# of them ended in a one-line fix that turned out to be half the story.
enc_platform_probe() {
    echo "--- platform probe: rendering of a\r\n ---"
    printf '  sed -n l : '; printf 'a\r\n' | sed -n l 2>&1 | head -1
    printf '  od -c    : '; printf 'a\r\n' | od -c 2>&1 | head -1
    printf '  cat -v   : '; printf 'a\r\n' | cat -v 2>&1 | head -1
    echo "  (bash renderer is used by enc_escaped_diff; the above is FYI only)"
}

# enc_self_test
# Controls for the controls, both directions. Prints failures; returns non-zero
# if any control is broken.
enc_self_test() {
    local tmp rc=0
    tmp="$(mktemp -d)"

    # (1) enc_has_non_ascii must FIRE on each real-world byte...
    printf 'ok line\nNEVER WRITTEN \xe2\x80\x94 reads: x\n' > "$tmp/utf8_emdash"
    enc_has_non_ascii "$tmp/utf8_emdash" >/dev/null \
        || { echo "!! enc_has_non_ascii missed a UTF-8 em dash"; rc=1; }
    printf 'ok line\nNEVER WRITTEN \x97 reads: x\n' > "$tmp/cp1252_emdash"
    enc_has_non_ascii "$tmp/cp1252_emdash" >/dev/null \
        || { echo "!! enc_has_non_ascii missed a cp1252 0x97"; rc=1; }

    # (2) ...and must NOT fire on clean ASCII, CRs included. Without this the
    #     control could be a constant true and (1) would still pass.
    printf 'NEVER WRITTEN -- reads: x\n' > "$tmp/ascii"
    enc_has_non_ascii "$tmp/ascii" >/dev/null \
        && { echo "!! enc_has_non_ascii false-positived on pure ASCII"; rc=1; }
    printf 'a\r\nb\r\n' > "$tmp/crlf"
    enc_has_non_ascii "$tmp/crlf" >/dev/null \
        && { echo "!! enc_has_non_ascii treated CR as non-ASCII"; rc=1; }

    # (3) The CRLF trap itself: two strings that PRINT identically must compare
    #     unequal before stripping and equal after. This is the exact shape that
    #     reddened build-windows on bd3ae57.
    local a b
    a="$(printf 'x NEVER-WRITTEN\r\ny WRITTEN-NEVER-READ\r')"
    b="$(printf 'x NEVER-WRITTEN\ny WRITTEN-NEVER-READ')"
    [ "$a" = "$b" ] && { echo "!! CRLF fixture is not reproducing the trap"; rc=1; }
    [ "$(printf '%s' "$a" | enc_strip_cr)" = "$b" ] \
        || { echo "!! enc_strip_cr did not normalise the CRLF fixture"; rc=1; }

    # (4) enc_escaped_diff must actually SHOW the difference -- a renderer that
    #     hides it is worse than none, since it argues the assertion is lying.
    #     Rendering is in-process precisely so this assertion does not depend on
    #     a platform's text tools; see the note on enc_escaped_diff.
    enc_escaped_diff expected "$b" actual "$a" | grep -q '\\r' \
        || { echo "!! enc_escaped_diff did not surface the CR"
             enc_platform_probe
             rc=1; }

    # (5) NEGATIVE CONTROL for (4). A renderer that emitted a literal \r for
    #     every input would satisfy (4) unconditionally, and (4) would then be
    #     asserting nothing. Clean ASCII must render with no \r.
    enc_escaped_diff expected "$b" actual "$b" | grep -q '\\r' \
        && { echo "!! enc_escaped_diff invented a CR on clean input"; rc=1; }

    rm -rf "$tmp"
    return $rc
}

if [ "${BASH_SOURCE[0]}" = "${0}" ] && [ "${1:-}" = "--self-test" ]; then
    if enc_self_test; then echo "encoding_controls self-test: OK"; exit 0
    else echo "encoding_controls self-test: FAILED"; exit 1; fi
fi
