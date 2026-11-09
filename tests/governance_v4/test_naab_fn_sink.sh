#!/usr/bin/env bash
# test_naab_fn_sink.sh -- a taint sink can name a NAAb function (F-33; Bundle 17)
#
# taint_tracking.sinks matched only the names the engine passes at its own
# check sites (a stdlib call's module.method, <lang>_exec, print, variable
# assignment), since sinks were introduced (03a7cae8). A sink naming a NAAb
# function -- `post`, or `net.post` from `import "./net.naab" as net` -- loaded
# without a word and was never checked: a tainted value reached it under level
# hard, exit 0, while the same value reaching `file.write` was refused (observed
# on master 314bccfd, Gemini's F-33 widened). The owner's decision (2026-11-09):
# enforce at the call. The VM checks a call that enters a NAAb function under
# the name it is written by (calleeName(), the text the static report judges),
# against the live rules; `naab-gov taint` reports the flow.
#
#   NS-00c  CONTROL a stdlib sink (file.write) refuses the same tainted value
#   NS-01   a local function named as a sink: refused, its body never runs
#   NS-02   an imported module's function named as a sink (net.post): refused
#   NS-03   a pipeline into the sink (s |> post): refused
#   NS-04   a function held in a dict field (api.post): refused
#   NS-04b  a struct method named by its type (Client.send) called on an
#           instance (c.send(s)): refused -- the written name is the variable's
#   NS-05   an async fn named as a sink: refused
#   NS-06c  CONTROL a clean argument reaches the sink: runs
#   NS-07c  CONTROL a tainted argument to a function NOT named as a sink: runs
#   NS-08c  CONTROL a sanitized value reaches the sink: runs
#   NS-09   naab-gov taint reports the local sink as a value flow (exit 1)
#   NS-10   naab-gov taint reports the imported sink as a value flow (exit 1)
#   NS-11c  CONTROL naab-gov taint on the clean program finds no flow at the sink
#
# Kill table (each mutant applied alone, rebuilt, this suite run; observed
# 2026-11-09):
#   the VM check returns before looking            NS-01..NS-05, NS-04b
#   the compiler records no call names             NS-01..NS-05 (NS-04b holds:
#                                                  the resolved name is checked)
#   the dict-closure route not checked             NS-02, NS-04
#   the struct route's resolved name not checked   NS-04b
#   pipelines not recorded                         NS-03
#   the report's sink check not extended           NS-09, NS-10
# Red first (observed, master 314bccfd): NS-01..NS-05 (NS-04b included), NS-09
# and NS-10 fail; the controls pass.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_fnsink_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       -> /'; }

if [ ! -x "$NAAB" ] && [ ! -x "$NAAB.exe" ]; then echo "UNMEASURABLE: naab-lang not built"; exit 1; fi

# proj ID SINKS_JSON PROGRAM: a project with a module net.naab; D is set
proj() {
    D="$W/$1"; mkdir -p "$D"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "taint_tracking": {"enabled": true, "level": "hard", "sources": ["env.get"], "sinks": %s, "sanitizers": ["sanitize_"]}}\n' "$2" > "$D/govern.json"
    printf 'export fn post(x) {\n    print("BODY_RAN")\n    return 1\n}\n' > "$D/net.naab"
    printf '%s\n' "$3" > "$D/p.naab"
}
run() { RC=0; OUT=$(cd "$D" && timeout 60s "$NAAB" p.naab 2>&1) || RC=$?; }
refused() {   # refused SINK: exit 3, the sink named, the body never ran
    [ "$RC" -eq 3 ] && grep -q "reached sink '$1'" <<<"$OUT" && ! grep -q "BODY_RAN" <<<"$OUT"
}
ran() { [ "$RC" -eq 0 ] && grep -q "BODY_RAN" <<<"$OUT" && grep -q "AFTER" <<<"$OUT"; }

LOCAL='fn post(x) {
    print("BODY_RAN")
    return 1
}
fn sanitize_v(x) {
    return "clean"
}
fn other(x) {
    print("BODY_RAN")
    return 1
}'

echo "=== a taint sink can name a NAAb function ==="

