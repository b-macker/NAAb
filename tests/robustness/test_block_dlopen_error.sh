#!/usr/bin/env bash
# ============================================================
# test_block_dlopen_error.sh -- a C++ block library that fails to load says why
#
# CppExecutor::loadCompiledBlock() called dlerror() twice after a failed
# dlopen(). dlerror() returns the message once and then NULL, and the second
# call was handed to std::string -- which threw. So a corrupt or truncated
# cached library ended the run with "basic_string: construction from null is
# not valid" instead of the loader's own reason.
#
#   DE-00  CONTROL: the block compiles, loads and returns 7 -- the fixture works
#   DE-01  after the cached .so is truncated, the run reports the loader's
#          reason ("file too short") and never the std::string error
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }
skip_all() { for id in DE-00 DE-01; do skip "$id" "$1"; done
             echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0; }

echo "=== A block library that fails to load reports why ==="

[ -x "$NAAB" ] || skip_all "naab-lang not built (UNMEASURABLE)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) skip_all "block libraries use dlopen -- POSIX-only (UNMEASURABLE here)" ;; esac
command -v g++ >/dev/null 2>&1 || command -v clang++ >/dev/null 2>&1 || skip_all "no C++ compiler (UNMEASURABLE)"

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-dlerr.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

H="$W/home"   # its own HOME, so the block cache starts empty and is ours to corrupt
mkdir -p "$H/.naab/language/blocks/library/cpp"
echo '{ "version": "4.0", "mode": "off" }' > "$W/govern.json"
printf 'extern "C" int seven() { return 7; }\n' > "$H/.naab/language/blocks/library/cpp/BLOCK-CPP-DLERR.cpp"
printf 'use BLOCK-CPP-DLERR as b\nmain {\n    print(b.seven())\n}\n' > "$W/b.naab"

run() { (cd "$W" && HOME="$H" timeout 120 "$NAAB" --tree-walk b.naab 2>&1); }

out=$(run)
so=$(find "$H" -name 'BLOCK_LIB_*.so' 2>/dev/null | head -1)
if grep <<<"$out" -qx "7" && [ -n "$so" ]; then
    ok "DE-00" "CONTROL: the block compiles, loads and returns 7"
else
    skip "DE-00" "the block did not compile and load here (UNMEASURABLE): $(printf '%s' "$out" | tail -1)"
    skip "DE-01" "no working fixture to corrupt (UNMEASURABLE)"
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

head -c 16 "$so" > "$W/trunc" && mv "$W/trunc" "$so"   # a cached library dlopen() must reject
out=$(run)
case "$out" in
    *"basic_string"*) bad "DE-01" "the load failure was reported as a std::string error" "$(printf '%s' "$out" | grep -m1 basic_string)" ;;
    *"file too short"*) ok "DE-01" "the loader's own reason is reported (file too short)" ;;
    *) bad "DE-01" "the truncated library's failure was not reported" "$(printf '%s' "$out" | tail -2 | tr '\n' ' ')" ;;
esac

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
