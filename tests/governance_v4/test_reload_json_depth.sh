#!/usr/bin/env bash
# ============================================================
# test_reload_json_depth.sh -- a refused mid-run reload changes nothing,
# including the process-wide JSON depth limit (A106)
#
# WHY THIS EXISTS. The loader applies limits.data.max_json_depth to a
# process-wide setting (naab::limits::setMaxJsonDepth) WHILE it parses, and a
# mid-run reload parses the new govern.json before it decides. So a reload the
# ratchet REFUSED still raised the JSON depth limit: observed, a program under
# max_json_depth 4 refused 12-deep JSON, a reload to 1000 was rejected ("Reload
# rejected: ratchet violation(s): limits.data.nesting_depth: 4 -> 1000"), and
# the same json.parse() then succeeded. The hand-written ratchet already
# refused that reload; the refusal did not hold. Found while classifying
# limits.data.max_json_depth for Bundle 8 PR 2.
#
#   JD-00  CONTROL: under max_json_depth 4, 12-deep JSON is refused and 3-deep
#          parses (the limit is live, and the probe can tell the two apart)
#   JD-01  a refused reload (4 -> 1000) is reported as rejected, and 12-deep
#          JSON is STILL refused after it
#   JD-02  CONTROL: an accepted tightening reload (4 -> 1) takes effect: the
#          3-deep JSON that parsed before it is refused after it (the install
#          still applies the accepted file's depth)
#
# Mutants (revert-checked; each turns the arm red):
#   evaluateReload() without the JsonDepthRestore guard  -> JD-01
#   the install without applyInstalledJsonDepth()        -> JD-02
# Red first: JD-01 fails on the binary before the fix (observed, this branch at
# cbee609a: master's reload path plus the declared orders; the setter and the
# parse-before-decide order are master's, unchanged); JD-00 and JD-02 pass on it.
# POSIX only: the mid-run swap needs POSIX rename semantics (config_swap.sh).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo ""
echo "=== A refused reload does not change the JSON depth limit (A106) ==="
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*)
    for id in JD-00 JD-01 JD-02; do skip "$id" "mid-run file swap requires POSIX file semantics"; done
    report; exit 0 ;;
esac
if [ ! -x "$NAAB" ]; then
    skip "JD-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

source "$SCRIPT_DIR/../helpers/config_swap.sh"
W="$(mktemp -d)"
cleanup() { stop_swap_operators; rm -rf "$W"; }
trap cleanup EXIT

cfg() {  # dir depth
    printf '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"},"limits":{"data":{"max_json_depth":%s}}}' "$2" > "$1/govern.json"
}
# prog DIR NEXT_DIR: parse 12-deep and 3-deep JSON, swap in NEXT_DIR's config,
# run a second block (the reload is attempted before every polyglot block),
# parse both again.
prog() {
    cat > "$1/t.naab" <<EOF
use json
main {
    let deep = "[[[[[[[[[[[[1]]]]]]]]]]]]"
    let shallow = "[[[1]]]"
    try { let a = json.parse(deep)
        print("BEFORE_DEEP_OK") } catch (e) { print("BEFORE_DEEP_REFUSED") }
    try { let b = json.parse(shallow)
        print("BEFORE_SHALLOW_OK") } catch (e) { print("BEFORE_SHALLOW_REFUSED") }
    let r1 = <<python
import time, os
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1)
_swap("$2/govern.json", "$1/govern.json")
"SWAPPED"
>>
    print(r1)
    let r2 = <<python
"BLOCK2"
>>
    print(r2)
    try { let c = json.parse(deep)
        print("AFTER_DEEP_OK") } catch (e) { print("AFTER_DEEP_REFUSED") }
    try { let d = json.parse(shallow)
        print("AFTER_SHALLOW_OK") } catch (e) { print("AFTER_SHALLOW_REFUSED") }
}
EOF
}
run() {  # dir -> OUT
    start_swap_operator "$1"
    OUT="$(cd "$1" && "$NAAB" t.naab 2>&1)"
    stop_swap_operators
}

# ---- JD-01: refused reload ---------------------------------------------------
mkdir -p "$W/r1" "$W/r1n"; cfg "$W/r1" 4; cfg "$W/r1n" 1000; prog "$W/r1" "$W/r1n"; run "$W/r1"
R1="$OUT"
# ---- JD-02: accepted tightening ----------------------------------------------
mkdir -p "$W/r2" "$W/r2n"; cfg "$W/r2" 4; cfg "$W/r2n" 1; prog "$W/r2" "$W/r2n"; run "$W/r2"
R2="$OUT"

# ---- JD-00 ---------------------------------------------------------------------
if grep -q BEFORE_DEEP_REFUSED <<<"$R1" && grep -q BEFORE_SHALLOW_OK <<<"$R1"; then
    ok "JD-00" "control: under depth 4, 12-deep JSON is refused and 3-deep parses"
else
    bad "JD-00" "control: the starting depth limit is not what the probe expects -- every arm is UNMEASURABLE" "$R1"
    report; exit 1
fi
if ! grep -q SWAPPED <<<"$R1" || ! grep -q BLOCK2 <<<"$R1"; then
    skip "JD-01" "the swap or the second block did not happen -- UNMEASURABLE"
elif ! grep -q 'Reload rejected' <<<"$R1"; then
    bad "JD-01" "the 4 -> 1000 reload was not rejected -- the arm cannot measure what a refusal leaves behind" "$R1"
elif grep -q AFTER_DEEP_REFUSED <<<"$R1"; then
    ok "JD-01" "a refused reload (4 -> 1000) leaves the JSON depth limit at 4"
else
    bad "JD-01" "after a REFUSED reload, 12-deep JSON parsed: the refused file's depth took effect" "$R1"
fi
if ! grep -q SWAPPED <<<"$R2" || ! grep -q BLOCK2 <<<"$R2"; then
    skip "JD-02" "the swap or the second block did not happen -- UNMEASURABLE"
elif grep -q 'Reload rejected' <<<"$R2"; then
    bad "JD-02" "control: the 4 -> 1 tightening was rejected" "$R2"
elif grep -q BEFORE_SHALLOW_OK <<<"$R2" && grep -q AFTER_SHALLOW_REFUSED <<<"$R2"; then
    ok "JD-02" "control: an accepted tightening (4 -> 1) takes effect (3-deep JSON refused after it)"
else
    bad "JD-02" "control: the accepted tightening did not take effect" "$R2"
fi

report
[ $FAIL -eq 0 ] || exit 1
exit 0
