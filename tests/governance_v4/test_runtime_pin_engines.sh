#!/usr/bin/env bash
# ============================================================
# test_runtime_pin_engines.sh -- runtime_versions pins hold on every engine
# and execution path
#
# Pins shipped (5d08322c) in the commit that made the VM the default engine,
# but were checked only on the tree-walker's polyglot path. On master a
# `runtime_versions` pin was never consulted on the VM, nor through codegen,
# and a pin on a runtime whose executor reports no version (everything but
# Python and SQL) passed silently on both engines. The check now runs inside
# checkPolyglotBlock(), which every execution path calls; an unreportable
# version is reported at the pin's level, never treated as a pass.
#
# SQL is the main measured runtime: it runs in-process on every platform.
# Every executor now reports the version of what it actually runs: embedded
# Python and QuickJS their linked library, subprocess runtimes their own
# binary's version line (the same binary the executor runs).
#
#   RP-00  CONTROL: with no pin, <<sql>> and <<javascript>> run (both engines)
#   RP-01  a hard pin SQL cannot meet blocks <<sql>> (exit 3, not run), both
#          engines; RP-01c: a pin it meets lets the block run
#   RP-02  a soft pin it cannot meet blocks too (exit 3), both engines
#   RP-03  an advisory pin warns and the block runs
#   RP-04  a pin whose version cannot be read blocks with "cannot be
#          determined": node under the default enforce sandbox, where starting
#          a process (even `node --version`) is not permitted
#   RP-05  the pin holds through codegen.run("sql"); RP-05c: a met pin runs
#   RP-06  a pin under "sql" holds for <<sqlite>> (one runtime, two tags)
#   RP-07  text-only checks do not judge the runtime: naab-gov check reports
#          no runtime_version finding; RP-07c: the same naab-gov does apply
#          the config it was given (a block list on sql fires)
#   RP-08  QuickJS reports its release date: a pin "2021-03" is met (runs),
#          ">=2099-01-01" and the template's "18" are not (exit 3)
#   RP-09  for every installed subprocess toolchain, the version a pin sees
#          is exactly the first line that binary prints itself (node, ruby,
#          bash, go, rustc, g++, php, tsx, nim, zig, julia, mcs); absent ones
#          are UNMEASURABLE, at least one must measure
#   RP-10  embedded Python reports the interpreter that runs <<python>>
#          blocks (it used to ask whatever python3 was first on PATH): the
#          pin sees the version the block itself reports
#   RP-12  an UNPINNED block starts no version probe: the per-block
#          execution audit record also reads the version, and a probe there
#          was a process launch per block (it broke the compile suites' stub
#          toolchains). A logging `node` first on PATH must see no
#          --version; RP-12c: with a pin it must (the probe does happen)
#   RP-11  every executor class main.cpp registers overrides
#          getRuntimeVersion() (itself or via its base) -- an executor added
#          without one would leave its pins permanently unverifiable
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
GOV="$REPO/build/naab-gov"

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== runtime_versions pins, every engine ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    skip RP-00 "naab-lang not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

PROG_SQL=$'main {\n    let r = <<sql\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
PROG_SQLITE=$'main {\n    let r = <<sqlite\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
PROG_JS=$'main {\n    let r = <<javascript\n1 + 1\n>>\n    print("RAN")\n}'
PROG_CG=$'use codegen\nmain {\n    let r = codegen.run("sql", "SELECT 1 AS v")\n    print("RAN")\n}'

mk() {  # $1 = dir, $2 = runtime_versions JSON array, $3 = program, [$4 = extra top-level JSON]
    mkdir -p "$W/$1"
    printf '{"mode":"enforce"%s,"runtime_versions":%s}\n' "${4:+,$4}" "$2" > "$W/$1/govern.json"
    printf '%s\n' "$3" > "$W/$1/p.naab"
}
run() {  # $1 = dir, $2 = "" or --tree-walk ; sets OUT and RC
    OUT="$(cd "$W/$1" && "$NAAB" ${2:+$2} p.naab --timeout 20 2>&1)"; RC=$?
}
ran() { [[ "$OUT" == *$'\nRAN'* || "$OUT" == RAN* ]]; }
pin() { printf '[{"language":"%s","required":"%s","level":"%s"}]' "$1" "$2" "$3"; }

