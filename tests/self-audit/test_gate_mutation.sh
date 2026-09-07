#!/usr/bin/env bash
# ============================================================
# test_gate_mutation.sh — can each governance gate actually block anything?
#
# THE QUESTION. The engine has 172 enforce() call sites across ~64 distinct
# rule names. A gate that no test exercises is indistinguishable, from CI, from
# a gate that cannot fire at all — and this campaign has repeatedly found the
# second kind: signals inert by construction, BSD patterns dead under the
# tree-walker, checks gated on state nothing writes. Every one of those was
# found by SAMPLING that population by hand. This enumerates it instead.
#
# THE METHOD is the canary test's, pointed at the engine rather than the
# prescan: neuter one gate, rebuild, and see whether anything notices.
#
#   nothing fails  -> UNPROTECTED. No test in the suite distinguishes this gate
#                     working from this gate absent. That is not proof the gate
#                     is inert, but it IS proof CI would not tell you if it were.
#   something fails -> PROTECTED, and the failing test's name is the evidence.
#
# WHY THE MUTATION IS INJECTED, NOT A FLAG. A runtime switch that disables an
# arbitrary gate by name is a governance backdoor, and shipping one in the
# engine this repo exists to harden would be indefensible whatever the guard on
# it. So the probe is INJECTED into the source, compiled, and reverted — it
# exists only between this script's own mutate and revert steps, exactly like
# test_prescan_canaries.sh injects defects into src/. The EXIT trap reverts
# unconditionally and the run verifies the tree is clean before it reports.
#
# WHY A MENTION IS NOT COVERAGE. Candidate tests are ordered by whether they
# mention the rule name, because running a likely test first is much cheaper
# than the full suite. That is an ORDERING heuristic and never a verdict: a
# test that names a rule may not exercise it, so a gate is only PROTECTED when
# a test actually FAILS, and only UNPROTECTED after the full suite has run.
# Trace to the point of effect, not the point of mention.
#
# COST, measured on this machine: 125s per incremental rebuild, one rebuild per
# gate. A full sweep is hours, so it is nightly/manual, not a per-commit gate.
# The ledger makes it resumable — each run does as many gates as you ask for
# and the coverage accumulates.
#
# KNOWN BLIND SPOT, and it produces FALSE UNPROTECTED verdicts, so read it
# before trusting one. The probe is a modification to src/, and
# run-all-tests.sh skips test_prescan_canaries.sh whenever src/ or include/ is
# dirty (run-all-tests.sh:2444, because the canary injects there itself). So
# during a probe the canary never runs, and a gate whose ONLY witness is a
# canary assertion will be reported UNPROTECTED when it is not. This is
# structural — the probe cannot both exist and leave src/ clean — so it is
# documented rather than fixed. Confirm any UNPROTECTED verdict by hand against
# tests/self-audit/test_prescan_canaries.sh before acting on it.
#
# The lock below is shared with the canary for the same reason: both edit
# tracked source and both rebuild into one build directory, so running them
# concurrently races on object files and can attribute one script's failure to
# the other's mutation. Found by doing exactly that.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

ENGINE="src/runtime/governance_engine.cpp"
LEDGER="tests/self-audit/gate_mutation_ledger.txt"
MARKER="NAAB_MUTATION_PROBE"
BIN="build/naab-lang"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

# Serialize: this edits tracked source and rebuilds. Two runs would see each
# other's injection and attribute the wrong verdict to the wrong gate.
# Shared with test_prescan_canaries.sh: both inject into tracked source and
# both rebuild into build/, so they must never overlap.
LOCKFILE="${TMPDIR:-/tmp}/naab_prescan_canary.lock"
exec 9>"$LOCKFILE"
if ! flock -n 9; then echo "another source-injecting run holds the lock — waiting"; flock 9; fi

BACKUP="$(mktemp "${TMPDIR:-/tmp}/engine-XXXXXX.cpp")"
REVERTED=0
revert_source() {
    [ "$REVERTED" = "1" ] && return
    cp "$BACKUP" "$ENGINE" 2>/dev/null && REVERTED=1
}
cleanup() {
    revert_source
    if grep -q "$MARKER" "$ENGINE" 2>/dev/null; then
        echo -e "${RED}!! $ENGINE STILL CONTAINS THE PROBE — revert by hand before committing${NC}" >&2
    fi
    rm -f "$BACKUP"
}
trap cleanup EXIT INT TERM

# Refuse to start on a dirty engine: a pre-existing edit would be reverted away
# by this script's own cleanup, destroying work that is not ours.
if ! git diff --quiet -- "$ENGINE" 2>/dev/null; then
    echo -e "${RED}$ENGINE has uncommitted changes — commit or stash first.${NC}"
    echo "This script rewrites that file and reverts it on exit; it will not risk your edits."
    exit 1
fi
cp "$ENGINE" "$BACKUP"

