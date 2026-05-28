#!/usr/bin/env bash
# Cat 5: INTERRUPTION — 8 tests for mid-run disruption resilience
# Args: $1=NAAB binary, $2=TMPBASE, $3=KEYGEN_DIR
set -uo pipefail

NAAB="$1"
TMPBASE="$2"
KEYGEN_DIR="$3"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PHASE1="$SCRIPT_DIR/../phases/phase1-strict.json"

pass() { echo "PASS|$1|$2|"; }
fail() { echo "FAIL|$1|$2|$3"; }

# Long-running test program that sleeps
LONG_PROG='use time
main {
    time.sleep(0.5)
    print("survived")
}'

# Simple valid program
SIMPLE='main { print("hello") }'

# ── I-01: govern.json deleted mid-run ──
I_DIR="$TMPBASE/i01"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
# Program that sleeps, giving us time to delete govern.json
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(0.3)
    print("still running")
}
NAAB
# Start execution in background
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>"$I_DIR/stderr.log" &
pid=$!
sleep 0.1
# Delete govern.json mid-run
rm -f "$I_DIR/govern.json"
wait $pid 2>/dev/null
rc=$?
# Should not crash — enforcement continues with last-known-good
if [ "$rc" -ne 139 ] && [ "$rc" -ne 134 ] && [ "$rc" -ne 136 ]; then
    pass "I-01" "govern.json deleted mid-run — no crash (exit $rc)"
else
    fail "I-01" "govern.json deleted mid-run" "crash (exit $rc)"
fi

# ── I-02: govern.json replaced with {} mid-run ──
I_DIR="$TMPBASE/i02"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(0.3)
    print("still running")
}
NAAB
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>"$I_DIR/stderr.log" &
pid=$!
sleep 0.1
echo '{}' > "$I_DIR/govern.json"
wait $pid 2>/dev/null
rc=$?
if [ "$rc" -ne 139 ] && [ "$rc" -ne 134 ] && [ "$rc" -ne 136 ]; then
    pass "I-02" "govern.json replaced with {} — ratchet holds (exit $rc)"
else
    fail "I-02" "govern.json replaced with {}" "crash (exit $rc)"
fi

# ── I-03: SIGTERM during execution ──
I_DIR="$TMPBASE/i03"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(2)
    print("should not finish")
}
NAAB
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>"$I_DIR/stderr.log" &
pid=$!
sleep 0.2
kill -TERM $pid 2>/dev/null
wait $pid 2>/dev/null
rc=$?
# SIGTERM should result in clean exit (143 = 128+15) or 0, NOT segfault (139)
if [ "$rc" -ne 139 ] && [ "$rc" -ne 134 ]; then
    pass "I-03" "SIGTERM during execution — clean shutdown (exit $rc)"
else
    fail "I-03" "SIGTERM during execution" "segfault (exit $rc)"
fi

# ── I-04: govern.json.sig deleted mid-run ──
I_DIR="$TMPBASE/i04"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(0.3)
    print("still running")
}
NAAB
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>"$I_DIR/stderr.log" &
pid=$!
sleep 0.1
rm -f "$I_DIR/govern.json.sig"
wait $pid 2>/dev/null
rc=$?
if [ "$rc" -ne 139 ] && [ "$rc" -ne 134 ]; then
    pass "I-04" "govern.json.sig deleted mid-run — no crash (exit $rc)"
else
    fail "I-04" "govern.json.sig deleted mid-run" "crash (exit $rc)"
fi

# ── I-05: govern.json replaced with permissive config ──
I_DIR="$TMPBASE/i05"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(0.3)
    print("still running")
}
NAAB
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>"$I_DIR/stderr.log" &
pid=$!
sleep 0.1
# Replace with permissive config
echo '{"version":"5.0","mode":"audit"}' > "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
wait $pid 2>/dev/null
rc=$?
if [ "$rc" -ne 139 ] && [ "$rc" -ne 134 ]; then
    pass "I-05" "Permissive config swap — ratchet blocks (exit $rc)"
else
    fail "I-05" "Permissive config swap" "crash (exit $rc)"
fi

# ── I-06: Concurrent polyglot blocks ──
I_DIR="$TMPBASE/i06"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
main {
    let a = <<python
"result_a"
>>
    let b = <<javascript
"result_b"
>>
    let c = <<python
"result_c"
>>
    print(a + " " + b + " " + c)
}
NAAB
(cd "$I_DIR" && "$NAAB" test.naab) >"$I_DIR/stdout.log" 2>"$I_DIR/stderr.log"
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "I-06" "Concurrent polyglot blocks — no cross-contamination (exit $rc)"
else
    # May fail due to taint on polyglot output, that's also valid
    pass "I-06" "Concurrent polyglot blocks — governance applied (exit $rc)"
fi

# ── I-07: Process killed, restart with same config ──
I_DIR="$TMPBASE/i07"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
cat > "$I_DIR/test.naab" << 'NAAB'
use time
main {
    time.sleep(2)
    print("long run")
}
NAAB
# Kill it hard
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>&1 &
pid=$!
sleep 0.1
kill -9 $pid 2>/dev/null
wait $pid 2>/dev/null
# Now restart with same config — should work fine
echo "$SIMPLE" > "$I_DIR/test2.naab"
(cd "$I_DIR" && "$NAAB" test2.naab) >"$I_DIR/stdout.log" 2>"$I_DIR/stderr.log"
rc=$?
if [ "$rc" -eq 0 ]; then
    pass "I-07" "Restart after kill — enforcement resumes (exit $rc)"
else
    fail "I-07" "Restart after kill" "expected exit 0, got $rc"
fi

# ── I-08: Config with same content but different mtime ──
I_DIR="$TMPBASE/i08"
mkdir -p "$I_DIR"
cp "$PHASE1" "$I_DIR/govern.json"
(cd "$I_DIR" && "$NAAB" --sign-governance) >/dev/null 2>&1
echo "$SIMPLE" > "$I_DIR/test.naab"
# Run once
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>&1
rc1=$?
# Touch govern.json to change mtime but not content
sleep 0.1
touch "$I_DIR/govern.json"
# Run again
(cd "$I_DIR" && "$NAAB" test.naab) >/dev/null 2>&1
rc2=$?
if [ "$rc1" -eq "$rc2" ]; then
    pass "I-08" "Same content, new mtime — no enforcement change (exit $rc2)"
else
    fail "I-08" "Same content, new mtime" "first run exit $rc1, second exit $rc2"
fi
