#!/usr/bin/env bash
# test_secret_scan_redos.sh — checkSecrets() must render a verdict, never crash.
#
# The private-key pattern in SECRET_PATTERNS was
#   -----BEGIN[\s\S]*PRIVATE KEY-----
# whose greedy [\s\S]* consumes to end of input and then backtracks one position
# at a time. libstdc++ runs that recursively (_Executor::_M_dfs), so any input
# carrying "-----BEGIN" plus roughly 30KB of following text exhausted the stack
# and killed the process with SIGSEGV.
#
# A crash is worse than a false negative here. code_quality.no_secrets is a HARD
# check: crashing renders NO verdict — it neither blocks nor passes, and the
# process dies with a signal rather than the documented exit code 3. It is also
# reachable from untrusted input, because checkSecrets() runs on every agent
# prompt, response and tool argument, so model output could terminate the
# interpreter.
#
# Group A is the crash regression. Group B is the positive control WITHOUT WHICH
# GROUP A PROVES NOTHING: deleting the pattern outright also stops the crash, so
# the suite must fail if real PEM headers stop being detected.

set -uo pipefail
PASS=0
FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built at $NAAB"
    exit 0
fi

cat > "$WORK/govern.json" <<'JSON'
{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":true}}
JSON

# ~40KB of filler, comfortably past the ~30KB stack-exhaustion threshold
mk_filler() {
    python3 -c 'print(("// " + "x"*70 + "\n")*540, end="")'
}

run_case() {
    local name="$1" first_line="$2" expect="$3"
    {
        echo 'main {'
        echo "    let m = \"$first_line\""
        mk_filler
        echo '    print(m)'
        echo '}'
    } > "$WORK/case.naab"
    ( cd "$WORK" && timeout 120 "$NAAB" case.naab >/dev/null 2>&1 )
    local rc=$?
    if [ "$rc" -ge 128 ]; then
        echo "  FAIL [$name] died with signal (exit $rc) — checkSecrets rendered no verdict"
        FAIL=$((FAIL+1))
        return
    fi
    if [ "$rc" != "$expect" ]; then
        echo "  FAIL [$name] exit $rc, expected $expect"
        FAIL=$((FAIL+1))
        return
    fi
    echo "  PASS [$name] exit $rc"
    PASS=$((PASS+1))
}

echo "=== Group A: pathological input must not crash the scanner ==="
# "-----BEGIN" with no "PRIVATE KEY-----" terminator anywhere. This is the shape
# that ran away: nothing to match, so the engine backtracks the whole file.
run_case "A-01 unterminated PEM header + 40KB" \
    "-----BEGIN RSA PRIVATE KEY" 0
# Terminator present but far from the header — greedy matching still walks to EOF
# before coming back for it, so this crashed too even though a match exists.
run_case "A-02 BEGIN marker, terminator absent, long tail" \
    "-----BEGIN CERTIFICATE" 0

echo "=== Group B: positive control — real keys must still be caught ==="
# Without this group, deleting the pattern would make Group A pass.
run_case "B-01 RSA private key header still blocked" \
    "-----BEGIN RSA PRIVATE KEY-----" 3
run_case "B-02 bare private key header still blocked" \
    "-----BEGIN PRIVATE KEY-----" 3
run_case "B-03 OPENSSH private key header still blocked" \
    "-----BEGIN OPENSSH PRIVATE KEY-----" 3

echo "=== Group C: negative control — clean input must still pass ==="
run_case "C-01 no PEM marker at all" \
    "harmless string" 0

echo
echo "secret-scan ReDoS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