mk c_sql '[]' "$PROG_SQL"
mk c_js  '[]' "$PROG_JS"
mk h_sql "$(pin sql '>=999' hard)" "$PROG_SQL"
mk m_sql "$(pin sql '>=0' hard)" "$PROG_SQL"
mk s_sql "$(pin sql '>=999' soft)" "$PROG_SQL"
mk a_sql "$(pin sql '>=999' advisory)" "$PROG_SQL"
PROG_NODE=$'main {\n    let r = <<node\n1 + 1\n>>\n    print("RAN")\n}'
mk h_node "$(pin node '>=0' hard)" "$PROG_NODE"
mk q_met  "$(pin javascript '2021-03' hard)" "$PROG_JS"
mk q_date "$(pin javascript '>=2099-01-01' hard)" "$PROG_JS"
mk q_18   "$(pin javascript '18' hard)" "$PROG_JS"
mk h_lite "$(pin sql '>=999' hard)" "$PROG_SQLITE"

JS_OK=1
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    run c_sql "$eng"
    if [ $RC -ne 0 ] || ! ran; then
        bad "RP-00/$e" "unpinned <<sql>> did not run (exit $RC) -- every arm below is void" "$OUT"
        continue
    fi
    run c_js "$eng"
    if [ $RC -eq 0 ] && ran; then ok "RP-00/$e" "with no pin, <<sql>> and <<javascript>> run"
    else ok "RP-00/$e" "with no pin, <<sql>> runs (javascript unavailable here)"; JS_OK=0; fi

    run h_sql "$eng"
    if [ $RC -eq 3 ] && ! ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok "RP-01/$e" "an unmet hard pin blocks <<sql>> (exit 3, not run)"
    else bad "RP-01/$e" "an unmet hard pin did not block (exit $RC)" "$OUT"; fi
    run m_sql "$eng"
    if [ $RC -eq 0 ] && ran; then ok "RP-01c/$e" "a met hard pin lets <<sql>> run"
    else bad "RP-01c/$e" "a pin SQL satisfies blocked it (exit $RC) -- RP-01 would prove nothing" "$OUT"; fi

    run s_sql "$eng"
    if [ $RC -eq 3 ] && ! ran; then ok "RP-02/$e" "an unmet soft pin blocks (exit 3)"
    else bad "RP-02/$e" "an unmet soft pin did not block (exit $RC)" "$OUT"; fi

    run a_sql "$eng"
    if [ $RC -eq 0 ] && ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok "RP-03/$e" "an unmet advisory pin warns and runs"
    else bad "RP-03/$e" "advisory pin: expected a warning and a run (exit $RC)" "$OUT"; fi

    run h_node "$eng"
    if [ $RC -eq 3 ] && ! ran && [[ "$OUT" == *"cannot be determined"* ]]; then
        ok "RP-04/$e" "a pin whose version cannot be read blocks (exit 3)"
    else bad "RP-04/$e" "an unverifiable pin passed, or was read anyway (exit $RC)" "$OUT"; fi

    if [ $JS_OK -eq 1 ]; then
        run q_met "$eng"; r1=$RC; o1="$OUT"; m1=0; ran && m1=1
        run q_date "$eng"; r2=$RC; o2="$OUT"
        run q_18 "$eng"; r3=$RC; o3="$OUT"
        if [ $r1 -eq 0 ] && [ $m1 -eq 1 ] && [ $r2 -eq 3 ] && [ $r3 -eq 3 ] \
           && [[ "$o2" == *"got 'QuickJS 2021-03-27'"* ]]; then
            ok "RP-08/$e" "QuickJS 2021-03-27: \"2021-03\" met, \">=2099-01-01\" and \"18\" not"
        else bad "RP-08/$e" "QuickJS version pins (exit $r1/$r2/$r3)" "$o1"$'\n'"$o2"$'\n'"$o3"; fi
    else
        skip "RP-08/$e" "javascript executor unavailable -- UNMEASURABLE"
    fi

    run h_lite "$eng"
    if [ $RC -eq 3 ] && ! ran; then ok "RP-06/$e" "a pin under \"sql\" blocks <<sqlite>>"
    else bad "RP-06/$e" "a pin under sql did not hold for <<sqlite>> (exit $RC)" "$OUT"; fi
done

# codegen: enforce upgrades the sandbox to standard, which refuses codegen
# outright -- elevated lets the pin be the thing that decides.
CG='"security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard"}'
mk g_unmet "$(pin sql '>=999' hard)" "$PROG_CG" "$CG"
mk g_met   "$(pin sql '>=0' hard)" "$PROG_CG" "$CG"
run g_met ""
if [ $RC -ne 0 ] || ! ran; then
    bad RP-05c "codegen.run(\"sql\") did not run under a met pin (exit $RC) -- RP-05 is void" "$OUT"
