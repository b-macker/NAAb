#!/usr/bin/env bash
# test_gov_patch_check.sh -- naab-gov patch-check BASE PATCHED: the gate a
# patch written by an agent must pass before a person looks at it (Bundle 15 PR 3)
#
# Each arm copies one base tree, makes ONE change in the copy, and asks the
# gate. A refusal arm names the reason it expects, so an arm cannot pass on
# another rule's refusal; each rule has a control that the same kind of change
# elsewhere is accepted.
#
#   PC-00   CONTROL: an honest patch (a program changed, a test added) is
#           ACCEPTED, exit 0, and the added test is listed
#   PC-00b  CONTROL: an unchanged copy is accepted with 0 files changed
#   PC-01   govern.json changed                          -> refused
#   PC-01b  a govern.json added in a subdirectory        -> refused
#   PC-01c  a .sig removed                               -> refused
#   PC-02   a capability manifest changed                -> refused
#   PC-03   a file under trusted-keys/ added             -> refused
#   PC-04   CLAUDE.md (a project context file) changed   -> refused
#   PC-05   the config's `extends` parent changed        -> refused
#   PC-05c  CONTROL: another .json beside it changed     -> accepted
#   PC-06   a governance plugin file changed             -> refused
#   PC-07   a file under patch_check.protected_paths     -> refused
#   PC-07c  CONTROL: the same change without that key    -> accepted
#   PC-08   an existing test's assertion swapped for `assert True` (the
#           assertion count is unchanged)                -> refused
#   PC-09   an existing test removed                     -> refused
#   PC-10   patch_check.test_paths names spec/: a change under spec/ is
#           refused, and (the list replaces the default) one under tests/ is not
#   PC-11   a symbolic link added                        -> refused
#   PC-11b  an existing file replaced by a link to a copy of itself -> refused
#   PC-12   the base holds no test file: UNMEASURABLE, exit 2
#   PC-12b  ... and a refusal outranks it: exit 1
#   PC-13   contract clauses in the config, no --run-contracts: exit 2
#   PC-13b  --run-contracts, the patch breaks a clause   -> refused
#   PC-13c  CONTROL: --run-contracts, the patch keeps the clause -> accepted
#           (and so the search ran: a dead interpreter fails this arm)
#   PC-13d  a governance file touched: the contract search is not run
#   PC-13e  a contract search whose worker reports nothing: exit 2, not accepted
#   PC-14   what the patch newly needs (a manifest action, a taint flow) is
#           printed and refuses nothing
#   PC-15   usage errors exit 4
#   PC-16   --json parses and carries the result and the refusals
#   PC-17   a difference inside .git is not part of the patch
#
# Kill table (each mutant built, suite re-run; observed 2026-11-13):
#   a govern.json is an ordinary file            -> PC-01 PC-01b red
#   a .sig is an ordinary file                   -> PC-01c red
#   a manifest is an ordinary file               -> PC-02 red
#   trusted-keys not protected                   -> PC-03 red
#   project context files not protected          -> PC-04 PC-13d red
#   extends parents not followed                 -> PC-05 red
#   governance plugins not named                 -> PC-06 red
#   protected_paths ignored                      -> PC-07 red
#   a changed test is accepted                   -> PC-08 PC-10 red
#   a removed test is accepted                   -> PC-09 PC-16 red
#   test_paths config ignored                    -> PC-10 red
#   an added link is accepted                    -> PC-11 red
#   a changed link is accepted                   -> PC-11b red
#   links followed when hashing                  -> PC-11 PC-11b red
#   no test file in the base is fine             -> PC-12 PC-12b red
#   UNMEASURABLE outranks a refusal              -> PC-12b red
#   unchecked clauses are accepted               -> PC-13 red
#   a new counterexample is accepted             -> PC-13b red
#   an unmeasured clause is accepted             -> PC-13e red
#   contracts run under a patched governance input -> PC-13d red
#   tools run under a patched governance input   -> PC-13d red
#   a new manifest action refuses                -> PC-14 red
#   .git is compared                             -> PC-17 red
#   every change is refused                      -> PC-00 PC-05c PC-07c PC-10 PC-12 PC-13 PC-14 red
#   the same directory twice is accepted         -> PC-15 red
# The command is new, so before it every arm fails on "unknown command"; the
# mutants are what show each arm can fail. Not mutated: reading PATCHED's
# config instead of BASE's -- any arm that could show it also changes
# govern.json, which is refused first.
#
# No secret or PII value is written here.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="$(dirname "$NAAB")/naab-gov"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/naab_patchcheck_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs
cleanup() { teardown_isolated_trust; rm -rf "$TMP"; }
trap cleanup EXIT

