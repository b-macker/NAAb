#!/usr/bin/env bash
# ============================================================
# test_sandbox_relative_path.sh — the `standard` sandbox must judge a bare
# relative name for a file that does not exist yet by where it would land
# (docs/open-investigations.md A73)
#
# THE FAILURE THIS CATCHES
#
# Sandbox::normalizePath() falls back to weakly_canonical() when realpath()
# fails (the file does not exist). weakly_canonical() returns a relative path
# UNCHANGED when its first component does not exist, so "out.txt" stayed
# relative while the sandbox's allowed paths (the working directory and the
# temp directory, createEnterpriseConfig() in main.cpp) are absolute, and no
# prefix matched. Observed before the fix (USER_GUIDE rewrite, 2026-10-25), in
# a project with filesystem.mode "write" at `standard` (the enforce default):
# file.write("out.txt") and file.exists("missing.txt") stopped the program
# with "SANDBOX VIOLATION ... capability required" (exit 1), while the same
# calls on "data/out.txt" -- an existing first component -- succeeded. The
# governance gate had the same defect and lost it (checkPathAccessImpl(),
# tests/security/test_relative_new_path.sh); that suite runs at `elevated`,
# so it never reached this one.
#
# The child gets its own TMPDIR beside the project, so the temp-directory
# allowance does not cover the project's parent and only the working
# directory is allowed. The C arms are the controls the fix must keep: a
# relative path that leaves the working directory, and an absolute one
# outside it, are still refused by the sandbox.
#
#   SR-01  file.write of a bare new name succeeds and the file holds the text
#   SR-02  file.exists of a bare missing name is false, not a sandbox stop
#   SR-03  file.append creates a bare new name
#   SR-04  io.write_file of a bare new name succeeds (a second module route)
#   SR-05  a new name under a directory that does not exist yet is not a
#          sandbox violation. What happens instead depends on the platform:
#          under a sandbox on POSIX, file.write opens with O_NOFOLLOW and does
#          not create the directory ("Failed to open file for writing"); on
#          Windows that branch is compiled out and FIX 26 creates it. Either
#          is accepted with its own evidence; neither is the sandbox's call
#          (A76(e) records the difference)
#   SR-C1  CONTROL: "../outside.txt" is still a sandbox violation
#   SR-C2  CONTROL: an absolute path outside the project is still refused
#   SR-C3  CONTROL: "data/out.txt" (existing directory) keeps working
# Each arm runs on both engines.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-sbrel-XXXXXX")"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
P="$W/proj"
mkdir -p "$P/data" "$W/childtmp"

# enforce mode with no sandbox_level: the CLI upgrades to `standard`.
echo '{"version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"write"}}}' > "$P/govern.json"

# run ID ENGINE_FLAG BODY -> OUT, RC; files the previous arm left are removed.
run() {
    rm -f "$P"/out.txt "$P"/new.log "$P"/io_out.txt "$P"/data/out.txt "$W"/outside.txt "$W"/abs_outside.txt
    rm -rf "$P/nodir"
    printf 'use file\nuse io\nmain {\n%s\n}\n' "$3" > "$P/t.naab"
    OUT=$(cd "$P" && TMPDIR="$W/childtmp" timeout 60 "$NAAB" $2 t.naab 2>&1); RC=$?
}

