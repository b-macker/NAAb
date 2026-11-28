#!/usr/bin/env bash
# ============================================================
# test_rationale_sections.sh -- every section that reads a `rationale` gives
# it to the rules refused under it (A236; Bundle 21 PR 3)
#
# THE DEFECT (observed on master d55ddbf8)
#
# The loader reads a `rationale` in 62 sections. A refusal, the JSON report,
# SARIF, the audit trail and the dashboard find a rule's rationale through
# lookupRationale(), a hand-written map from rule name to section that knew 46
# exact names and two wrong prefixes (`contract.` for the `contracts.` rules,
# `custom.` for `custom_rules.`). A rationale written under
# capabilities.env_vars, capabilities.shell's blocked commands, contracts,
# custom_rules, manifest, behavioral_sequences (and each pattern),
# temporal_coupling, drift_detection or restrictions.obfuscation reached none
# of them: a refusal under capabilities.env_vars.blocked_read,
# contracts.work.must_call or custom_rules.no_frob carried no rationale in the
# JSON report, while code_quality.no_placeholders' did -- the one the existing
# rationale suite (tests/cli/test_rationale_reports.sh) checks.
#
# THE FIX
#
# One table, GovernanceEngine::ruleSectionInfo(), keyed by each section's
# path and matched on whole path segments, the longest winning; entries of a
# list or map (a contract, a custom rule, a plugin rule, a BSD pattern) by
# their name. lookupRationale() and naab-gov context both read it.
#
#   RS-00  CONTROL: the resolver reads the loader and finds the known sections
#   RS-01  the resolver's sections and rationale_sections.psv are the same list
#          (a section that starts reading a rationale needs a row)
#   RS-R   one arm per row: with `rationale` set at PATH, the engine returns it
#          for RULE (naab-gov context RULE --json)
#   RS-02  CONTROL: with no rationale written, none is returned
#   RS-03  a rationale naming a variable that holds a key is withheld
#   RS-04  a secret-shaped rationale is withheld
#   RS-05  a PII-shaped rationale is withheld
#   RS-06  a rationale naming a configured sanitizer is withheld
#   RS-06c CONTROL: plain text beside the same config is shown
#   RS-07  a rule whose name only begins with a section's name (codegenx) is
#          not that section's; codegen.run is (its control)
#
# KILL TABLE (each mutant applied alone, naab-lang and naab-gov rebuilt, this
# suite run; observed 2026-11-28 on the default build; the arms that turned red):
#   K1  the capabilities.env_vars row removed   -> RS-R capabilities.env_vars (both rows),
#                                                  RS-02 RS-03..RS-06c (they use that rule)
#   K2  paths matched without the segment boundary -> RS-07
#   K3  the drift_detection alias row removed   -> RS-R code_quality.drift_detection
#   K4  the per-pattern BSD lookup removed      -> RS-R behavioral_sequences.patterns[]
#   K5  the key-variable filter removed         -> RS-03
#   K6  the ungated secret matcher replaced by secretFinding() (gated)
#                                               -> RS-04
#   K7  the sanitizer filter removed            -> RS-06
#   K8  "contract" for "contracts" (the old prefix) -> RS-R contracts.functions.* (both rows)
#   K9  a custom rule's id not matched          -> RS-R custom_rules[] (both rows)
#   C4  naab-gov context shows the rationale unfiltered -> RS-03 RS-04 RS-05 RS-06
# The resolver is checked by RS-00 and by RS-01 (a removed psv row fails it).
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
GOV="${NAAB_GOV:-${1:-$REPO/build/naab-gov}}"
PSV="$SCRIPT_DIR/rationale_sections.psv"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT
source "$REPO/tests/helpers/trust_setup.sh"; isolate_trust_store

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
# A CR in a FAIL line is shown as \r (a rule name read with its CR, 2026-11-28).
fail() { local d="${3:-}"; d="${d//$'\r'/\\r}"; FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] ${2//$'\r'/\\r}${d:+ -- $d}"; }

echo "=== rationale sections (A236) ==="
if [ ! -x "$GOV" ]; then
    echo "  FAIL [RS-00] naab-gov not built at $GOV -- UNMEASURABLE, not a pass"
    exit 1
fi

# ------------------------------------------------------------------
# RS-00 / RS-01 -- the loader's parse sites against the table
# ------------------------------------------------------------------
RES="$TEST_TMP/resolved.txt"
python3 -I "$REPO/tools/rationale_sections.py" "$REPO/src/runtime/governance_config.cpp" > "$RES" 2>&1
tr -d '\r' < "$RES" > "$RES.lf" && mv "$RES.lf" "$RES"
n_res=$(grep -c . "$RES" || true)
if [ "$n_res" -ge 50 ] && grep -qx "capabilities.network" "$RES" && grep -qx "code_quality.no_temporary_code" "$RES" \
   && grep -qx "custom_rules\[\]" "$RES" && grep -qx "codegen" "$RES"; then
    pass "RS-00" "CONTROL: the resolver finds $n_res sections, the known ones among them"
else
    fail "RS-00" "the resolver did not read the loader -- every arm below measures nothing" "$(head -3 "$RES" | tr '\n' ' ')"
    exit 1
fi
# A Windows checkout writes the table with CRLF and a native python3 prints
# CRLF: every line is read with its CR removed, or each name carries one.
tr -d '\r' < "$PSV" | grep -v '^#' | grep . | cut -d'|' -f1 | sort -u > "$TEST_TMP/psv_paths.txt"
tr -d '\r' < "$RES" | sort -u > "$TEST_TMP/res_sorted.txt"
missing=$(comm -23 "$TEST_TMP/res_sorted.txt" "$TEST_TMP/psv_paths.txt" | tr '\n' ' ')
extra=$(comm -13 "$TEST_TMP/res_sorted.txt" "$TEST_TMP/psv_paths.txt" | tr '\n' ' ')
if [ -z "$missing" ] && [ -z "$extra" ]; then
    pass "RS-01" "rationale_sections.psv names every section the loader reads a rationale in, and no other"
else
    fail "RS-01" "the table and the loader disagree" "no row: [${missing}] no parse site: [${extra}]"
fi

# ------------------------------------------------------------------
# RS-R -- one arm per row
# ------------------------------------------------------------------
# write_config DIR PATH MARKER [extra-json]
write_config() {
    python3 -I - "$1" "$2" "$3" "${4:-{\}}" <<'PY'
import json, os, sys
d, path, marker, extra = sys.argv[1], sys.argv[2], sys.argv[3], json.loads(sys.argv[4])
cfg = {"version": "3.0", "mode": "enforce"}
def put(obj, keys, leaf):
    for k in keys[:-1]:
        obj = obj.setdefault(k, {})
    obj.setdefault(keys[-1], {}).update(leaf)
leaf = {"rationale": marker} if marker else {}
if path == "custom_rules[]":
    cfg["custom_rules"] = [dict({"id": "r1", "pattern": "zzq_never_matches_zzq", "language": "naab",
                                 "level": "advisory", "message": "m"}, **leaf)]
elif path == "governance_plugins[].rules[]":
    open(os.path.join(d, "plugin.naab"), "w").write("fn check_x(code) {\n    return {\"pass\": true}\n}\n")
    cfg["governance_plugins"] = [{"file": "plugin.naab",
                                  "rules": [dict({"id": "p1", "function": "check_x", "trigger": "function"}, **leaf)]}]
elif path == "behavioral_sequences.patterns[]":
    cfg["behavioral_sequences"] = {"patterns": [dict({"name": "p1", "sequence": ["file.read", "process.exec"]}, **leaf)]}
elif path == "contracts.functions.*":
    cfg["contracts"] = {"functions": {"f": leaf}}
else:
    put(cfg, path.split("."), leaf)
for k, v in extra.items():
    if isinstance(v, dict) and isinstance(cfg.get(k), dict):
        cfg[k].update(v)
    else:
        cfg[k] = v
open(os.path.join(d, "govern.json"), "w").write(json.dumps(cfg, indent=1))
PY
}
# rationale_of DIR RULE -> the rationale (or "null"), and the withheld reason
rationale_of() {
    (cd "$1" && "$GOV" context "rule:$2" --config govern.json --json 2>"$1/err.txt") > "$1/out.json"
    python3 -I - "$1/out.json" <<'PY'
import json, sys
try:
    j = json.load(open(sys.argv[1], "rb"))
    r = j["rules"][0]
    print(r.get("rationale") if r.get("rationale") is not None else "null", r.get("rationale_withheld", "-"))
except Exception as e:
    print("ERROR", type(e).__name__)
PY
}

row=0; row_fail=0
while IFS='|' read -r path rule; do
    [ -z "$path" ] && continue
    case "$path" in \#*) continue;; esac
    row=$((row + 1))
    d="$TEST_TMP/row$row"; mkdir -p "$d"
    marker="RATMARK_${row}_Q"
    write_config "$d" "$path" "$marker"
    got=$(rationale_of "$d" "$rule")
    if [ "${got%% *}" = "$marker" ]; then
        pass "RS-R" "$path ($rule)"
    else
        row_fail=$((row_fail + 1))
        fail "RS-R" "$path ($rule): the section's rationale is not returned for the rule" "got [$got] $(head -c 200 "$d/err.txt")"
    fi
done < <(tr -d '\r' < "$PSV")
echo "  ($row rows)"

# ------------------------------------------------------------------
# RS-02 -- no rationale written, none returned
# ------------------------------------------------------------------
d="$TEST_TMP/rs02"; mkdir -p "$d"
write_config "$d" "capabilities.env_vars" ""
got=$(rationale_of "$d" "capabilities.env_vars.blocked_read")
if [ "${got%% *}" = "null" ]; then
    pass "RS-02" "CONTROL: no rationale written, none returned"
else
    fail "RS-02" "a rationale was returned that the config does not hold" "$got"
fi

# ------------------------------------------------------------------
# RS-03..RS-06 -- what a rationale may not show
# ------------------------------------------------------------------
KEYVAR="NAAB_RS_CHAIN_KEY_VAR"
withheld_arm() {  # ID TEXT EXPECTED-REASON DESCRIPTION
    local d="$TEST_TMP/$1"; mkdir -p "$d"
    write_config "$d" "capabilities.env_vars" "$2" \
        "{\"telemetry\": {\"enabled\": true, \"output_file\": \"t.jsonl\", \"tamper_evidence\": {\"enabled\": true, \"hmac_key_env\": \"$KEYVAR\"}}, \"taint_tracking\": {\"sanitizers\": [\"scrub_rs_\"]}}"
    local got; got=$(rationale_of "$d" "capabilities.env_vars.blocked_read")
    if [ "$got" = "null $3" ]; then
        pass "$1" "$4: withheld ($3)"
    else
        fail "$1" "$4: shown, or withheld for another reason" "$got"
    fi
}
withheld_arm "RS-03" "the chain key is in $KEYVAR, see ops" "key_variable" "a rationale naming a variable that holds a key"
SECRET="AK""IA""QWERTYUIOPASDFGH"
withheld_arm "RS-04" "rotate $SECRET quarterly" "secret" "a secret-shaped rationale"
PII="$((100 + 23))-$((40 + 5))-$((6000 + 789))"
withheld_arm "RS-05" "ask the owner, ssn $PII" "pii" "a PII-shaped rationale"
withheld_arm "RS-06" "call scrub_rs_input first" "sanitizer" "a rationale naming a configured sanitizer"
d="$TEST_TMP/rs06c"; mkdir -p "$d"
write_config "$d" "capabilities.env_vars" "plain reason for this rule" \
    "{\"telemetry\": {\"enabled\": true, \"output_file\": \"t.jsonl\", \"tamper_evidence\": {\"enabled\": true, \"hmac_key_env\": \"$KEYVAR\"}}, \"taint_tracking\": {\"sanitizers\": [\"scrub_rs_\"]}}"
got=$(rationale_of "$d" "capabilities.env_vars.blocked_read")
if [ "$got" = "plain reason for this rule -" ]; then
    pass "RS-06c" "CONTROL: plain text beside the same key variable and sanitizer is shown"
else
    fail "RS-06c" "plain text was not shown -- the withheld arms measure nothing" "$got"
fi

# ------------------------------------------------------------------
# RS-07 -- a section claims whole path segments only
# ------------------------------------------------------------------
d="$TEST_TMP/rs07"; mkdir -p "$d"
write_config "$d" "codegen" "RS07_MARK"
(cd "$d" && "$GOV" context "rule:codegenx.run" --config govern.json > out.txt 2>&1; echo $? > rc)
(cd "$d" && "$GOV" context "rule:codegen.run" --config govern.json --json > ok.json 2>&1)
if [ "$(cat "$d/rc")" = 2 ] && grep -q "no section of govern.json governs" "$d/out.txt" && grep -qF "RS07_MARK" "$d/ok.json"; then
    pass "RS-07" "a rule whose name only begins with a section's name is not that section's (codegenx is not codegen)"
else
    fail "RS-07" "a section claimed a rule by a partial name" "rc=$(cat "$d/rc") $(head -c 160 "$d/out.txt")"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
