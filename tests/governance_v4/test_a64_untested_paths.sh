#!/usr/bin/env bash
# ============================================================
# test_a64_untested_paths.sh -- A64's "test" rows: governance functions no
# unit executed (docs/sweeps/A64.tsv, coverage run sweep2 at 5b9f48c5), each
# given a behavioural arm that fails when the function's effect is removed.
# Bundle 19 PR 4.
#
# Each arm is a refusal or a value beside a control through the same probe:
#   UA-01  codegen.is_enabled() reads codegen.enabled       getCodegenEnabled
#          (true, and false: a function that always answers one way fails one)
#   UA-02  an approval_required rule passes with a valid     hasValidApproval,
#          signed token, and not with none, another rule's,  lpEncode
#          or one whose reason was edited (the signature covers it)
#   UA-03  an agent role's path refusal names its allowed    checkPathAccessImpl's
#          directories (the lambda that lists them)          listing lambda
#   UA-04  the risk-score breakdown lists the heaviest rule  formatScoreBreakdown's
#          first; the rule names sort the other way          comparator
#   UA-05  a block writing os.system in Unicode bold letters mapMathAlpha
#          (Python reads it as os.system) is refused like the
#          plain spelling; benign bold text runs
#   UA-06  must_vary refuses a function whose result never   checkMustVary
#          varies; a summing one passes
#   UA-07  tree-walker (audit): an async fn's polyglot block save/restoreCounterState
#          counts on top of main's -- 2 + 1 over a limit of 2
#          is reported, 1 + 1 is not (called through a dict
#          field; called by name it runs synchronously,
#          ASYNC-001 -- the dict route is A228)
#   UA-08  the reload ratchet: an allowlist pattern narrowed substringCovers,
#          by a longer one is a tightening, an added one a   exactCovers
#          loosening; an import blocklist losing an entry is
#          refused, a superset accepted
#   UA-09  agent.key_health() counts the keys present;       agentKeyHealth,
#          agent.check() names a missing key                 agentCheck
#   UA-10  a step-up challenge reply that uses only the      scoreStepUpChallengeRatio's
#          stems of the mandate's words passes; an off-topic  stem
#          one fails (lease expiry draws the challenge)
#   UA-11  context_strategy "summary" puts the DriftState    buildChallengeSummary
#          preamble at the head of a windowed request
#   UA-13  the summary preamble's key entities are the five  buildChallengeSummary's
#          widest-context ones (same run as UA-11; needs      entity-sort lambda
#          behavioral_sequences on: without it CDD gets no
#          AGENT_RESPONSE event and records no entity)
#   UA-12  the tool-call admissibility gate's "block" action enforceOutputAdmissibilityGate
#          ends the turn; "quarantine" refuses the call and
#          goes on (the control that the gate fires)
#
# Not here, each recorded in docs/sweeps/A64.tsv: saveGovernanceBaseline and
# checkGovernanceBaseline::check (test_baseline_protection.sh BP-00 fails
# without either), checkMustDifferentiate (test_exec_contract_scope.sh EC-04c),
# and rows no stub run reaches or no behaviour distinguishes.
#
# KILL TABLE (each mutant run 2026-11-26, one build each; each turned its arm red)
#   U1  getCodegenEnabled returns false                     -> UA-01
#   U2  hasValidApproval returns false                       -> UA-02
#   U3  lpEncode drops its length prefix                     -> UA-02
#   U4  the allowed-path listing lambda returns ""           -> UA-03
#   U5  the breakdown comparator reversed                    -> UA-04
#   U6  mapMathAlpha maps nothing                            -> UA-05 (survived the first
#       version, whose bold call sat beside a plain `import os`)
#   U7  checkMustVary returns ""                             -> UA-06
#   U8  restoreCounterState restores nothing                 -> UA-07 (U8 and U14
#       survived the first version, which called the async fn by name)
#   U9  substringCovers always false                         -> UA-08
#   U10 exactCovers always true                              -> UA-08
#   U11 agentKeyHealth counts no key available               -> UA-09
#   U12 the challenge stem returns its word unchanged        -> UA-10
#   U13 buildChallengeSummary returns ""                     -> UA-11 (and UA-13)
#   U14 saveCounterState saves nothing                       -> UA-07
#   U15 agentCheck reports a missing key as valid            -> UA-09
#   U16 enforceOutputAdmissibilityGate returns at once       -> UA-12
#   U17 the entity-sort comparator reversed                  -> UA-13
# ============================================================
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
GOV="$REPO/build/naab-gov"
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> ${3:0:400}"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