else
    ok RP-05c "codegen.run(\"sql\") runs under a met pin"
    run g_unmet ""
    if [ $RC -ne 0 ] && ! ran && [[ "$OUT" == *"Runtime version mismatch for sql"* ]]; then
        ok RP-05 "an unmet pin stops codegen.run(\"sql\") (exit $RC)"
    else bad RP-05 "codegen.run ignored the pin (exit $RC)" "$OUT"; fi
fi

# naab-gov check judges code text; it runs nothing, so it has no runtime.
if [ -x "$GOV" ] || [ -x "$GOV.exe" ]; then
    o7="$(printf 'SELECT 1;\n' | "$GOV" check --language sql --config-string \
        '{"mode":"enforce","runtime_versions":[{"language":"sql","required":">=999","level":"hard"}]}' 2>&1)"
    o7c="$(printf 'SELECT 1;\n' | "$GOV" check --language sql --config-string \
        '{"mode":"enforce","languages":{"blocked":["sql"]}}' 2>&1)"
    if [[ "$o7c" == *languages.blocked* ]]; then
        ok RP-07c "naab-gov check applies the config it is given"
        case "$o7" in *runtime_version*) bad RP-07 "naab-gov check judged a runtime it does not run" "$o7" ;;
            *) ok RP-07 "naab-gov check reports no runtime_version finding" ;; esac
    else
        bad RP-07c "naab-gov check did not apply a block list -- RP-07 would prove nothing" "$o7c"
    fi
else
    skip RP-07 "naab-gov not built -- UNMEASURABLE"
fi

# --- RP-09: subprocess runtimes report their own binary's version line ---
# tag|binary|args : the binary each executor runs (an independent oracle --
# written here, not read from the code under test).
ELEV='"security":{"sandbox_level":"elevated"}'
measured=0
for spec in "node|node|--version" "ruby|ruby|--version" "shell|bash|--version" \
            "go|go|version" "rust|rustc|--version" "cpp|g++|--version" \
            "php|php|--version" "typescript|tsx|--version" "nim|nim|--version" \
            "zig|zig|version" "julia|julia|--version" "csharp|mcs|--version"; do
    IFS='|' read -r tag bin arg <<< "$spec"
    if ! command -v "$bin" >/dev/null 2>&1; then
        skip "RP-09/$tag" "$bin not installed -- UNMEASURABLE"; continue
    fi
    want="$("$bin" "$arg" 2>&1 | tr -d '\r' | sed -n '/[^[:space:]]/{s/^[[:space:]]*//;s/[[:space:]]*$//;p;q;}')"
    mk "v_$tag" "$(pin "$tag" '>=99999' hard)" $'main {\n    let r = <<'"$tag"$'\n\n>>\n    print("RAN")\n}' "$ELEV"
    run "v_$tag" ""
    if [ $RC -eq 3 ] && [[ "$OUT" == *"got '$want'"* ]]; then
        ok "RP-09/$tag" "the pin sees $bin's own version line ($want)"; measured=$((measured+1))
    else
        bad "RP-09/$tag" "the pin did not see $bin's version line '$want' (exit $RC)" "$OUT"
    fi
done
[ $measured -gt 0 ] || bad RP-09 "no subprocess toolchain measured -- the arm proved nothing here"

# --- RP-10: embedded Python reports the interpreter that runs the blocks ---
PROG_PYV=$'main {\n    let v = <<python\nimport sys\nsys.version.split()[0]\n>>\n    print("PYV=" + v)\n}'
mk p_ver '[]' "$PROG_PYV"
run p_ver ""
pyv="$(printf '%s\n' "$OUT" | tr -d '\r' | sed -n 's/^PYV=//p' | head -1)"
if [ -z "$pyv" ]; then
    skip RP-10 "the python executor did not return its version -- UNMEASURABLE"
else
    mk p_pin "$(pin python '>=99999' hard)" $'main {\n    let r = <<python\n1\n>>\n    print("RAN")\n}'
    # A different python3 first on PATH (reports 0.0.1). An embedded
    # interpreter ignores PATH, so the block still reports $pyv and the pin
    # must too -- the old `python3 --version` probe read the impostor. If the
    # block itself changes under it, this executor runs python3 from PATH and
    # the impostor IS its runtime: unmeasurable here, not a failure.
    mkdir -p "$W/fakebin"
    printf '#!/bin/sh\necho "Python 0.0.1"\n' > "$W/fakebin/python3"; chmod +x "$W/fakebin/python3"
    OUT="$(cd "$W/p_ver" && PATH="$W/fakebin:$PATH" "$NAAB" p.naab --timeout 20 2>&1)"
    pyv2="$(printf '%s\n' "$OUT" | tr -d '\r' | sed -n 's/^PYV=//p' | head -1)"
    OUT="$(cd "$W/p_pin" && PATH="$W/fakebin:$PATH" "$NAAB" p.naab --timeout 20 2>&1)"; RC=$?
    if [ "$pyv2" != "$pyv" ]; then
        skip RP-10 "the python executor runs python3 from PATH (no embedded interpreter) -- UNMEASURABLE"
    elif [ $RC -eq 3 ] && [[ "$OUT" == *"got 'Python $pyv'"* ]]; then
        ok RP-10 "the pin sees Python $pyv, the embedded interpreter, not python3 on PATH"
    else
        bad RP-10 "the pin did not see the block's own interpreter (Python $pyv)" "$OUT"
    fi
