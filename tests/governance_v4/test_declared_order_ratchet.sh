#!/usr/bin/env bash
# ============================================================
# test_declared_order_ratchet.sh -- every key the reload ratchet did not
# compare now has a declared order, and the engine enforces it
#
# WHY THIS EXISTS. test_ratchet_coverage.sh pinned 773 govern.json keys that no
# part of the reload ratchet compared (Bundle 8): a signed mid-run reload could
# move each in either direction with no violation and no notice. Bundle 8 PR 2
# classified every one in ratchet_classification.txt; the `order`, `present`
# and `fixed` lines are compiled into the engine (ratchet_declared_orders.inc)
# and evaluateReload() refuses a reload that moves one the loosening way
# (checkDeclaredOrderRatchet). This suite asks the ENGINE, through
# `naab-gov ratchet OLD NEW --ratchet-only` (the reload decision itself), not
# the table.
#
#   DO-00  harness: naab-gov answers, and an identical reload is accepted
#   DO-01  tools/ratchet_orders_probe.py over EVERY declared key: for each, an
#          unchanged reload is accepted (the control that the pair loads), the
#          looser value is refused and names the key, the tighter value is
#          accepted, a `fixed` key's change is refused, and a key with no
#          declared default is refused when removed. 0 FAIL, 0 UNMEASURABLE.
#   DO-02  NEGATIVE CONTROL: the probe judges the engine, not the table -- a
#          copy of the table with one order reversed makes the probe FAIL that
#          key (the engine still enforces the real order)
#   Reproductions, each red on the pre-PR-2 binary (master 76c871cf), each
#   beside a control that the same key accepts a tightening:
#   RB-01  agent_dispatch.hard_stop.max_calls_per_run 10 -> 0 (0 = unlimited)
#          refused; 10 -> 5 accepted                         (run budgets)
#   LA-01  limits.array_size 100 -> 0 refused; 100 -> 50 accepted. The key
#          writes the flat max_array_size checkArrayGrowth() enforces, while
#          the ratchet compared only limits.data.array_size  (limits alias)
#   CD-01  context_drift.enabled true -> false refused; false -> true accepted
#   PO-01  restrictions.polyglot_output.format json -> any refused; any -> json
#          accepted (the hand-written ratchet compared neither form of the key)
#   A103   codegen.max_code_lines 50 -> 0 refused (0 = no cap; the hand-written
#          ratchet read 0 as the tightest); 50 -> 40 accepted. A103b:
#          codegen.timeout_seconds 10 -> 0 refused (0 falls back to 15 s);
#          20 -> 0 accepted
#   PR-01  a presence-enabled check block (code_quality.no_dead_code) removed
#          is refused; PR-01c added is accepted (the old check was off, so
#          nothing below it is compared); PR-02 its shorthand "hard" ->
#          "advisory" is refused
#   AS-01  CONTROL: adding a section that holds no declared key that changed
#          (limits.timeout.global) is accepted -- the over-refusal the first
#          version of the walker had (test_capability_list_ratchet.sh R-00)
#   SH-01  CONTROL: a bare-form "network": true -> false is accepted -- a
#          shorthand refuses only the keys it sets (test_capability_mirrors.sh
#          CM-06b)
#   EV-01  an environment overlay's quality_gate.enabled "true" -> "false",
#          under --env dev, is refused; EV-01c "false" -> "true" accepted
#   MS-01  a refusal names the key and the direction, not the values
#
# Mutants (revert-checked by hand; each turns the arm red):
#   checkDeclaredOrderRatchet not called from evaluateReload -> DO-01, RB, LA,
#       CD, PO, PR-01, EV (EV's parsed comparison is separate: drop the
#       quality_gate chkRestrict -> EV-01)
#   LowerZero compared as Lower                    -> RB-01, LA-01, DO-01
#   the present-block skip removed                 -> PR-01c, DO-01
#   the shorthand-target rule removed (refuse all) -> SH-01
#   the absent-section rule restored (refuse)      -> AS-01, PR-01c
#   chkCodegenCap reverted to chkCodegenLimit      -> A103
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${NAAB_BUILD:-$REPO/build}"
GOV="${NAAB_GOV:-$BUILD/naab-gov}"
PROBE="$REPO/tools/ratchet_orders_probe.py"
TABLE="$SCRIPT_DIR/ratchet_classification.txt"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | head -14 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo ""
echo "=== Declared orders: the reload refuses what the table says loosens ==="
if [ ! -x "$GOV" ]; then
    skip "DO-00" "naab-gov not built ($GOV) -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "DO-00" "python3 missing -- UNMEASURABLE"; report; exit 0
