#!/usr/bin/env bash
# ============================================================
# test_taint_report_line.sh -- a taint violation names the line of the call
#
# The parser built every call node with an empty SourceLocation, so on the
# tree-walker each report built from one -- taint sink violations among them --
# named line 0 ("x.naab:0") while the VM named the real line. Calls now take
# the location of their '('.
#
#   TL-00  CONTROL: both engines block both sinks (exit 3, taint message) --
#          without a report there is no line to check
#   TL-01  the tree-walker names the call's own line, not 0
#   TL-02  both engines name the same line
#   TL-03  "print" listed as a sink is enforced on BOTH engines. The VM's print
#          check passed an unmarked label to checkTaintedSink, which looks the
#          name up in the taint set, so it could never fire: the secret was
#          printed under Governance: PASS while the tree-walker blocked it
#   TL-03c CONTROL: the same config prints a clean string on both engines, so
#          TL-03 is not passing because print itself is broken
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== Taint violations name the line of the call ==="

if [ ! -x "$NAAB" ]; then
    for id in TL-00 TL-01 TL-02; do skip "$id" "naab-lang not built (UNMEASURABLE)"; done
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-taintline.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT

cat > "$W/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "taint_tracking": { "enabled": true, "level": "hard", "sources": ["env.get"],
                      "sinks": ["http.post", "file.write"] } }
EOF
# The sink call sits on line 6 of each program, after a blank line, so a
# location borrowed from a neighbouring statement would show as 4 or 7.
printf 'use env\nuse http\nmain {\n    let s = env.get("TL_SECRET")\n\n    http.post("http://127.0.0.1:9/", s)\n}\n' > "$W/http.naab"
printf 'use env\nuse file\nmain {\n    let s = env.get("TL_SECRET")\n\n    file.write("out.txt", s)\n}\n' > "$W/file.naab"

# line <prog> <engine flag> -> sets RC and LINE ("" when no taint report)
line_of() {
    local out
    out=$(cd "$W" && TL_SECRET=v timeout 20 "$NAAB" "$1" $2 2>&1); RC=$?
    case "$out" in
        *"Taint tracking violation"*)
            LINE=$(printf '%s\n' "$out" | grep -o "$1:[0-9]*" | head -1 | sed 's/.*://') ;;
        *) LINE="" ;;
    esac
}

blocked=1
for p in http file; do
    for e in vm tw; do
        flag=""; [ "$e" = tw ] && flag="--tree-walk"
        line_of "$p.naab" "$flag"
        eval "L_${p}_${e}=\"\$LINE\""
        { [ "$RC" -eq 3 ] && [ -n "$LINE" ]; } || blocked=0
    done
done

if [ "$blocked" -eq 1 ]; then
    ok "TL-00" "CONTROL: both engines block both sinks with a taint report"
else
    bad "TL-00" "a sink was not blocked with a taint report -- TL-01/TL-02 prove nothing" \
        "lines: http vm=${L_http_vm:-} tw=${L_http_tw:-} file vm=${L_file_vm:-} tw=${L_file_tw:-}"
fi

if [ "${L_http_tw:-}" = "6" ] && [ "${L_file_tw:-}" = "6" ]; then
    ok "TL-01" "the tree-walker names the call's line (6), not 0"
else
    bad "TL-01" "the tree-walker named the wrong line" "http=${L_http_tw:-} file=${L_file_tw:-} (expected 6)"
fi

if [ -n "${L_http_vm:-}" ] && [ "${L_http_vm:-}" = "${L_http_tw:-}" ] \
   && [ "${L_file_vm:-}" = "${L_file_tw:-}" ]; then
    ok "TL-02" "both engines name the same line"
else
    bad "TL-02" "the engines disagree on the line" \
        "http vm=${L_http_vm:-} tw=${L_http_tw:-}; file vm=${L_file_vm:-} tw=${L_file_tw:-}"
fi

# --- TL-03 / TL-03c: the print sink -------------------------------------------------
mkdir -p "$W/p"
cat > "$W/p/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "taint_tracking": { "enabled": true, "level": "hard", "sources": ["env.get"], "sinks": ["print"] } }
EOF
printf 'use env\nmain {\n    let s = env.get("TL_SECRET")\n    print(s)\n}\n' > "$W/p/tainted.naab"
printf 'use env\nmain {\n    let s = env.get("TL_SECRET")\n    print("CLEAN_OUTPUT")\n}\n' > "$W/p/clean.naab"
t3=""; c3=""
for flag in "" "--tree-walk"; do
    out=$(cd "$W/p" && TL_SECRET=LEAKED_SECRET timeout 20 "$NAAB" tainted.naab $flag 2>&1); rc=$?
    case "$out" in
        *LEAKED_SECRET*) t3="$t3 ${flag:-vm}:printed" ;;
        *"reached sink 'print'"*) [ "$rc" -eq 3 ] || t3="$t3 ${flag:-vm}:rc=$rc" ;;
        *) t3="$t3 ${flag:-vm}:no-report(rc=$rc)" ;;
    esac
    out=$(cd "$W/p" && TL_SECRET=x timeout 20 "$NAAB" clean.naab $flag 2>&1); rc=$?
    case "$out" in *CLEAN_OUTPUT*) [ "$rc" -eq 0 ] || c3="$c3 ${flag:-vm}:rc=$rc" ;; *) c3="$c3 ${flag:-vm}:silent(rc=$rc)" ;; esac
done
if [ -z "$t3" ]; then ok "TL-03" "a tainted print is blocked on both engines (exit 3)"
else bad "TL-03" "the print sink is not enforced on every engine" "$t3"; fi
if [ -z "$c3" ]; then ok "TL-03c" "CONTROL: a clean print still prints on both engines"
else bad "TL-03c" "a clean print did not print -- TL-03 proves nothing" "$c3"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