[ -x "$NAAB" ] && [ -x "$GOV" ] || { echo "naab-lang / naab-gov not built"; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs below; exit 3 must mean the gate
T="$(mktemp -d "${TMPDIR:-/tmp}/naab_a64_XXXXXX")"
cleanup() { [ -n "${STUB_PID:-}" ] && stop_stub 2>/dev/null; teardown_isolated_trust; [ -n "${A64_KEEP:-}" ] || rm -rf "$T"; }
trap cleanup EXIT
# A govern.json above $T would be discovered by every arm.
d="$T"; while [ "$d" != "/" ] && [ -n "$d" ]; do d="$(dirname "$d")"; [ -f "$d/govern.json" ] && { echo "a govern.json above $T"; exit 1; }; done

ENF='"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}'
cfg() { mkdir -p "$1"; printf '{%s, %s}\n' "$ENF" "$2" > "$1/govern.json"; }
run() {  # DIR PROGRAM [FLAGS...] -> OUT RC
    local d="$1" p="$2"; shift 2
    printf '%s\n' "$p" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" "$@" p.naab 2>&1)"; RC=$?
}

echo "=== A64: the untested governance paths ==="

# --- UA-01 ---------------------------------------------------------------
CG='use codegen
main { print("CG=" + string(codegen.is_enabled())) }'
cfg "$T/u1t" '"codegen": {"enabled": true}';  run "$T/u1t" "$CG"; o1="$OUT"
cfg "$T/u1f" '"codegen": {"enabled": false}'; run "$T/u1f" "$CG"; o2="$OUT"
if grep -q 'CG=true' <<<"$o1" && grep -q 'CG=false' <<<"$o2"; then
    pass UA-01 "codegen.is_enabled() is true with codegen.enabled true and false without"
else fail UA-01 "codegen.is_enabled() does not follow the setting" "on: $(grep CG= <<<"$o1") off: $(grep CG= <<<"$o2")"; fi

# --- UA-02 ---------------------------------------------------------------
# Its own trust store: a trusted key makes every govern.json need a signature.
(
    export NAAB_TRUST_STORE_DIR="$T/u2trust"; mkdir -p "$NAAB_TRUST_STORE_DIR"
    D="$T/u2"; mkdir -p "$D"; K="$T/u2key.pem"
    "$NAAB" --keygen "$K" >/dev/null 2>&1 && "$NAAB" --trust-key "$K.pub" >/dev/null 2>&1 || { echo "U2SETUP=fail"; exit 0; }
    FP="$("$NAAB" --list-keys 2>&1 | grep -oE '^ +[0-9a-f]{32}' | tr -d ' ' | head -1)"
    [ -n "$FP" ] || { echo "U2SETUP=nofp"; exit 0; }
    printf '{%s,\n "custom_rules": [{"id": "APR", "name": "apr", "pattern": "approval_marker", "languages": ["javascript"], "level": "approval_required", "message": "needs approval"}],\n "approval": {"store_path": ".naab/approvals.json", "approver_keys": ["%s"]}}\n' "$ENF" "$FP" > "$D/govern.json"
    (cd "$D" && NAAB_SIGNING_KEY="$K" "$NAAB" --sign-governance >/dev/null 2>&1)
    printf 'main {\n  let r = <<javascript\nvar approval_marker = 1;\napproval_marker\n>>\n  print("RAN=" + string(r))\n}\n' > "$D/p.naab"
    rr() { (cd "$D" && timeout 60 "$NAAB" p.naab > out.txt 2>&1); echo $?; }
    echo "NONE=$(rr)"
    (cd "$D" && NAAB_SIGNING_KEY="$K" "$NAAB" --approve custom_rules.OTHER --reason "other rule" >/dev/null 2>&1)
    echo "OTHER=$(rr)"
    (cd "$D" && NAAB_SIGNING_KEY="$K" "$NAAB" --approve custom_rules.APR --reason "reviewed" >/dev/null 2>&1)
    echo "VALID=$(rr) $(grep -c 'RAN=1' "$D/out.txt")"
    cp "$D/.naab/approvals.json" "$T/u2appr.bak"
    python3 -I -c 'import json,sys; j=json.load(open(sys.argv[1])); j["custom_rules.APR"]["reason"]="edited"; json.dump(j, open(sys.argv[1], "w"))' "$D/.naab/approvals.json"
    echo "EDITED=$(rr)"
) > "$T/u2.res" 2>&1
U2="$(cat "$T/u2.res")"
if grep -q 'U2SETUP=' <<<"$U2"; then
    unm UA-02 "approval: the signing key could not be made or trusted here ($(grep U2SETUP <<<"$U2"))"
elif grep -q '^NONE=3$' <<<"$U2" && grep -q '^OTHER=3$' <<<"$U2" && grep -q '^VALID=0 1$' <<<"$U2" && grep -q '^EDITED=3$' <<<"$U2"; then
    pass UA-02 "approval_required: no token, another rule's token and an edited reason refuse (exit 3); the valid signed token lets it run"
else fail UA-02 "the approval decision is wrong" "$(tr '\n' ' ' <<<"$U2")"; fi

# --- UA-03 ---------------------------------------------------------------
cfg "$T/u3" '"agents": {"worker": {"allowed_paths": ["./data"]}}'
mkdir -p "$T/u3/data"; echo inside > "$T/u3/data/a.txt"; echo outside > "$T/u3/b.txt"
run "$T/u3" 'use file
main { print(file.read("data/a.txt")) }' --agent-id worker; in_rc=$RC
run "$T/u3" 'use file
main { print(file.read("b.txt")) }' --agent-id worker
if [ "$in_rc" = 0 ] && [ "$RC" = 3 ] && grep -q 'restricts file access to: ./data' <<<"$OUT"; then
    pass UA-03 "a role's path refusal names the directories it allows (the read inside them runs)"
else fail UA-03 "the role refusal does not list its allowed paths" "inside=$in_rc outside=$RC $(grep -o 'restricts file access to:.*' <<<"$OUT" | head -1)"; fi

# --- UA-04 ---------------------------------------------------------------
# ALOW sorts first by name and weighs least: only the comparator puts ZHIGH first.
cfg "$T/u4" '"custom_rules": [{"id": "ALOW", "name": "alow", "pattern": "low_marker", "languages": ["javascript"], "level": "advisory", "message": "low"},
  {"id": "ZHIGH", "name": "zhigh", "pattern": "high_marker", "languages": ["javascript"], "level": "advisory", "message": "high"}],
 "scoring": {"enabled": true, "default_weight": 1, "rule_weights": {"custom_rules.ALOW": 2, "custom_rules.ZHIGH": 7}, "yellow_threshold": 1, "red_threshold": 5}'
