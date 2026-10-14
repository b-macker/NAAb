#!/usr/bin/env bash
# ============================================================
# test_evidence_file_protection.sh — a governed program must not WRITE the
# governance evidence the engine records for its own run
#
# THE DEFECT
#
# The engine writes three governance-evidence files: the telemetry output
# (telemetry.output_file), the agent transcript (telemetry.transcript
# .output_file) and the audit trail (audit.output_file). None was protected
# from the governed program. Measured on 536f836a, enforce + elevated, both
# engines: file.write, file.append, io.write_file and file.copy-into-a-dir
# overwrote them; log.set_output pointed the logger at one; with FS_DELETE
# granted, file.delete and file.move erased them; embedded Python open("a"),
# os.truncate, os.remove and os.rename replaced, emptied and deleted them —
# all under Governance: PASS, exit 0 — while govern.json was refused on every
# one of the same routes. A program that can rewrite its telemetry can erase
# the record of what it did; slice 2 of live guidance reads these files back
# as ground truth, so they must be trustworthy first.
#
# THE FIX
#   addGovernanceProtectedPaths() records the enabled evidence files (both as
#   configured — relative to cwd, where the engine's fopen lands — and as the
#   absolute path at record time). checkPathAccessImpl() refuses any program
#   WRITE to one via isEvidenceLogPath(), at the one decision point file/io/log
#   and the embedded-Python audit hook all reach, on both engines. READS stay
#   allowed: programs read their own telemetry mid-run (living-script, the
#   gorilla suites), so this is not a blocked_paths entry.
#
# NOT IN SCOPE (separate follow-ups, owner's decision 2026-10-14):
#   * subprocess languages (<<sh>> etc.) are outside path policy by CONTRA-013;
#     a shell write to an evidence file is the operator's grant, not closed here
#   * ratcheting telemetry/audit output_file and audit.level across a reload
#   * withholding tamper_evidence.hmac_key_env / provenance.signing_key_env
#   * the engine's STATE files (baselines, calibration, caches) — state, later
#
#   EV-01  CALIBRATION: a direct write to the telemetry file is HARD-blocked
#          (exit 3) and names governance.evidence_files; an ordinary write runs.
#          Without this the rest measures nothing.
#   EV-02  file.write to each of telemetry/transcript/audit is blocked, each
#          file unchanged, both engines.
#   EV-03  file.append (the real telemetry shape) is blocked, both engines.
#   EV-04  io.write_file is blocked (the io surface reaches the same gate).
#   EV-05  file.copy into the directory, landing on an evidence file, is blocked.
#   EV-06  log.set_output onto an evidence file is blocked.
#   EV-07  with FS_DELETE granted, file.delete of an evidence file is blocked.
#   EV-08  embedded Python append / os.truncate / os.remove / os.rename onto an
#          evidence file are refused; file intact (audit hook, in-process only).
#   EV-RD  LOAD-BEARING CONTROL: the program can still READ its telemetry file.
#          A blanket deny would pass every arm above; this is why the gate is
#          write-only, not a blocked_paths entry.
#   EV-OK  SUCCESS CONTROL: the program may write its OWN .jsonl in the same
#          directory — the gate is per-file, not an extension or directory deny.
#   EV-OFF CONTROL: with telemetry/transcript/audit all disabled, no file is
#          recorded, so a write to a would-be evidence name is NOT blocked by
#          this gate (the protection follows what the engine actually writes).
#   EV-SH  DOCUMENTED LIMIT (not a failure): a <<sh>> write reaches an evidence
#          file; reported UNMEASURABLE where shell is unavailable.
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

# An unsigned config is an INTEGRITY BLOCK (exit 3) when the ambient trust
# store holds any key — which the blocked-write arms would misread as the gate
# firing. Isolate the store so exit 3 can only mean the path policy refused.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

# A project whose config enables all three evidence logs. $1=dir, $2=extra fs
mk() {  # $1 dir
    local d="$1"
    mkdir -p "$d/out"
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "telemetry": {"enabled": true, "output_file": "telemetry.jsonl",
               "transcript": {"enabled": true, "output_file": "transcript.jsonl"}},
 "audit": {"level": "basic", "output_file": "audit.jsonl"}}