fi

C="$(mktemp -d)"; trap 'rm -rf "$C"' EXIT
unset NAAB_GOVERN_KEY NAAB_SIGNING_KEY
cfg() { printf '%s' "$2" > "$C/$1"; }
B='"version":"3.0","mode":"enforce"'
# ratchet OLD NEW [args]: RC, OUT. NEW gets a later mtime, as a reload sees it.
ratchet() {
    local o="$1" n="$2"; shift 2
    touch -d '2026-01-01 00:00:00' "$C/$o" 2>/dev/null || true
    RC=0; OUT=$(cd "$C" && "$GOV" ratchet "$o" "$n" --ratchet-only "$@" 2>&1) || RC=$?
}
names() { grep -q -F -- "$1" <<<"$OUT"; }
# pair ID DESC OLD_JSON LOOSE_JSON TIGHT_JSON KEY [args]: LOOSE refused naming
# KEY, TIGHT accepted (the control), OLD -> OLD accepted (harness).
pair() {
    local id="$1" desc="$2" old="$3" loose="$4" tight="$5" key="$6"; shift 6
    cfg "$id.old.json" "$old"; cfg "$id.loose.json" "$loose"; cfg "$id.tight.json" "$tight"
    ratchet "$id.old.json" "$id.old.json" "$@"; local s_rc=$RC
    ratchet "$id.old.json" "$id.loose.json" "$@"; local l_rc=$RC l_out="$OUT"
    ratchet "$id.old.json" "$id.tight.json" "$@"; local t_rc=$RC t_out="$OUT"
    if [ $s_rc -ne 0 ]; then
        skip "$id" "$desc -- the unchanged reload exited $s_rc, so the pair does not load -- UNMEASURABLE"
    elif [ $l_rc -ne 1 ] || ! grep -q -F -- "$key" <<<"$l_out"; then
        bad "$id" "$desc -- the loosening exited $l_rc (want 1, naming $key)" "$l_out"
    elif [ $t_rc -ne 0 ]; then
        bad "$id" "$desc -- control: the tightening exited $t_rc (want 0)" "$t_out"
    else
        ok "$id" "$desc (control: the tightening is accepted)"
    fi
}

# ---- DO-00 -------------------------------------------------------------------
cfg same.json "{$B}"
ratchet same.json same.json
if [ $RC -eq 0 ] && names "ACCEPTED"; then ok "DO-00" "harness: an identical reload is accepted"
else bad "DO-00" "harness: an identical reload exited $RC" "$OUT"; report; exit 1; fi

# ---- DO-01: every declared key through the engine ---------------------------
POUT="$(python3 "$PROBE" --naab-gov "$GOV" 2>&1)"; PRC=$?
keys="$(sed -n 's/^keys probed: //p' <<<"$POUT")"
arms="$(sed -n 's/^arms: //p' <<<"$POUT")"
if [ -z "$keys" ] || [ "${keys:-0}" -lt 100 ]; then
    bad "DO-01" "the probe probed ${keys:-no} keys -- broken probe, not a result" "$POUT"
elif [ $PRC -eq 0 ]; then
    ok "DO-01" "every declared key holds its order through the reload decision ($keys keys; $arms)"
else
    bad "DO-01" "declared keys the engine does not hold ($arms)" "$(grep -E '^(FAIL|UNMEASURABLE) ' <<<"$POUT")"
fi

# ---- DO-02: the probe can fail ------------------------------------------------
sed 's/^\(agent_dispatch\.hard_stop\.max_calls_per_run[[:space:]]*\)order lower0/\1order higher/' "$TABLE" > "$C/flipped.txt"
if cmp -s "$C/flipped.txt" "$TABLE"; then
    bad "DO-02" "control could not plant: the flip matched no table line" ""
else
    FOUT="$(python3 "$PROBE" --naab-gov "$GOV" --table "$C/flipped.txt" --only '^agent_dispatch\.hard_stop\.max_calls_per_run$' 2>&1)"
    if grep -q '^key FAIL agent_dispatch.hard_stop.max_calls_per_run' <<<"$FOUT"; then
        ok "DO-02" "control: a reversed order in the table makes the probe FAIL that key (it judges the engine)"
    else
        bad "DO-02" "control failed: the probe passed a key whose declared order was reversed" "$FOUT"
    fi
