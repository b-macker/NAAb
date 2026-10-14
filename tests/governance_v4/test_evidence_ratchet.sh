#!/usr/bin/env bash
# ============================================================
# test_evidence_ratchet.sh -- a signed mid-run reload cannot move or switch off
# the evidence the engine records about the run, and an accepted reload keeps
# the CLI's report and telemetry destinations (A33(a))
#
# WHY THIS EXISTS. The reload ratchet's evidence block (356f34c9) compared four
# switches: telemetry.tamper_evidence, decision_snapshots, audit.tamper_evidence
# and transcript.enabled. Everything else about the evidence -- where it goes,
# what is recorded, what is filtered out, the key that seals it -- a signed
# reload could change. Measured before the fix (signed config, signed reload,
# accepted with no violation): telemetry.enabled false left the file at 2 lines
# with no RunEnd (21 with the control reload); a moved output_file split the
# run across two files; audit.level "none" left 1 entry of 4; clearing
# log_events.polyglot_executed left 1 of 3; removing a report path meant no
# report. Separately, ANY accepted reload -- even of the same file -- dropped
# the report and telemetry destinations the CLI lays over govern.json
# (--governance-report/-telemetry, governance.report_* / governance.telemetry),
# because a reload builds its rules from govern.json alone; under --tree-walk
# governance.report_json and governance.telemetry did nothing at all; and a
# telemetry file named by --governance-telemetry was writable by the program
# (#314 protected only the ones govern.json names).
#
# Changes are judged by what is RECORDED (the writers' own predicates), so the
# "accepted" arms are as load-bearing as the refused ones: an entry diff would
# refuse each of them.
#
#   R-00   CONTROL: the swap harness works, an unrelated reload is accepted, and
#          every evidence probe sees its evidence (telemetry RunEnd, audit
#          polyglot entries, the report) -- without it, every "held" assertion
#          below could pass on a harness that never records anything
#   Refused (each: the violation names the key, AND the evidence is intact):
#   EV-01  telemetry.enabled true -> false            (RunEnd still written)
#   EV-02  telemetry.output_file moved                (all in the first file;
#          the message names neither path)
#   EV-03  telemetry.deduplicate_checks false -> true
#   EV-04  audit.level -> none                        (audit entries continue)
#   EV-05  audit.output_file moved
#   EV-06  audit.log_events.polyglot_executed off (timing off too)
#   EV-07  audit.log_events.polyglot_timing off
#   EV-08  audit.log_events.taint_decisions off
#   EV-09  audit.log_events.contract_checks off
#   EV-10  output.file_output.report_json removed     (report still written)
#   EV-11  output.file_output.report_json moved
#   EV-12  audit.provenance.record_attestations off
#   EV-13  audit.provenance.sign_records off
#   EV-14  audit.provenance.signing_key changed
#   EV-15  telemetry.tamper_evidence.hmac_key_env renamed (the message names
#          no variable)
#   EV-16  audit.tamper_evidence.hmac_key changed (the message carries no key)
#   EV-17  telemetry.webhook_url removed              (no URL in the message)
#   EV-18  telemetry.webhook_url redirected
#   EV-19  telemetry.webhook_auth_env renamed
#   EV-20  telemetry.forward_retry_count lowered
#   EV-21  telemetry.forward_buffer_max unbounded -> bounded
#   EV-22  telemetry.forward_timeout_ms lowered
#   EV-23  telemetry.forward_shutdown_drain_ms lowered
#   EV-24  telemetry.transcript.output_file moved
#   EV-25  telemetry.transcript.agents narrowed (all -> one)
#   Accepted (an entry diff would refuse each; the effect is checked):
#   AC-01  telemetry.output_file "tele.jsonl" -> "./tele.jsonl" (same file)
#   AC-02  audit.level basic -> full                   (both record)
#   AC-03  polyglot_executed off while polyglot_timing stays on (still logged)
#   AC-04  transcript.agents [worker] -> [] (every agent: more recorded)
#   AC-05  deduplicate_checks true -> false
#   AC-06  forward_retry_count raised; forward_buffer_max bounded -> unbounded
#   AC-07  audit.level none -> basic: accepted AND the trail starts
#   Startup outputs survive a reload (fail before the fix):
#   OV-01  governance.report_json, VM;  OV-01t the same under --tree-walk
#   OV-02  --governance-report
#   OV-03  --governance-telemetry: the reload is accepted and the file keeps
#          recording (before: refused, the CLI's tamper_evidence looked lost)
#   OV-04  governance.report_json / governance.telemetry work under --tree-walk
#          with no reload at all (the tree-walker ignored both)
#   OV-05  a --governance-telemetry file is refused to the program, both
#          engines; OV-05c another file in the same directory is writable
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Evidence ratchet: a reload cannot move or switch off the run's evidence ==="
if [ ! -x "$NAAB" ]; then
    skip "R-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "R-00" "python3 missing -- UNMEASURABLE"; report; exit 0
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
source "$REPO/tests/helpers/config_swap.sh"
W="$(mktemp -d)"
trap 'stop_swap_operators; teardown_isolated_trust; rm -rf "$W"' EXIT

