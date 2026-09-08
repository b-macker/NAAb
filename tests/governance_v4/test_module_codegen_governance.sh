#!/usr/bin/env bash
# A23/A24/A25: close three subsystem bypasses where a module/codegen ingestion
# path reached an operation without consulting governance.
#   A25 local `import "x.naab"` bypassed capabilities.filesystem (mode/blocked_paths)
#   A23 URL `import "http://..."` did raw curl egress + RCE with no network gate
#   A24 codegen.run_strict was omitted from the taint bridge (allow_tainted_code bypass)
#
# Each check pairs the fix with a control proving the gate can both FIRE and NOT
# over-block. A25/A23 controls: a permissive config still imports. A24: run and
# run_strict must now behave identically on tainted input (the fix), and clean
# input must still run (no over-block).

set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$HERE/../../build/naab-lang"
PASS=0; FAIL=0
ok(){ echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad(){ echo "  FAIL [$1] $2"; echo "        $3"; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "no naab-lang"; exit 1; }
# Isolate the trust store: these configs are unsigned and would INTEGRITY-BLOCK
# if any key were installed in the real store (D8). setup_isolated_trust
# repoints NAAB_TRUST_STORE_DIR for the duration of the suite.
source "$HERE/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d); SRV=""
cleanup(){ [ -n "$SRV" ] && kill "$SRV" 2>/dev/null; teardown_isolated_trust 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT

# ---- A25: local import + filesystem governance ----
mkdir -p "$W/a25"; echo 'export fn f() { return "MOD_SECRET" }' > "$W/a25/mod.naab"
cat > "$W/a25/deny.json" <<'J'
{ "version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"none"}} }
J
cat > "$W/a25/allow.json" <<'J'
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"} }
J
cat > "$W/a25/imp.naab" <<'J'
import "mod.naab" as m
main { print("GOT=" + string(m.f())) }
J
OUT=$(cd "$W/a25" && cp deny.json govern.json && timeout 20 "$NAAB" imp.naab 2>&1); EC=$?
if [ "$EC" = "3" ] && echo "$OUT" | grep -qi "Filesystem access is not allowed"; then
    ok "MG-01" "A25: local import under filesystem mode:none is HARD-blocked"
else bad "MG-01" "local import bypassed filesystem governance" "exit $EC; $(echo "$OUT"|head -2)"; fi

OUT=$(cd "$W/a25" && cp allow.json govern.json && timeout 20 "$NAAB" imp.naab 2>&1); EC=$?
if echo "$OUT" | grep -q "GOT=MOD_SECRET"; then
    ok "MG-02" "CONTROL: local import under a permissive fs policy still works"
else bad "MG-02" "the fs gate over-blocks a permitted import" "exit $EC; $(echo "$OUT"|head -2)"; fi

# ---- A23: URL import + network governance ----
mkdir -p "$W/srv"; echo 'export let s = "REMOTE"' > "$W/srv/rmod.naab"
rm -rf ~/.naab/cache 2>/dev/null
PORT=$(( (RANDOM % 15000) + 30000 ))
python3 -m http.server "$PORT" --directory "$W/srv" >/dev/null 2>&1 & SRV=$!
sleep 1
mkdir -p "$W/a23"
cat > "$W/a23/govern.json" <<'J'
{ "version":"5.0","mode":"enforce","capabilities":{"network":{"enabled":false}} }
J
cat > "$W/a23/imp.naab" <<J
import "http://127.0.0.1:$PORT/rmod.naab" as r
main { print("GOT=" + string(r.s)) }
J
OUT=$(cd "$W/a23" && timeout 20 "$NAAB" imp.naab 2>&1); EC=$?
if [ "$EC" != "0" ] && ! echo "$OUT" | grep -q "GOT=REMOTE" && echo "$OUT" | grep -qiE "denied|private network"; then
    ok "MG-03" "A23: URL import is gated by network governance (no download/RCE)"
else bad "MG-03" "URL import performed egress + code execution ungated" "exit $EC; $(echo "$OUT"|head -3)"; fi
kill "$SRV" 2>/dev/null; SRV=""

# ---- A24: codegen.run_strict taint parity ----
mkdir -p "$W/a24"
cat > "$W/a24/govern.json" <<'J'
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "taint_tracking":{"enabled":true,"level":"detect","sources":["env.get"],"sinks":["file.write"],"propagation":{"string_concat":true,"function_returns":true}},
  "codegen":{"enabled":true,"allow_tainted_code":false},
  "capabilities":{"env_vars":{"allowed_read":["EVIL_CODE"]}} }
J
cat > "$W/a24/run.naab" <<'J'
use codegen
use env
main { let c = env.get("EVIL_CODE") codegen.run("python", c) print("RAN") }
J
cat > "$W/a24/strict.naab" <<'J'
use codegen
use env
main { let c = env.get("EVIL_CODE") codegen.run_strict("python", c) print("RAN") }
J
cat > "$W/a24/clean.naab" <<'J'
use codegen
main { let r = codegen.run_strict("python", "print(1+1)") print("CLEAN=" + string(r.get("exit_code"))) }
J
export EVIL_CODE='print("x")'
R_RUN=$(cd "$W/a24" && timeout 20 "$NAAB" run.naab 2>&1); ER=$?
R_STR=$(cd "$W/a24" && timeout 20 "$NAAB" strict.naab 2>&1); ES=$?
if echo "$R_RUN" | grep -qi "tainted" && echo "$R_STR" | grep -qi "tainted"; then
    ok "MG-04" "A24: codegen.run_strict blocks tainted code identically to run"
else bad "MG-04" "run_strict bypassed the tainted-code gate" "run: $(echo "$R_RUN"|grep -i tainted|head -1) / strict: $(echo "$R_STR"|grep -i tainted|head -1)"; fi

OUT=$(cd "$W/a24" && timeout 20 "$NAAB" clean.naab 2>&1); EC=$?
if echo "$OUT" | grep -q "CLEAN=0"; then
    ok "MG-05" "CONTROL: run_strict on CLEAN code still executes (no over-block)"
else bad "MG-05" "run_strict over-blocks clean code" "exit $EC; $(echo "$OUT"|head -2)"; fi

echo
echo "module/codegen governance: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
