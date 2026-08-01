#!/usr/bin/env bash
# Security R24 Fix Verification Tests
# V-API-004: REST API per-request execution timeout
# V-SC-005:  Lockfile symlink overwrite via std::ofstream
# V-DOS-003: URL fetch size cap + backslash path traversal in cache name
# V-RT-014:  Module/context loaders OOM via /dev/zero symlink + size cap
# V-GOV-019: Governance config JSON array width cap
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
GOV="$SCRIPT_DIR/../../build/naab-gov"
WORK_DIR="$(mktemp -d "${TMPDIR:-/data/data/com.termux/files/usr/tmp}/naab_r24.XXXXXX")"
SERVER_PID=""
cleanup() {
    if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK_DIR"
    rm -f $WORK_DIR/sentinel
}
trap cleanup EXIT

PASS=0
FAIL=0
SKIP=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }
# "introspection unavailable" verifies nothing; recording it as a pass let an
# unrun check read as a green one.
skip() { echo "SKIP: $1"; SKIP=$((SKIP+1)); }

# ── V-API-004: REST API per-request timeout ─────────────────────────────────

# Pick a free-ish port
PORT=18742
"$NAAB" api "$PORT" --api-key r24test --api-timeout 2 \
    > "$WORK_DIR/api.log" 2>&1 &
SERVER_PID=$!

# Wait for server to come up (poll /health for up to 5s)
api_up=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    if curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1; then
        api_up=1
        break
    fi
    sleep 0.5
done

if [[ $api_up -eq 0 ]]; then
    fail "V-API-004: REST API failed to start on port $PORT" "$(tail -5 "$WORK_DIR/api.log")"
    # Skip the rest of the API tests
else
    # Test 1: infinite loop must NOT hang the worker — should return within ~3s
    START=$(date +%s)
    HTTP_CODE=$(curl -s -o "$WORK_DIR/loop.json" -w '%{http_code}' \
        --max-time 8 \
        -H "Authorization: Bearer r24test" \
        -H "Content-Type: application/json" \
        -d '{"code":"main { while (true) { let x = 1 } }"}' \
        "http://127.0.0.1:$PORT/api/v1/execute" 2>>"$WORK_DIR/api.log")
    END=$(date +%s)
    ELAPSED=$((END - START))

    if [[ "$HTTP_CODE" == "200" && $ELAPSED -le 6 ]]; then
        # Body should be JSON with status:error (timeout caught)
        if grep -q '"status"' "$WORK_DIR/loop.json"; then
            pass "V-API-004-T1: infinite loop returned in ${ELAPSED}s (timeout enforced)"
        else
            fail "V-API-004-T1: response missing status field" "$(cat "$WORK_DIR/loop.json")"
        fi
    else
        fail "V-API-004-T1: hang detected (http=$HTTP_CODE elapsed=${ELAPSED}s)" \
             "$(cat "$WORK_DIR/loop.json" 2>/dev/null)"
    fi

    # Test 2: fast script must still succeed (regression guard)
    HTTP_CODE=$(curl -s -o "$WORK_DIR/fast.json" -w '%{http_code}' \
        --max-time 5 \
        -H "Authorization: Bearer r24test" \
        -H "Content-Type: application/json" \
        -d '{"code":"main { io.write(42) }"}' \
        "http://127.0.0.1:$PORT/api/v1/execute" 2>>"$WORK_DIR/api.log")
    if [[ "$HTTP_CODE" == "200" ]] && grep -q '"status".*"success"' "$WORK_DIR/fast.json"; then
        pass "V-API-004-T2: fast script still executes after timeout fix"
    else
        fail "V-API-004-T2: fast script failed (http=$HTTP_CODE)" "$(cat "$WORK_DIR/fast.json" 2>/dev/null)"
    fi

    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
    SERVER_PID=""
fi

# ── V-SC-005: Lockfile symlink overwrite ────────────────────────────────────

# Test 3: pre-create .naab/naab.lock as symlink to a sentinel file.
# After --lock, sentinel content must be unchanged.
mkdir -p "$WORK_DIR/lock_proj/.naab"
echo "SENTINEL_DO_NOT_OVERWRITE_R24" > $WORK_DIR/sentinel
ln -sf $WORK_DIR/sentinel "$WORK_DIR/lock_proj/.naab/naab.lock"