fi

# ---- Reproductions --------------------------------------------------------------
pair RB-01 "run budget: max_calls_per_run 10 -> 0 (unlimited) is refused" \
    "{$B,\"agent_dispatch\":{\"hard_stop\":{\"max_calls_per_run\":10}}}" \
    "{$B,\"agent_dispatch\":{\"hard_stop\":{\"max_calls_per_run\":0}}}" \
    "{$B,\"agent_dispatch\":{\"hard_stop\":{\"max_calls_per_run\":5}}}" \
    "agent_dispatch.hard_stop.max_calls_per_run"
pair LA-01 "limits alias: limits.array_size 100 -> 0 (no limit) is refused" \
    "{$B,\"limits\":{\"array_size\":100}}" \
    "{$B,\"limits\":{\"array_size\":0}}" \
    "{$B,\"limits\":{\"array_size\":50}}" \
    "limits.array_size"
# both ID DESC TIGHT_JSON LOOSE_JSON KEY: TIGHT -> LOOSE refused naming KEY,
# LOOSE -> TIGHT accepted (the control). For a key whose tighter value is an
# end of its range, where `pair`'s further tightening does not exist.
both() {
    local id="$1" desc="$2" tight="$3" loose="$4" key="$5"
    cfg "$id.t.json" "$tight"; cfg "$id.l.json" "$loose"
    ratchet "$id.t.json" "$id.t.json"; local s_rc=$RC
    ratchet "$id.t.json" "$id.l.json"; local l_rc=$RC l_out="$OUT"
    ratchet "$id.l.json" "$id.t.json"; local t_rc=$RC t_out="$OUT"
    if [ $s_rc -ne 0 ]; then
        skip "$id" "$desc -- the unchanged reload exited $s_rc -- UNMEASURABLE"
    elif [ $l_rc -ne 1 ] || ! grep -q -F -- "$key" <<<"$l_out"; then
        bad "$id" "$desc -- the loosening exited $l_rc (want 1, naming $key)" "$l_out"
    elif [ $t_rc -ne 0 ]; then
        bad "$id" "$desc -- control: the reverse (a tightening) exited $t_rc (want 0)" "$t_out"
    else
        ok "$id" "$desc (control: the reverse is accepted)"
    fi
}
both CD-01 "context_drift.enabled true -> false is refused" \
    "{$B,\"context_drift\":{\"enabled\":true}}" \
    "{$B,\"context_drift\":{\"enabled\":false}}" \
    "context_drift.enabled"
both PO-01 "restrictions.polyglot_output.format json -> any is refused" \
    "{$B,\"restrictions\":{\"polyglot_output\":{\"format\":\"json\"}}}" \
    "{$B,\"restrictions\":{\"polyglot_output\":{\"format\":\"any\"}}}" \
    "restrictions.polyglot_output.format"
pair A103 "codegen.max_code_lines 50 -> 0 (no cap) is refused" \
    "{$B,\"codegen\":{\"enabled\":true,\"max_code_lines\":50}}" \
    "{$B,\"codegen\":{\"enabled\":true,\"max_code_lines\":0}}" \
    "{$B,\"codegen\":{\"enabled\":true,\"max_code_lines\":40}}" \
    "codegen.max_code_lines"
pair A103b "codegen.timeout_seconds 10 -> 0 (the 15 s fallback) is refused" \
    "{$B,\"codegen\":{\"enabled\":true,\"timeout_seconds\":10}}" \
    "{$B,\"codegen\":{\"enabled\":true,\"timeout_seconds\":0}}" \
    "{$B,\"codegen\":{\"enabled\":true,\"timeout_seconds\":5}}" \
    "codegen.timeout_seconds"
# A103b's other half: 20 -> 0 is 20 s -> 15 s, a tightening.
cfg t20.json "{$B,\"codegen\":{\"enabled\":true,\"timeout_seconds\":20}}"
cfg t0.json "{$B,\"codegen\":{\"enabled\":true,\"timeout_seconds\":0}}"
ratchet t20.json t0.json
if [ $RC -eq 0 ]; then ok "A103c" "codegen.timeout_seconds 20 -> 0 (15 s) is accepted as a tightening"
else bad "A103c" "codegen.timeout_seconds 20 -> 0 exited $RC (0 means the 15 s fallback, tighter than 20)" "$OUT"; fi

