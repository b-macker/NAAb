#!/usr/bin/env bash
# ============================================================
# test_governance_authority.sh -- govern.json decides, on BOTH engines
#
# WHAT THIS PINS. Two defects that are mirror images of each other, and each
# needs its own arm because either patch alone leaves the other's hole open:
#
#   GA-01  A CLI FLAG COULD OVERRIDE THE FILE. `--no-governance` disabled
#          governance on the tree-walker (main.cpp interpreter.disableGovernance())
#          while the VM had never honoured it as an off switch. So the VM's
#          hardening was exactly one flag deep: measured on 406b3a6,
#          `--no-governance` alone was refused (rc=3) and
#          `--tree-walk --no-governance` READ THE BLOCKED FILE (rc=0).
#
#   GB-01  THE FILE'S OWN OFF SWITCH DID NOT WORK. govern.json `mode: "off"`
#          was honoured by the tree-walker and IGNORED by the VM, which kept
#          enforcing at HARD. 11 governance calls in vm.cpp sat under a bare
#          `if (governance_)` while call_dispatch.cpp gates all 14 of its calls
#          on `isActive()` -- and, tellingly, those same VM blocks ALREADY gated
#          their siblings (checkDangerousCall, the taint sink,
#          checkFunctionInputContract). Same block, opposite answers.
#
# WHY BOTH ARMS, AND WHY EACH MUST FAIL ALONE. GA-01 is tree-walk-only and does
# not touch vm.cpp; GB-01 uses no flags and does not touch main.cpp. Revert
# either patch and exactly one arm goes red. Without that separation one patch
# silently carries the other's coverage, and a later revert ships green.
#
# THE CONTROLS ARE NOT DECORATION.
#   GC-01/GC-02  `mode: "enforce"` must still BLOCK on both engines. Without
#                them, a build that enforces NOTHING passes GB-01 (which expects
#                a successful read) and the suite would certify the hole.
#   GC-03        the engines must AGREE under `mode: "off"`. That is the actual
#                property -- "both permit" is the claim, not "the VM permits".
#   GB-01 is also the arm that expects SUCCESS. A suite made only of
#   "should be blocked" assertions cannot detect its own instrument failing:
#   an unreadable fixture, an empty path, a skipped binary all read as
#   "refused" and pass for free. This is the arm that catches that.
#
# NOT A CLAIM THIS SUITE MAKES: that governance cannot be escaped. It can --
# a program spawned into a directory where no govern.json is discoverable is
# ungoverned regardless of either patch, because discovery is per-process from
# the script's own directory. That is contained by capabilities.shell.enabled,
# not by anything here.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0
LAST=""
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; [ -n "$LAST" ] && echo "$LAST" | sed 's/^/        | /' | head -8; FAIL=$((FAIL+1)); }

echo "=== govern.json authority (flag vs file, both engines) ==="

# Usability check, separate from any result. Two identical verdicts are a match
# whatever produced them, so a stand-in that exits 3 every time would pass every
# "blocked" arm having run nothing. Ask the binary to identify itself.
if [ ! -x "$NAAB" ]; then
    echo "  SKIP [GA-00] naab-lang not built -- UNMEASURABLE, not a pass"; exit 0
fi
if ! "$NAAB" --version 2>/dev/null | grep -qi naab; then
    echo "  FAIL [GA-00] $NAAB does not identify as naab-lang -- comparison would be vacuous"
    exit 1
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

W="$(mktemp -d)"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

# Relative paths only: the config, the program and the target all resolve
# against the same cwd, so nothing here is in the shell's path vocabulary.
echo "AUTHORITY_SECRET" > "$W/secret.txt"
printf 'main {\n  let c = file.read("secret.txt")\n  print("READ_OK")\n}\n' > "$W/t.naab"

cfg() {  # $1 = mode
    cat > "$W/govern.json" <<EOF
{ "version": "4.0", "mode": "$1",
  "capabilities": { "filesystem": { "mode": "readwrite", "blocked_paths": ["./secret.txt"] } } }
EOF
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" 2>/dev/null \
            || { echo "  FAIL [GA-CFG] generated govern.json is not valid JSON"; FAIL=$((FAIL+1)); }
    fi
}

# Captures into a variable and matches with `case` -- never `| grep -q`, which
# under `set -o pipefail` reports FAILURE exactly when the pattern IS present,
# inverting every verdict in the safe-looking direction.
run() {  # $1.. = flags ; echoes "allowed" or "blocked"
    LAST=$( (cd "$W" && timeout 60s "$NAAB" "$@" t.naab) 2>&1 )
    case "$LAST" in
        *READ_OK*) echo "allowed" ;;
        *)         echo "blocked" ;;
    esac
}

# --- GA-01: a CLI flag must not override the file ------------------------
# Fails if main.cpp's disableGovernance() call is restored. Touches no vm.cpp
# behaviour: enforce mode, tree-walker, flag passed.
cfg enforce
if [ "$(run --tree-walk --no-governance)" = "blocked" ]; then
    ok "GA-01" "--tree-walk --no-governance cannot bypass an enforcing config"
else
    bad "GA-01" "the flag disabled governance on the tree-walker -- the file lost to a flag"
fi

# --- GB-01: the file's own off switch must work, on the DEFAULT engine ----
# Fails if vm.cpp's isActive() gates are removed. Uses no flags at all.
# This is also the suite's expects-SUCCESS arm (see header).
cfg off
if [ "$(run)" = "allowed" ]; then
    ok "GB-01" "mode:\"off\" is honoured on the default engine"
else
    bad "GB-01" "mode:\"off\" set and the VM enforced anyway -- govern.json is not in control"
fi

# --- GC-01/GC-02: enforce must still block, or GB-01 proves nothing -------
cfg enforce
if [ "$(run)" = "blocked" ]; then
    ok "GC-01" "CONTROL: mode:\"enforce\" still blocks on the VM"
else
    bad "GC-01" "enforce did not block on the VM -- GB-01's pass is meaningless"
fi
if [ "$(run --tree-walk)" = "blocked" ]; then
    ok "GC-02" "CONTROL: mode:\"enforce\" still blocks on the tree-walker"
else
    bad "GC-02" "enforce did not block on the tree-walker"
fi

# --- GC-03: the property is AGREEMENT, not one engine's answer -----------
cfg off
VM_OFF="$(run)"; TW_OFF="$(run --tree-walk)"
if [ "$VM_OFF" = "$TW_OFF" ]; then
    ok "GC-03" "both engines agree under mode:\"off\" ($VM_OFF)"
else
    bad "GC-03" "engines disagree under mode:\"off\" -- vm=$VM_OFF tree-walk=$TW_OFF"
fi

# --- GC-04: --no-governance must still run a CONFIG-FREE program ---------
# The flag's other job, and the only one nothing else can do: require_governance
# defaults true, so without this a program with no govern.json cannot run at all.
# Removing this job (rather than only the off switch) broke two suites when it
# was simulated, so it is pinned here.
FREE="$(mktemp -d)"
printf 'main { print("FREE_OK") }\n' > "$FREE/t.naab"
FOUT=$( (cd "$FREE" && timeout 60s "$NAAB" --no-governance t.naab) 2>&1 )
case "$FOUT" in
    *FREE_OK*) ok "GC-04" "--no-governance still runs a program with no govern.json" ;;
    *) LAST="$FOUT"; bad "GC-04" "a config-free program can no longer run -- the waiver was removed too" ;;
esac
rm -rf "$FREE"

echo "  Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