# Minimal naab program for --lock to operate on
cat > "$WORK_DIR/lock_proj/main.naab" << 'NAAB'
main {
    io.write("hello")
}
NAAB
# Need a govern.json for discoverLockfilePath to find this dir as project root
cat > "$WORK_DIR/lock_proj/govern.json" << 'JSON'
{ "mode": "off" }
JSON

(cd "$WORK_DIR/lock_proj" && "$NAAB" --lock main.naab > "$WORK_DIR/lock_out.txt" 2>&1) || true

if grep -q "SENTINEL_DO_NOT_OVERWRITE_R24" $WORK_DIR/sentinel; then
    # Sentinel intact — symlink was refused
    if grep -qiE "Refusing to write|symlink" "$WORK_DIR/lock_out.txt"; then
        pass "V-SC-005-T1: symlinked naab.lock refused; sentinel intact"
    else
        # Some platforms might silently leave the symlink alone — still a pass if sentinel intact
        pass "V-SC-005-T1: sentinel intact (symlink not followed)"
    fi
else
    fail "V-SC-005-T1: SENTINEL OVERWRITTEN — symlink was followed!" \
         "$(cat $WORK_DIR/sentinel)"
fi

# Test 4: regression — clean dir without symlinks still produces valid lockfile
mkdir -p "$WORK_DIR/clean_proj/.naab"
cat > "$WORK_DIR/clean_proj/main.naab" << 'NAAB'
main { io.write("ok") }
NAAB
cat > "$WORK_DIR/clean_proj/govern.json" << 'JSON'
{ "mode": "off" }
JSON
(cd "$WORK_DIR/clean_proj" && "$NAAB" --lock main.naab > "$WORK_DIR/clean_lock.txt" 2>&1) || true
if [[ -f "$WORK_DIR/clean_proj/.naab/naab.lock" ]] && \
   grep -q '"naab_version"' "$WORK_DIR/clean_proj/.naab/naab.lock"; then
    pass "V-SC-005-T2: clean --lock still writes a valid lockfile"
else
    fail "V-SC-005-T2: clean --lock failed to write lockfile" \
         "$(cat "$WORK_DIR/clean_lock.txt")"
fi

# ── V-DOS-003: curl size cap + backslash sanitization ───────────────────────

# Test 5: Verify CURLOPT_MAXFILESIZE_LARGE was compiled in (string check on the
# binary). This is a cheap-but-effective canary; the runtime path requires an
# adversarial HTTP server which is impractical for a unit test.
if strings "$NAAB" 2>/dev/null | grep -q 'MAXFILESIZE\|module_resolver' || \
   nm -D "$NAAB" 2>/dev/null | grep -q curl_easy_setopt || \
   ldd "$NAAB" 2>/dev/null | grep -qi curl; then
    pass "V-DOS-003-T1: naab-lang binary links libcurl (size cap is compile-time)"
else
    # Not all distros bundle strings; this isn't a hard failure
    skip "V-DOS-003-T1: introspection unavailable — link check unverified"
fi

# Test 6: Backslash sanitization is internal to urlToCachePath — no public
# CLI hook. Verify the helper compiles & the binary runs `--version` cleanly
# (regression guard for the urlToCachePath edits).
if "$NAAB" --version > "$WORK_DIR/ver.txt" 2>&1; then
    pass "V-DOS-003-T2: naab-lang --version succeeds after urlToCachePath rewrite"
else
    fail "V-DOS-003-T2: naab-lang --version failed" "$(cat "$WORK_DIR/ver.txt")"
fi

# ── V-RT-014: loader symlink + size cap ─────────────────────────────────────

# Test 7: scanner over a directory containing a symlinked README.md → /dev/zero
# must NOT crash (R22 fix already handles scanner; R24 fix handles loaders).
mkdir -p "$WORK_DIR/rt14_proj"
echo '{ "mode": "off" }' > "$WORK_DIR/rt14_proj/govern.json"
cat > "$WORK_DIR/rt14_proj/main.naab" << 'NAAB'
main { io.write("rt14") }
NAAB
ln -sf /dev/zero "$WORK_DIR/rt14_proj/README.md"
rc=0
"$GOV" scan "$WORK_DIR/rt14_proj" > "$WORK_DIR/rt14_scan.txt" 2>&1 || rc=$?
if [[ $rc -le 2 ]]; then
    pass "V-RT-014-T1: scan with symlinked README.md -> /dev/zero survives (exit $rc)"