# The harness must reach the sandbox at all: with the sandbox raised to
# elevated the same refusal must vanish, or the C arms prove nothing.
for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"

    run SR-01 "$eng" '    file.write("out.txt", "hello")
    print("WROTE")'
    if [ "$RC" = 0 ] && grep -qx 'WROTE' <<<"$OUT" && [ "$(cat "$P/out.txt" 2>/dev/null)" = "hello" ]; then
        ok "SR-01/$tag" "file.write(\"out.txt\") writes at standard"
    else
        bad "SR-01/$tag" "file.write(\"out.txt\") should succeed" "rc=$RC $(grep -m1 -E 'VIOLATION|Error' <<<"$OUT")"
    fi

    run SR-02 "$eng" '    print("EXISTS=" + string(file.exists("missing.txt")))'
    if [ "$RC" = 0 ] && grep -qx 'EXISTS=false' <<<"$OUT"; then
        ok "SR-02/$tag" "file.exists(\"missing.txt\") is false"
    else
        bad "SR-02/$tag" "file.exists(\"missing.txt\") should be false" "rc=$RC $(grep -m1 -E 'VIOLATION|Error|EXISTS' <<<"$OUT")"
    fi

    run SR-03 "$eng" '    file.append("new.log", "line\n")
    print("APPENDED")'
    if [ "$RC" = 0 ] && grep -qx 'APPENDED' <<<"$OUT" && [ -f "$P/new.log" ]; then
        ok "SR-03/$tag" "file.append(\"new.log\") creates the file"
    else
        bad "SR-03/$tag" "file.append(\"new.log\") should succeed" "rc=$RC $(grep -m1 -E 'VIOLATION|Error' <<<"$OUT")"
    fi

    run SR-04 "$eng" '    io.write_file("io_out.txt", "via io")
    print("IO_WROTE")'
    if [ "$RC" = 0 ] && grep -qx 'IO_WROTE' <<<"$OUT" && [ -f "$P/io_out.txt" ]; then
        ok "SR-04/$tag" "io.write_file(\"io_out.txt\") writes"
    else
        bad "SR-04/$tag" "io.write_file(\"io_out.txt\") should succeed" "rc=$RC $(grep -m1 -E 'VIOLATION|Error' <<<"$OUT")"
    fi

    run SR-05 "$eng" '    try {
        file.write("nodir/out.txt", "x")
        print("WROTE")
    } catch (e) {
        print("CAUGHT: " + string(e))
    }
    print("END")'
    sr05=""
    if grep -q 'SANDBOX VIOLATION' <<<"$OUT"; then
        sr05="the sandbox refused it"
    elif [ "$RC" != 0 ] || ! grep -qx 'END' <<<"$OUT"; then
        sr05="the program did not run to its end (rc=$RC)"
    elif grep -qx 'WROTE' <<<"$OUT" && [ "$(cat "$P/nodir/out.txt" 2>/dev/null)" = "x" ]; then
        ok "SR-05/$tag" "nodir/out.txt: not a sandbox violation; the directory was created and the file written"
    elif grep -q '^CAUGHT: .*Failed to open file for writing: nodir/out.txt' <<<"$OUT" && [ ! -e "$P/nodir" ]; then
        ok "SR-05/$tag" "nodir/out.txt: not a sandbox violation; the filesystem refused it (no such directory)"
    else
        sr05="neither the file was written nor the filesystem's refusal printed: $(grep -m1 -E 'WROTE|CAUGHT' <<<"$OUT")"
    fi
    # The outcome goes in the FAIL line itself: the suite summary quotes only
    # lines matching fail/error/..., which the detail line need not.
    [ -z "$sr05" ] || bad "SR-05/$tag" "nodir/out.txt: expected no sandbox violation, got: $sr05"

    run SR-C1 "$eng" '    file.write("../outside.txt", "x")
    print("WROTE")'
    if [ "$RC" != 0 ] && grep -q 'SANDBOX VIOLATION' <<<"$OUT" && [ ! -e "$W/outside.txt" ]; then
        ok "SR-C1/$tag" "control: ../outside.txt is still a sandbox violation"
    else
        bad "SR-C1/$tag" "../outside.txt should be refused by the sandbox" "rc=$RC exists=$([ -e "$W/outside.txt" ] && echo yes || echo no) $(grep -m1 -E 'VIOLATION|Error|WROTE' <<<"$OUT")"
    fi

    run SR-C2 "$eng" "    file.write(\"$W/abs_outside.txt\", \"x\")
    print(\"WROTE\")"
    if [ "$RC" != 0 ] && grep -q 'SANDBOX VIOLATION' <<<"$OUT" && [ ! -e "$W/abs_outside.txt" ]; then
        ok "SR-C2/$tag" "control: an absolute path outside the project is still refused"
    else
        bad "SR-C2/$tag" "an absolute path outside the project should be refused" "rc=$RC $(grep -m1 -E 'VIOLATION|Error|WROTE' <<<"$OUT")"
    fi

    run SR-C3 "$eng" '    file.write("data/out.txt", "x")
    print("WROTE")'
    if [ "$RC" = 0 ] && grep -qx 'WROTE' <<<"$OUT" && [ -f "$P/data/out.txt" ]; then
        ok "SR-C3/$tag" "control: data/out.txt (existing directory) still writes"
    else
        bad "SR-C3/$tag" "data/out.txt should write" "rc=$RC $(grep -m1 -E 'VIOLATION|Error' <<<"$OUT")"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
