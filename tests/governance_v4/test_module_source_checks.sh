#!/usr/bin/env bash
# ============================================================
# test_module_source_checks.sh -- an imported module gets the entry file's
# source checks, on both engines (docs/open-investigations.md A31)
#
# History, because the behaviour this reverses was pinned by a test:
#   247e229d (2026-03-12, EVA-8, anti-evasion) moved the function-body scan
#     from exported functions to ALL functions, so an adversarial author could
#     not route around the checks.
#   f9dd7e2c (the next day, a module-import PERFORMANCE fix) also skipped the
#     body scan during module loading "to avoid unnecessary overhead" -- which
#     reopened exactly that route: put the code in a module and import it.
#   3e7c94e1 / ae4bfb67 (2026-05-25) copied the skip to the VM for parity and
#     restored contracts, naming the HEURISTIC checks (oversimplification,
#     complexity floor, cosmetic sanitizer) as the ones to keep skipped.
#   00681b68 (2026-08-02) pinned what both engines did, for parity.
# No commit names secrets, PII, placeholders or incomplete logic as a reason.
# Project owner's decision (2026-10-09): run the entry file's source checks on
# every imported module, on both engines. The heuristics stay skipped.
#
#   MS-01..04  a placeholder / secret / PII / incomplete-logic return in an
#              imported module's function blocks the importer (exit 3, the
#              importer's main never runs). Each was exit 0 before.
#   MS-01c..04c CONTROL: the same violation in the ENTRY file is blocked -- the
#              fixture trips the check at all (both builds)
#   MS-05      a secret in a module's top-level `export let` blocks the
#              importer -- no function body holds it, so only a whole-source
#              check of the module can see it
#   MS-06      the same in the ENTRY file is blocked on the tree-walker too.
#              The VM already ran whole-source checks on the entry file; the
#              tree-walker ran per-function checks only, so it ran this.
#   MS-07      a secret two imports deep blocks (the check is per module
#              load, not per entry file)
#   MS-08c     CONTROL: a clean module imports and runs on both engines -- the
#              change does not refuse every import
#   MS-09      a heuristic (oversimplification) in a module still does NOT
#              block: ae4bfb67's decision, kept. MS-09c: the same stub in the
#              entry file is blocked, so MS-09 is not passing on a dead check
#   MS-10      a contracts-only config (no code_quality check enabled) blocks
#              an ENTRY-file must_call breach. checkNaabFunctionBody() used to
#              return before reaching the contract unless one of seven named
#              code_quality checks was on, while the module path called the
#              contract directly -- so the same function was blocked imported
#              and ran as the entry file. MS-10c: a satisfying body runs.
#   MS-11      a runtime rule that fires twice AFTER an import still produces
#              two rows with deduplicate_checks on. Checking a module stamps a
#              location; left set, later runtime rows look like located static
#              ones and dedup collapses them (test_dedup_runtime.sh's defect,
#              reopened through the import path). MS-11c: the same program
#              without the import also gives two rows. VM only: the
#              tree-walker emits no BSD events for stdlib calls at all
#              (docs/open-investigations.md B11), so the fixture gives 0 rows
#              there on every build.
#
# Against an interpreter that does nothing every blocked-arm fails.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

echo "=== Imported modules get the entry file's source checks ==="

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-modsrc.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
# Unsigned fixture configs: isolate the trust store, or a key trusted
# elsewhere turns every run into an integrity block.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

STRICT='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_secrets": { "enabled": true, "level": "hard" },
    "no_placeholders": { "enabled": true, "level": "hard" },
    "no_pii": { "enabled": true, "level": "hard" },
    "no_incomplete_logic": { "enabled": true, "level": "hard" } } }'
HEUR='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_oversimplification": { "enabled": true, "level": "hard" } } }'
CONTRACT='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "contracts": { "level": "hard", "functions": { "compute": { "must_call": ["math.sqrt"] } } } }'

# Function bodies, one per check.
BODY_TODO='fn compute(x) {\n    // TODO: handle negative input\n    return x\n}\n'
BODY_SECRET='fn compute(x) {\n    let k = "AKIAIOSFODNN7EXAMPLE"\n    return k\n}\n'
BODY_PII='fn compute(x) {\n    return "SSN 123-45-6789"\n}\n'
# The sentinel check keys on a validator's NAME: the same body named compute
# does not trip it (the MS-04c control caught that in this suite's first draft).
BODY_INCOMPLETE='fn validate_id(id) {\n    if id == null {\n        return "invalid-id"\n    }\n    return id\n}\n'
BODY_CLEAN='fn compute(x) {\n    return x * 2\n}\n'
BODY_STUB='fn compute(x) {\n    return []\n}\n'

