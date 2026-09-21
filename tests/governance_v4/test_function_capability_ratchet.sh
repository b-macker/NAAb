#!/usr/bin/env bash
# ============================================================
# test_function_capability_ratchet.sh — F5: capabilities.functions ratchets
#
# A capability tier with no ratchet can be widened mid-run by a config reload,
# which makes it advisory in the weakest sense: it constrains only operators who
# were not going to loosen it anyway. capabilities.functions is the third rung
# of the scope ladder (program -> role -> function) and needs the same one-way
# guarantee the first two already have.
#
# The reason this check compares EFFECTIVE permissions and not raw map entries
# is that a name-by-name diff gets four ordinary edits wrong, and two of them
# loosen while looking like additions or deletions:
#
#   * adding an entry for X that grants more than "default"   -> LOOSENS X
#   * adding one that grants less                             -> tightens X
#   * REMOVING X's entry                                      -> LOOSENS X
#                                                       whenever "default" is
#                                                       more permissive
#   * editing "default"                                       -> moves every
#                                                       function with no entry
#
# So FR-03/FR-05/FR-07 are the cases a naive diff would wave through, and
# FR-04/FR-06/FR-09/FR-10 are their controls: the same SHAPE of edit in the
# tightening direction has to still be accepted, or a check that refused every
# reload would pass the whole suite.
#
# FR-10 is the load-bearing one. Every other arm reads a verdict off stderr; a
# reload can be reported "accepted" and still not be installed. FR-10 applies an
# accepted tightening and then asserts the new map actually governs the next
# call — guidance that does not survive being followed is not guidance.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
TEST_TMP="${_SYSTMP}/fncap-ratchet-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  F5: capabilities.functions is ratcheted (tighten-only)      |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

ALL_IDS="FR-01 FR-02 FR-03 FR-04 FR-05 FR-06 FR-07 FR-08 FR-09 FR-10 FR-11 FR-12"

IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" -eq 1 ]; then
    for id in $ALL_IDS; do skip "$id" "mid-run file swap requires POSIX file semantics"; done
    echo ""
    echo "  Total: 12 | Pass: 0 | Fail: 0 | Skip: 12"
    exit 0
fi

if ! command -v python3 >/dev/null 2>&1; then
    for id in $ALL_IDS; do skip "$id" "python3 unavailable (fixture generator + polyglot swap)"; done
    echo ""
    echo "  Total: 12 | Pass: 0 | Fail: 0 | Skip: 12"
    exit 0
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
cleanup() { teardown_isolated_trust; rm -rf "$TEST_TMP"; }
trap cleanup EXIT
mkdir -p "$TEST_TMP/loose"

"$NAAB" --keygen "$TEST_TMP/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$TEST_TMP/k.pem.pub" 2>/dev/null
export NAAB_SIGNING_KEY="$TEST_TMP/k.pem"
sign_dir() { (cd "$1" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1) || true; }

# $1 = path, $2 = the capabilities.functions object as JSON (or "" to omit it).
# Everything else is held constant so the only thing the ratchet can react to is
# the functions map.
mkcfg() {
    python3 - "$1" "$2" << 'PY'
import json, sys
path, fns = sys.argv[1], sys.argv[2]
cfg = {
    "version": "5.0",
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "languages": {"allowed": ["python"]},
    "capabilities": {
        "filesystem": {"mode": "readwrite", "allowed_paths": ["."]},
        "shell": {"enabled": True},
    },
}
if fns:
    cfg["capabilities"]["functions"] = json.loads(fns)
json.dump(cfg, open(path, "w"), indent=1)
PY
}

# The program swaps govern.json underneath itself between two polyglot blocks;
# the second block's governance check is what calls reloadIfChanged().
cat > "$TEST_TMP/t.naab" << EOF
fn writer() {
    try { file.write("out.txt", "x") print("WROTE") }
    catch (e) { print("WRITE_FAILED") }
}
main {
    let r1 = <<python
import time, shutil
time.sleep(1)
shutil.copy("$TEST_TMP/loose/govern.json", "$TEST_TMP/govern.json")
shutil.copy("$TEST_TMP/loose/govern.json.sig", "$TEST_TMP/govern.json.sig")
print("swapped")
>>
    print(r1)
    let r2 = <<python
print("trigger reload")
>>
    print(r2)
    writer()
}
EOF

# run_swap BASE_FNS LOOSE_FNS -> echoes "<reload verdict> <advisory verdict>"
#   reload verdict:   rejected | accepted | NO_RELOAD
#   advisory verdict: ADVISED  | NO_ADVICE   (did the post-reload write draw the
#                                             capabilities.functions advisory?)
run_swap() {
    mkcfg "$TEST_TMP/govern.json"       "$1"; sign_dir "$TEST_TMP"
    mkcfg "$TEST_TMP/loose/govern.json" "$2"; sign_dir "$TEST_TMP/loose"
    rm -f "$TEST_TMP/out.txt"
    local o; o=$(cd "$TEST_TMP" && timeout 90s "$NAAB" t.naab 2>&1)
    local r a
    case "$o" in
        *"Reload rejected"*)   r=rejected ;;
        *"reloaded mid-run"*)  r=accepted ;;
        *)                     r=NO_RELOAD ;;
    esac
    case "$o" in
        *"capabilities.functions"*) a=ADVISED ;;
        *)                          a=NO_ADVICE ;;
    esac
    echo "$r $a"
}

RW='{"default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}}'
RO='{"default": {"allowed_actions": ["FS_READ"]}}'

# --- Direct edits to an existing entry --------------------------------------

# FR-01: gaining an action is loosening.
read -r R _ <<< "$(run_swap "$RO" "$RW")"
if [ "$R" = "rejected" ]; then
    pass "FR-01" "adding an action to an entry is refused"