"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }
cp "$W/k.pem" "$W/attest_a.pem"; cp "$W/k.pem" "$W/attest_b.pem"
export NAAB_ER_KEY_A="er-key-a-$$" NAAB_ER_KEY_B="er-key-b-$$"

HDR='"version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" }'

# reload_run NAME START NEXT [naab args...]
#   START / NEXT: top-level JSON members after the header. The program swaps
#   the config through the external operator, runs a <<sql>> block (the reload
#   is checked before every polyglot block), then three <<python>> blocks --
#   polyglot work recorded AFTER the reload -- and prints REACHED_END.
reload_run() {
    local name="$1" start="$2" next="$3"; shift 3
    local d="$W/$name" n="$W/$name.next"
    mkdir -p "$d" "$n"
    printf '{ %s,\n  %s }\n' "$HDR" "$start" > "$d/govern.json"
    printf '{ %s,\n  %s }\n' "$HDR" "$next" > "$n/govern.json"
    sign_dir "$d"; sign_dir "$n"
    if [ ! -f "$d/govern.json.sig" ] || [ ! -f "$n/govern.json.sig" ]; then OUT="__NOSIGN__"; RC=99; return; fi
    cat > "$d/r.naab" <<EOF
main {
    let s = <<python
import os, time
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
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let t = <<sql
SELECT 1 AS x
>>
    let a1 = <<python
"after_1"
>>
    let a2 = <<python
"after_2"
>>
    print(a1 + a2)
    print("REACHED_END")
}
EOF
    start_swap_operator "$d"
    OUT="$(cd "$d" && timeout 120 "$NAAB" "$@" r.naab 2>&1)"; RC=$?
}
D() { echo "$W/$1"; }
swapped() { [[ "$OUT" == *swapped* ]]; }
rejected_for() { [[ "$OUT" == *"Reload rejected: ratchet violation"* ]] && [[ "$OUT" == *"$1"* ]]; }

# refused ID DESC KEYTEXT [HELD_CMD] [NOT_IN_MESSAGE...]
#   PASS iff the reload was refused naming KEYTEXT, the program ran to the end,
#   HELD_CMD (a shell test of the evidence) succeeds, and none of the
#   NOT_IN_MESSAGE strings appear in the output.
refused() {
    local id="$1" desc="$2" key="$3" held="${4:-true}"; shift 4 2>/dev/null || shift $#
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    if ! rejected_for "$key"; then bad "$id" "$desc -- reload NOT refused for '$key'" "$OUT"; return; fi
    if [[ "$OUT" != *REACHED_END* ]]; then bad "$id" "$desc -- refused, but the program did not finish" "$OUT"; return; fi
    if ! eval "$held"; then bad "$id" "$desc -- refused, but the evidence did not hold ($held)" "$OUT"; return; fi
    # The evidence violation's own text: from its key to the next "; " or line
    # end. Other violations in the same rejection have their own wording (the
    # path-list ratchet names path entries by design, and a signing key file is
    # a protected path -- EV-14 also trips it).
    local s seg="${OUT#*"$key"}"
    seg="${seg%%; *}"; seg="${seg%%$'\n'*}"
    for s in "$@"; do
        if [[ "$seg" == *"$s"* ]]; then bad "$id" "$desc -- the violation names '$s'" "$seg"; return; fi
    done
    ok "$id" "$desc"
}
# accepted ID DESC EFFECT_CMD
accepted() {
    local id="$1" desc="$2" effect="$3"
    if [ "$OUT" = "__NOSIGN__" ]; then skip "$id" "signing unavailable -- UNMEASURABLE"; return; fi
    if ! swapped; then skip "$id" "config swap did not happen -- UNMEASURABLE"; return; fi
    if [[ "$OUT" == *"Reload rejected"* ]]; then bad "$id" "$desc -- the reload was refused" "$OUT"; return; fi
    if [[ "$OUT" != *REACHED_END* ]]; then bad "$id" "$desc -- the program did not finish" "$OUT"; return; fi
    if eval "$effect"; then ok "$id" "$desc"; else bad "$id" "$desc -- effect missing ($effect)" "$OUT"; fi
}