run "$T/u4" 'main {
  let a = <<javascript
var low_marker = 1;
low_marker
>>
  let b = <<javascript
var high_marker = 2;
high_marker
>>
  print("RAN")
}'
first="$(grep -A1 'Score breakdown:' <<<"$OUT" | tail -1)"
if grep -q 'custom_rules.ZHIGH.*fix first' <<<"$first" && grep -q 'custom_rules.ALOW' <<<"$OUT"; then
    pass UA-04 "the score breakdown lists the heaviest rule first (+7 before +2), against name order"
else fail UA-04 "the breakdown is not ordered by weight" "rc=$RC first line: $first"; fi

# --- UA-05 ---------------------------------------------------------------
# Every letter bold, the import included: a plain `import os` alone is refused
# by code_injection (the importonly control), so a plain import beside a bold
# call is refused whether or not the bold letters are read -- the first version
# of this arm did exactly that, and U6 survived it.
cfg "$T/u5" '"restrictions": {"code_injection": {"enabled": true, "level": "hard"}}'
python3 -I - "$T/u5" <<'PY'
import sys
d = sys.argv[1]
bold = lambda s: "".join(chr(0x1D41A + ord(c) - 97) if c.isalpha() else c for c in s)
blk = lambda body: "main {\n  let r = <<python\n" + body + "\n1\n>>\n  print(\"RAN\")\n}\n"
open(d + "/bold.naab", "w", encoding="utf-8").write(blk(bold("import os") + "\n" + bold("os.system") + "(\"true\")"))
open(d + "/plain.naab", "w", encoding="utf-8").write(blk("import os\nos.system(\"true\")"))
open(d + "/importonly.naab", "w", encoding="utf-8").write(blk("import os"))
open(d + "/benign.naab", "w", encoding="utf-8").write(blk("x = \"" + bold("hello") + "\""))
PY
r5() { (cd "$T/u5" && timeout 60 "$NAAB" "$1.naab" > "$1.out" 2>&1); echo $?; }
b=$(r5 bold); p=$(r5 plain); n=$(r5 benign); io=$(r5 importonly)
if [ "$p" != 3 ] || [ "$io" != 3 ]; then
    fail UA-05 "CONTROL: plain os.system / import os was not refused (rc=$p/$io) -- the arm cannot judge the bold spelling"