else
    fail "FR-01" "an entry gained an action mid-run" "reload=$R"
fi

# FR-02 CONTROL: the same edit in the other direction must be accepted, or a
# check that rejected every reload would pass FR-01.
read -r R _ <<< "$(run_swap "$RW" "$RO")"
if [ "$R" = "accepted" ]; then
    pass "FR-02" "control: dropping an action is accepted (tightening still works)"
else
    fail "FR-02" "tightening was refused" "reload=$R"
fi

# --- Adding an entry: direction depends on "default", not on the addition ----

# FR-03: a NEW entry that grants MORE than "default" loosens that function. A
# name-by-name diff sees only "a key appeared" and waves it through.
read -r R _ <<< "$(run_swap "$RO" '{"default": {"allowed_actions": ["FS_READ"]}, "writer": {"allowed_actions": ["FS_READ", "FS_WRITE"]}}')"
if [ "$R" = "rejected" ]; then
    pass "FR-03" "a new entry more permissive than default is refused"
else
    fail "FR-03" "a new entry granted more than default" "reload=$R"
fi

# FR-04 CONTROL: the same addition granting LESS than default is a tightening.
read -r R _ <<< "$(run_swap "$RW" '{"default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}, "writer": {"allowed_actions": ["FS_READ"]}}')"
if [ "$R" = "accepted" ]; then
    pass "FR-04" "control: a new entry more restrictive than default is accepted"
else
    fail "FR-04" "adding a restricting entry was refused" "reload=$R"
fi

# --- Removing an entry: also direction-dependent -----------------------------

# FR-05: dropping an entry hands that function back to "default". When default
# is the more permissive of the two, the deletion is a grant.
read -r R _ <<< "$(run_swap '{"default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}, "writer": {"allowed_actions": ["FS_READ"]}}' "$RW")"
if [ "$R" = "rejected" ]; then
    pass "FR-05" "removing an entry is refused when default is more permissive"
else
    fail "FR-05" "deleting an entry silently widened it" "reload=$R"
fi

# FR-06 CONTROL: the mirror image — default is the more restrictive of the two,
# so the same deletion tightens and must be accepted.
read -r R _ <<< "$(run_swap '{"default": {"allowed_actions": ["FS_READ"]}, "writer": {"allowed_actions": ["FS_READ", "FS_WRITE"]}}' "$RO")"
if [ "$R" = "accepted" ]; then
    pass "FR-06" "control: removing an entry is accepted when default is stricter"
else
    fail "FR-06" "a tightening deletion was refused" "reload=$R"
fi

# --- "default" and the section as a whole ------------------------------------

# FR-07: editing "default" moves every function that has no entry of its own,
# so it has to be compared as an effective permission rather than as one key.
read -r R _ <<< "$(run_swap '{"default": {"allowed_actions": ["FS_READ"]}, "other": {"allowed_actions": ["FS_READ"]}}' '{"default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}, "other": {"allowed_actions": ["FS_READ"]}}')"
if [ "$R" = "rejected" ]; then
    pass "FR-07" "widening default is refused (it moves every unlisted function)"
else
    fail "FR-07" "default was widened mid-run" "reload=$R"
fi

# FR-08: deleting the section un-governs every listed function at once.
read -r R _ <<< "$(run_swap "$RO" "")"
if [ "$R" = "rejected" ]; then
    pass "FR-08" "deleting the whole section is refused"
else
    fail "FR-08" "the section was deleted mid-run" "reload=$R"
fi

# FR-09 CONTROL: introducing the section for the first time restricts functions
# that were unrestricted, so it is a tightening and must be accepted. Without
# this arm, refusing every change involving an empty map would pass FR-08.
read -r R _ <<< "$(run_swap "" "$RO")"
if [ "$R" = "accepted" ]; then
    pass "FR-09" "control: introducing the section mid-run is accepted"
else
    fail "FR-09" "adding function capabilities mid-run was refused" "reload=$R"
fi

# FR-10 CONTROL: an accepted reload must INSTALL the tightened map, not merely
# report acceptance. The base config permits FS_WRITE; the reload drops it; the
# write that follows has to draw the capabilities.functions advisory. Every
# other arm here would pass for a build that accepted the reload and then threw
# the new rules away.
read -r R A <<< "$(run_swap "$RW" "$RO")"
if [ "$R" = "accepted" ] && [ "$A" = "ADVISED" ]; then
    pass "FR-10" "control: an accepted tightening actually governs the next call"
else
    fail "FR-10" "the reloaded map did not take effect" "reload=$R advisory=$A"
fi

# --- The section's enforcement level (F4) ------------------------------------

# FR-11: dropping the level turns blocks back into warnings, which is loosening
# in exactly the way every other enforcement level already ratchets.
read -r R _ <<< "$(run_swap '{"level": "hard", "default": {"allowed_actions": ["FS_READ"]}}' '{"level": "advisory", "default": {"allowed_actions": ["FS_READ"]}}')"
if [ "$R" = "rejected" ]; then
    pass "FR-11" "lowering capabilities.functions.level is refused"
else
    fail "FR-11" "the enforcement level was lowered mid-run" "reload=$R"
fi

# FR-12 CONTROL: raising it must still be accepted, or FR-11 could be refusing
# every reload that mentions a level at all.
read -r R _ <<< "$(run_swap '{"level": "advisory", "default": {"allowed_actions": ["FS_READ"]}}' '{"level": "soft", "default": {"allowed_actions": ["FS_READ"]}}')"
if [ "$R" = "accepted" ]; then
    pass "FR-12" "control: raising the level is accepted"
else
    fail "FR-12" "raising the level was refused" "reload=$R"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