# --- gate enumeration -------------------------------------------------------
list_gates() {
    grep -rhoE 'enforce\("[a-z_.]+"' src/runtime/*.cpp src/stdlib/*.cpp 2>/dev/null \
        | sed 's/enforce("//; s/"$//' | grep -vE '\.$' | sort -u
}

MODE="sample"; N=1; ONE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --gate) ONE="$2"; MODE="one"; shift 2;;
        --sample) N="$2"; MODE="sample"; shift 2;;
        --all) MODE="all"; shift;;
        --list) MODE="list"; shift;;
        *) echo "usage: $0 [--gate NAME | --sample N | --all | --list]"; exit 2;;
    esac
done

mkdir -p "$(dirname "$LEDGER")"; touch "$LEDGER"
ledger_has() { grep -q "^$1 " "$LEDGER" 2>/dev/null; }

if [ "$MODE" = "list" ]; then
    echo ""
    echo -e "${CYAN}Gate mutation coverage${NC}"
    tot=0; done_n=0; unprot=0
    while read -r g; do
        tot=$((tot+1))
        if ledger_has "$g"; then
            done_n=$((done_n+1))
            v=$(grep "^$g " "$LEDGER" | tail -1 | awk '{print $2}')
            [ "$v" = "UNPROTECTED" ] && { unprot=$((unprot+1)); printf "  %-46s %b\n" "$g" "${RED}UNPROTECTED${NC}"; }
        else
            printf "  %-46s %b\n" "$g" "${YELLOW}not yet probed${NC}"
        fi
    done < <(list_gates)
    echo ""
    echo "  $done_n of $tot gates probed; $unprot UNPROTECTED"
    exit 0
fi

case "$MODE" in
    one)    TARGETS="$ONE";;
    all)    TARGETS="$(list_gates)";;
    sample) TARGETS="$(list_gates | while read -r g; do ledger_has "$g" || echo "$g"; done | head -"$N")";;
esac
[ -z "$TARGETS" ] && { echo "no gates selected (all probed already? try --all or --list)"; exit 0; }

# --- mutation ---------------------------------------------------------------
# Injects a single early-return keyed to ONE rule name, immediately after
# enforce()'s parameter list. Re-targeting is a re-injection, so the sweep costs
# one rebuild per gate rather than two.
inject() {
    local rule="$1"
    cp "$BACKUP" "$ENGINE"
    python3 - "$ENGINE" "$rule" "$MARKER" <<'PY'
import sys
path, rule, marker = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding='utf-8') as fh: lines = fh.read().split('\n')
anchor = None
for i, l in enumerate(lines):
    if l.startswith('std::string GovernanceEngine::enforce('):
        for j in range(i, min(i+8, len(lines))):
            if lines[j].rstrip().endswith(') {'):
                anchor = j; break
        break
if anchor is None:
    sys.stderr.write('could not locate enforce() body\n'); sys.exit(1)
probe = '    if (rule_name == "%s") return "";  // %s' % (rule, marker)
lines.insert(anchor + 1, probe)
with open(path, 'w', encoding='utf-8') as fh: fh.write('\n'.join(lines))
PY
}

rebuild() { make -C build naab-lang -j4 >/dev/null 2>&1; }

# Shell suites that MENTION this rule name. Ordering heuristic only.
candidates() {
    grep -rl -- "$1" tests --include='*.sh' 2>/dev/null | head -6
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Gate mutation: does anything notice when a gate stops?      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

PROT=0; UNPROT=0; ERR=0
for rule in $TARGETS; do
    printf "  %-46s " "$rule"
    inject "$rule" || { echo -e "${RED}INJECT FAILED${NC}"; ERR=$((ERR+1)); continue; }
    if ! rebuild; then
        echo -e "${YELLOW}BUILD FAILED (mutation not compilable)${NC}"
        ERR=$((ERR+1)); continue
    fi

    verdict=""; witness=""
    for t in $(candidates "$rule"); do
        if ! timeout 600 bash "$t" >/dev/null 2>&1; then
            verdict="PROTECTED"; witness="$t"; break
        fi
    done
    if [ -z "$verdict" ]; then
        if ! timeout 3600 bash run-all-tests.sh >/dev/null 2>&1; then
            verdict="PROTECTED"; witness="run-all-tests.sh"
        else
            verdict="UNPROTECTED"; witness="-"
        fi
    fi

    if [ "$verdict" = "PROTECTED" ]; then
        PROT=$((PROT+1)); echo -e "${GREEN}PROTECTED${NC}  ($witness)"
    else
        UNPROT=$((UNPROT+1)); echo -e "${RED}UNPROTECTED${NC}  nothing failed"
    fi
    printf '%s %s %s %s\n' "$rule" "$verdict" "$witness" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$LEDGER"
done

revert_source
rebuild || echo -e "${YELLOW}  note: rebuild after revert failed — rebuild by hand${NC}"

echo ""
echo "  probed this run: $((PROT+UNPROT))   PROTECTED $PROT   UNPROTECTED $UNPROT   errors $ERR"
if git diff --quiet -- "$ENGINE"; then
    echo -e "  ${GREEN}source reverted cleanly${NC}"
else
    echo -e "  ${RED}SOURCE NOT CLEAN — inspect $ENGINE before committing${NC}"; exit 1
fi
[ "$ERR" -gt 0 ] && exit 1
exit 0
