#!/usr/bin/env bash
# ============================================================
# run.sh -- template-vs-default comparison (REPORT ONLY; not wired into CI)
#
# Answers: for each setting in a govern template, does copying the template's
# value give the same parsed governance rules as leaving the key out?
# Findings from the first run: docs/findings/template-vs-defaults.md (PR #281).
#
# Usage:  bash tools/template_defaults/run.sh [TEMPLATE] [OUTDIR]
#         TEMPLATE defaults to govern-template.json, OUTDIR to a new temp dir.
#
# Needs: a configured build dir with naab-lang built (its static libraries and
# link line are reused -- BUILD=... to point elsewhere), clang++ (for the JSON
# AST that lists struct fields), python3. Runs ~4000 config loads, about 3 min.
# ============================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="${BUILD:-$REPO/build}"
TEMPLATE="$(cd "$(dirname "${1:-$REPO/govern-template.json}")" && pwd)/$(basename "${1:-$REPO/govern-template.json}")"
OUT="${2:-$(mktemp -d)}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

die() { echo "run.sh: $*" >&2; exit 2; }
[ -f "$TEMPLATE" ] || die "template not found: $TEMPLATE"
[ -x "$BUILD/naab-lang" ] || die "no $BUILD/naab-lang -- build it first (mkdir -p build && cd build && cmake .. && make naab-lang)"
LINK="$BUILD/CMakeFiles/naab-lang.dir/link.txt"
FLAGS="$BUILD/CMakeFiles/naab-lang.dir/flags.make"
[ -f "$LINK" ] && [ -f "$FLAGS" ] || die "missing $LINK or $FLAGS"
command -v clang++ >/dev/null || die "clang++ not found (needed for the struct AST)"
command -v python3 >/dev/null || die "python3 not found"

echo "[1/5] struct field list from the clang AST"
echo '#include "naab/governance.h"' > "$OUT/tu.cpp"
clang++ -std=c++17 -fsyntax-only -I"$REPO/include" -I"$REPO/external/abseil-cpp" \
    -I"$REPO/external/fmt/include" -I"$REPO/external/json/include" \
    -I"$REPO/external/json/single_include" -I"$BUILD/include" \
    -Xclang -ast-dump=json -Xclang -ast-dump-filter=naab:: "$OUT/tu.cpp" > "$OUT/ast_naab.txt"
python3 "$HERE/ast_records.py" "$OUT/ast_naab.txt" "$OUT/records.json"

echo "[2/5] generate and compile the rules dumper"
python3 "$HERE/gen_dumper.py" "$OUT/records.json" "$OUT/dumper.cpp"
CXX_FLAGS=$(grep '^CXX_FLAGS' "$FLAGS" | cut -d= -f2-)
CXX_DEFINES=$(grep '^CXX_DEFINES' "$FLAGS" | cut -d= -f2-)
CXX_INCLUDES=$(grep '^CXX_INCLUDES' "$FLAGS" | cut -d= -f2-)
( cd "$BUILD" && eval "c++ $CXX_DEFINES $CXX_INCLUDES $CXX_FLAGS -O0 -w -c '$OUT/dumper.cpp' -o '$OUT/dumper.o'" )

echo "[3/5] link against naab-lang's own libraries"
# Same link line as naab-lang with main.o swapped for the dumper. The archive
# group is needed because without main.o nothing pulls interpreter symbols in
# early enough for the later archives to resolve them.
L=$(sed -e "s#\"CMakeFiles/naab-lang.dir/src/cli/main.cpp.o\"#$OUT/dumper.o#" \
        -e 's#"CMakeFiles/naab-lang.dir/src/cli/[a-z_]*.cpp.o"##g' \
        -e "s#-o naab-lang#-o $OUT/dumper -Wl,--start-group#" "$LINK")
( cd "$BUILD" && eval "$L -Wl,--end-group" )

echo "[4/5] positive control: the dumper must see a known default and a one-key change"
echo '{}' > "$OUT/ctl_empty.json"
a=$("$OUT/dumper" "$OUT/ctl_empty.json" 2>/dev/null | grep -P '^R\.context_drift\.adaptive_baseline_enabled\t' | cut -f2)
# Write the opposite of whatever the default is, so a future flip of the
# default does not turn this control into a false alarm.
if [ "$a" = "true" ]; then v=false; else v=true; fi
echo "{\"context_drift\":{\"adaptive_baseline_enabled\":$v}}" > "$OUT/ctl_flip.json"
b=$("$OUT/dumper" "$OUT/ctl_flip.json" 2>/dev/null | grep -P '^R\.context_drift\.adaptive_baseline_enabled\t' | cut -f2)
if [ -n "$a" ] && [ "$b" = "$v" ] && [ "$a" != "$b" ]; then
    echo "      control ok (default=$a, flipped=$b)"
else
    die "positive control FAILED (default='$a' flipped='$b') -- results would be UNMEASURABLE"
fi

echo "[5/5] compare (deletion, perturbation, isolation, alias masking) + scanner"
python3 "$HERE/compare.py" "$TEMPLATE" "$OUT/dumper" "$OUT"
python3 "$HERE/scanner_compare.py" "$REPO" "$TEMPLATE" > "$OUT/scanner.tsv"
echo "results in $OUT (leaf_diffs.tsv, masked.tsv, isolate.json, scanner.tsv)"
