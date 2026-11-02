#!/usr/bin/env bash
# The signed capability manifest (Bundle 11 PR 2, include/naab/capability_manifest.h).
#
# With "manifest": {"required": true} a run needs <stem>.manifest.json beside the
# program: present, signed by a key in the trust store (no unsigned mode, no
# untrusted key), well-formed, for this program's name and exact text -- and the
# run is then NARROWED to it like a lease: every action, language, host, file,
# agent and NAAb module it does not list is refused. `naab-gov manifest infer`
# drafts it, `naab-gov manifest sign` signs it, `naab-gov manifest check` asks.
#
# Every refusal arm is beside a control that the same program runs when the
# manifest allows it (the round trip MF-00 is the main one: infer -> sign ->
# run must pass), so a refuse-everything build fails the controls and a
# check-nothing build fails the refusals. A refusal is judged by its message AND
# a side effect that must not happen (a file not written, a request the stub
# never received). Manifests are edited through python reading stdin (no path
# crosses to a native python) and signed again after every edit.
#
# Kill table (each mutant built and run alone, naab-lang and naab-gov rebuilt;
# the controls stayed green), observed:
#   K1  the manifest signature is not checked          -> MF-02 MF-02b MF-04
#   K2  no trusted key required (unsigned mode allowed) -> MF-02c
#   K3  the program hash is not compared               -> MF-03 MF-18b
#   K4  the program name is not compared              -> MF-03b
#   K5  unknown manifest keys are accepted            -> MF-05 MF-05s
#   K6  checkFunctionCapability ignores the manifest   -> MF-10s
#   K7  the manifest's capability switches not applied -> MF-21
#   K8  languages not narrowed                         -> MF-08
#   K9  hosts not narrowed                             -> MF-09
#   K10 the files layer removed                        -> MF-07 MF-07r MF-07p
#   K11 module pin removed                             -> MF-11 MF-11b
#   K12 the agent check removed                        -> MF-10
#   K13 narrowing not re-applied on reload              -> MF-13c (the ratchet then
#       refuses the un-narrowed, looser reload; the first run of this mutant read
#       UNMEASURABLE because the control skipped on a refused reload -- now a FAIL)
#   K14 the manifest is not protected from the program -> MF-12
#   K15 --lint-only asked for a manifest               -> MF-14
#   K16 extends drops a parent's manifest.required      -> MF-19
#   K17 inference drops a process.run inline language   -> MF-15
#   K18 inference omits FS_READ for an import            -> MF-00i
#   K19 inference omits FS_READ beside FS_WRITE          -> MF-07 MF-07c
#   Not a mutant: MF-22c/MF-22 are an observation. Plugin exemptions in the files
#   layer and the module pin were written and SURVIVED as mutants (the plugin read
#   never reaches those gates on the default engine; it loads with no action
#   granted), so they were removed as dead code (A116).
#
# Usage: bash tests/governance_v4/test_capability_manifest.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
NAAB_GOV="$(dirname "$NAAB")/naab-gov"
source "$SCRIPT_DIR/../helpers/stub_platform.sh" 2>/dev/null
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -15 | sed 's/^/       /'; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$NAAB" ] || [ ! -x "$NAAB_GOV" ]; then
    echo "UNMEASURABLE: naab-lang or naab-gov not built"
    exit 1
fi

T="$(mktemp -d)"
cleanup() { stop_stub 2>/dev/null; stop_swap_operators 2>/dev/null; teardown_isolated_trust; rm -rf "$T"; }
trap cleanup EXIT
setup_isolated_trust
"$NAAB" --keygen "$T/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$T/k.pem.pub" >/dev/null 2>&1
"$NAAB" --keygen "$T/other.pem" >/dev/null 2>&1   # never trusted
export NAAB_SIGNING_KEY="$T/k.pem"
if [ ! -f "$T/k.pem" ] || [ ! -f "$T/other.pem" ]; then
    echo "UNMEASURABLE: key generation failed"
    exit 1
fi

BASE='{"version":"3.0","mode":"enforce","manifest":{"required":true},"security":{"sandbox_level":"elevated"}}'
NOREQ='{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"}}'

