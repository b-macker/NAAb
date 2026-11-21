#!/usr/bin/env bash
# ============================================================
# test_baseline_protection.sh -- the governance baseline is engine state (A196)
#
# THE DEFECT
#
# With governance_baseline enabled, checkGovernanceBaseline() reads the
# baseline file at the end of the run and refuses a regression (exit 2 with
# fail_on_regression). A missing file means "no baseline yet". A60 made the
# scoring-calibration file engine state; the baseline was never added, so the
# governed program could rewrite or delete the counts its own regression check
# reads. Observed on master c1312132: three warnings against a saved baseline
# of one exit 2; the same program writing the baseline file first exits 0.
#
# THE FIX
#
# governance_baseline.path joins the engine-state paths (rule
# governance.state_files, HARD); reads stay allowed. Saving it stays the
# operator's: --governance-baseline-save, outside the program.
#
#   BP-00  CONTROL: three warnings against a saved baseline of one exit 2 --
#          the gate this is about fires
#   BP-01  file.write over the baseline is refused (exit 3,
#          governance.state_files) and the file is unchanged
#   BP-02  with delete granted, file.delete of the baseline is refused and the
#          file is present (the delete control BP-02c runs first)
#   BP-02c CONTROL: an ordinary file.delete works under the same config
#   BP-03  file.read of the baseline is allowed (protection is write-only)
#   BP-04  the absolute path of the baseline is refused too
#   BP-05  with governance_baseline disabled the same path is an ordinary file:
#          the write lands (protection follows what the engine reads)
#   BP-06  embedded Python open(path, "w") is refused; UNMEASURABLE where
#          Python is not embedded
#
# Kill table (mutant applied alone, rebuilt, this suite run; observed
# 2026-11-21 on the default build):
#   M1 the baseline not added to the engine-state paths -> BP-01 BP-02 BP-04 BP-06 red
# On master c1312132: BP-01, BP-02, BP-04, BP-06 red; the controls, BP-03 and
# BP-05 green.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_baseprot_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$TEST_TMP"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

echo "=== test_baseline_protection.sh ==="
if [ ! -x "$NAAB" ]; then echo "  UNMEASURABLE: naab-lang not built"; echo "Result: 0 passed, 0 failed, 1 unmeasurable"; exit 1; fi

BL=".naab/governance-baseline.json"
# cfg ENABLED SANDBOX
cfg() {
    printf '{"mode": "enforce", "security": {"sandbox_level": "%s"},
 "capabilities": {"filesystem": {"mode": "write"}},
 "code_quality": {"no_placeholders": {"enabled": true, "level": "advisory"}},
 "codegen": {"enabled": true, "allowed_languages": ["python"]},
 "governance_baseline": {"enabled": %s, "fail_on_regression": true}}\n' "${2:-elevated}" "$1"
}
# prog WARNINGS PRE_BODY: PRE_BODY first, then WARNINGS placeholder advisories
prog() {
    printf 'use codegen\nuse file\nmain {\n%s\n    for i in 0..%s {\n        codegen.run("python", "# TODO: finish\\nx = 1")\n    }\n    print("DONE")\n}\n' "$2" "$1"
}
# fresh ID ENABLED [SANDBOX]: a project with a baseline of one warning saved by
# the operator. Sets D.
fresh() {
    D="$TEST_TMP/$1"; rm -rf "$D"; mkdir -p "$D/.naab"
    cfg "$2" "${3:-elevated}" > "$D/govern.json"
    prog 1 "" > "$D/t.naab"
    (cd "$D" && "$NAAB" run --governance-baseline-save t.naab >/dev/null 2>&1)
    BASE_SUM=$(cksum < "$D/$BL" 2>/dev/null)
}
run() { RC=0; OUT=$(cd "$D" && timeout 120s "$NAAB" run t.naab 2>&1) || RC=$?; }

# ---- BP-00 control ----------------------------------------------------------
fresh bp00 true
prog 3 "" > "$D/t.naab"; run
if [ -n "$BASE_SUM" ] && [ "$RC" = 2 ] && grep -q 'REGRESSION' <<<"$OUT"; then
    pass BP-00 "CONTROL: three warnings against a baseline of one is a regression (exit 2)"