echo "=== naab-gov patch-check (Bundle 15 PR 3) ==="
if [ ! -x "$GOV" ]; then
    unm PC-ALL "naab-gov is not built at $GOV"
    echo "Results: 0 passed, 0 failed, 1 unmeasurable"; exit 1
fi

# ---- the base tree -----------------------------------------------------------
# make_base DIR EXTRA_GOVERN_MEMBERS
make_base() {
    local d="$1"
    mkdir -p "$d/src" "$d/tests" "$d/spec" "$d/policy" "$d/docs" "$d/keys/trusted-keys"
    cat > "$d/govern.json" <<GOVEOF
{
    "mode": "enforce",
    "security": {"sandbox_level": "elevated"},
    "extends": "./policy/parent.json",
    "governance_plugins": [{"file": "policy/plugin.naab", "rules": []}],
    ${2:-}
    "taint_tracking": {"enabled": true, "level": "advisory", "sources": ["env.get"], "sinks": ["file.write"], "sanitizers": []}
}
GOVEOF
    echo '{"mode": "enforce"}' > "$d/policy/parent.json"
    echo '{"note": "not governance"}' > "$d/policy/other.json"
    printf 'fn rule(x) {\n    return true\n}\n' > "$d/policy/plugin.naab"
    printf 'sig\n' > "$d/govern.json.sig.keep"       # not a .sig: must not be protected by name
    # A .sig beside an ordinary file: one beside the extends parent would be
    # read as that parent's signature and the base config would not load.
    printf 'signature\n' > "$d/docs/design.md.sig"
    echo '{"naab_manifest": 1}' > "$d/src/p.manifest.json"
    printf '# project notes\n' > "$d/CLAUDE.md"
    printf 'design\n' > "$d/docs/design.md"
    printf 'use file\nfn absval(n) {\n    if n < 0 { return 0 - n }\n    return n\n}\nmain {\n    file.write("out.txt", "x")\n    print(absval(-3))\n}\n' > "$d/src/p.naab"
    printf 'def test_a():\n    assert f(2) == 4\n' > "$d/tests/test_a.py"
    printf 'def test_spec():\n    assert g(1) == 1\n' > "$d/spec/test_spec.py"
}
BASE="$TMP/base"; make_base "$BASE"

# arm NAME -> a fresh copy of $BASE_FOR_ARM (default $BASE) in $P
arm() { P="$TMP/$1"; rm -rf "$P"; cp -a "${BASE_FOR_ARM:-$BASE}" "$P"; }
# check [ARGS...] -> OUT, RC   (the gate on ${BASE_FOR_ARM:-$BASE} -> $P)
check() { RC=0; OUT=$(timeout 300s "$GOV" patch-check "${BASE_FOR_ARM:-$BASE}" "$P" "$@" 2>&1) || RC=$?; }
has() { grep -q -F -- "$1" <<<"$OUT"; }
# refused ID LINE-FRAGMENT WHAT
refused() {
    if [ "$RC" -eq 1 ] && has "result: REFUSED" && has "$2"; then pass "$1" "$3 is refused"
    else fail "$1" "$3 should be refused for its own reason" "rc=$RC wanted '$2': $(grep -A4 '^Refused' <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi
}
accepted() {
    if [ "$RC" -eq 0 ] && has "result: ACCEPTED" && has "Refused (0)"; then pass "$1" "$2"
    else fail "$1" "$2" "rc=$RC: $(grep -A4 '^Refused\|^Not measured' <<<"$OUT" | tr '\n' ' ' | cut -c1-300)"; fi
}

HONEST='use file
fn absval(n) {
    if n < 0 { return 0 - n }
    return n
}
main {
    file.write("out.txt", "y")
    print(absval(-4))
}
'

