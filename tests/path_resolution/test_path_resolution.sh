#!/usr/bin/env bash
# A123 (Bundle 16): a relative file path is resolved against the working
# directory, from the entry file and from an imported module alike.
#
# This replaces tests/path_resolution/main.naab, which expected a module's
# `file.read("./data.txt")` to find the file beside the MODULE. That was never
# built: `Interpreter::resolveRelativePath()` (3e38c203, "Issue #3") was
# declared and defined but never called (`git log -S resolveRelativePath`: the
# definition and the declaration only). The engine opens a relative path where
# the OS does, against the process's working directory, and the path policy
# judges it there too (CLAUDE.md: "A relative path target is resolved against
# the process cwd"). main.naab ran from the repo root, printed
# "FAILED: Module could not find data.txt" and exited 0 (A123).
#
# A .naab test cannot check this: the suite runs every .naab from the repo
# root, so the answer depends on where it is run. Each arm here builds its
# fixture in a scratch directory and chooses the working directory. Three
# data.txt files with distinct contents tell the three possible bases apart:
# the entry file's directory (ENTRY_DIR), the module's directory (MODULE_DIR)
# and the working directory (CWD_DIR). Every arm runs on the VM and on the
# tree-walker (no govern.json is found, so --no-governance only waives that).
#
#   PR-00  CONTROL: the entry file reads ./data.txt from its own directory as
#          cwd -- the harness reads a value at all
#   PR-01  the module reads ./data.txt with cwd = the module's directory: the
#          module's file (what main.naab expected, true only because cwd is there)
#   PR-02  cwd = the entry file's directory: the module reads the ENTRY
#          directory's file, not its own -- the load-bearing arm
#   PR-03  cwd = a third directory, the program named by path: the entry file
#          and the module both read the working directory's file
#   PR-04  cwd = a directory with no data.txt: the module's file.exists is
#          false although its own directory holds one
#
# Red first: none -- this suite pins existing behaviour (observed 2026-11-06,
# 01fb3552); the old main.naab printed its FAILED line under the same
# behaviour. Its control against a dead interpreter is that every arm needs a
# printed value.
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
case "$NAAB" in /*) ;; *) NAAB="$PWD/$NAAB" ;; esac

PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       /'; }

echo "=== A relative path resolves against the working directory ==="
if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then
    echo "  FAIL naab-lang not built (UNMEASURABLE, not a pass)"; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-pathres.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# The fixture: an entry file in $W/app importing $W/app/lib/reader.naab.
mkdir -p "$W/app/lib" "$W/cwd" "$W/empty"
printf 'ENTRY_DIR' > "$W/app/data.txt"
printf 'MODULE_DIR' > "$W/app/lib/data.txt"
printf 'CWD_DIR' > "$W/cwd/data.txt"
cat > "$W/app/lib/reader.naab" <<'EOF'
export fn readLocal() -> string {
    if (file.exists("./data.txt")) {
        return file.read("./data.txt")
    }
    return "ABSENT"
}
EOF
cat > "$W/app/main.naab" <<'EOF'
import { readLocal } from "./lib/reader.naab"

main {
    let mine = "ABSENT"
    if (file.exists("./data.txt")) {
        mine = file.read("./data.txt")
    }
    print("MAIN=" + mine)
    print("MOD=" + readLocal())
}
EOF

# arm ID CWD WANT_MAIN WANT_MOD WHY: run app/main.naab from CWD on both engines
arm() {
    local id="$1" cwd="$2" want_main="$3" want_mod="$4" why="$5"
    local eng out rc m d bad_eng=""
    for eng in vm tw; do
        local flag=""; [ "$eng" = tw ] && flag="--tree-walk"
        out=$(cd "$cwd" && timeout 30 "$NAAB" --no-governance $flag "$W/app/main.naab" </dev/null 2>&1); rc=$?
        m=$(grep -o 'MAIN=.*' <<<"$out" | head -1 | tr -d '\r')
        d=$(grep -o 'MOD=.*' <<<"$out" | head -1 | tr -d '\r')
        if [ "$rc" -ne 0 ] || { [ -n "$want_main" ] && [ "$m" != "MAIN=$want_main" ]; } || [ "$d" != "MOD=$want_mod" ]; then
            bad_eng="$bad_eng $eng:rc=$rc:${m:-none}:${d:-none}"
        fi
    done
    if [ -z "$bad_eng" ]; then ok "$id" "$why"
    else bad "$id" "$why" "$bad_eng"$'\n'"$out"; fi
}

arm PR-00 "$W/app" ENTRY_DIR ENTRY_DIR \
    "CONTROL: cwd = the entry file's directory, the entry file reads ENTRY_DIR"
arm PR-01 "$W/app/lib" MODULE_DIR MODULE_DIR \
    "cwd = the module's directory: the module reads MODULE_DIR"
arm PR-02 "$W/app" "" ENTRY_DIR \
    "cwd = the entry directory: the module reads ENTRY_DIR, not its own MODULE_DIR"
arm PR-03 "$W/cwd" CWD_DIR CWD_DIR \
    "cwd = a third directory: entry file and module both read CWD_DIR"
arm PR-04 "$W/empty" ABSENT ABSENT \
    "cwd holds no data.txt: the module finds none, though its own directory has one"

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
