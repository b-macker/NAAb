#!/usr/bin/env bash
# test_no_std_regex.sh -- every regex in the engine goes through naab::rx (A89)
#
# libstdc++'s std::regex recurses once per character, so a long enough input
# crashed the governor (A89). The 485 call sites were moved to naab::rx
# (include/naab/rx.h, QuickJS's libregexp) in one change; this keeps a new one
# from coming back. It fails on any std::regex, std::smatch, std::cmatch,
# std::sregex_iterator, std::regex_search/_match/_replace, std::wregex or
# std::basic_regex in the engine's sources -- src/, include/ and the LSP --
# outside the wrapper itself. std::regex_error and std::regex_constants are
# allowed: naab::rx throws and takes those on purpose.
#
#   NR-00  CONTROL: the scan finds a planted std::regex_search, a planted
#          std::regex member and a planted std::smatch in a scratch tree, and
#          passes the allowed spellings -- the scan can fail, and is not
#          tripped by regex_error / regex_constants
#   NR-01  the engine's sources hold no std::regex use outside naab::rx
#
# Kill table (observed 2026-10-30): reintroduce one std::regex_search in
# src/runtime/governance_checks.cpp -> NR-01 red; widen the scan's allowlist to
# every file -> NR-00 red.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | sed 's/^/       -> /'; }

FORBIDDEN='std::(regex|wregex|basic_regex|smatch|cmatch|wsmatch|sregex_iterator|cregex_iterator|sregex_token_iterator|regex_search|regex_match|regex_replace)\b'
ALLOWED_FILES='^(include/naab/rx\.h|src/utils/rx\.cpp):'

# scan ROOT -> prints offending "path:line: text", relative to ROOT
scan() {
    local root="$1" out
    out=$(cd "$root" && grep -rnE --include='*.cpp' --include='*.cc' --include='*.h' --include='*.hpp' \
            "$FORBIDDEN" src include tools/naab-lsp 2>/dev/null)
    grep -vE "$ALLOWED_FILES" <<<"$out" | grep -v '^$' || true
}

echo "=== naab::rx is the only regex engine (A89) ==="

T=$(mktemp -d "${TMPDIR:-/tmp}/naab_norx_XXXXXX")
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/src/x" "$T/src/utils" "$T/include/naab" "$T/tools/naab-lsp"
printf 'bool f(const std::string& s){ return std::regex_search(s, re); }\n' > "$T/src/x/a.cpp"
printf 'struct S { std::regex compiled; };\n' > "$T/include/naab/b.h"
printf 'void g(){ std::smatch m; }\n' > "$T/tools/naab-lsp/c.cpp"
printf 'void h(){ try {} catch (const std::regex_error&) {} auto f = std::regex_constants::format_first_only; }\n' > "$T/src/x/ok.cpp"
printf 'std::regex r;  // the wrapper itself may use it\n' > "$T/src/utils/rx.cpp"
HITS=$(scan "$T")
if grep -q 'src/x/a.cpp' <<<"$HITS" && grep -q 'include/naab/b.h' <<<"$HITS" \
   && grep -q 'tools/naab-lsp/c.cpp' <<<"$HITS" && ! grep -q 'ok.cpp' <<<"$HITS" \
   && ! grep -q 'src/utils/rx.cpp' <<<"$HITS"; then
    pass NR-00 "CONTROL: the scan finds the planted uses and passes the allowed ones"
else
    fail NR-00 "the scan cannot be trusted" "$HITS"
fi

HITS=$(scan "$REPO")
if [ -z "$HITS" ]; then
    pass NR-01 "no std::regex use in the engine outside naab::rx"
else
    fail NR-01 "std::regex is back -- use naab::rx (include/naab/rx.h)" "$HITS"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
