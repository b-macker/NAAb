#!/usr/bin/env bash
# ============================================================
# test_polyglot_gate_coverage.sh -- every language, not the ones we remembered
#
# WHY THIS EXISTS. The sandbox capability check for polyglot execution was
# written per executor, sixteen times, and most of the copies were wrong:
# some never had it, and GenericSubprocessExecutor (php, typescript) put it on
# execute() while polyglot blocks go through executeWithReturn(). Measured on
# 96acf93 under sandbox_level "restricted": python, ruby, node and shell were
# refused; RUST AND PHP RAN AND WROTE FILES TO /tmp, and go got as far as
# starting its runtime before a resource limit stopped it.
#
# A per-language test would have found the two we happened to name. This test
# asks the BINARY which languages it has registered and requires a case for
# each, so a new executor is covered the day it is registered rather than the
# day someone remembers it. That is the whole point: the gap was never
# "somebody wrote a bad check", it was "somebody added a language".
#
# WHAT COUNTS AS EXECUTION. The assertion is a file the block writes. Stdout
# would be the obvious choice and does not work here: a polyglot block's stdout
# is captured rather than forwarded, so a language that ran successfully prints
# nothing the test can see. The first draft of this file asserted on a printed
# token and reported fifteen of nineteen languages UNMEASURABLE, including ones
# already measured as running. A marker file is also the stronger claim: it is
# a side effect outside the interpreter, on a sandbox level that grants no
# write capability at all.
#
# EVERY CASE NEEDS ITS POSITIVE CONTROL, and here it is per language rather
# than per suite: the same block must RUN at "elevated". A language whose
# toolchain is missing or whose snippet is wrong cannot produce the token in
# either arm, and without the control that reads as a pass -- the exact way
# the first PHP fixture in this campaign reported "blocked" when really it had
# been handed invalid source. A language that does not run at elevated is
# reported UNMEASURABLE, never PASS.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Polyglot sandbox gate: every registered language ==="

if [ ! -x "$NAAB" ]; then
    skip "PG-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

WDIR="$(mktemp -d)"
trap 'rm -rf "$WDIR"' EXIT
cd "$WDIR" || exit 1

cfg() {  # $1 = sandbox level
    cat > "$WDIR/govern.json" <<EOF
{
  "version": "4.0",
  "mode": "enforce",
  "security": { "sandbox_level": "$1" },
  "codegen": { "enabled": true }
}
EOF
}

# ---- the language list comes FROM THE BINARY ----------------------------
cfg elevated
cat > "$WDIR/langs.naab" <<'EOF'
use codegen
main {
    let langs = codegen.supported_languages()
    for l in langs { print(l) }
}
EOF
REGISTERED="$("$NAAB" "$WDIR/langs.naab" 2>/dev/null | grep -E '^[a-z]+$' | sort -u)"
REG_COUNT="$(echo "$REGISTERED" | grep -c . || true)"
if [ "${REG_COUNT:-0}" -lt 2 ]; then
    skip "PG-00" "could not enumerate languages from the binary -- UNMEASURABLE"
    report; exit 0
fi
ok "PG-00" "binary reports $REG_COUNT registered languages"

# ---- one snippet per language, each printing the token ------------------
# Each snippet writes MARKER. Absence of MARKER is the gate holding; presence
# is execution, and on a "restricted" sandbox it is also a write the config
# does not permit.
snippet_for() {  # $1 = language, $2 = marker path
    local m="$2"
    case "$1" in
        bash|sh|shell)      printf 'echo x > %s\n' "$m" ;;
        python)             printf 'open("%s","w").write("x")\n' "$m" ;;
        ruby)               printf 'File.write("%s","x")\n' "$m" ;;
        node|ts|typescript) printf 'require("fs").writeFileSync("%s","x")\n' "$m" ;;
        javascript)         printf 'var f = std.open("%s","w"); f.puts("x"); f.close();\n' "$m" ;;
        php)                printf '<?php\nfile_put_contents("%s","x");\n' "$m" ;;
        rust)               printf 'use std::fs;\nfn main() { fs::write("%s","x").ok(); }\n' "$m" ;;
        go|golang)          printf 'package main\nimport "os"\nfunc main() { os.WriteFile("%s", []byte("x"), 0644) }\n' "$m" ;;
        cpp)                printf '#include <fstream>\nint main() { std::ofstream o("%s"); o << "x"; return 0; }\n' "$m" ;;
        cs|csharp)          printf 'class P { static void Main() { System.IO.File.WriteAllText("%s","x"); } }\n' "$m" ;;
        nim)                printf 'writeFile("%s", "x")\n' "$m" ;;
        zig)                printf 'const std = @import("std");\npub fn main() !void { try std.fs.cwd().writeFile(.{ .sub_path = "%s", .data = "x" }); }\n' "$m" ;;
        julia)              printf 'write("%s", "x")\n' "$m" ;;
        *)                  return 1 ;;
    esac
}

# Returns 0 when the block EXECUTED (marker written), 1 when it did not.
run_block() {  # $1=language $2=sandbox level
    local lang="$1" level="$2" body marker
    marker="$WDIR/marker_${lang}_${level}.txt"
    rm -f "$marker"
    body="$(snippet_for "$lang" "$marker")" || return 2
    cfg "$level"
    {
        echo 'main {'
        echo "    <<$lang"
        printf '%s\n' "$body"
        echo '>>'
        echo '}'
    } > "$WDIR/blk.naab"
    timeout 120 "$NAAB" "$WDIR/blk.naab" >/dev/null 2>&1
    [ -f "$marker" ]
}

# ---- PG-01: every registered language must have a case ------------------
# Coverage by construction. Registering an executor without adding a snippet
# here fails the suite, which is the only reason this test stays honest as
# languages are added.
MISSING=""
for lang in $REGISTERED; do
    snippet_for "$lang" /dev/null >/dev/null 2>&1 || MISSING="$MISSING $lang"
done
if [ -z "$MISSING" ]; then
    ok "PG-01" "every registered language has a test case"
else
    bad "PG-01" "registered languages with no case (add one):$MISSING"
fi

# ---- PG-02..N: per language, control first then the gate ----------------
echo "--- per language: must RUN at elevated, must NOT run at restricted"
for lang in $REGISTERED; do
    snippet_for "$lang" /dev/null >/dev/null 2>&1 || continue

    if ! run_block "$lang" elevated; then
        skip "PG-$lang" "does not run at elevated (toolchain or snippet) -- UNMEASURABLE"
        continue
    fi

    if run_block "$lang" restricted; then
        bad "PG-$lang" "EXECUTED under a restricted sandbox"
    else
        ok "PG-$lang" "refused under a restricted sandbox"
    fi
done

report
[ $FAIL -eq 0 ] || exit 1
exit 0