# ---- PR: presence-enabled blocks -------------------------------------------------
cfg pr_on.json "{$B,\"code_quality\":{\"no_dead_code\":{\"level\":\"hard\"}}}"
cfg pr_off.json "{$B}"
cfg pr_adv.json "{$B,\"code_quality\":{\"no_dead_code\":{\"level\":\"advisory\",\"patterns\":[\"x\"]}}}"
ratchet pr_on.json pr_off.json
if [ $RC -eq 1 ] && names "code_quality.no_dead_code"; then ok "PR-01" "a presence-enabled check block removed mid-run is refused"
else bad "PR-01" "removing code_quality.no_dead_code exited $RC (want 1, naming it)" "$OUT"; fi
ratchet pr_off.json pr_adv.json
if [ $RC -eq 0 ]; then ok "PR-01c" "control: the block added (any configuration of a check that was off) is accepted"
else bad "PR-01c" "adding code_quality.no_dead_code exited $RC (want 0)" "$OUT"; fi
cfg sh_hard.json "{$B,\"code_quality\":{\"no_dead_code\":\"hard\"}}"
cfg sh_adv.json "{$B,\"code_quality\":{\"no_dead_code\":\"advisory\"}}"
ratchet sh_hard.json sh_adv.json
if [ $RC -eq 1 ] && names "code_quality.no_dead_code"; then ok "PR-02" "its shorthand \"hard\" -> \"advisory\" is refused"
else bad "PR-02" "shorthand hard -> advisory exited $RC (want 1)" "$OUT"; fi

# ---- AS / SH: the walker's controls -------------------------------------------------
cfg as_old.json "{$B}"
cfg as_new.json "{$B,\"limits\":{\"timeout\":{\"global\":600}}}"
ratchet as_old.json as_new.json
if [ $RC -eq 0 ]; then ok "AS-01" "control: adding a section whose declared keys stay absent is accepted"
else bad "AS-01" "adding limits.timeout.global exited $RC (want 0)" "$OUT"; fi
cfg sh_net_on.json "{$B,\"capabilities\":{\"network\":true}}"
cfg sh_net_off.json "{$B,\"capabilities\":{\"network\":false}}"
ratchet sh_net_on.json sh_net_off.json
if [ $RC -eq 0 ]; then ok "SH-01" "control: bare \"network\": true -> false is accepted (a shorthand refuses only the keys it sets)"
else bad "SH-01" "bare network true -> false exited $RC (want 0)" "$OUT"; fi

# ---- EV: an environment's overlay is compared on the overlaid rules ---------------
cfg ev_on.json "{$B,\"quality_gate\":{\"enabled\":false},\"environments\":{\"dev\":{\"quality_gate.enabled\":\"true\"}}}"
cfg ev_off.json "{$B,\"quality_gate\":{\"enabled\":false},\"environments\":{\"dev\":{\"quality_gate.enabled\":\"false\"}}}"
ratchet ev_on.json ev_off.json --env dev
if [ $RC -eq 1 ] && names "quality_gate.enabled"; then ok "EV-01" "under --env dev, the environment's quality_gate.enabled true -> false is refused"
else bad "EV-01" "the environment's quality_gate true -> false exited $RC (want 1)" "$OUT"; fi
ratchet ev_off.json ev_on.json --env dev
if [ $RC -eq 0 ]; then ok "EV-01c" "control: the environment's false -> true is accepted"
else bad "EV-01c" "the environment's quality_gate false -> true exited $RC (want 0)" "$OUT"; fi

# ---- MS-01: the message names the key and the direction, not values -----------------
cfg ms_old.json "{$B,\"agent_dispatch\":{\"hard_stop\":{\"max_tokens_per_run\":123457}}}"
cfg ms_new.json "{$B,\"agent_dispatch\":{\"hard_stop\":{\"max_tokens_per_run\":987653}}}"
ratchet ms_old.json ms_new.json
if [ $RC -ne 1 ]; then bad "MS-01" "the probe loosening exited $RC (want 1)" "$OUT"
elif grep -q -E '123457|987653' <<<"$OUT"; then bad "MS-01" "the refusal prints a configured value" "$OUT"
elif names "agent_dispatch.hard_stop.max_tokens_per_run: loosened"; then ok "MS-01" "a refusal names the key and the direction only"
else bad "MS-01" "the refusal does not name the key and direction" "$OUT"; fi

report
[ $FAIL -eq 0 ] || exit 1
exit 0