# Evidence probes.
runend()   { grep -q '"RunEnd"' "$1" 2>/dev/null; }                       # a sealed telemetry run
pg_count() { grep -c '"event":"polyglot_executed"' "$1" 2>/dev/null || true; }  # audit polyglot entries
absent()   { [ ! -e "$1" ]; }

TEL='"telemetry": { "enabled": true, "output_file": "tele.jsonl" }'
AUD='"audit": { "level": "basic", "output_file": "audit.jsonl", "log_events": { "polyglot_executed": true } }'
REP='"output": { "file_output": { "report_json": "rep.json" } }'
NOP='"limits": { "timeout": { "global": 600 } }'

# ---- R-00 -------------------------------------------------------------------
reload_run R00 "$TEL, $AUD, $REP" "$TEL, $AUD, $REP, $NOP" --tree-walk
if [ "$OUT" = "__NOSIGN__" ]; then skip "R-00" "signing unavailable -- UNMEASURABLE"
elif ! swapped; then bad "R-00" "harness: the config swap did not happen" "$OUT"
elif [[ "$OUT" == *"Reload rejected"* ]]; then bad "R-00" "harness: an unrelated reload was refused" "$OUT"
elif runend "$(D R00)/tele.jsonl" && [ "$(pg_count "$(D R00)/audit.jsonl")" -ge 3 ] && [ -s "$(D R00)/rep.json" ]; then
    ok "R-00" "control: an unrelated reload is accepted; telemetry sealed, $(pg_count "$(D R00)/audit.jsonl") audit polyglot entries, report written"
else bad "R-00" "control: an evidence probe saw nothing (runend/audit/report)" "$OUT"; fi
R00_PG="$(pg_count "$(D R00)/audit.jsonl")"

echo ""
echo "-- telemetry"
reload_run EV01 "$TEL" '"telemetry": { "enabled": false, "output_file": "tele.jsonl" }'
refused "EV-01" "telemetry.enabled true -> false is refused; the run is still sealed" \
    "telemetry.enabled" 'runend "$(D EV01)/tele.jsonl"'
reload_run EV02 "$TEL" '"telemetry": { "enabled": true, "output_file": "elsewhere.jsonl" }'
refused "EV-02" "telemetry.output_file moved is refused; everything stays in the first file" \
    "telemetry.output_file" 'runend "$(D EV02)/tele.jsonl" && absent "$(D EV02)/elsewhere.jsonl"' \
    "elsewhere.jsonl" "$(D EV02)/tele.jsonl"
reload_run EV03 '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": false }' \
    '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": true }'
refused "EV-03" "telemetry.deduplicate_checks false -> true is refused" \
    "telemetry.deduplicate_checks" 'runend "$(D EV03)/tele.jsonl"'

echo ""
echo "-- audit trail (tree-walker: only it records polyglot_executed, see A37)"
reload_run EV04 "$AUD" '"audit": { "level": "none", "output_file": "audit.jsonl", "log_events": { "polyglot_executed": true } }' --tree-walk
refused "EV-04" "audit.level -> none is refused; entries after the reload are recorded" \
    "audit.level" '[ "$(pg_count "$(D EV04)/audit.jsonl")" -ge "$R00_PG" ]'
reload_run EV05 "$AUD" '"audit": { "level": "basic", "output_file": "audit2.jsonl", "log_events": { "polyglot_executed": true } }' --tree-walk
refused "EV-05" "audit.output_file moved is refused" \
    "audit.output_file" '[ "$(pg_count "$(D EV05)/audit.jsonl")" -ge "$R00_PG" ] && absent "$(D EV05)/audit2.jsonl"' \
    "audit2.jsonl"