# mk NAME CONFIG PROGRAM -> D, a signed project with p.naab, lib.naab, data/in.txt
mk() {
    D="$T/$1"
    mkdir -p "$D/out" "$D/data"
    printf '%s\n' "$2" > "$D/govern.json"
    printf '%s\n' "$3" > "$D/p.naab"
    printf 'export fn twice(x) { return x * 2 }\n' > "$D/lib.naab"
    printf 'seed\n' > "$D/data/in.txt"
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$D/govern.json" || echo "FIXTURE: bad govern.json in $1"
    (cd "$D" && "$NAAB" --sign-governance >/dev/null 2>&1)
}
infer() { (cd "$D" && "$NAAB_GOV" manifest infer p.naab --force >/dev/null 2>&1); }
sign()  { (cd "$D" && NAAB_SIGNING_KEY="${1:-$NAAB_SIGNING_KEY}" "$NAAB_GOV" manifest sign p.manifest.json >/dev/null 2>&1); }
# The generic signer signs any file: used where the manifest is malformed on
# purpose, since `naab-gov manifest sign` refuses to sign one (MF-05s).
sign_raw() { (cd "$D" && "$NAAB" --sign-governance p.manifest.json >/dev/null 2>&1); }
# edit 'PYTHON' -- rewrite the manifest dict `m`; the file is fed as bytes.
edit() {
    python3 -c 'import json,sys
m = json.load(sys.stdin)
exec(sys.argv[1])
sys.stdout.write(json.dumps(m, indent=2) + "\n")' "$1" < "$D/p.manifest.json" > "$D/p.manifest.json.new" \
        && mv "$D/p.manifest.json.new" "$D/p.manifest.json"
}
go()  { OUT="$(cd "$D" && timeout 60 "$NAAB" run "${1:-p.naab}" 2>&1)"; RC=$?; }
has() { grep -qF -- "$1" <<<"$OUT"; }

P_W='import { twice } from "./lib.naab"
main {
    file.write("out/a.txt", "A")
    print("TWICE=" + string(twice(21)))
}'

echo "=== capability manifest: presence, signature, binding ==="

mk mf00c "$NOREQ" "$P_W"; go
if [ "$RC" -eq 0 ] && [ -f "$D/out/a.txt" ] && has "TWICE=42" && ! has "capability manifest"; then
    ok "MF-00c" "without manifest.required no manifest is read and the program runs"
else bad "MF-00c" "control: the program must run without the requirement (rc=$RC)" "$OUT"; fi

mk mf00 "$BASE" "$P_W"; infer; sign; go
if [ "$RC" -eq 0 ] && [ -f "$D/out/a.txt" ] && has "TWICE=42" && has "Capability manifest applied"; then
    ok "MF-00" "round trip: infer -> sign -> run passes and is narrowed (the main control)"
else bad "MF-00" "an inferred, signed manifest must let its own program run (rc=$RC)" "$OUT"; fi

# An import alone needs FS_READ (the import is a filesystem read): the draft
# must say so, or the round trip of a program that only imports fails.
P_I='import { twice } from "./lib.naab"
main {
    print("TWICE=" + string(twice(4)))
}'
mk mf00i "$BASE" "$P_I"; infer; sign; go
if [ "$RC" -eq 0 ] && has "TWICE=8"; then
    ok "MF-00i" "round trip of a program that only imports a module"
else bad "MF-00i" "an import-only program must run under its inferred manifest (rc=$RC)" "$OUT"; fi

mk mf01 "$BASE" "$P_W"; go
if [ "$RC" -eq 3 ] && has "has no capability manifest" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-01" "a missing manifest refuses the run before anything runs (exit 3)"
else bad "MF-01" "a missing manifest must refuse the run (rc=$RC)" "$OUT"; fi

mk mf02 "$BASE" "$P_W"; infer; go
if [ "$RC" -eq 3 ] && has "not signed by a trusted key" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-02" "an unsigned manifest is refused"
else bad "MF-02" "an unsigned manifest must be refused (rc=$RC)" "$OUT"; fi

