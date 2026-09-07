#!/usr/bin/env bash
# ============================================================
# test_gate_mutation.sh — can each governance gate actually block anything?
#
# THE QUESTION. The engine has 172 enforce() call sites across 99 distinct rule
# names. A gate no test exercises is indistinguishable, from CI, from a gate
# that cannot fire at all — and this campaign has repeatedly found the second
# kind: signals inert by construction, BSD patterns dead under the tree-walker,
# checks gated on state nothing writes. Every one was found by SAMPLING that
# population by hand, at a high hit rate. This enumerates it instead.
#
# THE METHOD. Neuter one gate, rebuild, see whether anything notices.
#   nothing fails   -> UNPROTECTED. No test distinguishes this gate working from
#                      this gate absent. Not proof the gate is inert; proof CI
#                      would not tell you if it were.
#   something fails -> PROTECTED, and the failing test is recorded as witness.
#
# ISOLATION: THIS NEVER TOUCHES YOUR WORKING TREE.
# Everything happens in a throwaway `git worktree` with its own copy of build/.
# That is a correction, not gold-plating. The first version injected into the
# shared tree the way test_prescan_canaries.sh does, and in one afternoon that
# pattern caused four separate confusions: a dirty `git status` mid-run that
# nearly got an injected defect committed; the canary suite silently SKIPPED on
# CI because src/ was dirty; a build race that failed test_vocab_baseline.sh
# inside a full suite while it passed 11/0 alone; and a false-UNPROTECTED blind
# spot in this harness. One cause: mutating shared state everything else reads.
#
# The probe is injected in the worktree AND COMMITTED THERE. Committing is what
# removes the last of those four. run-all-tests.sh skips the canary whenever
# src/ or include/ is dirty (run-all-tests.sh:2444), so an uncommitted probe
# silently disables it, and a gate witnessed only by a canary assertion would
# read UNPROTECTED when it is not. Committed, the worktree is clean against its
# own HEAD, the canary runs, and the blind spot is gone. That commit dies with
# the worktree and reaches no branch.
#
# Consequences: your tree stays clean, `git status` stays truthful, this is safe
# to run while a full suite is going, and two can run at once. No lockfile is
# needed because nothing shared is written.
#
# WHY THE MUTATION IS INJECTED, NOT A FLAG. A runtime switch that disables an
# arbitrary gate by name is a governance backdoor — a supported way to turn off
# any check, shipped inside the engine this repo exists to harden. Indefensible
# whatever guard sits on it. So the probe is compiled in, in a tree that is
# thrown away.
#
# WHY A MENTION IS NOT COVERAGE. Tests naming the rule run first because that is
# far cheaper than the full suite. That is an ORDERING heuristic, never a
# verdict: a gate is PROTECTED only when a test actually FAILS, and UNPROTECTED
# only after the full suite has run. Trace to the point of effect.
#
# COST, measured here: 125s per incremental rebuild, one per gate. A full sweep
# is hours — nightly/manual, not per-commit. The ledger makes it resumable.
#
# USAGE
#   --gate NAME    probe one gate        --sample N   probe N not yet in ledger
#   --all          sweep everything      --list       accumulated coverage
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

LEDGER="$REPO/tests/self-audit/gate_mutation_ledger.txt"
MARKER="NAAB_MUTATION_PROBE"
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'