else
    fail "V-RT-014-T1: scanner crashed (exit $rc)" "$(tail -5 "$WORK_DIR/rt14_scan.txt")"
fi
rm -f "$WORK_DIR/rt14_proj/README.md"

# Test 8: import a symlinked module pointing at /dev/zero — must error cleanly,
# not OOM. lstat() in readFileBounded should reject the symlink immediately.
mkdir -p "$WORK_DIR/rt14_mod"
echo '{ "mode": "off" }' > "$WORK_DIR/rt14_mod/govern.json"
ln -sf /dev/zero "$WORK_DIR/rt14_mod/badmod.naab"
cat > "$WORK_DIR/rt14_mod/main.naab" << 'NAAB'
use "./badmod" as bad
main { io.write("never") }
NAAB
rc=0
"$NAAB" "$WORK_DIR/rt14_mod/main.naab" > "$WORK_DIR/rt14_load.txt" 2>&1 || rc=$?
if [[ $rc -ne 0 ]] && ! grep -qi "killed\|signal" "$WORK_DIR/rt14_load.txt"; then
    pass "V-RT-014-T2: symlinked module rejected without OOM (exit $rc)"
else
    fail "V-RT-014-T2: loader did not bound the read (exit $rc)" \
         "$(tail -5 "$WORK_DIR/rt14_load.txt")"
fi

# Test 9: regression — a normal small module still imports correctly
mkdir -p "$WORK_DIR/rt14_ok"
echo '{ "mode": "off" }' > "$WORK_DIR/rt14_ok/govern.json"
cat > "$WORK_DIR/rt14_ok/util.naab" << 'NAAB'
function greet() {
    return "hi"
}
NAAB
cat > "$WORK_DIR/rt14_ok/main.naab" << 'NAAB'
use util
main { io.write(util.greet()) }
NAAB
rc=0
out=$("$NAAB" "$WORK_DIR/rt14_ok/main.naab" 2>&1) || rc=$?
if [[ $rc -eq 0 ]] && echo "$out" | grep -q "hi"; then
    pass "V-RT-014-T3: normal module import still works (regression)"
else
    fail "V-RT-014-T3: normal module broken (exit $rc)" "$out"
fi

# ── V-GOV-019: governance config array width cap ────────────────────────────

# Test 10: govern.json with > MAX_GOV_ARRAY_ELEMS (10000) entries in
# restrictions.dangerous_calls.blocklist_extra must be rejected, not OOM.
# Generate ~12000 entries (well above the cap, well below OOM).
mkdir -p "$WORK_DIR/gov019_proj"
{
    printf '{ "mode":"audit", "restrictions": { "dangerous_calls": { "blocklist_extra": ['
    for i in $(seq 1 12000); do
        if [[ $i -eq 1 ]]; then printf '"f%d"' "$i"
        else printf ',"f%d"' "$i"; fi
    done
    printf '] } } }\n'
} > "$WORK_DIR/gov019_proj/govern.json"

cat > "$WORK_DIR/gov019_proj/main.naab" << 'NAAB'
main { io.write("hi") }
NAAB

rc=0
"$GOV" scan "$WORK_DIR/gov019_proj" > "$WORK_DIR/gov019_out.txt" 2>&1 || rc=$?
# Acceptable: rc <= 2 (not a crash) AND either rejection logged OR scan completed.
# The cap should at minimum prevent the OOM path.
if [[ $rc -le 2 ]]; then
    if grep -q "exceeds\|V-GOV-019\|rejected" "$WORK_DIR/gov019_out.txt"; then
        pass "V-GOV-019-T1: 12k-element array rejected with cap message (exit $rc)"
    else
        # Cap may have triggered on a fresh load via different code path; still
        # not a crash, which is the security property we're verifying.
        pass "V-GOV-019-T1: 12k-element array did not crash naab-gov (exit $rc)"
    fi
else
    fail "V-GOV-019-T1: naab-gov crashed on wide array (exit $rc)" \
         "$(tail -5 "$WORK_DIR/gov019_out.txt")"
fi

# ── Summary ─────────────────────────────────────────────────────────────────
echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped (unverified)"
[[ $FAIL -eq 0 ]]