mk mf02b "$BASE" "$P_W"; infer; sign "$T/other.pem"; go
if [ "$RC" -eq 3 ] && has "not signed by a trusted key" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-02b" "a manifest signed by a key not in the trust store is refused"
else bad "MF-02b" "an untrusted signer must be refused (rc=$RC)" "$OUT"; fi

# No key installed at all: govern.json loads in unsigned mode, which the
# signature check accepts for configs -- and must not for a manifest.
mk mf02c "$BASE" "$P_W"; rm -f "$D/govern.json.sig"; infer
OUT="$(cd "$D" && NAAB_TRUST_STORE_DIR="$T/empty-store" timeout 60 "$NAAB" run p.naab 2>&1)"; RC=$?
if [ "$RC" -eq 3 ] && has "no trusted signing key is installed" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-02c" "with no trusted key installed an (unsigned) manifest is refused, not accepted as unsigned mode"
else bad "MF-02c" "no trusted key must refuse the manifest (rc=$RC)" "$OUT"; fi

mk mf03 "$BASE" "$P_W"; infer; sign; printf '// edited after approval\n' >> "$D/p.naab"; go
if [ "$RC" -eq 3 ] && has "not the text its capability manifest approved" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-03" "a program edited after its manifest was signed is refused"
else bad "MF-03" "changed program text must be refused (rc=$RC)" "$OUT"; fi

mk mf03b "$BASE" "$P_W"; infer; sign
cp "$D/p.naab" "$D/q.naab"; cp "$D/p.manifest.json" "$D/q.manifest.json"; cp "$D/p.manifest.json.sig" "$D/q.manifest.json.sig"
go q.naab
if [ "$RC" -eq 3 ] && has "for another program" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-03b" "another program's manifest (same text, other name) is refused"
else bad "MF-03b" "a manifest names the program it approves (rc=$RC)" "$OUT"; fi

mk mf04 "$BASE" "$P_W"; infer; sign; edit 'm["actions"].append("SHELL_EXEC")'; go
if [ "$RC" -eq 3 ] && has "not signed by a trusted key" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-04" "a manifest widened after signing is refused"
else bad "MF-04" "an edited manifest must fail its signature (rc=$RC)" "$OUT"; fi

mk mf05 "$BASE" "$P_W"; infer; edit 'm["agent"] = ["x"]'
OUT="$(cd "$D" && "$NAAB_GOV" manifest sign p.manifest.json 2>&1)"; RC=$?
if [ "$RC" -eq 4 ] && [ ! -f "$D/p.manifest.json.sig" ] && has 'unknown key "agent"'; then
    ok "MF-05s" "naab-gov manifest sign refuses to sign a malformed manifest"
else bad "MF-05s" "a malformed manifest must not be signed (rc=$RC)" "$OUT"; fi
sign_raw; go
if [ "$RC" -eq 3 ] && has "malformed" && has 'unknown key "agent"'; then
    ok "MF-05" "a signed manifest with a key the format does not define is refused"
else bad "MF-05" "an unknown key must refuse the manifest (rc=$RC)" "$OUT"; fi

mk mf05b "$BASE" "$P_W"; infer; edit 'del m["hosts"]'; sign_raw; go
if [ "$RC" -eq 3 ] && has 'missing key "hosts"'; then
    ok "MF-05b" "a signed manifest missing a key is refused"
else bad "MF-05b" "a missing key must refuse the manifest (rc=$RC)" "$OUT"; fi

echo "=== narrowing: actions, files, languages, hosts, agents, modules ==="

mk mf06 "$BASE" "$P_W"; infer; edit 'm["actions"].remove("FS_WRITE"); m["files"]["write"] = []'; sign; go
if [ "$RC" -eq 3 ] && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-06" "a withheld FS_WRITE refuses file.write"
else bad "MF-06" "file.write must be refused without FS_WRITE (rc=$RC)" "$OUT"; fi