list_gates() {
    grep -rhoE 'enforce\("[a-z_.]+"' "$REPO"/src/runtime/*.cpp "$REPO"/src/stdlib/*.cpp 2>/dev/null \
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
    echo ""; echo -e "${CYAN}Gate mutation coverage${NC}"
    tot=0; done_n=0; unprot=0
    while read -r g; do
        tot=$((tot+1))
        if ledger_has "$g"; then
            done_n=$((done_n+1))
            [ "$(grep "^$g " "$LEDGER" | tail -1 | awk '{print $2}')" = "UNPROTECTED" ] && \
                { unprot=$((unprot+1)); printf "  %-46s %b\n" "$g" "${RED}UNPROTECTED${NC}"; }
        else
            printf "  %-46s %b\n" "$g" "${YELLOW}not yet probed${NC}"
        fi
    done < <(list_gates)
    echo ""; echo "  $done_n of $tot gates probed; $unprot UNPROTECTED"
    exit 0
fi

case "$MODE" in
    one)    TARGETS="$ONE";;
    all)    TARGETS="$(list_gates)";;
    sample) TARGETS="$(list_gates | while read -r g; do ledger_has "$g" || echo "$g"; done | head -"$N")";;
esac
[ -z "$TARGETS" ] && { echo "no gates selected (all probed? try --all or --list)"; exit 0; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/gate-probe-XXXXXX")"
WT="$WORK/tree"
cleanup() {
    [ -d "$WT" ] && { git -C "$REPO" worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"; }
    rm -rf "$WORK"
    git -C "$REPO" worktree prune >/dev/null 2>&1
}
trap cleanup EXIT INT TERM

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Gate mutation: does anything notice when a gate stops?      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""
echo "  preparing isolated worktree — your tree is not touched"
if ! git -C "$REPO" worktree add --detach "$WT" HEAD >/dev/null 2>&1; then
    echo -e "  ${RED}could not create a worktree — aborting rather than mutating your tree${NC}"; exit 1
fi
[ -d "$REPO/build" ] || { echo -e "  ${RED}no build/ to copy — build first${NC}"; exit 1; }
cp -a "$REPO/build" "$WT/build"
ENGINE="$WT/src/runtime/governance_engine.cpp"
# Pristine binary, kept for witness confirmation (see confirms_witness).
PRISTINE_BIN="$WORK/naab-lang.pristine"
PROBED_BIN="$WORK/naab-lang.probed"
cp "$WT/build/naab-lang" "$PRISTINE_BIN"
echo ""

inject() {
    git -C "$WT" checkout -q -- src/runtime/governance_engine.cpp 2>/dev/null
    python3 - "$ENGINE" "$1" "$MARKER" <<'PY'
import sys
path, rule, marker = sys.argv[1], sys.argv[2], sys.argv[3]
with open(path, encoding='utf-8') as fh: lines = fh.read().split('\n')
anchor = None
for i, l in enumerate(lines):
    if l.startswith('std::string GovernanceEngine::enforce('):
        for j in range(i, min(i + 8, len(lines))):
            if lines[j].rstrip().endswith(') {'): anchor = j; break
        break
if anchor is None:
    sys.stderr.write('could not locate enforce() body\n'); sys.exit(1)
lines.insert(anchor + 1, '    if (rule_name == "%s") return "";  // %s' % (rule, marker))
with open(path, 'w', encoding='utf-8') as fh: fh.write('\n'.join(lines))
PY
}
commit_probe() {
    git -C "$WT" add -A src/runtime/governance_engine.cpp >/dev/null 2>&1
    git -C "$WT" -c user.email=probe@local -c user.name=probe \
        commit -q --allow-empty -m "gate probe" >/dev/null 2>&1
}
rebuild() { make -C "$WT/build" naab-lang -j4 >/dev/null 2>&1; }
# --include AFTER the path operand is IGNORED by this grep, which let
# gate_mutation_ledger.txt through as a "candidate test" on the first real run.
# bash could not run a .txt, the non-zero exit read as "a test failed", and the
# gate was reported PROTECTED with the ledger as its witness — a FALSE
# PROTECTED, which is the worst verdict this harness can produce because it
# silently claims coverage that does not exist. Flags before the path, and the
# result is filtered to real test scripts regardless.
candidates() {
    grep -rl --include='*.sh' -- "$1" "$WT/tests" 2>/dev/null \
        | grep -E '(^|/)tests/.*/(test_|run_)[^/]*\.sh$' | head -6
}

# A candidate that FAILS under the probe is only a witness if it PASSES without
# it. Otherwise a test broken for unrelated reasons is indistinguishable from
# one the probe broke — the same conflation as above, one level up. Swapping the
# pristine binary back is cheap (a copy, not a rebuild), and only happens on the
# failing path.
confirms_witness() {
    local t="$1" rc
    cp "$PRISTINE_BIN" "$WT/build/naab-lang" 2>/dev/null || return 1
    ( cd "$WT" && timeout 600 bash "$t" >/dev/null 2>&1 ); rc=$?
    cp "$PROBED_BIN" "$WT/build/naab-lang" 2>/dev/null
    return $rc
}

PROT=0; UNPROT=0; ERR=0
for rule in $TARGETS; do
    printf "  %-46s " "$rule"
    inject "$rule" || { echo -e "${RED}INJECT FAILED${NC}"; ERR=$((ERR+1)); continue; }
    commit_probe
    if ! rebuild; then
        echo -e "${YELLOW}BUILD FAILED (mutation not compilable)${NC}"; ERR=$((ERR+1)); continue
    fi

    cp "$WT/build/naab-lang" "$PROBED_BIN"
    verdict=""; witness=""
    for t in $(candidates "$rule"); do
        if ! ( cd "$WT" && timeout 600 bash "$t" >/dev/null 2>&1 ); then
            if confirms_witness "$t"; then
                verdict="PROTECTED"; witness="${t#$WT/}"; break
            fi
        fi
    done
    if [ -z "$verdict" ]; then
        if ! ( cd "$WT" && timeout 3600 bash run-all-tests.sh >/dev/null 2>&1 ); then
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

echo ""
echo "  probed this run: $((PROT+UNPROT))   PROTECTED $PROT   UNPROTECTED $UNPROT   errors $ERR"
if git -C "$REPO" diff --quiet -- src/ include/; then
    echo -e "  ${GREEN}your working tree was never modified${NC}"
else
    echo -e "  ${YELLOW}note: src/ is dirty — not from this script, which works in a worktree${NC}"
fi
[ "$ERR" -gt 0 ] && exit 1
exit 0
