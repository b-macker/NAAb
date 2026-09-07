#!/usr/bin/env bash
# ============================================================
# test_coverage_visibility.sh — two gaps that are invisible by construction
#
# Neither of these is a failing test. Both are places where the suite cannot
# report on itself, which is the shape that let four inert mechanisms and two
# vacuous differentials survive review (registers A4/A5/B10/D8).
#
# CV-01  DIRECTORIES NOTHING RUNS.
#        run-all-tests.sh discovers .naab files from TEST_DIRS, an explicit
#        ALLOWLIST. SKIP_DIRS is a red herring — a directory absent from
#        TEST_DIRS is invisible whether or not it is skipped. Measured
#        2026-09-06: 12 directories holding 573 .naab files are reachable by
#        no runner at all, against 447 tests the suite reports. More test
#        files are outside the sweep than inside it.
#
#        The 13th was found by fixing this gate's own encoding bug. An earlier
#        version counted files by shelling out to `find {p}` UNQUOTED, so any
#        directory whose name contains a space produced an error and a count of
#        zero, and a zero count is skipped as "holds no tests". That hid
#        `tests/chapter verification` -- two tracked files, one of them 110KB,
#        in a directory one character away from `tests/chapter_verification`,
#        which IS in TEST_DIRS. The counter walks the tree in-process now.
#
#        The largest is tests/gorilla — 491 .naab files and 36 shell scripts,
#        including four assertions that grep C++ SOURCE TEXT for pattern names
#        (register B10). A source-text grep passes whether or not anything
#        fires; three of those four sat latent because nothing ran them.
#
#        This gate does NOT decide whether those directories should be adopted
#        or deleted. It pins the set so it cannot grow silently, which is the
#        part that can be settled without a judgement call.
#
# CV-02  SUITES THAT WRITE AN UNSIGNED govern.json WITHOUT TRUST ISOLATION.
#        With any key in the trust store, an unsigned govern.json is an
#        INTEGRITY BLOCK (exit 3) that --no-governance cannot escape. Suites
#        using tests/helpers/trust_setup.sh repoint NAAB_TRUST_STORE_DIR and
#        are safe; the rest inherit whatever the ambient store holds, so their
#        result depends on what else has run (register D8). Measured
#        2026-09-06: 81 of 165 such suites have no isolation. The register
#        said ~35.
#
# CV-03  REGISTERED SUITES THAT ARE NOT THERE.
#        run-all-tests.sh registers 88 shell suites by path, and every one of
#        the 113 "not found, skipping" branches is a silent pass: none of them
#        increments FAILED. Delete or rename any registered suite and CI stays
#        green without mentioning it. Measured by walking each branch back to
#        the if/elif that guards it: 112 of the 113 guard a path that is tracked
#        in git and therefore SHOULD exist; the single exception is the LSP
#        suite, whose script must exist but whose build/naab-lsp binary is
#        genuinely optional.
#
#        Found the way it usually is -- by accident. Switching git branches
#        under a running suite deleted a newly added test from the worktree; the
#        suite printed "not found, skipping" and exited 0, and it was only
#        noticed because someone happened to grep the log for that test's name.
#
#        This is asserted here rather than by editing 112 branches: one gate in
#        one place, with a control, beats 112 mechanical edits and 112 chances
#        to typo. It also keeps the assertion where a reader already looks for
#        "what can this suite not see about itself".
#
# CV-04  REGISTERED BUT UNTRACKED. A suite registered by path but not committed
#        exists only on the machine that wrote it, and skips silently for
#        everyone else -- the same silence as CV-03 with a longer fuse.
#
# CV-01/CV-02 are BASELINE gates: they fail when the number GROWS, not while it
# is nonzero. A gate that fails today would be switched off today. CV-03/CV-04
# are ABSOLUTE: the correct count is zero and it is zero today, so there is no
# baseline to erode.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO" || exit 1

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }

# Baselines. Raise ONLY with a reason; lowering is progress and is expected.
BASELINE_ORPHAN_DIRS=13
BASELINE_UNISOLATED=81

# Support directories: sourced by other tests or developer tooling, never run
# standalone. Not orphans.
SUPPORT_DIRS="helpers scripts"
export SUPPORT_DIRS

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Coverage visibility: what the suite cannot see about itself  |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

read -r ORPHAN_DIRS ORPHAN_FILES ORPHAN_LIST <<<"$(python3 - <<'PY'
import os, re
# Read with an EXPLICIT encoding. run-all-tests.sh contains non-ASCII (box
# rules, em dashes), and Python's default for open() is the locale's encoding
# -- cp1252 under MSYS2 on the Windows runner, which dies on those bytes with
# UnicodeDecodeError. This exact bug shipped once already in
# test_hivemind_governed.sh; LC_ALL=C does NOT reproduce it, because PEP 538
# coerces the C locale back to UTF-8. Reproduce with:
#   PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 LC_ALL=C bash tests/self-audit/test_coverage_visibility.sh
# Directory walking is done in-process rather than by shelling out to find,
# so there is no second locale-dependent decode to get wrong.
with open('run-all-tests.sh', encoding='utf-8', errors='replace') as fh:
    src = fh.read()