P_AB='main {
    file.write("out/a.txt", "A")
    file.write("out/b.txt", "B")
    print("WROTE_BOTH")
}'
mk mf07 "$BASE" "$P_AB"; infer; edit 'm["files"]["write"].remove("out/b.txt")'; sign; go
if [ "$RC" -eq 3 ] && has "File not in this program's capability manifest" && [ -f "$D/out/a.txt" ] && [ ! -f "$D/out/b.txt" ]; then
    ok "MF-07" "a write the manifest lists runs; one it does not is refused (exit 3)"
else bad "MF-07" "files.write must admit out/a.txt and refuse out/b.txt (rc=$RC)" "$OUT"; fi

mk mf07c "$BASE" "$P_AB"; infer; edit 'm["files"]["write"] = ["out"]'; sign; go
if [ "$RC" -eq 0 ] && has "WROTE_BOTH" && [ -f "$D/out/b.txt" ]; then
    ok "MF-07c" "a directory entry admits what is under it (control)"
else bad "MF-07c" "files.write [\"out\"] must admit out/b.txt (rc=$RC)" "$OUT"; fi

P_R='main {
    print("READ=" + file.read("data/in.txt"))
}'
mk mf07rc "$BASE" "$P_R"; infer; sign; go
if [ "$RC" -eq 0 ] && has "READ=seed"; then ok "MF-07rc" "a listed read runs (control)"
else bad "MF-07rc" "control: a read the manifest lists must run (rc=$RC)" "$OUT"; fi
mk mf07r "$BASE" "$P_R"; infer; edit 'm["files"]["read"] = ["data/other.txt"]'; sign; go
if [ "$RC" -eq 3 ] && has "files.read" && ! has "READ=seed"; then
    ok "MF-07r" "a read the manifest does not list is refused"
else bad "MF-07r" "an unlisted read must be refused (rc=$RC)" "$OUT"; fi

# Embedded Python's open() reaches the same files layer (the audit hook).
P_PY='main {
    let s = <<python
open("data/in.txt").read().strip()
>>
    print("PY=" + string(s))
}'
mk mf07pv "$NOREQ" "$P_PY"; go
if [ "$RC" -eq 0 ] && has "PY=seed"; then
    mk mf07pc "$BASE" "$P_PY"; infer; edit 'm["files"]["read"] = ["data/in.txt"]; m["actions"] = sorted(set(m["actions"]) | {"FS_READ"})'; sign; go
    if [ "$RC" -eq 0 ] && has "PY=seed"; then ok "MF-07pc" "an embedded Python read the manifest lists runs (control)"
    else bad "MF-07pc" "control: a listed Python read must run (rc=$RC)" "$OUT"; fi
    mk mf07p "$BASE" "$P_PY"; infer; edit 'm["actions"] = sorted(set(m["actions"]) | {"FS_READ"})'; sign; go
    if ! has "PY=seed"; then ok "MF-07p" "an embedded Python read the manifest does not list is refused"
    else bad "MF-07p" "a Python open() of an unlisted file must be refused (rc=$RC)" "$OUT"; fi
else
    skip "MF-07pc" "Python blocks do not read files here without a manifest (rc=$RC)"
    skip "MF-07p" "Python blocks do not read files here without a manifest"
fi

P_JS='main {
    let v = <<javascript
6 * 7
>>
    print("JS=" + string(v))
}'
mk mf08c "$BASE" "$P_JS"; infer; sign; go
if [ "$RC" -eq 0 ] && has "JS=42"; then ok "MF-08c" "a language the manifest lists runs (control)"
else bad "MF-08c" "control: listed javascript must run (rc=$RC)" "$OUT"; fi
mk mf08 "$BASE" "$P_JS"; infer; edit 'm["languages"] = []'; sign; go
if [ "$RC" -eq 3 ] && ! has "JS=42"; then ok "MF-08" "a language the manifest does not list is refused"
else bad "MF-08" "unlisted javascript must be refused (rc=$RC)" "$OUT"; fi

# The sandbox's host decision runs before DNS: a listed host gets past it (and
# then fails to resolve), an unlisted one is denied by it.
P_HTTP='main {
    try {
        http.get("https://listed.example.invalid/x")
        print("FETCHED")
    } catch (e) {
        print("ERR=" + string(e))
    }
    print("DONE")
}'
mk mf09c "$BASE" "$P_HTTP"; infer; sign; go
if has "DONE" && ! has "denied by sandbox" && ! has "not allowed"; then
    ok "MF-09c" "a host the manifest lists gets past the host decision (control)"