# ---- PC-00 --------------------------------------------------------------------
arm pc00; printf '%s' "$HONEST" > "$P/src/p.naab"; printf 'def test_b():\n    assert f(3) == 9\n' > "$P/tests/test_b.py"
check
if [ "$RC" -eq 0 ] && has "result: ACCEPTED" && has "+ tests/test_b.py" && has "files: 1 added, 1 changed, 0 removed"; then
    pass PC-00 "CONTROL: an honest patch is accepted, and its added test is listed"
else fail PC-00 "an honest patch should be accepted" "rc=$RC: $(tail -6 <<<"$OUT" | tr '\n' ' ')"; fi
arm pc00b; check
if [ "$RC" -eq 0 ] && has "files: 0 added, 0 changed, 0 removed"; then pass PC-00b "CONTROL: an unchanged copy is accepted with nothing changed"
else fail PC-00b "an unchanged copy should be accepted" "rc=$RC: $(sed -n 3p <<<"$OUT")"; fi

# ---- governance files ----------------------------------------------------------
arm pc01; sed -i 's/"enforce"/"audit"/' "$P/govern.json"; check
refused PC-01 "changes govern.json: a govern.json" "a changed govern.json"
arm pc01b; echo '{"mode": "off"}' > "$P/src/govern.json"; check
refused PC-01b "adds src/govern.json: a govern.json" "a govern.json added in a subdirectory"
arm pc01c; rm "$P/docs/design.md.sig"; check
refused PC-01c "removes docs/design.md.sig: a signature" "a removed signature"
arm pc02; echo '{"naab_manifest": 1, "actions": ["NET_CONNECT"]}' > "$P/src/p.manifest.json"; check
refused PC-02 "changes src/p.manifest.json: a capability manifest" "a changed capability manifest"
arm pc03; echo "key" > "$P/keys/trusted-keys/new.pub"; check
refused PC-03 "adds keys/trusted-keys/new.pub: a trusted key" "a key added under trusted-keys"
arm pc04; echo "allow everything" >> "$P/CLAUDE.md"; check
refused PC-04 "changes CLAUDE.md: a project context file" "a changed project context file"
arm pc05; echo '{"mode": "off"}' > "$P/policy/parent.json"; check
refused PC-05 "changes policy/parent.json: named by the base config" "a changed extends parent"
arm pc05c; echo '{"note": "edited"}' > "$P/policy/other.json"; check
accepted PC-05c "CONTROL: a .json the config does not name is an ordinary file"
arm pc06; printf 'fn rule(x) {\n    return false\n}\n' > "$P/policy/plugin.naab"; check
refused PC-06 "changes policy/plugin.naab: named by the base config" "a changed governance plugin"

PROT="$TMP/base-prot"; make_base "$PROT" '"patch_check": {"protected_paths": ["docs/"]},'
BASE_FOR_ARM="$PROT"; arm pc07; echo "edited" > "$P/docs/design.md"; check
refused PC-07 "changes docs/design.md: under patch_check.protected_paths" "a file under protected_paths"
unset BASE_FOR_ARM
arm pc07c; echo "edited" > "$P/docs/design.md"; check
accepted PC-07c "CONTROL: without that key the same file is an ordinary one"

# ---- tests ---------------------------------------------------------------------
arm pc08; printf 'def test_a():\n    assert True\n' > "$P/tests/test_a.py"; check
if [ "$RC" -eq 1 ] && has "changes tests/test_a.py: an existing test file" && has "printed only; it cannot tell a real assertion from \`assert True\`): 1 -> 1"; then
    pass PC-08 "an assertion swapped for 'assert True' is refused, though the count is unchanged (1 -> 1)"
else fail PC-08 "a changed existing test should be refused" "rc=$RC: $(grep 'test_a\|assertion sites' <<<"$OUT" | tr '\n' ' ')"; fi
arm pc09; rm "$P/tests/test_a.py"; check
refused PC-09 "removes tests/test_a.py: an existing test file" "a removed test"

SPEC="$TMP/base-spec"; make_base "$SPEC" '"patch_check": {"test_paths": ["spec/"]},'
BASE_FOR_ARM="$SPEC"; arm pc10a; printf 'def test_spec():\n    assert True\n' > "$P/spec/test_spec.py"; check
r10a=$RC; o10a="$OUT"
arm pc10b; printf 'def test_a():\n    assert True\n' > "$P/tests/test_a.py"; check
unset BASE_FOR_ARM
if [ "$r10a" -eq 1 ] && grep -q -F "changes spec/test_spec.py: an existing test file" <<<"$o10a" && [ "$RC" -eq 0 ] && has "(paths: spec/)"; then
    pass PC-10 "patch_check.test_paths is read: spec/ is held, and tests/ (no longer listed) is not"