proj ns00 '["file.write", "post"]' "main {
    let s = env.get(\"HOME\")
    file.write(\"o.txt\", s)
    print(\"AFTER\")
}"
run
if [ "$RC" -eq 3 ] && grep -q "reached sink 'file.write'" <<<"$OUT"; then pass NS-00c "CONTROL: the stdlib sink refuses the tainted value"
else fail NS-00c "the taint config is not live (rc=$RC)" "$OUT"
     echo ""; echo "PASS=$PASS FAIL=$FAIL"; exit 1; fi

proj ns01 '["post"]' "$LOCAL
main {
    let s = env.get(\"HOME\")
    post(s)
    print(\"AFTER\")
}"
run
if refused post; then pass NS-01 "a local function named as a sink refuses the tainted argument"
else fail NS-01 "a tainted value reached the local sink function (rc=$RC)" "$OUT"; fi

proj ns02 '["net.post"]' "import \"./net.naab\" as net
main {
    let s = env.get(\"HOME\")
    net.post(s)
    print(\"AFTER\")
}"
run
if refused net.post; then pass NS-02 "an imported function named as a sink refuses the tainted argument"
else fail NS-02 "a tainted value reached the imported sink function (rc=$RC)" "$OUT"; fi

proj ns03 '["post"]' "$LOCAL
main {
    let s = env.get(\"HOME\")
    let r = s |> post
    print(\"AFTER\")
}"
run
if refused post; then pass NS-03 "a pipeline into the sink function is refused"
else fail NS-03 "a tainted value piped into the sink function ran (rc=$RC)" "$OUT"; fi

proj ns04 '["api.post"]' "main {
    let api = {post: fn(x) {
        print(\"BODY_RAN\")
        return 1
    }}
    let s = env.get(\"HOME\")
    api.post(s)
    print(\"AFTER\")
}"
run
if refused api.post; then pass NS-04 "a function in a dict field named as a sink is refused"
else fail NS-04 "a tainted value reached the dict-field sink function (rc=$RC)" "$OUT"; fi

proj ns04b '["Client.send"]' "struct Client {
    name: string
}
fn Client.send(x) {
    print(\"BODY_RAN\")
    return 1
}
main {
    let c = Client { name: \"a\" }
    let s = env.get(\"HOME\")
    c.send(s)
    print(\"AFTER\")
}"
run
if refused Client.send; then pass NS-04b "a struct method named as a sink is refused when called on an instance"
else fail NS-04b "a tainted value reached the struct-method sink through an instance (rc=$RC)" "$OUT"; fi

proj ns05 '["post"]' "async fn post(x) {
    print(\"BODY_RAN\")
    return 1
}
main {
    let s = env.get(\"HOME\")
    let r = await post(s)
    print(\"AFTER\")
}"
run
if refused post; then pass NS-05 "an async fn named as a sink refuses the tainted argument"
else fail NS-05 "a tainted value reached the async sink function (rc=$RC)" "$OUT"; fi

proj ns06 '["post"]' "$LOCAL
main {
    post(\"plain\")
    print(\"AFTER\")
}"
run
if ran; then pass NS-06c "CONTROL: a clean argument reaches the sink function"
else fail NS-06c "a clean call to the sink function did not run (rc=$RC)" "$OUT"; fi

proj ns07 '["post"]' "$LOCAL
main {
    let s = env.get(\"HOME\")
    other(s)
    print(\"AFTER\")
}"
run
if ran; then pass NS-07c "CONTROL: a tainted argument to a function not named as a sink runs"
else fail NS-07c "a function not named as a sink was refused (rc=$RC)" "$OUT"; fi

proj ns08 '["post"]' "$LOCAL
main {
    let s = env.get(\"HOME\")
    let c = sanitize_v(s)
    post(c)
    print(\"AFTER\")
}"
run
if ran; then pass NS-08c "CONTROL: a sanitized value reaches the sink function"
else fail NS-08c "a sanitized value was refused at the sink function (rc=$RC)" "$OUT"; fi

taint_rc() { RC=0; OUT=$(cd "$D" && timeout 60s "$GOV" taint p.naab 2>&1) || RC=$?; }
if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then
    echo "  UNMEASURABLE [NS-09..11] naab-gov not built"
else
    D="$W/ns01"; taint_rc
    if [ "$RC" -eq 1 ] && grep -qE "post" <<<"$OUT" && grep -qi "value" <<<"$OUT"; then pass NS-09 "naab-gov taint reports the local sink as a value flow"
    else fail NS-09 "naab-gov taint did not report the local sink (rc=$RC)" "$OUT"; fi
    D="$W/ns02"; taint_rc
    if [ "$RC" -eq 1 ] && grep -qE "net\.post" <<<"$OUT"; then pass NS-10 "naab-gov taint reports the imported sink as a flow"
    else fail NS-10 "naab-gov taint did not report the imported sink (rc=$RC)" "$OUT"; fi
    D="$W/ns06"; taint_rc
    if [ "$RC" -ne 1 ] && ! grep -q "reaches post\|sink post\|-> post" <<<"$OUT"; then pass NS-11c "CONTROL: the clean program has no flow at the sink"
    else fail NS-11c "naab-gov taint reported a flow in the clean program (rc=$RC)" "$OUT"; fi
fi

echo ""
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
