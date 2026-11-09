#!/usr/bin/env bash
# test_export_fn_tooling.sh -- `export fn` is a function declaration to the
# parser's @intent and to naab-gov contracts (F-36, F-08; Bundle 17)
#
#   F-36  Parser::parseExportStmt() dropped a `/// @intent "..."` written above
#         `export fn` (parseProgram attached it only to a plain `fn`), so
#         intent validation asked every exported function for the @intent it
#         already had ("has no keyword overlap with project intent" ... "Add
#         /// @intent"). Observed on master 314bccfd.
#   F-08  `naab-gov contracts` collected only top-level `fn` declarations, so a
#         module's exported contracted function read "UNMEASURABLE -- not
#         declared in this file", exit 2. Its requires/ensures were enforced at
#         run time all along (observed); only the search could not find it.
#
#   IX-00c  CONTROL @intent above a plain fn is used (self-declared intent)
#   IX-01   @intent above export fn is used the same way
#   IX-02c  CONTROL naab-gov contracts searches a plain fn (exit 0)
#   IX-02   naab-gov contracts searches an export fn (exit 0, cases run)
#
# Kill table (each mutant applied alone, rebuilt, this suite run):
#   parseExportStmt without setIntent        IX-01
#   gov_contracts without the export loop    IX-02
# Red first (observed, master 314bccfd): IX-01 and IX-02 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_exportfn_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then echo "UNMEASURABLE: naab-gov not built"; exit 1; fi

intent_case() {   # intent_case ID KEYWORD
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "code_quality": {"intent_validation": {"enabled": true, "level": "advisory", "mode": "hybrid",
   "min_function_lines": 3, "project_intent": "Verify artifact digests and attestation reports"}}}\n' > "$d/govern.json"
    printf '/// @intent "Verify artifact digests"\n%sfn load_feed(path_in) {\n    let p = path_in.trim()\n    let n = len(p)\n    let q = p + "x"\n    return q\n}\nmain {\n    print("RAN=" + load_feed("a"))\n}\n' "$2" > "$d/p.naab"
    RC=0; OUT=$(cd "$d" && timeout 60s "$GOV" lint p.naab 2>&1) || RC=$?
}
used_intent()   { grep -q "intent_validation.self_declared" <<<"$OUT"; }
asked_intent()  { grep -q "has no keyword overlap with project intent" <<<"$OUT"; }

echo "=== export fn: @intent and naab-gov contracts ==="

intent_case ix00 ""
if used_intent && ! asked_intent; then pass IX-00c "CONTROL: @intent above a plain fn is used"
else fail IX-00c "@intent above a plain fn was not used (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

intent_case ix01 "export "
if used_intent && ! asked_intent; then pass IX-01 "@intent above export fn is used the same way"
else fail IX-01 "@intent above export fn was dropped (rc=$RC)" "$OUT"; fi

contracts_case() {   # contracts_case ID KEYWORD
    local d="$W/$1"; mkdir -p "$d"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "contracts": {"level": "hard", "functions": {"clamp": {"requires": ["n >= 0"], "ensures": ["result <= 10"]}}}}\n' > "$d/govern.json"
    printf '%sfn clamp(n) {\n    if n > 10 { return 10 }\n    return n\n}\nmain {\n    print(string(clamp(3)))\n}\n' "$2" > "$d/m.naab"
    RC=0; OUT=$(cd "$d" && timeout 120s "$GOV" contracts m.naab 2>&1) || RC=$?
}
# The search runs in a POSIX worker process; on Windows naab-gov says so and
# exits 4 (test_contract_search.sh is UNMEASURABLE there too).
search_unsupported() { [ "$RC" -eq 4 ] && grep -q "which this platform does not support" <<<"$OUT"; }

contracts_case ix02c ""
if search_unsupported; then unm IX-02c "the contract search runs in a worker process this platform does not support"
elif [ "$RC" -eq 0 ] && grep -qE "clamp: [1-9][0-9]* cases" <<<"$OUT"; then pass IX-02c "CONTROL: a plain fn is searched"
else fail IX-02c "a plain fn was not searched (rc=$RC)" "$OUT"; fi

contracts_case ix02 "export "
if search_unsupported; then unm IX-02 "the contract search runs in a worker process this platform does not support"
elif [ "$RC" -eq 0 ] && grep -qE "clamp: [1-9][0-9]* cases" <<<"$OUT" && ! grep -q "not declared" <<<"$OUT"; then
    pass IX-02 "an export fn is searched"
else fail IX-02 "an export fn was not found by naab-gov contracts (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$UNM"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