else fail PC-10 "test_paths should replace the default list" "spec-rc=$r10a tests-rc=$RC"; fi

# ---- links ---------------------------------------------------------------------
arm pc11
if ln -s ../docs/design.md "$P/src/link.md" 2>/dev/null && [ -L "$P/src/link.md" ]; then
    check; refused PC-11 "adds src/link.md: a symbolic link" "an added symbolic link"
    arm pc11b; cp "$P/docs/design.md" "$P/docs/copy.md"; rm "$P/docs/design.md"; ln -s copy.md "$P/docs/design.md"; check
    refused PC-11b "changes docs/design.md: a symbolic link" "a file replaced by a link to a copy of itself"
else
    unm PC-11 "this platform makes no symbolic link here"; unm PC-11b "this platform makes no symbolic link here"
fi

# ---- unmeasurable ----------------------------------------------------------------
NOTEST="$TMP/base-notest"; make_base "$NOTEST"; rm -rf "$NOTEST/tests"
BASE_FOR_ARM="$NOTEST"; arm pc12; printf '%s' "$HONEST" > "$P/src/p.naab"; check
if [ "$RC" -eq 2 ] && has "result: UNMEASURABLE" && has "the base holds no file under the test paths"; then
    pass PC-12 "a base with no test file: UNMEASURABLE, exit 2"
else fail PC-12 "no test file in the base should be UNMEASURABLE" "rc=$RC: $(tail -3 <<<"$OUT" | tr '\n' ' ')"; fi
arm pc12b; sed -i 's/"enforce"/"audit"/' "$P/govern.json"; check
if [ "$RC" -eq 1 ] && has "changes govern.json" && has "the base holds no file under the test paths"; then
    pass PC-12b "a refusal outranks UNMEASURABLE: exit 1, both stated"
else fail PC-12b "a refusal should outrank UNMEASURABLE" "rc=$RC"; fi
unset BASE_FOR_ARM

# ---- contracts -------------------------------------------------------------------
CON="$TMP/base-con"
make_base "$CON" '"contracts": {"level": "hard", "functions": {"absval": {"params": ["n:int"], "ensures": ["result >= 0"]}}},'
BROKEN='use file
fn absval(n) {
    if n < -5 { return n }
    if n < 0 { return 0 - n }
    return n
}
main {
    file.write("out.txt", "x")
    print(absval(-3))
}
'
BASE_FOR_ARM="$CON"
arm pc13; printf '%s' "$HONEST" > "$P/src/p.naab"; check
if [ "$RC" -eq 2 ] && has "--run-contracts was not given"; then
    pass PC-13 "contract clauses and no --run-contracts: UNMEASURABLE, exit 2"
else fail PC-13 "an unchecked clause should be UNMEASURABLE" "rc=$RC: $(grep -A3 'Not measured' <<<"$OUT" | tr '\n' ' ')"; fi
arm pc13c; printf '%s' "$HONEST" > "$P/src/p.naab"; check --run-contracts --naab "$NAAB"
c_rc=$RC; c_out="$OUT"
if [ "$c_rc" -eq 0 ] && grep -q -F "result: ACCEPTED" <<<"$c_out"; then
    pass PC-13c "CONTROL: --run-contracts on a patch that keeps the clause is accepted (the search ran)"
    arm pc13b; printf '%s' "$BROKEN" > "$P/src/p.naab"; check --run-contracts --naab "$NAAB"
    refused PC-13b "src/p.naab: absval breaks a contract clause it did not break before" "a patch that breaks a contract clause"
else
    unm PC-13c "the contract search did not measure the honest patch (rc=$c_rc): $(grep -A3 'Not measured\|^Refused' <<<"$c_out" | tr '\n' ' ' | cut -c1-200)"
    unm PC-13b "no working contract search to compare against"