reload_run EV06 "$AUD" '"audit": { "level": "basic", "output_file": "audit.jsonl", "log_events": { "polyglot_executed": false } }' --tree-walk
refused "EV-06" "audit.log_events.polyglot_executed off is refused; polyglot entries continue" \
    "audit.log_events.polyglot_executed" '[ "$(pg_count "$(D EV06)/audit.jsonl")" -ge "$R00_PG" ]'
LE() { printf '"audit": { "level": "basic", "output_file": "audit.jsonl", "log_events": { %s } }' "$1"; }
reload_run EV07 "$(LE '"polyglot_executed": true, "polyglot_timing": true')" "$(LE '"polyglot_executed": true, "polyglot_timing": false')"
refused "EV-07" "audit.log_events.polyglot_timing off is refused" "audit.log_events.polyglot_timing"
reload_run EV08 "$(LE '"taint_decisions": true')" "$(LE '"taint_decisions": false')"
refused "EV-08" "audit.log_events.taint_decisions off is refused" "audit.log_events.taint_decisions"
reload_run EV09 "$(LE '"contract_checks": true')" "$(LE '"contract_checks": false')"
refused "EV-09" "audit.log_events.contract_checks off is refused" "audit.log_events.contract_checks"

echo ""
echo "-- reports"
reload_run EV10 "$REP" "$NOP"
refused "EV-10" "output.file_output.report_json removed is refused; the report is written" \
    "output.file_output.report_json" '[ -s "$(D EV10)/rep.json" ]'
reload_run EV11 "$REP" '"output": { "file_output": { "report_json": "rep_other.json" } }'
refused "EV-11" "output.file_output.report_json moved is refused" \
    "output.file_output.report_json" '[ -s "$(D EV11)/rep.json" ] && absent "$(D EV11)/rep_other.json"' "rep_other.json"

echo ""
echo "-- attestations and keys"
PROV() { printf '"audit": { "level": "basic", "output_file": "audit.jsonl", "provenance": { "enabled": true, %s } }' "$1"; }
reload_run EV12 "$(PROV '"record_attestations": true')" "$(PROV '"record_attestations": false')"
refused "EV-12" "audit.provenance.record_attestations off is refused" "audit.provenance.record_attestations"
reload_run EV13 "$(PROV "\"record_attestations\": true, \"sign_records\": true, \"signing_key\": \"$W/attest_a.pem\"")" \
    "$(PROV "\"record_attestations\": true, \"sign_records\": false, \"signing_key\": \"$W/attest_a.pem\"")"
refused "EV-13" "audit.provenance.sign_records off is refused" "audit.provenance.sign_records"
reload_run EV14 "$(PROV "\"record_attestations\": true, \"sign_records\": true, \"signing_key\": \"$W/attest_a.pem\"")" \
    "$(PROV "\"record_attestations\": true, \"sign_records\": true, \"signing_key\": \"$W/attest_b.pem\"")"
refused "EV-14" "audit.provenance.signing_key changed is refused (no path in the message)" \
    "audit.provenance.signing_key" true "attest_a.pem" "attest_b.pem"
TE() { printf '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "tamper_evidence": { "enabled": true, %s } }' "$1"; }
reload_run EV15 "$(TE '"hmac_key_env": "NAAB_ER_KEY_A"')" "$(TE '"hmac_key_env": "NAAB_ER_KEY_B"')"
refused "EV-15" "telemetry.tamper_evidence.hmac_key_env renamed is refused (no variable named, no value)" \
    "telemetry.tamper_evidence.hmac_key" 'runend "$(D EV15)/tele.jsonl"' \
    "NAAB_ER_KEY_A" "NAAB_ER_KEY_B" "$NAAB_ER_KEY_A" "$NAAB_ER_KEY_B"
ATE() { printf '"audit": { "level": "basic", "output_file": "audit.jsonl", "tamper_evidence": { "enabled": true, %s } }' "$1"; }
reload_run EV16 "$(ATE '"hmac_key": "er-literal-one"')" "$(ATE '"hmac_key": "er-literal-two"')"
refused "EV-16" "audit.tamper_evidence.hmac_key changed is refused (the key is not printed)" \
    "audit.tamper_evidence.hmac_key" true "er-literal-one" "er-literal-two"

echo ""
echo "-- off-site forwarding (.invalid collectors: nothing is ever contacted)"
WH() { printf '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "webhook_url": "https://collector-a.invalid/ingest"%s }' "$1"; }
reload_run EV17 "$(WH '')" "$TEL"
refused "EV-17" "telemetry.webhook_url removed is refused (no URL in the message)" \
    "telemetry.webhook_url" true "collector-a.invalid"