elif [ "$b" = 3 ] && grep -q 'restrictions.code_injection' "$T/u5/bold.out" && [ "$n" = 0 ]; then
    pass UA-05 "a block written in Unicode bold letters (import os; os.system) is refused like the plain spelling; benign bold text runs"
else fail UA-05 "the bold spelling is not normalised" "bold=$b benign=$n"; fi

# --- UA-06 ---------------------------------------------------------------
cfg "$T/u6" '"contracts": {"functions": {"total": {"must_vary": [{"args": [1, 2]}, {"args": [3, 4]}]}}}'
run "$T/u6" 'fn total(a, b) { return a + b }
main { print(total(1, 2)) }'; sum_rc=$RC
run "$T/u6" 'fn total(a, b) { return 7 }
main { print(total(1, 2)) }'
if [ "$sum_rc" = 0 ] && [ "$RC" = 3 ] && grep -q 'contracts.must_vary' <<<"$OUT"; then
    pass UA-06 "must_vary refuses a function whose result never varies; a summing one passes"
else fail UA-06 "must_vary decided wrongly" "sum=$sum_rc const=$RC"; fi

# --- UA-07 ---------------------------------------------------------------
# Called through a dict field: on the tree-walker an async fn called BY NAME
# runs synchronously on main's engine (ASYNC-001), while this call reaches the
# async branch that entry calls unreachable (A228), with its fresh engine and the
# counts it inherits. TY=future is the control that it did.
mkdir -p "$T/u7"; printf '{"version": "5.0", "mode": "audit", "limits": {"execution": {"polyglot_blocks": 2}}}\n' > "$T/u7/govern.json"
ASYNC='async fn later() {
  let c = <<javascript
3
>>
  return c
}
'
run "$T/u7" "${ASYNC}main {
  let a = <<javascript
1
>>
  let b = <<javascript