fi
arm pc13d; printf '%s' "$BROKEN" > "$P/src/p.naab"; echo "x" >> "$P/CLAUDE.md"; check --run-contracts --naab "$NAAB" --json
J=$(python3 -c '
import json, sys
d = json.load(sys.stdin)
ok = d["result"] == "refused" and d["contracts"] == [] and d["printed_only"] == [] and \
     any("CLAUDE.md" in r for r in d["refused"]) and not any("contract clause" in r for r in d["refused"])
sys.stdout.write("ok" if ok else "bad")' <<<"$OUT" 2>/dev/null)
if [ "$RC" -eq 1 ] && [ "$J" = "ok" ]; then pass PC-13d "a governance file touched: refused, and no tool is run on the patched program"
else fail PC-13d "patched code must not be run under a patched governance input" "rc=$RC verdict=${J:-unparsed}"; fi
# A worker that reports nothing (a stand-in naab-lang that exits 0) must never
# read as "no counterexample": found when the dead-interpreter check ACCEPTED
# the honest patch.
printf '#!/bin/sh\nexit 0\n' > "$TMP/dead-naab"; chmod +x "$TMP/dead-naab"
arm pc13e; printf '%s' "$BROKEN" > "$P/src/p.naab"; check --run-contracts --naab "$TMP/dead-naab"
if [ "$RC" -eq 2 ] && has "the contract search could not measure absval in the patch"; then
    pass PC-13e "a contract search whose worker reports nothing: UNMEASURABLE, exit 2, never accepted"
else fail PC-13e "an unmeasured clause must not be accepted" "rc=$RC: $(grep -A3 'Not measured\|^Refused' <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi
unset BASE_FOR_ARM

# ---- printed only ----------------------------------------------------------------
arm pc14; printf 'use file\nuse env\nfn absval(n) {\n    if n < 0 { return 0 - n }\n    return n\n}\nmain {\n    file.write("out.txt", env.get("HOME"))\n    print(absval(-3))\n}\n' > "$P/src/p.naab"; check
if [ "$RC" -eq 0 ] && has "the draft manifest newly lists actions: ENV_READ" && has "src/p.naab: taint flows 0 -> 1" && has "these decide nothing"; then
    pass PC-14 "a new manifest action and a new taint flow are printed and refuse nothing"
else fail PC-14 "what the patch newly needs should be printed, not refused" "rc=$RC: $(grep -A4 'Printed only' <<<"$OUT" | tr '\n' ' ')"; fi

# ---- usage, json, .git -----------------------------------------------------------
u1=0; "$GOV" patch-check "$BASE" >/dev/null 2>&1 || u1=$?
u2=0; "$GOV" patch-check "$BASE" "$BASE" >/dev/null 2>&1 || u2=$?
u3=0; "$GOV" patch-check "$BASE" "$TMP/pc00" --approve >/dev/null 2>&1 || u3=$?
u4=0; "$GOV" patch-check "$BASE" "$TMP/missing" >/dev/null 2>&1 || u4=$?
if [ "$u1" -eq 4 ] && [ "$u2" -eq 4 ] && [ "$u3" -eq 4 ] && [ "$u4" -eq 4 ]; then pass PC-15 "usage errors exit 4 (one directory, the same directory, an unknown option, a missing directory)"
else fail PC-15 "usage errors should exit 4" "one=$u1 same=$u2 option=$u3 missing=$u4"; fi

arm pc16; rm "$P/tests/test_a.py"; check --json
J=$(python3 -c '
import json, sys
d = json.load(sys.stdin)
ok = d["result"] == "refused" and d["files"]["removed"] == 1 and \
     d["refused"] == ["removes tests/test_a.py: an existing test file"] and d["tests"]["base_files"] == 1
sys.stdout.write("ok" if ok else "bad")' <<<"$OUT" 2>/dev/null)
if [ "$RC" -eq 1 ] && [ "$J" = "ok" ]; then pass PC-16 "--json parses and carries the result and the one refusal"
else fail PC-16 "--json should carry the result" "rc=$RC verdict=${J:-unparsed}"; fi

arm pc17; mkdir -p "$P/.git/hooks"; echo "x" > "$P/.git/hooks/pre-commit"; check
if [ "$RC" -eq 0 ] && has "files: 0 added, 0 changed, 0 removed"; then pass PC-17 "a difference inside .git is not part of the patch"
else fail PC-17 ".git should not be compared" "rc=$RC: $(sed -n 3p <<<"$OUT")"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