# case <dir> <config>: fresh directory with that govern.json
newcase() { rm -rf "$W/$1"; mkdir -p "$W/$1"; printf '%s\n' "$2" > "$W/$1/govern.json"; }
# module <dir> <body>: mod.naab exporting the body's function
module() { printf "export $2" > "$W/$1/mod.naab"; }
importer() { printf 'import "mod.naab" as m\nmain {\n    print("RAN")\n}\n' > "$W/$1/main.naab"; }
entry() { printf "$2"'main {\n    print("RAN")\n}\n' > "$W/$1/main.naab"; }
# run <dir> <engine flag>: sets RC and OUT
run() { OUT="$(cd "$W/$1" && timeout 60 "$NAAB" run $2 main.naab 2>&1)"; RC=$?; }
ran() { case "$OUT" in *RAN*) return 0 ;; *) return 1 ;; esac; }
blocked_by() { [ "$RC" -eq 3 ] && ! ran && case "$OUT" in *"$1"*) true ;; *) false ;; esac; }

ENGINES="vm tw"
flag() { [ "$1" = tw ] && echo "--tree-walk" || echo ""; }

check_pair() {  # <id> <rule> <body> <label>
    local id="$1" rule="$2" body="$3" label="$4" e
    for e in $ENGINES; do
        newcase "$id-$e" "$STRICT"; module "$id-$e" "$body"; importer "$id-$e"
        run "$id-$e" "$(flag $e)"
        if blocked_by "$rule"; then
            pass "$id/$e" "$label in an imported module blocks the importer"
        else
            fail "$id/$e" "$label in an imported module was not caught" "exit=$RC ran=$(ran && echo yes || echo no)"
        fi
        newcase "${id}c-$e" "$STRICT"; entry "${id}c-$e" "$body"
        run "${id}c-$e" "$(flag $e)"
        if blocked_by "$rule"; then
            pass "${id}c/$e" "CONTROL: $label in the entry file is blocked"
        else
            fail "${id}c/$e" "CONTROL: the fixture does not trip $rule at all" "exit=$RC"
        fi
    done
}

check_pair MS-01 code_quality.no_placeholders "$BODY_TODO" "a TODO placeholder"
check_pair MS-02 code_quality.no_secrets "$BODY_SECRET" "a secret"
check_pair MS-03 code_quality.no_pii "$BODY_PII" "a PII string"
check_pair MS-04 code_quality.no_incomplete_logic "$BODY_INCOMPLETE" "an incomplete-logic sentinel return"