2
>>
  let d = {\"h\": later}
  let f = d.h()
  print(\"TY=\" + typeof(f))
  print(\"V=\" + string(await f))
}" --tree-walk; o2="$OUT"
run "$T/u7" "${ASYNC}main {
  let a = <<javascript
1
>>
  let d = {\"h\": later}
  let f = d.h()
  print(\"TY=\" + typeof(f))
  print(\"V=\" + string(await f))
}" --tree-walk; o1="$OUT"
if ! grep -q 'V=3' <<<"$o1$o2"; then
    unm UA-07 "the tree-walker's async fn did not run here"
elif ! grep -q 'TY=future' <<<"$o2"; then
    # A call that runs synchronously counts on main's own engine and reads the
    # same (2 + 1 reported) with save/restore removed -- not a test of them.
    unm UA-07 "the call through a dict field did not run asynchronously on this build (A228), so save/restore are not reached"
elif grep -q 'AUDIT limits.execution.polyglot_blocks.*count 3 exceeds limit of 2' <<<"$o2" && ! grep -q 'polyglot_blocks' <<<"$o1"; then
    pass UA-07 "an async fn's block counts on top of main's: 2 + 1 over a limit of 2 is reported, 1 + 1 is not"
else fail UA-07 "the async fn's count does not start from main's" "$(grep -m1 polyglot_blocks <<<"$o2")"; fi

# --- UA-08 ---------------------------------------------------------------
mkdir -p "$T/u8"
w() { printf '%s\n' "$2" > "$T/u8/$1.json"; }
w op '{"mode": "enforce", "code_quality": {"no_pii": {"enabled": true, "allowlist_patterns": ["test-"]}}}'
w ap '{"mode": "enforce", "code_quality": {"no_pii": {"enabled": true, "allowlist_patterns": ["test-", "demo-"]}}}'
w np '{"mode": "enforce", "code_quality": {"no_pii": {"enabled": true, "allowlist_patterns": ["test-x"]}}}'
w oi '{"mode": "enforce", "restrictions": {"imports": {"enabled": true, "blocked": {"python": ["os", "re"]}}}}'
w ri '{"mode": "enforce", "restrictions": {"imports": {"enabled": true, "blocked": {"python": ["os"]}}}}'
w si '{"mode": "enforce", "restrictions": {"imports": {"enabled": true, "blocked": {"python": ["os", "re", "ctypes"]}}}}'
rt() { (cd "$T/u8" && "$GOV" ratchet "$1.json" "$2.json" --ratchet-only >/dev/null 2>&1); echo $?; }
a=$(rt op ap); b=$(rt op np); c=$(rt oi ri); e=$(rt oi si)
if [ "$a$b$c$e" = "1010" ]; then
    pass UA-08 "ratchet: an added allowlist pattern is refused, a narrowed one accepted; a blocklist losing an entry is refused, a superset accepted"
else fail UA-08 "the list comparison decided wrongly" "added=$a narrowed=$b removed=$c superset=$e (want 1 0 1 0)"; fi

# --- UA-09 ---------------------------------------------------------------
cfg "$T/u9" '"agents": {"bot": {"provider": "gemini", "model": "m", "api_key_env": ["UA9_K1", "UA9_K2"], "system_prompt": "x"}}'
KH='use agent
main {
  let h = agent.key_health("bot")
  print("KH=" + string(h.get("available")))
  let c = agent.check("bot")
  print("CK=" + string(c.get("valid")) + ":" + string(c.get("error", "")))
}'
printf '%s\n' "$KH" > "$T/u9/p.naab"
o0="$(cd "$T/u9" && env -u UA9_K1 -u UA9_K2 timeout 60 "$NAAB" p.naab 2>&1)"
o1="$(cd "$T/u9" && UA9_K1=fake-ua9 timeout 60 "$NAAB" p.naab 2>&1)"
if grep -q 'KH=0' <<<"$o0" && grep -q "CK=false:API key 'UA9_K1' not found" <<<"$o0" \
   && grep -q 'KH=1' <<<"$o1" && grep -q 'CK=true:' <<<"$o1"; then
    pass UA-09 "agent.key_health() counts the keys present (0, then 1); agent.check() names the missing key, then passes"
else fail UA-09 "key_health/check answered wrongly" "$(grep -E 'KH=|CK=' <<<"$o0$o1" | tr '\n' ' ')"; fi

# --- UA-10 / UA-11: the agent stub ---------------------------------------
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
# The stub hangs on Windows/MSYS2 (stub_platform.sh says why); that helper
# exits the whole suite, so the same test is made here for these two arms only.
stub_ok=1
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) stub_ok=0 ;; esac
[ -n "${WINDIR:-}" ] && stub_ok=0
command -v python3 >/dev/null 2>&1 || stub_ok=0
if [ "$stub_ok" = 0 ]; then
    unm UA-10 "the agent stub does not run on this platform"; unm UA-11 "the agent stub does not run on this platform"
else
    agent_cfg() {  # DIR EXTRA_AGENT_JSON
        cfg "$1" "\"telemetry\": {\"enabled\": true, \"output_file\": \"tele.jsonl\"}, \"behavioral_sequences\": {\"enabled\": true}, \"context_drift\": {\"enabled\": true, \"level\": \"advisory\", \"check_interval_turns\": 1}, \"circuit_breaker\": {\"enabled\": true, \"step_up_enabled\": true},
 \"agents\": {\"dev\": {\"provider\": \"gemini\", \"model\": \"stub-model\", \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"FAKE_UA_KEY\", \"max_tokens\": 100, \"max_turns\": 20, $2}}"
    }
    SENDS2='use agent
main {
  let h = agent.create("dev")
  agent.send(h, "one")
  let r2 = agent.send(h, "two")
  print("R2=" + r2.get("content"))
}'
    # UA-10: standing_lease_turns 1 expires the lease after one turn, so the
    # second send draws a mandate challenge, scored against the system prompt.
    ua10() {  # ID CHALLENGE_REPLY -> RC10
        local d="$T/$1"; mkdir -p "$d"
        printf '{"responses": [{"content": "first answer"}, {"content": "%s"}, {"content": "second answer"}]}' "$2" > "$d/fx.json"
        start_stub "$d/fx.json" "$d" || { RC10=nostub; return; }
        agent_cfg "$d" '"standing_lease_turns": 1, "system_prompt": "You are a deploying assistant reviewing builds"'
        printf '%s\n' "$SENDS2" > "$d/p.naab"
        (cd "$d" && FAKE_UA_KEY=k timeout 60 "$NAAB" p.naab > out.txt 2>&1); RC10=$?
        stop_stub
    }
    ua10 u10s "I assist with deploy work and review each build carefully all day long today"; s_rc=$RC10
    ua10 u10o "Bananas are yellow fruit that grow in warm places and taste sweet indeed"; o_rc=$RC10
    if [ "$s_rc" = nostub ] || [ "$o_rc" = nostub ]; then unm UA-10 "the stub did not start"
    elif [ "$o_rc" = 3 ] && grep -q 'Step-up challenge failed' "$T/u10o/out.txt" \
         && [ "$s_rc" = 0 ] && grep -q 'R2=second answer' "$T/u10s/out.txt"; then
        pass UA-10 "a challenge reply using only the mandate words' stems passes; an off-topic one fails (exit 3)"
    else fail UA-10 "the stemmed challenge reply was judged wrongly" "stems=$s_rc offtopic=$o_rc"; fi

    # UA-11: a window of 2 with the summary strategy -- the third request's
    # first message starts with the DriftState preamble.
    # UA-13, same run: the preamble's "Key entities" are the reply words of more
    # than four characters, each with the other words of its reply as context, and
    # are listed widest context first (top 5). Reply 1 has six such words (each
    # with five others), reply 2 two (one other each), so neither of reply 2's
    # may be listed.
    d="$T/u11"; mkdir -p "$d"
    printf '{"responses": [{"content": "kubernetes postgres rabbitmq terraform grafana prometheus"}, {"content": "zebrafish quokkas"}, {"content": "third"}]}' > "$d/fx.json"
    if start_stub "$d/fx.json" "$d"; then
        agent_cfg "$d" '"system_prompt": "You are a deployment assistant", "context_window": 2, "context_strategy": "summary"'
        printf 'use agent\nmain {\n  let h = agent.create("dev")\n  agent.send(h, "tell me about the cluster")\n  agent.send(h, "and the cache")\n  agent.send(h, "now summarise")\n  print("OK")\n}\n' > "$d/p.naab"
        (cd "$d" && FAKE_UA_KEY=k timeout 60 "$NAAB" p.naab > out.txt 2>&1); rc=$?
        stop_stub
        first="$(python3 -I -c 'import json,sys; c=json.load(sys.stdin.buffer).get("contents",[]); sys.stdout.buffer.write((c[0]["parts"][0]["text"] if c else "").encode("utf-8"))' < "$d/req_3.json" 2>/dev/null)"
        one="$(python3 -I -c 'import json,sys; c=json.load(sys.stdin.buffer).get("contents",[]); sys.stdout.buffer.write((c[0]["parts"][0]["text"] if c else "").encode("utf-8"))' < "$d/req_1.json" 2>/dev/null)"
        if [ "$rc" = 0 ] && grep -q '^\[Context: Turn 2 of ongoing session. Your role:' <<<"$first" && ! grep -q 'Context: Turn' <<<"$one"; then
            pass UA-11 "a windowed request under the summary strategy starts with the DriftState preamble (the unwindowed first one does not)"
        else fail UA-11 "no summary preamble in the windowed request" "rc=$rc first message: ${first:0:160}"; fi
        ents="$(grep -o 'Key entities:[^.]*' <<<"$first" | head -1)"
        listed=0; for w in kubernetes postgres rabbitmq terraform grafana prometheus; do grep -q "\b$w\b (" <<<"$ents" && listed=$((listed+1)); done
        if [ -z "$ents" ]; then
            fail UA-13 "CONTROL: the preamble lists no key entities -- the arm cannot judge their order" "${first:0:200}"
        elif [ "$listed" = 5 ] && ! grep -qE 'zebrafish|quokkas' <<<"$ents"; then
            pass UA-13 "the preamble's key entities are the five with the widest context (reply 1's), none of reply 2's"
        else fail UA-13 "the key entities are not ordered by context width" "$ents"; fi
    else unm UA-11 "the stub did not start"; unm UA-13 "the stub did not start"; fi

    # UA-12: the tool-call admissibility gate with action "block" (the
    # quarantine action refuses the call and the turn goes on; block ends the
    # turn). Two clean turns, four that break the json format so coherence
    # falls, then a tool call. The quarantine run is the control that the gate
    # fires on this fixture at all. "detect" keeps the response gate's blocks of
    # the warm-up turns catchable; a hard level would end the run there.
    ua12() {  # ID ACTION_JSON -> RC12, dir $T/ID
        local d="$T/$1"; mkdir -p "$d"
        cat > "$d/fx.json" <<'EOF'
{"responses": [
  {"content": "{\"revenue\": 1, \"period\": \"q1\"}", "output_tokens": 30},
  {"content": "{\"revenue\": 2, \"period\": \"q2\"}", "output_tokens": 30},
  {"content": "not json response one for coherence decay", "output_tokens": 30},
  {"content": "not json response two for coherence decay", "output_tokens": 30},
  {"content": "not json response three for coherence decay", "output_tokens": 30},
  {"content": "not json response four for coherence decay", "output_tokens": 30},
  {"tool_calls": [{"name": "get_data", "args": {"query": "revenue"}}]},
  {"content": "final text after denied tool", "output_tokens": 20}
]}
EOF
        start_stub "$d/fx.json" "$d" || { RC12=nostub; return; }
        cfg "$d" "\"telemetry\": {\"enabled\": true, \"output_file\": \"tele.jsonl\"}, \"behavioral_sequences\": {\"enabled\": true},
 \"context_drift\": {\"enabled\": true, \"check_interval_turns\": 1, \"adaptive_baseline_window\": 2},
 \"circuit_breaker\": {\"enabled\": true, \"output_admissibility\": {\"enabled\": true, \"threshold\": 0.95, $2, \"gate_tool_calls\": true, \"max_quarantine_streak\": 0}},
 \"agents\": {\"tooler\": {\"provider\": \"gemini\", \"model\": \"stub-model\", \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"FAKE_UA_KEY\", \"max_tokens\": 100, \"max_turns\": 20, \"response_format\": \"json\", \"tools_enabled\": true, \"tools\": [\"get_data\"]}}"
        cat > "$d/p.naab" <<'EOF'
use agent
use file
fn get_data(query) {
    file.write("tool_side_effect.txt", "tool ran")
    return "data for: " + string(query)
}
main {
    agent.register_tool("get_data", get_data, {
        "description": "Fetch data by query",
        "parameters": { "query": {"type": "string", "description": "what to fetch"} }
    })
    let h = agent.create("tooler")
    for i in 0..6 {
        try { let w = agent.send(h, "warm up turn " + string(i)) } catch (e) { print("WARMUP_ERR") }
    }
    try {
        let r = agent.send(h, "use the tool to fetch revenue data")
        let res = r.get("tool_results")
        print("TURN_DONE")
        if res != null { if res.length() > 0 { print("TOOL_ERROR=" + string(res[0].get("error"))) } }
    } catch (e) {
        print("TURN_BLOCKED=" + string(e))
    }
}
EOF
        (cd "$d" && FAKE_UA_KEY=k timeout 90 "$NAAB" p.naab > out.txt 2>&1); RC12=$?
        stop_stub
    }
    ua12 u12q '"action": "quarantine"'; q_rc=$RC12
    ua12 u12b '"action": "block", "level": "detect"'; b_rc=$RC12
    if [ "$q_rc" = nostub ] || [ "$b_rc" = nostub ]; then unm UA-12 "the stub did not start"
    elif ! grep -q 'TOOL_ERROR=admissibility_gate' "$T/u12q/out.txt" || grep -q 'TURN_BLOCKED' "$T/u12q/out.txt"; then
        fail UA-12 "CONTROL: the quarantine gate did not refuse the tool call and go on" "$(grep -E 'TURN_|TOOL_' "$T/u12q/out.txt" | head -3)"
    elif grep -q 'TURN_BLOCKED=.*Tool execution not admissible' "$T/u12b/out.txt" && ! grep -q 'TURN_DONE' "$T/u12b/out.txt" \
         && [ ! -e "$T/u12b/tool_side_effect.txt" ] && [ ! -e "$T/u12q/tool_side_effect.txt" ]; then
        pass UA-12 "the tool gate's block action ends the turn (quarantine refuses the call and goes on); the tool ran in neither"
    else fail UA-12 "the block action did not end the turn" "rc=$b_rc $(grep -E 'TURN_|TOOL_' "$T/u12b/out.txt" | head -3)"; fi
fi

echo
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
