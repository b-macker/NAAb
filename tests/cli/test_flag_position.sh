#!/usr/bin/env bash
# ============================================================
# test_flag_position.sh — a run option works before the file name as well as
# after it (docs/open-investigations.md A70(a))
#
# THE FAILURE THIS CATCHES
#
# main.cpp reads options twice: a global pre-scan before the command, and the
# run command's own loop after the file name (CLAUDE.md: "CLI flags must be in
# BOTH"). Seven run options were only in the second, so written first they
# were taken for the command: `naab-lang --governance-report r.json app.naab`
# printed "Unknown command: --governance-report" and exited 1. Observed before
# the fix (USER_GUIDE rewrite, 2026-10-25) for --governance-report,
# --governance-sarif, --governance-junit, --governance-record-baselines,
# --governance-check-baselines, --memory-limit and --allow-network. A CI recipe
# written option-first failed outright. The seven were found by listing every
# option the run loop accepts against the pre-scan's, not by trying the three
# a README review had hit.
#
#   FP-01..03  each report option, first and after: the report file is written
#   FP-04      --governance-record-baselines, first and after: baselines saved
#   FP-05      --governance-check-baselines, first and after: a changed block
#              output is refused against the recorded baseline
#   FP-06      --memory-limit N and --allow-network, first and after: the
#              program runs
#   FP-S       structural (screened, reads main.cpp): every option the run loop
#              accepts is also in the pre-scan, so the next one added to the run
#              loop alone fails here instead of in someone's CI recipe
#   FP-C1      CONTROL: an unknown option written first is still refused
#   FP-C2      CONTROL: FP-05's check passes when the output did not change,
#              so the refusal in FP-05 is the baseline and not the option
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2
    exit 1
fi

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-flagpos-XXXXXX")"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT

echo '{"version":"5.0","mode":"enforce","languages":{"allowed":["python"]}}' > "$W/govern.json"
printf 'main {\n    print("RAN")\n}\n' > "$W/t.naab"
cat > "$W/b.naab" <<'EOF'
main {
    let v = <<python
open("val.txt").read().strip()
>>
    print("V=" + v)
}
EOF

# naab ARGS... from the project directory -> OUT, RC
naab() { OUT=$(cd "$W" && timeout 60 "$NAAB" "$@" 2>&1); RC=$?; }
ran() { [ "$RC" = 0 ] && grep -qx 'RAN' <<<"$OUT"; }

# FP-01..03: each report option in both positions
for spec in "FP-01 --governance-report rep.json" "FP-02 --governance-sarif rep.sarif" "FP-03 --governance-junit rep.xml"; do
    set -- $spec; id=$1; opt=$2; file=$3
    for pos in first after; do
        rm -f "$W/$file"
        if [ "$pos" = first ]; then naab "$opt" "$file" t.naab; else naab t.naab "$opt" "$file"; fi
        if ran && [ -s "$W/$file" ]; then
            ok "$id/$pos" "$opt PATH written $pos the file name writes the report"
        else
            bad "$id/$pos" "$opt PATH $pos the file name should run and write $file" "rc=$RC $(grep -m1 -E 'Unknown|Error' <<<"$OUT")"
        fi
    done
done

# FP-04/05 and FP-C2 need embedded Python (the block's output is what a
# baseline records).
echo "A" > "$W/val.txt"
naab b.naab
if ! grep -qx 'V=A' <<<"$OUT"; then
    unm "FP-04" "the Python block did not run here, so baselines cannot be measured: $(grep -m1 -E 'Error|not available' <<<"$OUT")"
    unm "FP-05" "as FP-04"
    unm "FP-C2" "as FP-04"