else bad "MF-09c" "control: a listed host must not be denied by the host lists (rc=$RC)" "$OUT"; fi
mk mf09 "$BASE" "$P_HTTP"; infer; edit 'm["hosts"] = ["other.example.invalid"]'; sign; go
if has "denied by sandbox" && ! has "FETCHED"; then ok "MF-09" "a host the manifest does not list is denied"
else bad "MF-09" "an unlisted host must be denied (rc=$RC)" "$OUT"; fi

# Agents. A fixture stub stands in for the provider.
FX='{"responses":[{"content":"invoice ok","input_tokens":10,"output_tokens":20}]}'
agent_cfg() {
    printf '{"version":"3.0","mode":"enforce","manifest":{"required":true},"security":{"sandbox_level":"elevated"},
 "agents":{"reviewer":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:%s","api_key_env":"FAKE_KEY_MF",
   "max_tokens":100,"system_prompt":"You review billing invoices."},
   "other":{"provider":"gemini","model":"stub","api_base":"http://127.0.0.1:%s","api_key_env":"FAKE_KEY_MF",
   "max_tokens":100,"system_prompt":"You review billing invoices."}}}' "$1" "$1"
}
export FAKE_KEY_MF=fake
P_AG='use agent
main {
    let h = agent.create("reviewer")
    print("CREATED")
    let r = agent.send(h, "check the invoices")
    print("SENT")
}'
agent_arm() {  # agent_arm NAME EDIT -> OUT RC REQS
    local name="$1" ed="$2"
    mkdir -p "$T/$name"
    printf '%s\n' "$FX" > "$T/$name/fixture.json"
    if ! start_stub "$T/$name/fixture.json" "$T/$name" >/dev/null 2>&1; then OUT="STUBFAIL"; RC=99; REQS=0; return; fi
    mk "$name" "$(agent_cfg "$STUB_PORT")" "$P_AG"; infer
    [ -n "$ed" ] && edit "$ed"
    sign; go
    stop_stub 2>/dev/null
    REQS=$(ls "$D"/req_*.json 2>/dev/null | wc -l | tr -d ' ')
}
agent_arm mf10c ""
if [ "$OUT" = "STUBFAIL" ]; then
    skip "MF-10c" "the stub did not start"; skip "MF-10" "the stub did not start"; skip "MF-10s" "the stub did not start"
else
    if [ "$RC" -eq 0 ] && has "SENT" && [ "$REQS" -ge 1 ]; then
        ok "MF-10c" "a listed agent is created and its send reaches the provider (control)"
    else bad "MF-10c" "control: the inferred manifest must let the agent run (rc=$RC reqs=$REQS)" "$OUT"; fi
    agent_arm mf10 'm["agents"] = ["other"]'
    if [ "$RC" -eq 3 ] && has "Agent not in this program's capability manifest" && ! has "CREATED" && [ "$REQS" -eq 0 ]; then
        ok "MF-10" "agent.create of an agent the manifest does not list is refused"
    else bad "MF-10" "an unlisted agent must be refused (rc=$RC reqs=$REQS)" "$OUT"; fi
    agent_arm mf10s 'm["actions"].remove("AGENT_SEND")'
    if [ "$RC" -eq 3 ] && has "Action not in this program's capability manifest: AGENT_SEND" && has "CREATED" && [ "$REQS" -eq 0 ]; then
        ok "MF-10s" "a withheld AGENT_SEND refuses the send before any request leaves"
    else bad "MF-10s" "agent.send without AGENT_SEND must be refused (rc=$RC reqs=$REQS)" "$OUT"; fi
fi

