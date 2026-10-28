#!/usr/bin/env bash
# ============================================================
# test_manifest_unapplied.sh -- naab.toml security keys that nothing applies
# are reported, naming the govern.json key that does (A28, Bundle 3)
#
# `naab-lang run` loads naab.toml and uses it only to print the package name
# under --verbose (traced, A28): `[polyglot] python = false` and
# `[features] sandbox_mode = false` change nothing, silently, and the default
# manifest's comment says the [polyglot] section picks "which language
# runtimes to enable". By decision there is no second policy source: the keys
# stay unapplied, and a value that asks for something NAAb is not doing is
# reported on every run and by `naab-lang manifest check`, naming the
# govern.json key that does it.
#
#   MF-00  CONTROL: the probe program's <<python>> block runs here
#   MF-01  [polyglot] python = false: the block still runs (unapplied, as
#          before) and stderr names languages.blocked in govern.json
#   MF-02  [features] sandbox_mode = false: stderr names security.sandbox_level
#   MF-03  `naab-lang manifest check` reports the same keys
#   MF-04  CONTROL: the default naab.toml (`createDefaultManifest`, every value
#          at its default) prints no such warning
#   MF-05  CONTROL: no naab.toml prints no such warning
#
# Kill table (each mutant rebuilt, this suite run):
#   the run path's report removed                   MF-01, MF-02
#   the manifest check's report removed             MF-03
#   every polyglot value reported, not only false   MF-04
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${NAAB:-$SCRIPT_DIR/../../build/naab-lang}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo "=== naab.toml keys that nothing applies are reported ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-manifest-XXXXXX")"
trap 'rm -rf "$W"' EXIT

PROG='main {
    let v = <<python
"PY" + "RAN"
>>
    print("V=" + string(v))
}'
# run NAME TOML -> OUT ERR RC (TOML "" = no naab.toml); config-free directory
run() {
    local d="$W/$1"; mkdir -p "$d"
    [ -n "$2" ] && printf '%s\n' "$2" > "$d/naab.toml"
    printf '%s\n' "$PROG" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run --no-governance p.naab 2>"$d/err.txt")"; RC=$?
    ERR="$(cat "$d/err.txt")"
}
PKG='[package]
name = "probe"
version = "0.1.0"'

run mf0 ""
if [ "$RC" = 0 ] && [[ "$OUT" == *V=PYRAN* ]]; then ok "MF-00" "control: the <<python>> block runs"
else unm "MF-00" "no <<python>> executor here: MF-01's 'still runs' half is unmeasured ($OUT)"; fi
PY_OK=$([ "$RC" = 0 ] && [[ "$OUT" == *V=PYRAN* ]] && echo 1 || echo 0)

run mf1 "$PKG
[polyglot]
python = false"
if [[ "$ERR" == *"python = false"* && "$ERR" == *"languages.blocked"* ]] && { [ "$PY_OK" = 0 ] || [[ "$OUT" == *V=PYRAN* ]]; }; then
    ok "MF-01" "[polyglot] python = false: reported, naming languages.blocked; the block still runs"
else bad "MF-01" "an unapplied python = false was not reported" "rc=$RC out=$OUT err=$ERR"; fi

run mf2 "$PKG
[features]
sandbox_mode = false"
if [[ "$ERR" == *"sandbox_mode = false"* && "$ERR" == *"security.sandbox_level"* ]]; then
    ok "MF-02" "[features] sandbox_mode = false: reported, naming security.sandbox_level"
else bad "MF-02" "an unapplied sandbox_mode = false was not reported" "err=$ERR"; fi

C="$(cd "$W/mf1" && "$NAAB" manifest check 2>&1)"
if [[ "$C" == *"python = false"* && "$C" == *"languages.blocked"* ]]; then
    ok "MF-03" "manifest check reports the unapplied key"
else bad "MF-03" "manifest check said nothing about python = false" "$C"; fi

mkdir -p "$W/mf4"
if (cd "$W/mf4" && "$NAAB" init >/dev/null 2>&1) && [ -f "$W/mf4/naab.toml" ]; then
    rm -f "$W/mf4/govern.json"
    printf '%s\n' "$PROG" > "$W/mf4/p.naab"
    E="$(cd "$W/mf4" && timeout 60 "$NAAB" run --no-governance p.naab 2>&1 >/dev/null)"
    if [[ "$E" != *"naab.toml"* ]]; then ok "MF-04" "control: the default naab.toml prints no warning"
    else bad "MF-04" "the default manifest was reported" "$E"; fi
else
    unm "MF-04" "naab-lang init did not write a naab.toml here"
fi

run mf5 ""
if [[ "$ERR" != *"naab.toml"* ]]; then ok "MF-05" "control: no naab.toml prints no warning"
else bad "MF-05" "a run with no naab.toml mentioned one" "$ERR"; fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
