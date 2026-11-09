#!/usr/bin/env bash
# test_reload_extends.sh -- a reload resolves `extends` as startup does
# (F-04 / F-21 / F-28; Bundle 17)
#
# Startup loads a child config through loadWithExtends(): merged with every
# parent and floored (A98). A mid-run reload -- GovernanceEngine::
# evaluateReload(), the decision `naab-gov ratchet` asks -- parsed the child
# alone, so every setting the child inherits read as removed, and a project
# that uses `extends` could not reload at all. Observed on master 314bccfd: a
# reload of the child by ITSELF was refused with five violations (inherited
# restrictions, languages and audit settings "loosened"); the same rules in one
# flat file were accepted. Gemini's report found it three times (F-04 for
# restrictions, F-21 for audit, F-28 for languages). It failed closed, but it
# made `extends` and live reload exclusive. evaluateReload() now resolves the
# chain before the ratchet compares; a chain that does not load refuses the
# reload (ExtendsRefused) without ending the run.
#
#   RX-00c  CONTROL the flattened rules reload themselves: accepted
#   RX-01   the extends child reloads itself: accepted (was refused)
#   RX-02   the child dropping a key it set itself: refused (the ratchet still
#           compares the child's own settings)
#   RX-03   the child pointed at a looser parent: refused, naming the loosening
#   RX-04   the child pointed at a stricter parent: accepted (a tightening
#           through the chain; master refused it as well)
#   RX-05   a parent that does not exist: refused (extends chain does not load),
#           exit 1, not accepted
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   the chain not resolved at reload (master's code)   RX-01, RX-04, RX-05
#   a chain failure ignored (outcome left Accepted)    RX-05
# Red first (observed, master 314bccfd): RX-01, RX-04 and RX-05 fail; RX-02,
# RX-03 and the control pass (master refuses everything an extends child
# reloads, which is why the two refusal arms cannot tell it apart).
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_rlext_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -10 | sed 's/^/       -> /'; }

if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then echo "UNMEASURABLE: naab-gov not built"; exit 1; fi

mkdir -p "$W/policies"
cat > "$W/policies/base.json" <<'EOF'
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "languages": {"blocked": ["ruby", "php"]},
 "restrictions": {"dangerous_calls": {"enabled": true, "level": "hard"},
                  "shell_injection": {"enabled": true, "level": "hard"}},
 "audit": {"level": "detailed", "output_file": "audit.jsonl", "tamper_evidence": {"enabled": true}}}
EOF
cat > "$W/policies/loose.json" <<'EOF'
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "restrictions": {"dangerous_calls": {"enabled": true, "level": "hard"},
                  "shell_injection": {"enabled": true, "level": "hard"}},
 "audit": {"level": "detailed", "output_file": "audit.jsonl", "tamper_evidence": {"enabled": true}}}
EOF
python3 - "$W" <<'EOF'
import json, sys
w = sys.argv[1]
base = json.load(open(w + "/policies/base.json"))
strict = json.loads(json.dumps(base)); strict["languages"]["blocked"].append("go")
json.dump(strict, open(w + "/policies/strict.json", "w"))
child = {"version": "5.0", "mode": "enforce", "extends": "./policies/base.json",
         "restrictions": {"information_disclosure": {"enabled": True, "level": "soft"}}}
json.dump(child, open(w + "/govern.json", "w"))
dropped = {k: v for k, v in child.items() if k != "restrictions"}
json.dump(dropped, open(w + "/dropped.json", "w"))
for name, parent in [("loose", "./policies/loose.json"), ("strict", "./policies/strict.json"),
                     ("missing", "./policies/nowhere.json")]:
    c = dict(child); c["extends"] = parent
    json.dump(c, open(w + "/" + name + ".json", "w"))
flat = json.loads(json.dumps(base)); flat["restrictions"].update(child["restrictions"])
json.dump(flat, open(w + "/flat.json", "w"))
EOF
ratchet() { RC=0; OUT=$(cd "$W" && timeout 60s "$GOV" ratchet "$1" "$2" --ratchet-only 2>&1) || RC=$?; }

echo "=== a reload resolves extends as startup does ==="

ratchet flat.json flat.json
if [ "$RC" -eq 0 ] && grep -q "ACCEPTED" <<<"$OUT"; then pass RX-00c "CONTROL: the flattened rules reload themselves"
else fail RX-00c "the flat control was refused (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

ratchet govern.json govern.json
if [ "$RC" -eq 0 ] && grep -q "ACCEPTED" <<<"$OUT"; then pass RX-01 "the extends child reloads itself"
else fail RX-01 "an extends child could not reload itself (rc=$RC)" "$OUT"; fi

ratchet govern.json dropped.json
if [ "$RC" -eq 1 ] && grep -q "information_disclosure" <<<"$OUT"; then pass RX-02 "the child dropping its own restriction is refused"
else fail RX-02 "dropping a child's own setting was not refused (rc=$RC)" "$OUT"; fi

ratchet govern.json loose.json
if [ "$RC" -eq 1 ] && grep -qE "ruby|php" <<<"$OUT"; then pass RX-03 "a looser parent is refused, naming the languages it frees"
else fail RX-03 "a looser parent was not refused (rc=$RC)" "$OUT"; fi

ratchet govern.json strict.json
if [ "$RC" -eq 0 ] && grep -q "ACCEPTED" <<<"$OUT"; then pass RX-04 "a stricter parent is accepted"
else fail RX-04 "a stricter parent was refused (rc=$RC)" "$OUT"; fi

ratchet govern.json missing.json
if [ "$RC" -eq 1 ] && grep -q "extends chain does not load" <<<"$OUT"; then pass RX-05 "a parent that does not exist refuses the reload"
else fail RX-05 "a missing parent did not refuse the reload (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