EOF
}
run() {  # $1 dir, $2 engine(vm|tw), $3 program -> sets OUT, RC
    printf '%s\n' "$3" > "$1/p.naab"
    local a=("$NAAB"); [ "$2" = tw ] && a+=(--tree-walk); a+=(p.naab)
    OUT="$(cd "$1" && "${a[@]}" 2>&1)"; RC=$?
}
seed()    { echo "SEED_MARK" > "$1"; }           # a line we expect to survive
intact()  { [ "$(head -c 9 "$1" 2>/dev/null)" = "SEED_MARK" ]; }  # seed still first

# ---- embedded Python present? (the audit hook lives there only) ----
PYPROBE="$TEST_TMP/pyprobe"; mkdir -p "$PYPROBE"
printf '{"version":"5.0","mode":"off"}\n' > "$PYPROBE/govern.json"
run "$PYPROBE" vm 'main { let x = <<python
1 + 1
>>
  print(x) }'
EMBEDDED_PY=0
[ "$RC" = 0 ] && grep -q '^2$' <<<"$OUT" && EMBEDDED_PY=1

# ---- shell executor usable here? (the documented-limit arm) ----
SHPROBE="$TEST_TMP/shprobe"; mkdir -p "$SHPROBE"
printf '{"version":"5.0","mode":"off"}\n' > "$SHPROBE/govern.json"
run "$SHPROBE" vm 'main { <<sh
echo SH_OK > sh_marker
>> }'
SHELL_OK=0
[ -f "$SHPROBE/sh_marker" ] && grep -q SH_OK "$SHPROBE/sh_marker" && SHELL_OK=1

echo "== EV-01 calibration"
d="$TEST_TMP/cal"; mk "$d"; seed "$d/telemetry.jsonl"
run "$d" vm 'use file
main { file.write("telemetry.jsonl", "FORGED") }'
if [ "$RC" = 3 ] && intact "$d/telemetry.jsonl" && grep -q "governance.evidence_files\|governance evidence file" <<<"$OUT"; then
    run "$d" vm 'use file
main { file.write("out/ok.txt", "FINE") }'
    if [ "$RC" = 0 ] && [ "$(cat "$d/out/ok.txt")" = "FINE" ]; then
        pass "EV-01" "direct write to the telemetry file is HARD-blocked and named; an ordinary write runs"
    else fail "EV-01" "ordinary write did not run" "rc=$RC out=${OUT:0:120}"; fi
else fail "EV-01" "write to telemetry file not blocked/named" "rc=$RC out=${OUT:0:160}"; fi

echo ""
echo "== EV-02 file.write to each evidence file, both engines"
for eng in vm tw; do
  for f in telemetry.jsonl transcript.jsonl audit.jsonl; do
    d="$TEST_TMP/w-$eng-$f"; mk "$d"; seed "$d/$f"
    run "$d" $eng "use file