mk mf11 "$BASE" "$P_W"; infer; sign; printf '// changed\n' >> "$D/lib.naab"; go
if [ "$RC" -eq 3 ] && has "Module text is not the text the capability manifest approved" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-11" "a module changed after approval is refused at import"
else bad "MF-11" "a changed module must be refused (rc=$RC)" "$OUT"; fi
mk mf11b "$BASE" "$P_W"; infer; edit 'm["modules"] = {}'; sign; go
if [ "$RC" -eq 3 ] && has "Module not in this program's capability manifest" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-11b" "a module the manifest does not list is refused at import"
else bad "MF-11b" "an unlisted module must be refused (rc=$RC)" "$OUT"; fi

# The manifest is a governance artifact: listing it does not let the program read it.
P_SELF='main {
    print("SELF=" + file.read("p.manifest.json"))
}'
mk mf12 "$BASE" "$P_SELF"; infer; sign; go
if [ "$RC" -eq 3 ] && ! has "SELF=" && has "blocked"; then
    ok "MF-12" "the program cannot read its own manifest, even when the manifest lists it"
else bad "MF-12" "the manifest must be protected from the program (rc=$RC)" "$OUT"; fi

# A mid-run reload to a config that drops manifest.required keeps the narrowing:
# javascript, which the manifest does not list, stays refused after the reload.
reload_arm() {  # reload_arm NAME KEEP_JS -> OUT RC
    local name="$1" keep="$2" n="$T/$1_next"
    mkdir -p "$n"; printf '%s\n' "$NOREQ" > "$n/govern.json"; (cd "$n" && "$NAAB" --sign-governance >/dev/null 2>&1)
    local prog
    prog="main {
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(\".swap_done\"):
        os.remove(\".swap_done\")
    with open(\".swap_req\", \"w\") as f:
        f.write(src + \"\\t\" + dst + \"\\n\")
    open(\".swap_req.ready\", \"w\").close()
    for _ in range(1200):
        if os.path.exists(\".swap_done\"):
            return
        time.sleep(0.05)
    raise RuntimeError(\"config swap operator did not respond\")
time.sleep(1)
_swap(\"$n/govern.json.sig\", \"$T/$name/govern.json.sig\")
_swap(\"$n/govern.json\", \"$T/$name/govern.json\")
\"swapped\"
>>
    print(\"SWAP=\" + string(s))
    let v = <<javascript
6 * 7
>>
    print(\"JS=\" + string(v))
}"
    mk "$name" "$BASE" "$prog"; infer
    edit 'm["files"]["write"] = [".swap_req", ".swap_req.ready", ".swap_done"]; m["actions"] = sorted(set(m["actions"]) | {"FS_READ", "FS_WRITE"})'
    [ "$keep" = 0 ] && edit 'm["languages"].remove("javascript")'
    sign
    start_swap_operator "$D"
    OUT="$(cd "$D" && timeout 120 "$NAAB" run p.naab 2>&1)"; RC=$?
}
reload_arm mf13c 1
if has "SWAP=swapped" && ! has "Config reloaded mid-run"; then
    # The swap happened and the reload was not accepted: an engine that
    # refuses every reload under a manifest fails here (mutant K13: the
    # reloaded rules come out un-narrowed, looser, and the ratchet refuses).
    bad "MF-13c" "a reload that only drops manifest.required must be accepted (rc=$RC)" "$OUT"
    skip "MF-13" "the reload control failed"
elif has "SWAP=swapped" && has "Config reloaded mid-run" && has "JS=42"; then
    ok "MF-13c" "the reload is accepted and a listed language runs after it (control)"
    reload_arm mf13 0
    if has "SWAP=swapped" && has "Config reloaded mid-run" && ! has "JS=42" && [ "$RC" -eq 3 ]; then
        ok "MF-13" "after an accepted reload that drops manifest.required, the narrowing holds"
    else bad "MF-13" "the manifest must narrow the reloaded rules too (rc=$RC)" "$OUT"; fi
else
    skip "MF-13c" "the swap did not complete here (rc=$RC): $(grep -m1 -E 'SWAP|rror' <<<"$OUT")"
    skip "MF-13" "the reload control did not run"
fi

# A governance plugin is the operator's code: under a manifest that grants no
# action at all it still loads and its rule still runs (observed: the plugin
# read does not reach the manifest's gates on this engine).
PLUG_CFG='{"version":"3.0","mode":"enforce","manifest":{"required":true},"security":{"sandbox_level":"elevated"},
 "governance_plugins":[{"file":"plugin.naab","rules":[{"id":"MF-PLUG","function":"check_no_eval","description":"no eval",
   "level":"hard","trigger":"polyglot_block","languages":["python"],"message":"eval() is not allowed here","help":"do not use eval"}]}]}'
mkplugin() {
    cat > "$D/plugin.naab" <<'PLUGIN'
use string

export function check_no_eval(ctx) {
    let code = ctx["code"]
    if string.contains(code, "eval(") {
        return { "pass": false, "message": "eval() is forbidden by project policy" }
    }
    return { "pass": true }
}
PLUGIN
}
P_PLUG_OK='main {
    let e = <<python
str(1 + 1)
>>
    print("PY=" + e)
}'
P_PLUG_BAD='main {
    let e = <<python
str(eval("1 + 1"))
>>
    print("PY=" + e)
}'
mk mf22c "$PLUG_CFG" "$P_PLUG_OK"; mkplugin; infer; edit 'm["actions"] = []'; sign; go
if [ "$RC" -eq 0 ] && has "PY=2"; then
    ok "MF-22c" "a project with a governance plugin runs under its inferred manifest (control)"
    mk mf22 "$PLUG_CFG" "$P_PLUG_BAD"; mkplugin; infer; edit 'm["actions"] = []'; sign; go
    if [ "$RC" -eq 3 ] && has "MF-PLUG" && ! has "PY=2"; then
        ok "MF-22" "the plugin loads under the manifest and its rule refuses the block"
    else bad "MF-22" "the plugin must load and fire under a manifest (rc=$RC)" "$OUT"; fi
else
    bad "MF-22c" "a plugin project must run under its inferred manifest (rc=$RC)" "$OUT"
    skip "MF-22" "the control did not run"
fi

echo "=== scope: lint-only, extends, types ==="

mk mf14 "$BASE" "$P_W"
OUT="$(cd "$D" && timeout 60 "$NAAB" run --lint-only p.naab 2>&1)"; RC=$?
if ! has "has no capability manifest" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-14" "--lint-only runs nothing and is not asked for a manifest (MF-01 is the same project run)"
else bad "MF-14" "lint-only must not require a manifest (rc=$RC)" "$OUT"; fi

mkdir -p "$T/mf19"
printf '%s\n' '{"version":"3.0","mode":"enforce","manifest":{"required":true}}' > "$T/mf19/parent.json"
mk mf19 '{"version":"3.0","mode":"enforce","extends":"./parent.json","security":{"sandbox_level":"elevated"}}' "$P_W"
(cd "$D" && "$NAAB" --sign-governance parent.json >/dev/null 2>&1)
go
if [ "$RC" -eq 3 ] && has "has no capability manifest" && [ ! -f "$D/out/a.txt" ]; then
    ok "MF-19" "a parent's manifest.required holds in an extends child"
else bad "MF-19" "extends must keep the parent's requirement (rc=$RC)" "$OUT"; fi

mk mf20 '{"version":"3.0","mode":"enforce","manifest":{"required":"yes"}}' "$P_W"; go
if [ "$RC" -eq 4 ] && [ ! -f "$D/out/a.txt" ]; then ok "MF-20" "manifest.required that is not a boolean refuses the config (exit 4)"
else bad "MF-20" "a non-boolean manifest.required must refuse the config (rc=$RC)" "$OUT"; fi

# ENV_READ withheld also scrubs what polyglot code sees (the capability switch,
# not only the env.get gate): a Python block reads the variable as absent.
P_ENV='main {
    let s = <<python
import os
str(os.environ.get("MF_VISIBLE_VAR"))
>>
    print("ENV=" + string(s))
}'
export MF_VISIBLE_VAR=visible-123
mk mf21c "$BASE" "$P_ENV"; infer; edit 'm["actions"].append("ENV_READ")'; sign; go
if [ "$RC" -eq 0 ] && has "ENV=visible-123"; then
    ok "MF-21c" "with ENV_READ listed a Python block sees the variable (control)"
    mk mf21 "$BASE" "$P_ENV"; infer; sign; go
    if has "ENV=None" && ! has "visible-123"; then
        ok "MF-21" "with ENV_READ withheld a Python block does not see the variable"
    else bad "MF-21" "a withheld ENV_READ must scrub the polyglot environment (rc=$RC)" "$OUT"; fi
else
    skip "MF-21c" "Python blocks do not run here under the manifest (rc=$RC)"; skip "MF-21" "control did not run"
fi

echo "=== naab-gov manifest: infer, check ==="

mk mf15 "$NOREQ" 'import { twice } from "./lib.naab"
use agent
main {
    let t = file.read("data/in.txt")
    file.write("out/a.txt", t)
    let home = env.get("HOME")
    try { http.get("https://api.example.invalid/v1") } catch (e) { print("x") }
    let h = agent.create("reviewer")
    let r = agent.send(h, "hi")
    let v = <<javascript
1
>>
    process.run("python3", ["-c", "print(1)"])
    print(twice(1))
}'
(cd "$D" && "$NAAB_GOV" manifest infer p.naab >/dev/null 2>&1); RC=$?
GOT="$(python3 -c 'import json,sys
m = json.load(sys.stdin)
print("actions=" + ",".join(m["actions"]))
print("languages=" + ",".join(m["languages"]))
print("hosts=" + ",".join(m["hosts"]))
print("read=" + ",".join(m["files"]["read"]))
print("write=" + ",".join(m["files"]["write"]))
print("agents=" + ",".join(m["agents"]))
print("modules=" + ",".join(sorted(m["modules"])))' < "$D/p.manifest.json" 2>&1)"
WANT='actions=AGENT_SEND,ENV_READ,FS_READ,FS_WRITE,NET_CONNECT,SHELL_EXEC
languages=javascript,python
hosts=api.example.invalid
read=data/in.txt
write=out/a.txt
agents=reviewer
modules=lib.naab'
if [ "$RC" -eq 0 ] && [ "$GOT" = "$WANT" ]; then
    ok "MF-15" "infer lists every action, language, host, file, agent and module the program shows"
else bad "MF-15" "the draft must hold exactly what the program shows (rc=$RC)" "$(diff <(printf '%s\n' "$WANT") <(printf '%s\n' "$GOT"))"; fi

cp "$D/p.manifest.json" "$T/mf15.before"
(cd "$D" && "$NAAB_GOV" manifest infer p.naab >/dev/null 2>&1); RC=$?
if [ "$RC" -eq 4 ] && cmp -s "$D/p.manifest.json" "$T/mf15.before"; then
    ok "MF-16" "infer will not replace an existing manifest without --force"
else bad "MF-16" "infer must refuse to overwrite (rc=$RC)"; fi

sign
(cd "$D" && "$NAAB_GOV" manifest infer p.naab --force >/dev/null 2>&1)
if [ ! -f "$D/p.manifest.json.sig" ]; then ok "MF-17" "a re-drafted manifest loses its old signature"
else bad "MF-17" "a stale .sig must not survive a new draft"; fi

mk mf18 "$BASE" "$P_W"; infer; sign
OUT="$(cd "$D" && "$NAAB_GOV" manifest check p.naab 2>&1)"; RC=$?
if [ "$RC" -eq 0 ] && has "ACCEPTED"; then ok "MF-18" "check accepts a signed manifest for this text"
else bad "MF-18" "check must accept the round-trip manifest (rc=$RC)" "$OUT"; fi
printf '// edited\n' >> "$D/p.naab"
OUT="$(cd "$D" && "$NAAB_GOV" manifest check p.naab 2>&1)"; RC=$?
if [ "$RC" -eq 3 ] && has "not the text"; then ok "MF-18b" "check refuses the manifest of an edited program (exit 3)"
else bad "MF-18b" "check must refuse after the program changed (rc=$RC)" "$OUT"; fi

echo ""
echo "PASS=$PASS FAIL=$FAIL UNMEASURABLE=$SKIP"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
