#!/usr/bin/env bash
# test_process_run_inline_gate.sh -- inline interpreter code passed through
# process.run gets the same governance checks as a <<python>> block and
# codegen.run().
#
# Found building repo-sentinel in NAAb: a helper that swallowed every error
# (`except Exception: return 0`) and reported every C++ file as 0 bytes. The
# project's own `code_quality.no_incomplete_logic` (HARD) blocks that code in a
# <<python>> block and in codegen.run(), but the program ran it as
# process.run("python3", ["-c", code]) and it went through silently.
#
# Every blocking arm asserts on a SIDE EFFECT (the code writes a file), not only
# on the exit code -- an exit code is not a block. The controls matter as much:
#   PI-00  the <<python>> block is blocked under this config (the reference)
#   PI-02  benign inline code still runs (a gate that refuses every
#          process.run("python3", ["-c", ...]) passes PI-01 for free)
#   PI-05  a non-interpreter command carrying the same text as an argument
#          is not treated as code
#   PI-06  a script FILE is not inline code; it is outside this gate (stated
#          scope, not an endorsement)

set -uo pipefail
PASS=0
FAIL=0
SKIP=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/naab_procgate.XXXXXX")"
[ -n "$WORK" ] && [ -d "$WORK" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'rm -rf "$WORK"; teardown_isolated_trust' EXIT

pass() { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
fail() { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "         $3"; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2 (UNMEASURABLE)"; SKIP=$((SKIP+1)); }

if [ ! -x "$NAAB" ]; then
    echo "FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi
PY="$(command -v python3 || true)"
if [ -z "$PY" ]; then
    echo "SKIP: python3 not available -- every arm needs a real interpreter (UNMEASURABLE)"
    exit 0
fi

cat > "$WORK/govern.json" <<'EOF'
{
  "version": "4.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "code_quality": { "no_incomplete_logic": { "enabled": true, "level": "hard" } }
}
EOF

# The swallowed-error helper, plus a marker write so "it ran" is observable.
BAD='import os\ndef size_of(p):\n    try:\n        return os.path.getsize(p)\n    except Exception:\n        return 0\nopen(\"MARK\", \"w\").write(\"x\")\nprint(size_of(\"missing.cpp\"))\n'
GOOD='import os\nopen(\"MARK\", \"w\").write(\"x\")\nprint(os.path.exists(\"MARK\"))\n'

# write_prog NAME CMD ARGS_EXPR MARK -- ARGS_EXPR is a NAAb list literal
write_prog() {
    local name="$1" cmd="$2" args="$3" mark="$4"
    printf 'use process\nmain {\n    let r = process.run("%s", %s)\n    print("EXIT:" + string(r["exit_code"]))\n}\n' \
        "$cmd" "${args//MARK/$mark}" > "$WORK/$name.naab"
}

run() {  # run FLAG NAME -> sets OUT, RC
    OUT="$(cd "$WORK" && timeout 30 "$NAAB" $1 "$WORK/$2.naab" 2>&1)"
    RC=$?
}

for eng in "" "--tree-walk"; do
    tag="${eng:-vm}"; tag="${tag#--}"
    echo "=== engine: $tag ==="

    # PI-00: reference -- the same code as a <<python>> block
    rm -f "$WORK"/m00
    printf 'main {\n    let x = <<python\nimport os\ndef size_of(p):\n    try:\n        return os.path.getsize(p)\n    except Exception:\n        return 0\nopen("m00", "w").write("x")\nsize_of("missing.cpp")\n>>\n    print(x)\n}\n' > "$WORK/p00.naab"
    run "$eng" p00
    if [ "$RC" -eq 3 ] && [ ! -e "$WORK/m00" ]; then
        pass "PI-00/$tag" "reference: the <<python>> block is HARD-blocked and did not run"
    else
        fail "PI-00/$tag" "reference block not blocked (rc=$RC) -- the config does not exercise the check, every other arm is void" "$(head -3 <<<"$OUT")"
    fi

    # PI-01: python3 -c <bad>
    rm -f "$WORK"/m01
    write_prog p01 python3 "[\"-c\", \"$BAD\"]" m01
    run "$eng" p01
    if [ "$RC" -eq 3 ] && [ ! -e "$WORK/m01" ]; then
        pass "PI-01/$tag" "process.run(\"python3\", [\"-c\", code]) gets the block's check"
    else
        fail "PI-01/$tag" "inline python ran past the check (rc=$RC, marker $( [ -e "$WORK/m01" ] && echo written || echo absent ))" "$(head -3 <<<"$OUT")"
    fi

    # PI-02: control -- benign inline code still runs
    rm -f "$WORK"/m02
    write_prog p02 python3 "[\"-c\", \"$GOOD\"]" m02
    run "$eng" p02
    if [ "$RC" -eq 0 ] && [ -e "$WORK/m02" ] && [[ "$OUT" == *"EXIT:0"* ]]; then
        pass "PI-02/$tag" "control: benign inline python still runs"
    else
        fail "PI-02/$tag" "benign inline python refused (rc=$RC)" "$(head -3 <<<"$OUT")"
    fi

    # PI-03: absolute interpreter path + clustered short flags (-Ic)
    rm -f "$WORK"/m03
    write_prog p03 "$PY" "[\"-Ic\", \"$BAD\"]" m03
    run "$eng" p03
    if [ "$RC" -eq 3 ] && [ ! -e "$WORK/m03" ]; then
        pass "PI-03/$tag" "absolute path and a clustered flag (-Ic) do not evade the check"
    else
        fail "PI-03/$tag" "evaded via path/flag spelling (rc=$RC)" "$(head -3 <<<"$OUT")"
    fi

    # PI-04: the same code reached through sh -c is shell, not python -- but a
    # python3 -c nested inside sh -c is still one hop away. Stated scope: only
    # the outer interpreter is recognised. Measure that a plain sh -c benign
    # command still runs (the shell mapping must not refuse ordinary use).
    rm -f "$WORK"/m04
    write_prog p04 sh '["-c", "echo x > MARK"]' m04
    run "$eng" p04
    if [ "$RC" -eq 0 ] && [ -e "$WORK/m04" ]; then
        pass "PI-04/$tag" "control: benign sh -c still runs"
    else
        fail "PI-04/$tag" "benign sh -c refused (rc=$RC)" "$(head -3 <<<"$OUT")"
    fi

    # PI-05: a non-interpreter command carrying the text is not code
    write_prog p05 echo "[\"-c\", \"$BAD\"]" m05
    run "$eng" p05
    if [ "$RC" -eq 0 ] && [[ "$OUT" == *"EXIT:0"* ]]; then
        pass "PI-05/$tag" "control: echo -c <text> is not treated as inline code"
    else
        fail "PI-05/$tag" "non-interpreter command refused (rc=$RC)" "$(head -3 <<<"$OUT")"
    fi

    # PI-06: a script file is outside this gate (scope, not endorsement)
    rm -f "$WORK"/m06
    printf "$BAD" | sed 's/MARK/m06/; s/\\"/"/g' > "$WORK/s06.py"
    write_prog p06 python3 '["s06.py"]' m06
    run "$eng" p06
    if [ "$RC" -eq 0 ] && [ -e "$WORK/m06" ]; then
        pass "PI-06/$tag" "scope: a script file is not inline code and is not scanned here"
    else
        fail "PI-06/$tag" "script-file behaviour changed (rc=$RC) -- update the stated scope" "$(head -3 <<<"$OUT")"
    fi
done

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