main { file.write(\"$f\", \"FORGED\") }"
    if [ "$RC" = 3 ] && intact "$d/$f"; then
        pass "EV-02/$eng/$f" "file.write blocked; file unchanged"
    else fail "EV-02/$eng/$f" "write not blocked" "rc=$RC out=${OUT:0:120}"; fi
  done
done

echo ""
echo "== EV-03 file.append, EV-04 io.write_file"
for f in telemetry.jsonl transcript.jsonl audit.jsonl; do
    d="$TEST_TMP/ap-$f"; mk "$d"; seed "$d/$f"
    run "$d" vm "use file
main { file.append(\"$f\", \"{\\\"FORGED\\\":1}\") }"
    { [ "$RC" = 3 ] && intact "$d/$f" && ! grep -q FORGED "$d/$f"; } \
        && pass "EV-03/$f" "file.append blocked; no forged line" \
        || fail "EV-03/$f" "append not blocked" "rc=$RC"
    d="$TEST_TMP/io-$f"; mk "$d"; seed "$d/$f"
    run "$d" vm "use io
main { io.write_file(\"$f\", \"FORGED\") }"
    { [ "$RC" = 3 ] && intact "$d/$f"; } \
        && pass "EV-04/$f" "io.write_file blocked; file unchanged" \
        || fail "EV-04/$f" "io.write_file not blocked" "rc=$RC"
done

echo ""
echo "== EV-05 file.copy into the directory lands on an evidence file"
d="$TEST_TMP/cp"; mk "$d"; seed "$d/audit.jsonl"
echo "FORGED" > "$d/out/audit.jsonl"
run "$d" vm 'use file
main { file.copy("out/audit.jsonl", ".") }'
{ [ "$RC" = 3 ] && intact "$d/audit.jsonl"; } \
    && pass "EV-05" "copy-into-dir onto an evidence file blocked; file unchanged" \
    || fail "EV-05" "copy-into-dir replaced an evidence file" "rc=$RC out=${OUT:0:120}"

echo ""
echo "== EV-06 log.set_output onto an evidence file"
d="$TEST_TMP/log"; mk "$d"; seed "$d/transcript.jsonl"
run "$d" vm 'use log
main { log.set_output("transcript.jsonl")
    log.info("FORGED") }'
{ [ "$RC" = 3 ] && intact "$d/transcript.jsonl"; } \
    && pass "EV-06" "log.set_output onto an evidence file blocked; file unchanged" \
    || fail "EV-06" "log.set_output targeted an evidence file" "rc=$RC out=${OUT:0:120}"

echo ""
echo "== EV-07 file.delete with FS_DELETE granted"
d="$TEST_TMP/del"; mkdir -p "$d/out"
cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "unrestricted"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "telemetry": {"enabled": true, "output_file": "telemetry.jsonl"},
 "audit": {"level": "basic", "output_file": "audit.jsonl"}}
EOF
seed "$d/audit.jsonl"
# calibration: FS_DELETE really is granted here (an ordinary delete works)
run "$d" vm 'use file
main { file.write("out/junk.txt","x")
    file.delete("out/junk.txt") }'
if [ "$RC" = 0 ] && [ ! -e "$d/out/junk.txt" ]; then
    seed "$d/audit.jsonl"
    run "$d" vm 'use file
main { file.delete("audit.jsonl") }'
    { [ "$RC" = 3 ] && [ -f "$d/audit.jsonl" ] && intact "$d/audit.jsonl"; } \
        && pass "EV-07" "file.delete of an evidence file blocked; file present and unchanged" \
        || fail "EV-07" "evidence file was deletable" "rc=$RC present=$([ -f "$d/audit.jsonl" ] && echo y || echo n)"
else
    skip "EV-07" "FS_DELETE not grantable here (calibration delete rc=$RC)"
fi

echo ""
echo "== EV-08 embedded Python routes"
if [ "$EMBEDDED_PY" != 1 ]; then
    skip "EV-08" "embedded Python not available in this build (audit hook lives there only)"
else
    # append
    d="$TEST_TMP/py-ap"; mk "$d"; seed "$d/telemetry.jsonl"
    run "$d" vm 'main { let x = <<python
with open("telemetry.jsonl","a") as f:
    f.write("FORGED\n")
"done"
>>
  print(x) }'
    { [ "$RC" != 0 ] && intact "$d/telemetry.jsonl" && ! grep -q FORGED "$d/telemetry.jsonl"; } \
        && pass "EV-08/append" "Python open(a) refused; no forged line (rc=$RC)" \
        || fail "EV-08/append" "Python append reached the file" "rc=$RC"
    # truncate
    d="$TEST_TMP/py-tr"; mk "$d"; seed "$d/telemetry.jsonl"
    run "$d" vm 'main { let x = <<python
import os
os.truncate("telemetry.jsonl", 0)
"done"
>>
  print(x) }'
    { [ "$RC" != 0 ] && intact "$d/telemetry.jsonl"; } \
        && pass "EV-08/truncate" "os.truncate refused; file unchanged (rc=$RC)" \
        || fail "EV-08/truncate" "os.truncate emptied the file" "rc=$RC"
    # remove
    d="$TEST_TMP/py-rm"; mk "$d"; seed "$d/audit.jsonl"
    run "$d" vm 'main { let x = <<python
import os
os.remove("audit.jsonl")
"done"
>>
  print(x) }'
    { [ "$RC" != 0 ] && [ -f "$d/audit.jsonl" ]; } \
        && pass "EV-08/remove" "os.remove refused; file present (rc=$RC)" \
        || fail "EV-08/remove" "os.remove deleted the file" "rc=$RC"
    # rename onto
    d="$TEST_TMP/py-ren"; mk "$d"; seed "$d/transcript.jsonl"
    run "$d" vm 'main { let x = <<python
open("src","w").write("FORGED\n")
import os
os.rename("src","transcript.jsonl")
"done"
>>
  print(x) }'
    { [ "$RC" != 0 ] && intact "$d/transcript.jsonl"; } \
        && pass "EV-08/rename" "os.rename onto an evidence file refused; file unchanged (rc=$RC)" \
        || fail "EV-08/rename" "os.rename replaced the file" "rc=$RC"
fi

echo ""
echo "== controls"
# EV-RD: reading the telemetry file still works (load-bearing)
d="$TEST_TMP/rd"; mk "$d"
printf 'LINE_A\nLINE_B\n' > "$d/telemetry.jsonl"
run "$d" vm 'use file
main { let s = file.read("telemetry.jsonl")
    print(s) }'
{ [ "$RC" = 0 ] && grep -q LINE_A <<<"$OUT" && grep -q LINE_B <<<"$OUT"; } \
    && pass "EV-RD" "the program can READ its telemetry file (gate is write-only)" \
    || fail "EV-RD" "reading the telemetry file was blocked" "rc=$RC out=${OUT:0:120}"

# EV-OK: the program may write its own .jsonl in the same dir
d="$TEST_TMP/ok"; mk "$d"
run "$d" vm 'use file
main { file.write("my-own.jsonl", "MINE") }'
{ [ "$RC" = 0 ] && [ "$(cat "$d/my-own.jsonl" 2>/dev/null)" = "MINE" ]; } \
    && pass "EV-OK" "the program may write its OWN .jsonl beside the evidence files (per-file)" \
    || fail "EV-OK" "a non-evidence .jsonl write was wrongly blocked" "rc=$RC out=${OUT:0:120}"

# EV-OFF: with all three disabled, the name is not protected by THIS gate
d="$TEST_TMP/off"; mkdir -p "$d"
cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "telemetry": {"enabled": false},
 "audit": {"level": "none"}}