m = re.search(r'TEST_DIRS=\((.*?)\n\)', src, re.S)
covered = [l.strip().strip('"') for l in m.group(1).split('\n') if l.strip().startswith('"')]
support = set(os.environ["SUPPORT_DIRS"].split())
dirs = files = 0; names = []
for d in sorted(os.listdir('tests')):
    p = f'tests/{d}'
    if not os.path.isdir(p) or d in support: continue
    n = sum(1 for _, _, fs in os.walk(p) for f in fs if f.endswith('.naab'))
    if n == 0: continue
    # Covered if TEST_DIRS names it (or a descendant of it), or if
    # run-all-tests.sh names the path anywhere else -- a dedicated runner, as
    # tests/property, tests/vm, tests/differential and tests/governance each
    # have. The reference must end on a path boundary: a bare substring test
    # excuses tests/governance on the strength of "tests/governance_v3", which
    # is a namesake, not a runner. (It happens to have a real runner too, so
    # this correction does not move the baseline -- it removes the accident.)
    if any(p == c or c.startswith(p + '/') for c in covered): continue
    if re.search(re.escape(p) + r'(?=[/"\s])', src): continue
    dirs += 1; files += n; names.append(f"{d}:{n}")
print(dirs, files, ",".join(names))
PY
)"

echo "  orphaned dirs: $ORPHAN_DIRS   orphaned .naab files: $ORPHAN_FILES"
echo "  $ORPHAN_LIST" | tr ',' '\n' | sed 's/^/    /' | head -15
echo ""

if [ "${ORPHAN_DIRS:-999}" -le "$BASELINE_ORPHAN_DIRS" ]; then
    ok "CV-01" "no new directory has fallen outside the sweep ($ORPHAN_DIRS <= $BASELINE_ORPHAN_DIRS, $ORPHAN_FILES files)"
else
    bad "CV-01" "a directory with .naab files is reachable by no runner" \
        "$ORPHAN_DIRS orphaned dirs, baseline $BASELINE_ORPHAN_DIRS — add it to TEST_DIRS in run-all-tests.sh, give it a runner, or delete it. A test directory nobody runs reads as coverage while providing none."
fi

# Any redirect whose target ends in govern.json. An earlier, narrower form of
# this pattern required `cat >` or an UPPERCASE variable and undercounted by 3
# -- it missed a lowercase `$test_dir/govern.json`, a `$TMPDIR/gov/govern.json`
# and a bare relative `> govern.json`. The broad form is a strict superset:
# it caught those three and dropped none.
UNISO=0
for f in $(grep -rl "govern.json" tests --include='*.sh' 2>/dev/null); do
    grep -qE '>[[:space:]]*"?[^"[:space:]]*govern\.json"?' "$f" || continue
    grep -q "trust_setup.sh\|NAAB_TRUST_STORE_DIR" "$f" || UNISO=$((UNISO+1))
done
echo "  suites writing an unsigned govern.json with no trust isolation: $UNISO"
echo ""

if [ "$UNISO" -le "$BASELINE_UNISOLATED" ]; then
    ok "CV-02" "no new suite writes an unsigned govern.json unisolated ($UNISO <= $BASELINE_UNISOLATED)"
else
    bad "CV-02" "a new suite writes an unsigned govern.json without isolating the trust store" \
        "$UNISO unisolated, baseline $BASELINE_UNISOLATED — source tests/helpers/trust_setup.sh and call setup_isolated_trust, or its result depends on whatever else populated ~/.naab/trusted-keys"
fi

read -r MISSING MISSING_LIST UNTRACKED UNTRACKED_LIST <<<"$(python3 - <<'PY'
import os, re, subprocess
# Explicit UTF-8: run-all-tests.sh carries box rules and em dashes, and Python's
# default for open() is the locale's encoding -- cp1252 under MSYS2, which dies
# on those bytes. LC_ALL=C does NOT reproduce that (PEP 538 coerces it back to
# UTF-8); use PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 LC_ALL=C. See CLAUDE.md.
with open('run-all-tests.sh', encoding='utf-8', errors='replace') as fh:
    src = fh.read()
paths = sorted(set(re.findall(r'^[A-Z_]+=\"((?:tests|examples)/[^\"]+)\"', src, re.M)))
missing = [p for p in paths if not os.path.exists(p)]
untracked = []
for p in paths:
    if p in missing:
        continue
    if subprocess.run(['git', 'ls-files', '--error-unmatch', p],
                      capture_output=True).returncode != 0:
        untracked.append(p)
print(len(missing), ",".join(missing) or "-", len(untracked), ",".join(untracked) or "-")
PY
)"

echo "  registered suite paths: $(grep -cE '^[A-Z_]+=\"(tests|examples)/' run-all-tests.sh)   missing: $MISSING   untracked: $UNTRACKED"
echo ""

if [ "${MISSING:-99}" -eq 0 ]; then
    ok "CV-03" "every registered suite path exists"
else
    bad "CV-03" "a suite is registered in run-all-tests.sh but its file is not there" \
        "$MISSING missing: $MISSING_LIST — the suite would print 'not found, skipping' and exit 0, because none of the 113 skip branches increments FAILED. Restore the file, or remove its registration; do not leave it registered and absent."
fi

if [ "${UNTRACKED:-99}" -eq 0 ]; then
    ok "CV-04" "every registered suite path is tracked in git"
else
    bad "CV-04" "a suite is registered but not committed" \
        "$UNTRACKED untracked: $UNTRACKED_LIST — it runs only on the machine that wrote it and skips silently everywhere else. Commit it, or remove the registration."
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}coverage visibility: $PASS passed, 0 failed${NC}"
else
    echo -e "${RED}coverage visibility: $FAIL failed${NC}, $PASS passed"; exit 1
fi