fi

# --- RP-12: no version probe without a pin ---
realnode="$(command -v node 2>/dev/null || true)"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) realnode="" ;; esac
if [ -z "$realnode" ]; then
    skip RP-12 "node not installed (or Windows) -- UNMEASURABLE"
else
    mkdir -p "$W/logbin"
    printf '#!/bin/sh\necho "$*" >> "%s"\nexec "%s" "$@"\n' "$W/node-calls" "$realnode" > "$W/logbin/node"
    chmod +x "$W/logbin/node"
    PROG_N=$'main {\n    let r = <<node\n1 + 1\n>>\n    print("RAN")\n}'
    mk n_free '[]' "$PROG_N" "$ELEV"
    mk n_pin "$(pin node '>=0' hard)" "$PROG_N" "$ELEV"
    : > "$W/node-calls"
    OUT="$(cd "$W/n_free" && PATH="$W/logbin:$PATH" "$NAAB" p.naab --timeout 30 2>&1)"; RC=$?
    free_calls="$(cat "$W/node-calls")"
    : > "$W/node-calls"
    OUT2="$(cd "$W/n_pin" && PATH="$W/logbin:$PATH" "$NAAB" p.naab --timeout 30 2>&1)"; RC2=$?
    pin_calls="$(cat "$W/node-calls")"
    if [ $RC2 -eq 0 ] && [[ "$pin_calls" == *--version* ]]; then
        ok RP-12c "with a pin, node is asked for --version (and the met pin runs)"
        if [ $RC -eq 0 ] && [[ "$OUT" == *RAN* ]] && [[ -n "$free_calls" ]] && [[ "$free_calls" != *--version* ]]; then
            ok RP-12 "an unpinned <<node>> block runs without a version probe"
        else
            bad RP-12 "an unpinned block probed the version, or did not run through the logger (exit $RC)" "calls: $free_calls"$'\n'"$OUT"
        fi
    else
        bad RP-12c "a pinned block never asked node for its version (exit $RC2) -- RP-12 would prove nothing" "calls: $pin_calls"$'\n'"$OUT2"
    fi
fi

# --- RP-11: every registered executor can report a version ---
o11="$(python3 - "$REPO" <<'PY'
import re, sys, os, glob
repo = sys.argv[1]
main = open(os.path.join(repo, "src/cli/main.cpp"), encoding="utf-8", errors="replace").read()
classes = sorted(set(re.findall(r"registerExecutor\(\s*\"[^\"]+\",\s*std::make_unique<(?:naab::runtime::)?(\w+)>", main)))
src = ""
for f in glob.glob(os.path.join(repo, "include/naab/*.h")):
    src += open(f, encoding="utf-8", errors="replace").read() + "\n"
bases = dict(re.findall(r"class\s+(\w+)\s*(?:final\s*)?:\s*public\s+(?:\w+::)*(\w+)", src))
def body(cls):
    m = re.search(r"class\s+" + cls + r"\b[^;{]*\{", src)
    if not m: return None
    i, depth = m.end() - 1, 0
    for j in range(i, len(src)):
        depth += src[j] == "{"; depth -= src[j] == "}"
        if depth == 0: return src[i:j]
def reports(cls):
    while cls and cls != "Executor":
        b = body(cls)
        if b is None: return "no-header"
        if re.search(r"getRuntimeVersion\s*\(\s*\)\s*const\s*override", b): return "ok"
        cls = bases.get(cls)
    return "missing"
bad = [(c, r) for c in classes for r in [reports(c)] if r != "ok"]
print(len(classes))
for c, r in bad: print("BAD", c, r)
PY
)"
n11="$(printf '%s\n' "$o11" | head -1 | tr -d '\r')"
if [ "${n11:-0}" -ge 10 ] && ! printf '%s' "$o11" | grep -q '^BAD'; then
    ok RP-11 "all $n11 registered executor classes override getRuntimeVersion()"
else
    bad RP-11 "a registered executor cannot report its version (or the scan found too few classes)" "$o11"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