else
    for pos in first after; do
        rm -rf "$W/.naab"
        echo "A" > "$W/val.txt"
        if [ "$pos" = first ]; then naab --governance-record-baselines b.naab; else naab b.naab --governance-record-baselines; fi
        if [ "$RC" = 0 ] && [ -s "$W/.naab/baselines.json" ]; then
            ok "FP-04/$pos" "--governance-record-baselines $pos the file name saves baselines"
        else
            bad "FP-04/$pos" "--governance-record-baselines $pos the file name should save .naab/baselines.json" "rc=$RC $(grep -m1 -E 'Unknown|Error' <<<"$OUT")"
            continue
        fi

        if [ "$pos" = first ]; then naab --governance-check-baselines b.naab; else naab b.naab --governance-check-baselines; fi
        if [ "$RC" = 0 ] && grep -qx 'V=A' <<<"$OUT"; then
            ok "FP-C2/$pos" "control: an unchanged output passes the baseline check"
        else
            bad "FP-C2/$pos" "an unchanged output should pass the baseline check" "rc=$RC $(grep -m1 -E 'Unknown|Error' <<<"$OUT")"
        fi

        echo "B" > "$W/val.txt"
        if [ "$pos" = first ]; then naab --governance-check-baselines b.naab; else naab b.naab --governance-check-baselines; fi
        if [ "$RC" = 3 ] && grep -qi 'baseline' <<<"$OUT"; then
            ok "FP-05/$pos" "--governance-check-baselines $pos the file name refuses a changed output"
        else
            bad "FP-05/$pos" "--governance-check-baselines $pos the file name should refuse a changed output (exit 3)" "rc=$RC $(grep -m1 -E 'Unknown|Error|baseline' <<<"$OUT")"
        fi
    done
fi

# FP-06: options whose effect this suite does not observe -- accepted and run
for opt in "--memory-limit 256" "--allow-network"; do
    for pos in first after; do
        if [ "$pos" = first ]; then naab $opt t.naab; else naab t.naab $opt; fi
        if ran; then
            ok "FP-06/$pos" "$opt $pos the file name is accepted and the program runs"
        else
            bad "FP-06/$pos" "$opt $pos the file name should be accepted" "rc=$RC $(grep -m1 -E 'Unknown|Error' <<<"$OUT")"
        fi
    done
done

# FP-S: the option lists of the two loops, read from the source. The run loop
# starts at its `arg_start` line and ends where the file name is checked; the
# pre-scan is the `while (command_arg_index < argc)` loop.
MAIN_CPP="$SCRIPT_DIR/../../src/cli/main.cpp"
if [ ! -f "$MAIN_CPP" ]; then
    unm "FP-S" "src/cli/main.cpp not found beside this suite"
else
    MISSING=$(python3 - "$MAIN_CPP" <<'PY'
import re, sys
src = open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n")
try:
    start = next(i for i, l in enumerate(src) if l.strip() == "while (command_arg_index < argc) {")
    end = next(i for i in range(start + 1, len(src)) if src[i] == "    }")
    rs = next(i for i, l in enumerate(src) if "int arg_start = auto_run ? command_arg_index : command_arg_index + 1;" in l)
    re_end = next(i for i in range(rs, len(src)) if "filename.empty()" in src[i])
except StopIteration:
    print("UNREADABLE"); sys.exit(0)
pre = set(re.findall(r'arg == "(--?[\w-]+)"', "\n".join(src[start:end])))
run = set(re.findall(r'arg == "(--?[\w-]+)"', "\n".join(src[rs:re_end])))
if len(pre) < 10 or len(run) < 10:
    print("UNREADABLE"); sys.exit(0)
print(" ".join(sorted(run - pre)))
PY
)
    if [ "$MISSING" = "UNREADABLE" ]; then
        unm "FP-S" "could not find the two option loops in main.cpp (the reader needs updating)"
    elif [ -z "$MISSING" ]; then
        ok "FP-S" "every run-loop option is also in the pre-scan"
    else
        bad "FP-S" "run-loop options missing from the pre-scan" "$MISSING"
    fi
fi

naab --no-such-option t.naab
if [ "$RC" != 0 ] && grep -q 'Unknown command: --no-such-option' <<<"$OUT"; then
    ok "FP-C1" "control: an unknown option written first is still refused"
else
    bad "FP-C1" "an unknown option written first should be refused" "rc=$RC $(head -2 <<<"$OUT")"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