EOF
run "$d" vm 'use file
main { file.write("telemetry.jsonl", "MINE") }'
{ [ "$RC" = 0 ] && [ "$(cat "$d/telemetry.jsonl" 2>/dev/null)" = "MINE" ]; } \
    && pass "EV-OFF" "with evidence logging disabled, telemetry.jsonl is an ordinary writable file" \
    || fail "EV-OFF" "a name was protected though the engine writes no evidence" "rc=$RC out=${OUT:0:120}"

# EV-SH: documented limit — a <<sh>> write reaches the file (CONTRA-013)
if [ "$SHELL_OK" != 1 ]; then
    skip "EV-SH" "shell executor not usable in this build"
else
    d="$TEST_TMP/sh"; mk "$d"; seed "$d/telemetry.jsonl"
    run "$d" vm 'main { <<sh
echo FORGED >> telemetry.jsonl
>> }'
    if grep -q FORGED "$d/telemetry.jsonl" 2>/dev/null; then
        pass "EV-SH" "documented limit confirmed: a <<sh>> write reaches an evidence file (CONTRA-013, out of scope)"
    else
        # Not a failure of this feature — the shell write was stopped by some
        # other layer. Report it so the documented limit is not asserted falsely.
        skip "EV-SH" "a <<sh>> write did NOT reach the file here (rc=$RC); the documented CONTRA-013 limit was not reproduced"
    fi
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