for e in $ENGINES; do
    # MS-05: top-level export in a module
    newcase "MS-05-$e" "$STRICT"
    printf 'export let KEY = "AKIAIOSFODNN7EXAMPLE"\nexport fn get() {\n    return 1\n}\n' > "$W/MS-05-$e/mod.naab"
    importer "MS-05-$e"; run "MS-05-$e" "$(flag $e)"
    if blocked_by code_quality.no_secrets; then
        pass "MS-05/$e" "a secret in a module's top-level export blocks the importer"
    else
        fail "MS-05/$e" "a module's top-level secret was not caught" "exit=$RC"
    fi
    # MS-06: top-level export in the entry file
    newcase "MS-06-$e" "$STRICT"
    printf 'export let KEY = "AKIAIOSFODNN7EXAMPLE"\nmain {\n    print("RAN")\n}\n' > "$W/MS-06-$e/main.naab"
    run "MS-06-$e" "$(flag $e)"
    if blocked_by code_quality.no_secrets; then
        pass "MS-06/$e" "a secret in the entry file's top-level export is blocked"
    else
        fail "MS-06/$e" "the entry file's top-level secret ran" "exit=$RC"
    fi
    # MS-07: two imports deep
    newcase "MS-07-$e" "$STRICT"
    printf "export $BODY_SECRET" > "$W/MS-07-$e/deep.naab"
    printf 'import "deep.naab" as d\nexport fn relay(x) {\n    return x\n}\n' > "$W/MS-07-$e/mod.naab"
    importer "MS-07-$e"; run "MS-07-$e" "$(flag $e)"
    if blocked_by code_quality.no_secrets; then
        pass "MS-07/$e" "a secret two imports deep blocks the importer"
    else
        fail "MS-07/$e" "a transitive module's secret was not caught" "exit=$RC"
    fi
    # MS-08c: clean module
    newcase "MS-08c-$e" "$STRICT"; module "MS-08c-$e" "$BODY_CLEAN"; importer "MS-08c-$e"
    run "MS-08c-$e" "$(flag $e)"
    if [ "$RC" -eq 0 ] && ran; then
        pass "MS-08c/$e" "CONTROL: a clean module imports and runs"
    else
        fail "MS-08c/$e" "CONTROL: a clean module was refused" "exit=$RC $(printf '%s' "$OUT" | grep -m1 -E 'Rule|rror')"
    fi
    # MS-09 / MS-09c: heuristic stays skipped for modules
    newcase "MS-09-$e" "$HEUR"; module "MS-09-$e" "$BODY_STUB"; importer "MS-09-$e"
    run "MS-09-$e" "$(flag $e)"
    if [ "$RC" -eq 0 ] && ran; then
        pass "MS-09/$e" "an oversimplified module function does not block (heuristics stay skipped)"
    else
        fail "MS-09/$e" "a heuristic check now blocks module imports" "exit=$RC"
    fi
    newcase "MS-09c-$e" "$HEUR"; entry "MS-09c-$e" "$BODY_STUB"
    run "MS-09c-$e" "$(flag $e)"
    if blocked_by code_quality.no_oversimplification; then
        pass "MS-09c/$e" "CONTROL: the same stub in the entry file is blocked"
    else
        fail "MS-09c/$e" "CONTROL: the stub fixture does not trip oversimplification" "exit=$RC"
    fi
    # MS-10 / MS-10c: contracts-only config, entry file
    newcase "MS-10-$e" "$CONTRACT"; entry "MS-10-$e" "$BODY_CLEAN"
    run "MS-10-$e" "$(flag $e)"
    if blocked_by must_call; then
        pass "MS-10/$e" "a contracts-only config blocks an entry-file must_call breach"
    else
        fail "MS-10/$e" "the entry file's contract breach ran" "exit=$RC"
    fi
    newcase "MS-10c-$e" "$CONTRACT"
    entry "MS-10c-$e" 'use math\nfn compute(x) {\n    return math.sqrt(x)\n}\n'
    run "MS-10c-$e" "$(flag $e)"
    if [ "$RC" -eq 0 ] && ran; then
        pass "MS-10c/$e" "CONTROL: a body that satisfies the contract runs"
    else
        fail "MS-10c/$e" "CONTROL: a satisfying body was refused" "exit=$RC"
    fi
done

# MS-11: runtime rows after an import are not collapsed by dedup
BSD_DEDUP='{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": { "enabled": true, "output_file": "tele.jsonl", "deduplicate_checks": true },
  "behavioral_sequences": { "enabled": true, "patterns": [
    { "name": "repeat_read", "sequence": ["file.read","file.read"], "max_gap": 20, "level": "advisory" } ] } }'
READS='    file.write("a.txt","x")\n    let a = file.read("a.txt")\n    let b = file.read("a.txt")\n    let c = file.read("a.txt")\n    let d = file.read("a.txt")\n    print("RAN")\n'
rows() {  # telemetry file -> repeat_read rows
    python3 -c '
import json, sys
n = 0
for line in sys.stdin:
    try: e = json.loads(line)
    except Exception: continue
    d = e.get("fields", e)
    if d.get("event_type") in ("RuleViolation", "GovernanceCheck") and "repeat_read" in (d.get("rule_name") or ""): n += 1
print(n)' < "$1"
}
for e in vm; do
    newcase "MS-11-$e" "$BSD_DEDUP"; module "MS-11-$e" "$BODY_CLEAN"
    printf 'use file\nimport "mod.naab" as m\nmain {\n'"$READS"'}\n' > "$W/MS-11-$e/main.naab"
    run "MS-11-$e" "$(flag $e)"
    n="$( [ -f "$W/MS-11-$e/tele.jsonl" ] && rows "$W/MS-11-$e/tele.jsonl" || echo none)"
    if [ "$n" = "2" ] && ran; then
        pass "MS-11/$e" "a runtime rule after an import keeps both rows under dedup"
    else
        fail "MS-11/$e" "runtime rows after an import were collapsed or lost" "rows=$n exit=$RC"
    fi
    newcase "MS-11c-$e" "$BSD_DEDUP"
    printf 'use file\nmain {\n'"$READS"'}\n' > "$W/MS-11c-$e/main.naab"
    run "MS-11c-$e" "$(flag $e)"
    n="$( [ -f "$W/MS-11c-$e/tele.jsonl" ] && rows "$W/MS-11c-$e/tele.jsonl" || echo none)"
    if [ "$n" = "2" ] && ran; then
        pass "MS-11c/$e" "CONTROL: without the import the fixture gives two rows"
    else
        fail "MS-11c/$e" "CONTROL: the fixture does not give two rows" "rows=$n exit=$RC"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