reload_run EV18 "$(WH '')" '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "webhook_url": "https://collector-b.invalid/ingest" }'
refused "EV-18" "telemetry.webhook_url redirected is refused" \
    "telemetry.webhook_url" true "collector-a.invalid" "collector-b.invalid"
reload_run EV19 "$(WH ', "webhook_auth_env": "NAAB_ER_KEY_A"')" "$(WH ', "webhook_auth_env": "NAAB_ER_KEY_B"')"
refused "EV-19" "telemetry.webhook_auth_env renamed is refused (no variable named)" \
    "telemetry.webhook_auth_header" true "NAAB_ER_KEY_A" "NAAB_ER_KEY_B"
reload_run EV20 "$(WH ', "forward_retry_count": 3')" "$(WH ', "forward_retry_count": 0')"
refused "EV-20" "telemetry.forward_retry_count lowered is refused" "telemetry.forward_retry_count"
reload_run EV21 "$(WH ', "forward_buffer_max": 0')" "$(WH ', "forward_buffer_max": 100')"
refused "EV-21" "telemetry.forward_buffer_max unbounded -> bounded is refused" "telemetry.forward_buffer_max"
reload_run EV22 "$(WH ', "forward_timeout_ms": 5000')" "$(WH ', "forward_timeout_ms": 100')"
refused "EV-22" "telemetry.forward_timeout_ms lowered is refused" "telemetry.forward_timeout_ms"
reload_run EV23 "$(WH ', "forward_shutdown_drain_ms": 3000')" "$(WH ', "forward_shutdown_drain_ms": 0')"
refused "EV-23" "telemetry.forward_shutdown_drain_ms lowered is refused" "telemetry.forward_shutdown_drain_ms"

echo ""
echo "-- agent transcript"
TR() { printf '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "transcript": { "enabled": true, %s } }' "$1"; }
reload_run EV24 "$(TR '"output_file": "tr.jsonl"')" "$(TR '"output_file": "tr2.jsonl"')"
refused "EV-24" "telemetry.transcript.output_file moved is refused" "telemetry.transcript.output_file" true "tr2.jsonl"
reload_run EV25 "$(TR '"output_file": "tr.jsonl", "agents": []')" "$(TR '"output_file": "tr.jsonl", "agents": ["worker"]')"
refused "EV-25" "telemetry.transcript.agents narrowed (all -> one) is refused" "telemetry.transcript.agents"

echo ""
echo "-- accepted: same evidence, different spelling (an entry diff refuses each)"
reload_run AC01 "$TEL" '"telemetry": { "enabled": true, "output_file": "./tele.jsonl" }'
accepted "AC-01" "telemetry.output_file tele.jsonl -> ./tele.jsonl (the same file) is accepted" \
    'runend "$(D AC01)/tele.jsonl"'
reload_run AC02 "$AUD" '"audit": { "level": "full", "output_file": "audit.jsonl", "log_events": { "polyglot_executed": true } }' --tree-walk
accepted "AC-02" "audit.level basic -> full is accepted (both record)" \
    '[ "$(pg_count "$(D AC02)/audit.jsonl")" -ge "$R00_PG" ]'
reload_run AC03 "$(LE '"polyglot_executed": true, "polyglot_timing": true')" \
    "$(LE '"polyglot_executed": false, "polyglot_timing": true')" --tree-walk
accepted "AC-03" "polyglot_executed off while polyglot_timing stays on is accepted; every run still logged" \
    '[ "$(pg_count "$(D AC03)/audit.jsonl")" -ge "$R00_PG" ] && grep -q "duration=" "$(D AC03)/audit.jsonl"'
reload_run AC04 "$(TR '"output_file": "tr.jsonl", "agents": ["worker"]')" "$(TR '"output_file": "tr.jsonl", "agents": []')"
accepted "AC-04" "transcript.agents [worker] -> [] (every agent) is accepted" true
reload_run AC05 '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": true }' \
    '"telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": false }'