else
    fail BP-00 "the regression gate did not fire; nothing below can be read" "rc=$RC baseline=${BASE_SUM:-none}"
    echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# ---- BP-01 overwrite ----------------------------------------------------------
fresh bp01 true
prog 3 "    file.write(\"$BL\", \"{}\")" > "$D/t.naab"; run
if [ "$RC" = 3 ] && grep -q 'governance.state_files' <<<"$OUT" && [ "$(cksum < "$D/$BL")" = "$BASE_SUM" ]; then
    pass BP-01 "file.write over the baseline refused (exit 3) and the file is unchanged"
else
    fail BP-01 "the program rewrote its own baseline" "rc=$RC $(grep -m1 -E 'state_files|REGRESSION|DONE' <<<"$OUT")"
fi

# ---- BP-02 delete, with its control ------------------------------------------
fresh bp02 true unrestricted
printf 'use file\nmain {\n    file.write("junk.txt", "x")\n    file.delete("junk.txt")\n    print("DELETED=" + string(file.exists("junk.txt") == false))\n}\n' > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'DELETED=true' <<<"$OUT"; then
    pass BP-02c "CONTROL: an ordinary file.delete works under this config"
    prog 3 "    file.delete(\"$BL\")" > "$D/t.naab"; run
    if [ "$RC" = 3 ] && grep -q 'governance.state_files' <<<"$OUT" && [ -f "$D/$BL" ]; then
        pass BP-02 "file.delete of the baseline refused and the file is present"
    else
        fail BP-02 "the program deleted its own baseline" "rc=$RC present=$([ -f "$D/$BL" ] && echo y || echo n)"
    fi
else
    unm BP-02c "delete is not granted under this config (rc=$RC)"; unm BP-02 "as BP-02c"
fi

# ---- BP-03 read allowed --------------------------------------------------------
fresh bp03 true
printf 'use file\nmain {\n    print("READ=" + string(file.read("%s").length() > 0))\n}\n' "$BL" > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q 'READ=true' <<<"$OUT"; then
    pass BP-03 "file.read of the baseline is allowed"
else
    fail BP-03 "reading the baseline was refused" "rc=$RC $(grep -m1 -E 'READ=|rror' <<<"$OUT")"
fi

# ---- BP-04 absolute path ---------------------------------------------------------
fresh bp04 true
prog 3 "    file.write(\"$D/$BL\", \"{}\")" > "$D/t.naab"; run
if [ "$RC" = 3 ] && [ "$(cksum < "$D/$BL")" = "$BASE_SUM" ]; then
    pass BP-04 "the absolute path of the baseline is refused too"
else
    fail BP-04 "the baseline was written through its absolute path" "rc=$RC"
fi

# ---- BP-05 disabled: an ordinary file ---------------------------------------------
fresh bp05 false
mkdir -p "$D/.naab"; printf 'x\n' > "$D/$BL"
printf 'use file\nmain {\n    file.write("%s", "{}")\n    print("WROTE")\n}\n' "$BL" > "$D/t.naab"; run
if [ "$RC" = 0 ] && grep -q '^WROTE' <<<"$OUT" && [ "$(cat "$D/$BL")" = "{}" ]; then
    pass BP-05 "with governance_baseline disabled the path is an ordinary file"
else
    fail BP-05 "the path was protected though the engine does not read it" "rc=$RC"
fi

# ---- BP-06 embedded Python ---------------------------------------------------------
fresh bp06 true
printf 'main {\n    let r = <<python\nopen("%s", "w").write("{}")\n"WROTE"\n>>\n    print("PY=" + r)\n}\n' "$BL" > "$D/t.naab"; run
if grep -q 'Python support not available' <<<"$OUT"; then
    unm BP-06 "Python is not embedded in this build"
elif [ "$(cksum < "$D/$BL")" = "$BASE_SUM" ] && ! grep -q 'PY=WROTE' <<<"$OUT"; then
    pass BP-06 "embedded Python cannot write the baseline"
else
    fail BP-06 "embedded Python wrote the baseline" "rc=$RC"
fi

echo "Result: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
