#!/usr/bin/env bash
# ============================================================
# test_inert_key_sweep.sh — a config key that enforces nothing must not appear
#                           quietly
#
# THE FAILURE THIS CATCHES
#
# `limits.code.max_total_polyglot_lines` is declared in governance.h, parsed by
# the loader, clamped, RATCHETED against mid-run loosening, merged across
# `extends`, written into every generated config by `naab governance init`, and
# shipped at 5000 in both copies of govern-template.json. Nothing reads it. An
# operator setting that limit gets no enforcement and no warning, and the key has
# more plumbing around it than most keys that work — which is exactly why it
# reads as alive.
#
# It is not alone: 23 enforcement-shaped keys are in the same state, including
# trust_policy.check_revocation, trust_policy.require_fresh_signature and the
# limits.memory.* pair.
#
# WHY A BASELINE RATHER THAN AN ASSERTION OF ZERO
#
# Asserting zero would fail on day one and stay failing, which trains people to
# ignore it. The baseline records the known set; the gate fails when the set
# CHANGES. A new inert key is a regression (someone added config surface that
# enforces nothing). A key leaving the list is progress and still fails, so the
# baseline cannot silently drift out of date while claiming to be current.
#
# Not every entry is a defect — `meta.*` configures the loader itself, so its
# only legitimate consumer IS the loader. The baseline header says which is which.
#
# WHY THE HEURISTIC IS LEAF-NAME AND NOT FULL-PATH
#
# Consumers read these through local aliases (`auto& cfg = rules_.circuit_breaker;
# cfg.enabled`), so a full-path grep reports working keys as dead — an earlier
# pass produced 11 false positives that way, every one of them alias reads. Leaf
# name is the conservative direction: it can only UNDER-report, never invent an
# inert key. A leaf that collides with an unrelated identifier drops off the list,
# so this is a floor on the problem, not a measurement of it.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BASELINE="$SCRIPT_DIR/inert_keys_baseline.txt"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "${RED}$3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT+1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Inert config keys: parsed, plumbed, and enforcing nothing    |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

if ! command -v python3 >/dev/null 2>&1; then
    skip "IK-00" "python3 not available"
    echo "  Results: 0 passed, 0 failed, 1 skipped"
    exit 0
fi
if [ ! -f "$BASELINE" ]; then
    fail "IK-00" "baseline file missing" "       expected $BASELINE"
    echo -e "${RED}Failures:${NC}$FAILURES"; exit 1
fi

CURRENT=$(cd "$REPO_ROOT" && python3 - <<'PY'
import io, re, glob
cfg = io.open('src/runtime/governance_config.cpp', encoding='utf-8').read()
assigned = sorted({a for a in re.findall(r'rules_\.([A-Za-z_][A-Za-z0-9_.]*)\s*=(?!=)', cfg)
                   if '.' in a})
SKIP = ('runtime/governance_config.cpp', 'cli/governance_init.cpp', 'naab/governance.h')
blob = ''
for f in (glob.glob('src/**/*.cpp', recursive=True) +
          glob.glob('src/**/*.h', recursive=True) +
          glob.glob('include/**/*.h', recursive=True)):
    if f.replace('\\', '/').endswith(SKIP):
        continue
    try:
        blob += io.open(f, encoding='utf-8', errors='ignore').read()
    except OSError:
        pass
for a in assigned:
    if not re.search(r'\b' + re.escape(a.split('.')[-1]) + r'\b', blob):
        print(a)
PY
)

EXPECTED=$(grep -v '^#' "$BASELINE" | grep -v '^[[:space:]]*$' | sort)
ACTUAL=$(printf '%s\n' "$CURRENT" | grep -v '^[[:space:]]*$' | sort)

# --- IK-01: the sweep produced data (positive control) --------------------
# Two empty sets compare equal. Without a floor, a scan that silently matched
# nothing — a moved file, a renamed field prefix — would pass as "no change".
N_ACTUAL=$(printf '%s\n' "$ACTUAL" | grep -c . || true)
if [ "${N_ACTUAL:-0}" -ge 20 ]; then
    pass "IK-01" "sweep analysed the loader and produced $N_ACTUAL entries"
else
    fail "IK-01" "sweep produced almost nothing ($N_ACTUAL entries)" \
         "       the analysis did not run against the real source; equality below would be vacuous"
fi

# --- IK-02: the inert set has not changed ---------------------------------
if [ "${N_ACTUAL:-0}" -ge 20 ]; then
    ADDED=$(comm -13 <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$ACTUAL"))
    REMOVED=$(comm -23 <(printf '%s\n' "$EXPECTED") <(printf '%s\n' "$ACTUAL"))
    if [ -z "$ADDED" ] && [ -z "$REMOVED" ]; then
        pass "IK-02" "no new config key enforces nothing (${N_ACTUAL} known)"
    else
        DETAIL=""
        [ -n "$ADDED" ]   && DETAIL="${DETAIL}       NEW inert keys (parsed but nothing reads them):\n$(printf '%s\n' "$ADDED" | sed 's/^/         + /')\n"
        [ -n "$ADDED" ]   && DETAIL="${DETAIL}       Wire them up or delete them. Do NOT add them to the baseline to go green.\n"
        [ -n "$REMOVED" ] && DETAIL="${DETAIL}       Now enforced (good) — delete these from the baseline:\n$(printf '%s\n' "$REMOVED" | sed 's/^/         - /')\n"
        fail "IK-02" "the set of unenforced config keys changed" "$(printf '%b' "$DETAIL")"
    fi
else
    skip "IK-02" "sweep produced no usable data (see IK-01)"
fi

echo ""
echo -e "${CYAN}==============================================================${NC}"
echo -e "  Results: ${GREEN}$PASS_COUNT passed${NC}, ${RED}$FAIL_COUNT failed${NC}, ${YELLOW}$SKIP_COUNT skipped${NC}"
echo -e "${CYAN}==============================================================${NC}"
[ "$FAIL_COUNT" -eq 0 ] || { echo -e "${RED}Failures:${NC}$FAILURES"; exit 1; }
exit 0