accepted "AC-05" "deduplicate_checks true -> false is accepted" 'runend "$(D AC05)/tele.jsonl"'
reload_run AC06 "$(WH ', "forward_retry_count": 1, "forward_buffer_max": 50')" "$(WH ', "forward_retry_count": 4, "forward_buffer_max": 0')"
accepted "AC-06" "forward_retry_count raised and forward_buffer_max bounded -> unbounded are accepted" true
reload_run AC07 '"audit": { "level": "none", "output_file": "audit.jsonl", "log_events": { "polyglot_executed": true } }' "$AUD" --tree-walk
accepted "AC-07" "audit.level none -> basic is accepted and the trail starts after the reload" \
    '[ "$(pg_count "$(D AC07)/audit.jsonl")" -ge 2 ]'

echo ""
echo "-- startup outputs survive a reload"
GREP='"governance": { "report_json": "grep.json" }'
for eng in vm tw; do
    id="OV-01"; a=(); [ "$eng" = tw ] && { id="OV-01t"; a=(--tree-walk); }
    reload_run "OV01$eng" "$GREP" "$GREP, $NOP" "${a[@]}"
    accepted "$id" "governance.report_json is still written after an accepted reload ($eng)" '[ -s "$(D OV01'"$eng"')/grep.json" ]'
done
reload_run OV02 "$NOP" "$NOP" run --governance-report cli_rep.json   # only after `run`: not in the global pre-scan
accepted "OV-02" "--governance-report is still written after a same-config reload" '[ -s "$(D OV02)/cli_rep.json" ]'
reload_run OV03 "$NOP" "$NOP" --governance-telemetry cli_tel.jsonl
accepted "OV-03" "--governance-telemetry: the reload is accepted and the file is sealed" 'runend "$(D OV03)/cli_tel.jsonl"'

echo ""
echo "-- startup outputs without a reload"
d="$W/OV04"; mkdir -p "$d"
printf '{ %s, "governance": { "report_json": "grep.json", "telemetry": "gtel.jsonl" } }\n' "$HDR" > "$d/govern.json"; sign_dir "$d"
printf 'main {\n    let x = <<sql\nSELECT 1 AS x\n>>\n    print("RAN")\n}\n' > "$d/p.naab"
out="$(cd "$d" && "$NAAB" --tree-walk p.naab 2>&1)"
if [[ "$out" != *RAN* ]]; then bad "OV-04" "the program did not run under --tree-walk" "$out"
elif [ -s "$d/grep.json" ] && runend "$d/gtel.jsonl"; then
    ok "OV-04" "governance.report_json and governance.telemetry work under --tree-walk"
else bad "OV-04" "under --tree-walk: report=$([ -s "$d/grep.json" ] && echo y || echo n) telemetry=$([ -s "$d/gtel.jsonl" ] && echo y || echo n)" "$out"; fi

d="$W/OV05"; mkdir -p "$d"
printf '{ %s }\n' "$HDR" > "$d/govern.json"; sign_dir "$d"
for eng in vm tw; do
    id="OV-05"; a=(); [ "$eng" = tw ] && { id="OV-05t"; a=(--tree-walk); }
    rm -f "$d/cli_tel.jsonl" "$d/other.txt"
    printf 'use file\nmain {\n    file.write("cli_tel.jsonl", "FORGED\\n")\n    print("WROTE")\n}\n' > "$d/w.naab"
    out="$(cd "$d" && "$NAAB" "${a[@]}" --governance-telemetry cli_tel.jsonl w.naab 2>&1)"; rc=$?
    if [ "$rc" = 3 ] && [[ "$out" == *"evidence file is blocked"* ]] && [[ "$out" != *WROTE* ]] &&
       ! grep -q FORGED "$d/cli_tel.jsonl" 2>/dev/null; then
        ok "$id" "a --governance-telemetry file is refused to the program ($eng)"
    else bad "$id" "the program wrote its CLI-named telemetry file ($eng, rc=$rc)" "$out"; fi
done
printf 'use file\nmain {\n    file.write("other.txt", "fine\\n")\n    print("WROTE")\n}\n' > "$d/o.naab"
out="$(cd "$d" && "$NAAB" --governance-telemetry cli_tel.jsonl o.naab 2>&1)"; rc=$?
if [ "$rc" = 0 ] && [[ "$out" == *WROTE* ]] && [ -s "$d/other.txt" ]; then
    ok "OV-05c" "control: another file beside it is writable"
else bad "OV-05c" "the refusal is wider than the telemetry file (rc=$rc)" "$out"; fi

echo ""
report
[ "$FAIL" -eq 0 ]
