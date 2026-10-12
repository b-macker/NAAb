#!/usr/bin/env bash
# ============================================================
# test_guidance.sh — live governance guidance (docs/plan-live-guidance.md)
#
# Operator-written project knowledge, declared in govern.json, delivered when
# governance observes something: an agent's first send, a drift signal that
# paid a penalty, a failed validation, or a rule violation. The channel is a
# new route into a model, so most arms here are about governing IT:
#
#   reaches the model     G-02 start, G-03 signal (+cooldown), G-04 keyword
#                         relevance, G-05 a live edit mid-run, G-13 budget
#   never stored          G-02 (turn 2's request carries no turn-1 guidance)
#   governed read         G-07 a source outside the path policy is not read,
#                         G-08 govern.json can never be a source
#   withheld, not blocked G-06 a section holding a secret (control G-06b)
#   no self-instruction   G-09 the program cannot write a source: file, io,
#                         a hard link, embedded Python (controls: another file,
#                         guidance off), both engines
#   terminal              G-10 a HARD block carries the section, both engines
#                         (control: a non-matching rule), G-11 advisory
#   pull                  G-12 governance.guidance(), by keyword and anchor
#   provenance            G-15 GUIDANCE_DELIVERED sha256 = the delivered text,
#                         chain still verifies
#   config                G-16 extends inherits, G-17 load warnings
#
# Every refusal is paired with a success through the same fixture, and every
# stub arm first proves the run completed (RUN_DONE) and the stub saw requests
# -- a dead interpreter or dead stub fails G-00, never passes an absence check.
#
# A stub proves WIRING. Whether a real model follows guidance is not measured
# here (the same line test_steering_efficacy.sh draws).
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
TEST_TMP="$(mktemp -d)"

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo "  SKIP [$1] UNMEASURABLE: $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
STUB_STARTED=0
cleanup() {
    [ "$STUB_STARTED" = 1 ] && stop_stub >/dev/null 2>&1
    command -v stop_swap_operators >/dev/null 2>&1 && stop_swap_operators
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] || rm -rf "$TEST_TMP"
}
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

# A secret-shaped token, built at runtime so no literal pattern sits in the
# repository for secret scanners to flag.
FAKE_KEY="AKIA$(printf 'Q%.0s' $(seq 1 16))"

write_playbook() {  # $1 = dir
    mkdir -p "$1/docs"
    cat > "$1/docs/playbook.md" <<EOF
# Agent playbook

Preamble that belongs to no section.

## Onboarding
GUIDE_ONBOARD_q7: read the README before answering.

## Avoid repetition
GUIDE_REPEAT_q7: never send the same answer twice; vary the wording and add new detail.

## Database migrations
GUIDE_DB_q7: a failed migration is rolled back with the rollback script; check the schema version first.

## Frontend styling
GUIDE_CSS_q7: layout and colors come from the shared stylesheet; never inline styles.

## Stubs
GUIDE_STUBS_q7: unfinished work goes in stubs/ with a ticket number.

## Secrets
GUIDE_SECRET_q7: the staging key is $FAKE_KEY.

## Contact
GUIDE_PII_q7: escalate billing questions to the on-call, SSN 123-45-6789.

## Long
GUIDE_LONG_q7: $(printf 'word%.0s ' $(seq 1 150))
EOF
}

# guidance block: $1 = rules JSON array, $2 = extra guidance keys (may be empty)
GUIDANCE_ENABLED=true
guidance_json() {
    printf '"guidance": {"enabled": %s, %s "sources": {"playbook": {"type": "file", "path": "docs/playbook.md"}}, "rules": %s}' "$GUIDANCE_ENABLED" "${2:-}" "$1"
}

# ---------------------------------------------------------------------------
# Part 1 — no stub needed: write protection, terminal channel, pull, config
# ---------------------------------------------------------------------------
echo ""
echo "== Part 1: write protection, terminal channel, pull, load warnings"

# run DIR ENGINE PROG -> "rc=<n>|<stdout>" ; stderr kept in DIR/err.txt
run() {
    local d="$1" eng="$2" prog="$3"
    printf '%s\n' "$prog" > "$d/p.naab"
    local args=("$NAAB"); [ "$eng" = tw ] && args+=(--tree-walk); args+=(p.naab)
    local out; out="$(cd "$d" && "${args[@]}" 2>"$d/err.txt")"; local rc=$?
    printf 'rc=%s|%s' "$rc" "$out"
}

wp_cfg() {  # $1 dir, $2 "on"|"off"
    write_playbook "$1"
    local en=true; [ "$2" = off ] && en=false
    cat > "$1/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "guidance": {"enabled": $en,
   "sources": {"playbook": {"type": "file", "path": "docs/playbook.md"}},
   "rules": [{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]}}
EOF
}

for eng in vm tw; do
    d="$TEST_TMP/wp_$eng"; mkdir -p "$d"; wp_cfg "$d" on
    before="$(cat "$d/docs/playbook.md")"
    r="$(run "$d" "$eng" 'use file
main {
    file.write("docs/playbook.md", "OVERWRITTEN by the program")
    print("WROTE")
}')"
    after="$(cat "$d/docs/playbook.md")"
    ctl="$(run "$d" "$eng" 'use file
main {
    file.write("notes.md", "ordinary output")
    print("WROTE")
}')"
    if [[ "$ctl" != rc=0*WROTE* ]]; then
        fail "G-09a/$eng" "control: an ordinary write must succeed under the same config" "${ctl}"
    elif [[ "$r" == *WROTE* ]] || [ "$before" != "$after" ]; then
        fail "G-09a/$eng" "the program wrote a guidance source" "$r"
    elif [[ "$r" == rc=3* ]] && [[ "$r$(cat "$d/err.txt")" == *"guidance file is blocked"* ]]; then
        pass "G-09a/$eng" "file.write to the source is a HARD block (exit 3); the file is unchanged; another file writes fine"
    else
        fail "G-09a/$eng" "expected exit 3 naming the guidance file" "$r / $(head -3 "$d/err.txt")"
    fi
done

d="$TEST_TMP/wp_io"; mkdir -p "$d"; wp_cfg "$d" on
r="$(run "$d" vm 'use io
main {
    io.write_file("docs/playbook.md", "OVERWRITTEN")
    print("WROTE")
}')"
if [[ "$r" == rc=3* ]] && ! grep -q OVERWRITTEN "$d/docs/playbook.md"; then
    pass "G-09b" "io.write_file to the source is refused too"
else
    fail "G-09b" "io.write_file reached the source" "$r"
fi

d="$TEST_TMP/wp_link"; mkdir -p "$d"; wp_cfg "$d" on
if ln "$d/docs/playbook.md" "$d/alias.md" 2>/dev/null; then
    r="$(run "$d" vm 'use file
main {
    file.write("alias.md", "OVERWRITTEN through a hard link")
    print("WROTE")
}')"
    if [[ "$r" == rc=3* ]] && ! grep -q OVERWRITTEN "$d/docs/playbook.md"; then
        pass "G-09c" "a hard link to the source is the same source: refused"
    else
        fail "G-09c" "a hard link let the program write the source" "$r"
    fi
else
    skip "G-09c" "this filesystem cannot make a hard link"
fi

d="$TEST_TMP/wp_off"; mkdir -p "$d"; wp_cfg "$d" off
r="$(run "$d" vm 'use file
main {
    file.write("docs/playbook.md", "rewritten while guidance is off")
    print("WROTE")
}')"
if [[ "$r" == rc=0*WROTE* ]] && grep -q "rewritten while guidance" "$d/docs/playbook.md"; then
    pass "G-09d" "scope control: with guidance off the same file is an ordinary file"
else
    fail "G-09d" "guidance off still refused the write -- the gate is wider than the feature" "$r"
fi

# Embedded Python: the audit hook asks the same path decision.
d="$TEST_TMP/wp_py"; mkdir -p "$d"; wp_cfg "$d" on
pyc="$(run "$d" vm 'main {
    let x = <<python
open("py_ok.txt", "w").write("ok")
"PYRAN"
>>
    print(x)
}')"
if [[ "$pyc" != *PYRAN* ]] || [ ! -f "$d/py_ok.txt" ]; then
    skip "G-09e" "embedded Python not usable here: ${pyc:0:120}"
else
    r="$(run "$d" vm 'main {
    let x = <<python
try:
    open("docs/playbook.md", "w").write("OVERWRITTEN from python")
    r = "PYWROTE"
except PermissionError:
    r = "PYDENIED"
r
>>
    print(x)
}')"
    if [[ "$r" == *PYDENIED* ]] && ! grep -q OVERWRITTEN "$d/docs/playbook.md"; then
        pass "G-09e" "embedded Python open(w) on the source: PermissionError; a write elsewhere ran"
    else
        fail "G-09e" "embedded Python wrote the source" "$r"
    fi
fi

# --- terminal channel ---
term_cfg() {  # $1 dir, $2 rule-on, $3 level
    write_playbook "$1"
    cat > "$1/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "code_quality": {"no_placeholders": {"enabled": true, "level": "$3"}},
 $(guidance_json "[{\"on\": \"$2\", \"from\": \"playbook#stubs\", \"to\": [\"terminal\"]}]")}
EOF
}
PLACEHOLDER_PROG='fn work() {
    // TODO: finish this later
    return 1
}
main {
    print(work())
    print("RAN_ON")
}'
for eng in vm tw; do
    d="$TEST_TMP/term_$eng"; mkdir -p "$d"
    term_cfg "$d" "violation:code_quality.no_placeholders" hard
    # NAAb prints the Error: block on STDOUT; the message is read from both.
    r="$(run "$d" "$eng" "$PLACEHOLDER_PROG")"
    e="$r$(cat "$d/err.txt")"
    dc="$TEST_TMP/termctl_$eng"; mkdir -p "$dc"
    term_cfg "$dc" "violation:code_quality.no_secrets" hard
    rc_="$(run "$dc" "$eng" "$PLACEHOLDER_PROG")"
    ec="$rc_$(cat "$dc/err.txt")"
    if [[ "$rc_" != rc=3* ]] || [[ "$ec" != *Placeholder* ]]; then
        fail "G-10/$eng" "control: the placeholder must be blocked under both configs" "$rc_"
    elif [[ "$ec" == *GUIDE_STUBS_q7* ]]; then
        fail "G-10/$eng" "a rule for a different check attached guidance to this block"
    elif [[ "$r" == rc=3* ]] && [[ "$e" == *"Project guidance (docs/playbook.md > Stubs):"* ]] \
         && [[ "$e" == *GUIDE_STUBS_q7* ]] && [[ "$e" != *GUIDE_DB_q7* ]]; then
        pass "G-10/$eng" "the HARD block carries the Stubs section (exit 3 unchanged); a non-matching rule adds nothing"
    else
        fail "G-10/$eng" "the block message lacks the guidance section" "$r / $(echo "$e" | tail -5)"
    fi
done

# G-10b: a prefix rule (violation:code_quality.*) matches the check by prefix.
d="$TEST_TMP/term_prefix"; mkdir -p "$d"
term_cfg "$d" "violation:code_quality.*" hard
r="$(run "$d" vm "$PLACEHOLDER_PROG")"
if [[ "$r$(cat "$d/err.txt")" == *GUIDE_STUBS_q7* ]] && [[ "$r" == rc=3* ]]; then
    pass "G-10b" "a prefix rule (violation:code_quality.*) attaches the section to code_quality.no_placeholders"
else
    fail "G-10b" "prefix rule did not match" "${r:0:200}"
fi

d="$TEST_TMP/term_adv"; mkdir -p "$d"
term_cfg "$d" "violation:code_quality.no_placeholders" advisory
r="$(run "$d" vm "$PLACEHOLDER_PROG")"
if [[ "$r" == rc=0*RAN_ON* ]] && grep -q "GUIDE_STUBS_q7" "$d/err.txt"; then
    pass "G-11" "an advisory prints the section and the run continues"
else
    fail "G-11" "advisory path: guidance missing or run did not continue" "$r / $(tail -4 "$d/err.txt")"
fi

# --- pull ---
d="$TEST_TMP/pull"; mkdir -p "$d"; wp_cfg "$d" on
r="$(run "$d" vm 'use governance
use array
main {
    let a = governance.guidance("migration rollback schema")
    print("KW=" + a[0].get("heading") + "|" + a[0].get("select"))
    let b = governance.guidance("playbook#stubs")
    print("AN=" + b[0].get("text"))
    let c = governance.guidance("zzqx nothing matches")
    print("NONE=" + string(array.length(c)))
}')"
if [[ "$r" == *"KW=Database migrations|keywords"* ]] && [[ "$r" == *"AN=GUIDE_STUBS_q7"* ]] && [[ "$r" == *"NONE=0"* ]]; then
    pass "G-12a" "governance.guidance: keyword match picks Database migrations; anchor returns Stubs; no match is empty"
else
    fail "G-12a" "pull results wrong" "$r"
fi
d="$TEST_TMP/pull_off"; mkdir -p "$d"; wp_cfg "$d" off
r="$(run "$d" vm 'use governance
use array
main {
    print("N=" + string(array.length(governance.guidance("migration rollback"))))
}')"
if [[ "$r" == rc=0*"N=0"* ]]; then
    pass "G-12b" "guidance off: the pull returns nothing"
else
    fail "G-12b" "guidance off still returned entries" "$r"
fi

# --- policy-gated read, protected source (pull exercises the read path) ---
d="$TEST_TMP/policy"; mkdir -p "$d"; write_playbook "$d"
cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "capabilities": {"filesystem": {"blocked_paths": ["docs"]}},
 $(guidance_json '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]')}
EOF
r="$(run "$d" vm 'use governance
use array
main {
    print("N=" + string(array.length(governance.guidance("playbook#onboarding"))))
}')"
if [[ "$r" == *"N=0"* ]] && grep -q "is not read: the filesystem policy" "$d/err.txt"; then
    pass "G-07" "a source under a blocked path is not read (warning names it); G-12a is the readable control"
else
    fail "G-07" "a blocked source was read, or no warning" "$r / $(cat "$d/err.txt" | head -3)"
fi

d="$TEST_TMP/protected"; mkdir -p "$d"
cat > "$d/govern.json" <<'EOF'
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "guidance": {"enabled": true,
   "sources": {"cfg": {"type": "file", "path": "govern.json"}},
   "rules": [{"on": "start", "from": "cfg", "to": ["agent"]}]}}
EOF
r="$(run "$d" vm 'use governance
use array
main {
    let a = governance.guidance("version mode enforce guidance")
    print("N=" + string(array.length(a)))
}')"
if [[ "$r" == *"N=0"* ]] && grep -q "is not read: the filesystem policy" "$d/err.txt"; then
    pass "G-08" "govern.json (auto-protected) can never be a guidance source"
else
    fail "G-08" "govern.json was readable as guidance" "$r"
fi

# --- load warnings ---
d="$TEST_TMP/warn"; mkdir -p "$d"; write_playbook "$d"
cat > "$d/govern.json" <<'EOF'
{"version": "5.0", "mode": "enforce",
 "guidance": {"enabled": true,
   "sources": {"playbook": {"type": "file", "path": "docs/playbook.md"}},
   "rules": [{"on": "signal:circular", "from": "playbook", "to": ["agent"]},
             {"on": "start", "from": "nosuch#x", "to": ["agent"]},
             {"on": "start", "from": "playbook", "to": ["agent"], "agnets": ["a"]}]}}
EOF
r="$(run "$d" vm 'main { print("OK") }')"
e="$(cat "$d/err.txt")"
if [[ "$e" == *'circular'*'circular_actions'* ]] && [[ "$e" == *'no source "nosuch"'* ]] && [[ "$e" == *agnets* ]] && [[ "$r" == rc=0*OK* ]]; then
    pass "G-17" "a telemetry label names the setting key, an undeclared source and an unknown key are reported; the run continues"
else
    fail "G-17" "load warnings missing" "$(echo "$e" | grep -i 'guidance\|agnets' | head -5)"
fi

# ---------------------------------------------------------------------------
# Part 2 — agent channel through the stub
# ---------------------------------------------------------------------------
echo ""
echo "== Part 2: agent channel (stub)"
# stub_platform.sh's skip exits the whole script; Part 1 above must still
# count on Windows, so the same two signals are checked here instead.
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) WINDOWS=1 ;; *) WINDOWS=0 ;; esac
[ -n "${WINDIR:-}" ] && WINDOWS=1
if [ "$WINDOWS" = 1 ]; then
    skip "G-AGENT" "stub-backed arms are not run on Windows (stub_platform.sh)"
else
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"
export FAKE_KEY_GUIDE="fake-key-guidance"

FIXTURE="$TEST_TMP/fixture.json"
cat > "$FIXTURE" <<'EOF'
{"responses": [{"content": "The billing total is computed from the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}
EOF

# agent_run DIR RULES EXTRA_GUIDANCE_KEYS EXTRA_TOPLEVEL PROGRAM_BODY
# EXTRA_TOPLEVEL is inserted verbatim (with its trailing comma) before "agents".
agent_run() {
    local d="$1" rules="$2" gextra="$3" top="$4" body="$5"
    mkdir -p "$d"; write_playbook "$d"
    start_stub "$FIXTURE" "$d" >/dev/null 2>&1 || { echo "STUB_FAIL"; return; }
    STUB_STARTED=1
    cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl"},
 $top
 $(guidance_json "$rules" "$gextra"),
 "agents": {"worker": {"provider": "gemini", "model": "stub-model",
   "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_GUIDE",
   "max_tokens": 200, "max_turns": 40, ${AGENT_EXTRA:-}
   "system_prompt": "You maintain the billing service and answer questions about invoices."}}}
EOF
    printf 'use agent\nuse array\nuse file\nuse time\nmain {\n    let h = agent.create("worker")\n%s\n    print("RUN_DONE")\n}\n' "$body" > "$d/p.naab"
    local out; out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    printf '%s' "$out"
}
req() { cat "$1/req_$2.json" 2>/dev/null; }   # $1 dir, $2 request number (1-based)
has() { case "$(req "$1" "$2")" in *"$3"*) return 0;; *) return 1;; esac; }

# G-01: guidance present but disabled -- the no-injection control
d="$TEST_TMP/a_disabled"
GUIDANCE_ENABLED=false
out="$(agent_run "$d" '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]' "" "" '
    let r = agent.send(h, "How is the total computed?")
    print("HASG=" + string(r.has("guidance")))' )"
GUIDANCE_ENABLED=true
if [[ "$out" != *RUN_DONE* ]] || [ ! -f "$d/req_1.json" ]; then
    fail "G-00" "harness: the stub run did not complete -- nothing below is readable" "$out / $(tail -3 "$d/err.txt")"
else
    pass "G-00" "harness: run completed and the stub recorded the request"
    if [[ "$out" == *HASG=false* ]] && ! has "$d" 1 "GUIDE_"; then
        pass "G-01" "guidance disabled: no section in the request and no guidance field in the response"
    else
        fail "G-01" "guidance disabled still injected or added the field" "$out"
    fi
fi

# G-02: start rule, ephemeral
d="$TEST_TMP/a_start"
out="$(agent_run "$d" '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]' "" "" '
    let r1 = agent.send(h, "How is the total computed?")
    print("G1=" + string(array.length(r1.get("guidance"))))
    let r2 = agent.send(h, "And the tax?")
    print("G2=" + string(array.length(r2.get("guidance"))))' )"
if [[ "$out" != *RUN_DONE* ]]; then
    fail "G-02" "run did not complete" "$out / $(tail -3 "$d/err.txt")"
elif has "$d" 1 "GUIDE_ONBOARD_q7" && has "$d" 1 "[Project guidance: docs/playbook.md > Onboarding]" \
     && ! has "$d" 1 "GUIDE_DB_q7" && ! has "$d" 2 "GUIDE_ONBOARD_q7" \
     && [[ "$out" == *G1=1* ]] && [[ "$out" == *G2=0* ]]; then
    pass "G-02" "start: the first request carries exactly the Onboarding section; the second carries none (never stored in history)"
else
    fail "G-02" "start delivery wrong" "$out; req1 has onboard: $(has "$d" 1 GUIDE_ONBOARD_q7 && echo y || echo n); req2: $(has "$d" 2 GUIDE_ONBOARD_q7 && echo y || echo n)"
fi
G02_DIR="$d"

# G-03: a paid drift signal (response_repetition fires on identical replies)
d="$TEST_TMP/a_signal"
out="$(agent_run "$d" '[{"on": "signal:response_repetition", "from": "playbook#avoid-repetition", "to": ["agent"]}]' '"cooldown_turns": 3,' \
  '"behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},
   "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "reality_checkpoint": {"enabled": false}},' '
    let i = 0
    while i < 7 { i = i + 1; let r = agent.send(h, "Explain the invoice total.") }' )"
pat=""
for n in 1 2 3 4 5 6 7; do if has "$d" "$n" "GUIDE_REPEAT_q7"; then pat="${pat}Y"; else pat="${pat}n"; fi; done
fired="$(grep -c '"response_repetition' "$d/tele.jsonl" 2>/dev/null || true)"
if [[ "$out" != *RUN_DONE* ]] || [ ! -f "$d/req_7.json" ]; then
    fail "G-03" "run did not complete 7 sends" "$out / $(tail -3 "$d/err.txt")"
elif [ "$pat" = "nnYnnYn" ]; then
    pass "G-03" "signal: queued after the first repeat, delivered with the NEXT request, then held by the 3-turn cooldown (requests: $pat)"
else
    fail "G-03" "signal delivery pattern $pat, expected nnYnnYn" "repetition events in telemetry: $fired"
fi

# G-04: failed validation; the detail's keywords pick the section (and a
# different detail picks a different one -- the relevance control)
val_prog() {
    printf '%s' "
    let r1 = agent.send(h, \"Summarise the invoice flow.\")
    agent.record_validation(h, false, \"$1\")
    let r2 = agent.send(h, \"Try again.\")"
}
d1="$TEST_TMP/a_val_db"; d2="$TEST_TMP/a_val_css"
o1="$(agent_run "$d1" '[{"on": "validation_failed", "from": "playbook", "to": ["agent"]}]' "" "" "$(val_prog 'migration rollback failed: schema version mismatch')")"
o2="$(agent_run "$d2" '[{"on": "validation_failed", "from": "playbook", "to": ["agent"]}]' "" "" "$(val_prog 'stylesheet layout broken, colors wrong')")"
if [[ "$o1" != *RUN_DONE* ]] || [[ "$o2" != *RUN_DONE* ]]; then
    fail "G-04" "runs did not complete" "$o1 / $o2"
elif ! has "$d1" 1 GUIDE_ && has "$d1" 2 GUIDE_DB_q7 && ! has "$d1" 2 GUIDE_CSS_q7 \
     && has "$d2" 2 GUIDE_CSS_q7 && ! has "$d2" 2 GUIDE_DB_q7; then
    pass "G-04" "validation_failed: the detail selects the matching section (migration -> Database, stylesheet -> Frontend)"
else
    fail "G-04" "keyword selection wrong" "db-run req2 DB=$(has "$d1" 2 GUIDE_DB_q7 && echo y || echo n) CSS=$(has "$d1" 2 GUIDE_CSS_q7 && echo y || echo n); css-run CSS=$(has "$d2" 2 GUIDE_CSS_q7 && echo y || echo n)"
fi

# G-05: a live edit between two triggers is what arrives -- no reload
d="$TEST_TMP/a_live"; mkdir -p "$d"
write_playbook "$d"
sed 's/GUIDE_DB_q7: a failed migration/GUIDE_DB_q7_EDITED: after the edit, a failed migration/' "$d/docs/playbook.md" > "$TEST_TMP/playbook_edited.md"
start_swap_operator "$d"
out="$(agent_run "$d" '[{"on": "validation_failed", "from": "playbook#database-migrations", "to": ["agent"]}]' '"cooldown_turns": 0,' "" "
    let a = agent.send(h, \"Summarise the invoice flow.\")
    agent.record_validation(h, false, \"migration failed\")
    let b = agent.send(h, \"Try again.\")
    file.write(\".swap_req\", \"$TEST_TMP/playbook_edited.md\t$d/docs/playbook.md\n\")
    file.write(\".swap_req.ready\", \"\")
    let waited = 0
    while !file.exists(\".swap_done\") && waited < 200 { time.sleep(0.05); waited = waited + 1 }
    agent.record_validation(h, false, \"migration failed again\")
    let c = agent.send(h, \"Once more.\")")"
if [[ "$out" != *RUN_DONE* ]]; then
    fail "G-05" "run did not complete" "$out / $(tail -3 "$d/err.txt")"
elif ! grep -q EDITED "$d/docs/playbook.md"; then
    skip "G-05" "the external edit did not happen (operator), so the live read was not exercised"
elif has "$d" 2 "GUIDE_DB_q7: a failed" && has "$d" 3 "GUIDE_DB_q7_EDITED" && ! has "$d" 2 "EDITED"; then
    pass "G-05" "a mid-run edit to the source is delivered on the next trigger, with no reload"
else
    fail "G-05" "live edit not reflected" "req2 orig=$(has "$d" 2 'GUIDE_DB_q7: a failed' && echo y || echo n) req3 edited=$(has "$d" 3 EDITED && echo y || echo n)"
fi

# G-06: a section holding a secret is withheld, not blocked; G-06b control
SEC_RULE='[{"on": "start", "from": ["playbook#secrets", "playbook#onboarding"], "to": ["agent"], "max_entries": 2}]'
d="$TEST_TMP/a_secret"
out="$(agent_run "$d" "$SEC_RULE" "" '"code_quality": {"no_secrets": {"enabled": true, "level": "hard"}},' '
    let r = agent.send(h, "How is the total computed?")' )"
d2="$TEST_TMP/a_secret_ctl"
out2="$(agent_run "$d2" "$SEC_RULE" "" "" '
    let r = agent.send(h, "How is the total computed?")' )"
if [[ "$out2" != *RUN_DONE* ]] || ! has "$d2" 1 "GUIDE_SECRET_q7"; then
    fail "G-06b" "control: without the secret scan the Secrets section must be delivered" "$out2"
else
    pass "G-06b" "control: with no_secrets off the Secrets section is delivered (the fixture is reachable)"
fi
if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 "GUIDE_ONBOARD_q7" && ! has "$d" 1 "GUIDE_SECRET_q7" \
   && ! has "$d" 1 "$FAKE_KEY" && grep -q '"GUIDANCE_WITHHELD"' "$d/tele.jsonl" \
   && grep '"GUIDANCE_WITHHELD"' "$d/tele.jsonl" | grep -q '"secret"'; then
    pass "G-06" "a section holding a secret is withheld (telemetry reason secret); the run is not blocked; the clean section still arrives"
else
    fail "G-06" "secret section handling wrong" "$out; withheld events: $(grep -c GUIDANCE_WITHHELD "$d/tele.jsonl" 2>/dev/null)"
fi

# G-13: budget truncates
d="$TEST_TMP/a_budget"
out="$(agent_run "$d" '[{"on": "start", "from": "playbook#long", "to": ["agent"]}]' '"max_chars_per_turn": 200,' "" '
    let r = agent.send(h, "How is the total computed?")
    print("CH=" + string(r.get("guidance")[0].get("chars")))' )"
ch="${out#*CH=}"; ch="${ch%%[^0-9]*}"
if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 "[... guidance truncated]" && [ -n "$ch" ] && [ "$ch" -le 200 ]; then
    pass "G-13" "a section longer than max_chars_per_turn is cut and marked ($ch chars)"
else
    fail "G-13" "budget not applied" "$out"
fi

# G-15: provenance -- the logged sha256 is the hash of what was delivered
if [ -f "$G02_DIR/tele.jsonl" ]; then
    chk="$(python3 - "$G02_DIR" <<'PY'
import hashlib, json, sys
d = sys.argv[1]
ev = [json.loads(l) for l in open(d + "/tele.jsonl", encoding="utf-8", errors="replace") if '"GUIDANCE_DELIVERED"' in l]
# The delivered text is the Onboarding body; recompute from the source file.
src = open(d + "/docs/playbook.md", encoding="utf-8").read()
sec = src.split("## Onboarding\n", 1)[1].split("\n## ", 1)[0].strip()
want = hashlib.sha256(sec.encode()).hexdigest()
ok = any(e.get("sha256") == want and e.get("channel") == "agent" and e.get("trigger") == "start" for e in ev)
sys.stdout.write("OK" if ok else "MISMATCH %d events" % len(ev))
PY
)"
    vout="$("$NAAB" --verify-telemetry-chain "$G02_DIR/tele.jsonl" 2>&1)"; vrc=$?
    if [ "$chk" = OK ] && [ "$vrc" -eq 0 ]; then
        pass "G-15" "GUIDANCE_DELIVERED carries the sha256 of the delivered section; the telemetry chain verifies"
    else
        fail "G-15" "provenance wrong" "$chk / chain rc=$vrc ${vout:0:200}"
    fi
else
    fail "G-15" "no telemetry from G-02"
fi

# G-16: extends -- a child with no guidance section inherits the parent's
d="$TEST_TMP/a_extends"; mkdir -p "$d/parent"
cat > "$d/parent/govern.json" <<'EOF'
{"version": "5.0", "mode": "enforce",
 "guidance": {"enabled": true,
   "sources": {"playbook": {"type": "file", "path": "docs/playbook.md"}},
   "rules": [{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]}}
EOF
write_playbook "$d"
start_stub "$FIXTURE" "$d" >/dev/null 2>&1 && STUB_STARTED=1
cat > "$d/govern.json" <<EOF
{"version": "5.0", "mode": "enforce", "extends": "./parent/govern.json",
 "security": {"sandbox_level": "elevated"},
 "agents": {"worker": {"provider": "gemini", "model": "stub-model",
   "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "FAKE_KEY_GUIDE",
   "max_tokens": 200, "max_turns": 40, "system_prompt": "You maintain the billing service."}}}
EOF
printf 'use agent\nmain {\n    let h = agent.create("worker")\n    let r = agent.send(h, "Hello?")\n    print("RUN_DONE")\n}\n' > "$d/p.naab"
out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
stop_stub; STUB_STARTED=0
if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 "GUIDE_ONBOARD_q7"; then
    pass "G-16" "extends: the parent's guidance reaches a child that does not configure its own"
else
    fail "G-16" "extends did not inherit guidance" "$out / $(tail -3 "$d/err.txt")"
fi
# ---------------------------------------------------------------------------
# Full chain: what was sent, the reply it got, and what followed
# ---------------------------------------------------------------------------
# A reply that USES the guidance: the stub answers with the Onboarding words.
UPTAKE_FX="$TEST_TMP/fixture_uptake.json"
cat > "$UPTAKE_FX" <<'EOF2'
{"responses": [{"content": "Following the README before answering: the total sums the invoice lines.", "input_tokens": 12, "output_tokens": 24}]}
EOF2
d="$TEST_TMP/a_uptake"
FIXTURE_SAVE="$FIXTURE"; FIXTURE="$UPTAKE_FX"
out="$(agent_run "$d" '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]' "" "" '
    let r1 = agent.send(h, "How is the total computed?")
    let g = r1.get("guidance")[0]
    print("ID=" + g.get("id"))
    print("TEXT=" + g.get("text"))' )"
FIXTURE="$FIXTURE_SAVE"
chain="$(python3 - "$d" "$G02_DIR" <<'PY'
import json, sys
def events(d, kind):
    out = []
    for l in open(d + "/tele.jsonl", encoding="utf-8", errors="replace"):
        if '"%s"' % kind in l:
            try: out.append(json.loads(l))
            except Exception: pass
    return out
up, ctl = sys.argv[1], sys.argv[2]
dl = events(up, "GUIDANCE_DELIVERED"); rs = events(up, "GUIDANCE_RESPONSE")
src = open(up + "/docs/playbook.md", encoding="utf-8").read()
sec = src.split("## Onboarding\n", 1)[1].split("\n## ", 1)[0].strip()
ok_text = len(dl) == 1 and dl[0].get("text") == sec and dl[0].get("delivery_id", "").startswith("g") \
          and float(dl[0].get("lookup_ms", "-1")) >= 0
ok_join = len(rs) == 1 and rs[0].get("delivery_id") == dl[0].get("delivery_id") \
          and rs[0].get("response") == "ok" and rs[0].get("turn_at_request") == "0"
used = int(rs[0].get("terms_used", "0")) if rs else -1
ok_used = used >= 2 and "readme" in rs[0].get("matched", "")
crs = events(ctl, "GUIDANCE_RESPONSE")
ok_ctl = len(crs) == 1 and crs[0].get("terms_used") == "0"
print("text=%s join=%s used=%s(%d) control=%s" % (ok_text, ok_join, ok_used, used, ok_ctl))
PY
)"
if [[ "$out" != *RUN_DONE* ]]; then
    fail "G-18" "uptake run did not complete" "$out / $(tail -3 "$d/err.txt")"
elif [[ "$chain" == "text=True join=True used=True"*"control=True" ]]; then
    pass "G-18" "telemetry carries the delivered text and id; GUIDANCE_RESPONSE joins the reply, which used the guidance terms ($chain); a reply that ignored it reads 0 (control)"
else
    fail "G-18" "full-chain telemetry wrong" "$chain"
fi
if [[ "$out" == *"ID=g"* ]] && [[ "$out" == *"TEXT=GUIDE_ONBOARD_q7"* ]]; then
    pass "G-21" "the script sees the delivered text and its id in the response"
else
    fail "G-21" "response guidance entry lacks text or id" "$out"
fi

# GUIDANCE_RESPONSE through both paths: behavioural sequences off (G-02 run)
# and on, where it also records whether the triggering signal paid again (G-03).
sig_rows="$(grep '"GUIDANCE_RESPONSE"' "$TEST_TMP/a_signal/tele.jsonl" 2>/dev/null | grep -c '"trigger_recurred":"true"')"
off_rows="$(grep -c '"GUIDANCE_RESPONSE"' "$G02_DIR/tele.jsonl" 2>/dev/null)"
if [ "${sig_rows:-0}" -eq 2 ] && [ "${off_rows:-0}" -eq 1 ]; then
    pass "G-18b" "the reply is linked with behavioural sequences on (signal recurrence recorded: 2 of 2 replies repeated) and off"
else
    fail "G-18b" "reply linkage missing on one path" "with BSD: $sig_rows recurred rows; without: $off_rows rows"
fi

# Validation linkage: the first validation AFTER a delivery closes its chain;
# one recorded before any delivery produces nothing (control).
d="$TEST_TMP/a_valchain"
out="$(agent_run "$d" '[{"on": "validation_failed", "from": "playbook#database-migrations", "to": ["agent"]}]' "" "" '
    let r1 = agent.send(h, "Summarise the invoice flow.")
    agent.record_validation(h, false, "migration failed")
    let r2 = agent.send(h, "Try again.")
    agent.record_validation(h, true)' )"
vrows="$(grep '"GUIDANCE_VALIDATION"' "$d/tele.jsonl" 2>/dev/null)"
nrows="$(printf '%s' "$vrows" | grep -c GUIDANCE_VALIDATION)"
if [[ "$out" != *RUN_DONE* ]]; then
    fail "G-20" "run did not complete" "$out"
elif [ "$nrows" -eq 1 ] && [[ "$vrows" == *'"passed":"true"'* ]] && [[ "$vrows" == *'"delivery_ids":"g1"'* ]] \
     && grep -q "next validation passed" "$d/err.txt"; then
    pass "G-20" "the validation after the guidance is linked to it (passed, g1); the one before any guidance produced nothing"
else
    fail "G-20" "validation linkage wrong" "rows=$nrows $vrows"
fi

# Terminal at the end: the end-of-run block lists what reached the agent;
# a run with guidance disabled prints none (G-01's run is the control).
if grep -q -- "-- Guidance: 2 delivered to agents" "$TEST_TMP/a_signal/err.txt" \
   && grep -q "Avoid repetition  (reply used 0/" "$TEST_TMP/a_signal/err.txt" \
   && grep -q "signal paid again" "$TEST_TMP/a_signal/err.txt" \
   && ! grep -q -- "-- Guidance:" "$TEST_TMP/a_disabled/err.txt"; then
    pass "G-19" "the end of the run lists each delivery (agent, turn, trigger, section, reply uptake, signal recurrence); none when guidance is off"
else
    fail "G-19" "end-of-run guidance block wrong" "$(grep -A3 -- '-- Guidance:' "$TEST_TMP/a_signal/err.txt" | head -4)"
fi
# ---------------------------------------------------------------------------
# Gap arms (the list after #309): agents filter, wildcard and prefix rules,
# PII, a mid-run reload, batch concurrency, the commit path, the tree-walker
# ---------------------------------------------------------------------------
two_agent_cfg() {  # $1 dir, $2 rules JSON
    cat > "$1/govern.json" <<EOF2
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 "telemetry": {"enabled": true, "output_file": "tele.jsonl"},
 $(guidance_json "$2"),
 "agents": {
   "worker":   {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$STUB_PORT",
                "api_key_env": "FAKE_KEY_GUIDE", "max_tokens": 200, "max_turns": 40,
                "system_prompt": "You maintain the billing service."},
   "reviewer": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$STUB_PORT",
                "api_key_env": "FAKE_KEY_GUIDE", "max_tokens": 200, "max_turns": 40,
                "system_prompt": "You review billing changes."}}}
EOF2
}

# G-22: a rule scoped to one agent reaches that agent only.
d="$TEST_TMP/a_agents"; mkdir -p "$d"; write_playbook "$d"
if start_stub "$FIXTURE" "$d" >/dev/null 2>&1; then
    STUB_STARTED=1
    two_agent_cfg "$d" '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"], "agents": ["reviewer"]}]'
    printf 'use agent\nmain {\n    let w = agent.create("worker")\n    let a = agent.send(w, "Hello worker")\n    let r = agent.create("reviewer")\n    let b = agent.send(r, "Hello reviewer")\n    print("RUN_DONE")\n}\n' > "$d/p.naab"
    out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    if [[ "$out" == *RUN_DONE* ]] && ! has "$d" 1 GUIDE_ONBOARD_q7 && has "$d" 2 GUIDE_ONBOARD_q7; then
        pass "G-22" "a rule with agents:[reviewer] reaches the reviewer's request and not the worker's"
    else
        fail "G-22" "agents filter wrong" "$out; worker=$(has "$d" 1 GUIDE_ONBOARD_q7 && echo y || echo n) reviewer=$(has "$d" 2 GUIDE_ONBOARD_q7 && echo y || echo n)"
    fi
else
    fail "G-22" "stub failed to start"
fi

# G-23: signal:* matches any paid signal (same run shape as G-03).
d="$TEST_TMP/a_sigany"
out="$(agent_run "$d" '[{"on": "signal:*", "from": "playbook#avoid-repetition", "to": ["agent"]}]' '"cooldown_turns": 3,' \
  '"behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},
   "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "reality_checkpoint": {"enabled": false}},' '
    let i = 0
    while i < 3 { i = i + 1; let r = agent.send(h, "Explain the invoice total.") }' )"
# Several signals can pay on one turn; the delivery is attributed to the first
# one queued (observed: signal:circular_actions), so any concrete signal: name
# is correct here -- what must NOT appear is the literal "*".
sig_trig="$(grep '"GUIDANCE_DELIVERED"' "$d/tele.jsonl" 2>/dev/null | grep -o '"trigger":"signal:[a-z_]*"' | head -1)"
if [[ "$out" == *RUN_DONE* ]] && ! has "$d" 1 GUIDE_REPEAT_q7 && ! has "$d" 2 GUIDE_REPEAT_q7 && has "$d" 3 GUIDE_REPEAT_q7 \
   && [ -n "$sig_trig" ]; then
    pass "G-23" "signal:* fires on any paid signal and records which one ($sig_trig)"
else
    fail "G-23" "signal:* did not fire" "$out"
fi

# G-24: a section holding PII is withheld under no_pii; delivered without it.
PII_RULE='[{"on": "start", "from": ["playbook#contact", "playbook#onboarding"], "to": ["agent"], "max_entries": 2}]'
d="$TEST_TMP/a_pii"
out="$(agent_run "$d" "$PII_RULE" "" '"code_quality": {"no_pii": {"enabled": true, "level": "hard"}},' '
    let r = agent.send(h, "How is the total computed?")' )"
d2="$TEST_TMP/a_pii_ctl"
out2="$(agent_run "$d2" "$PII_RULE" "" "" '
    let r = agent.send(h, "How is the total computed?")' )"
if [[ "$out2" != *RUN_DONE* ]] || ! has "$d2" 1 GUIDE_PII_q7; then
    fail "G-24b" "control: without no_pii the Contact section must be delivered" "$out2"
else
    pass "G-24b" "control: with no_pii off the Contact section is delivered"
fi
if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 GUIDE_ONBOARD_q7 && ! has "$d" 1 GUIDE_PII_q7 \
   && grep '"GUIDANCE_WITHHELD"' "$d/tele.jsonl" | grep -q '"pii"'; then
    pass "G-24" "a section holding PII is withheld (reason pii); the clean section still arrives"
else
    fail "G-24" "PII section handling wrong" "$out"
fi

# G-25: a mid-run govern.json reload replaces the rules; the next send follows
# the NEW rules (the first request is the control that the old ones applied).
d="$TEST_TMP/a_reload"; mkdir -p "$d"; write_playbook "$d"
if start_stub "$FIXTURE" "$d" >/dev/null 2>&1; then
    STUB_STARTED=1
    agent_block="\"agents\": {\"worker\": {\"provider\": \"gemini\", \"model\": \"stub-model\", \"api_base\": \"http://127.0.0.1:$STUB_PORT\", \"api_key_env\": \"FAKE_KEY_GUIDE\", \"max_tokens\": 200, \"max_turns\": 40, \"system_prompt\": \"You maintain the billing service.\"}}"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "telemetry": {"enabled": true, "output_file": "tele.jsonl"}, %s, %s}\n' \
        "$(guidance_json '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]')" "$agent_block" > "$d/govern.json"
    printf '{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"}, "telemetry": {"enabled": true, "output_file": "tele.jsonl"}, %s, %s}\n' \
        "$(guidance_json '[{"on": "validation_failed", "from": "playbook#database-migrations", "to": ["agent"]}]')" "$agent_block" > "$TEST_TMP/reload_new.json"
    touch -d '2 minutes ago' "$d/govern.json" 2>/dev/null || true
    start_swap_operator "$d"
    cat > "$d/p.naab" <<EOF2
use agent
use file
use time
main {
    let h = agent.create("worker")
    let a = agent.send(h, "Summarise the invoice flow.")
    file.write(".swap_req", "$TEST_TMP/reload_new.json\t$d/govern.json\n")
    file.write(".swap_req.ready", "")
    let waited = 0
    while !file.exists(".swap_done") && waited < 200 { time.sleep(0.05); waited = waited + 1 }
    // A reload is applied at the next agent.send(): one send first, so the
    // trigger below is judged under the NEW rules.
    let b = agent.send(h, "Continue.")
    agent.record_validation(h, false, "migration failed")
    let c = agent.send(h, "Try again.")
    print("RUN_DONE")
}
EOF2
    out="$( (cd "$d" && timeout 120s "$NAAB" p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    if [[ "$out" != *RUN_DONE* ]]; then
        fail "G-25" "run did not complete" "$out / $(tail -3 "$d/err.txt")"
    elif grep -q "Reload rejected" "$d/err.txt"; then
        fail "G-25" "the reload was rejected" "$(grep 'Reload rejected' "$d/err.txt" | head -1)"
    elif grep -q "Config reloaded mid-run" "$d/err.txt" && has "$d" 1 GUIDE_ONBOARD_q7 \
         && ! has "$d" 2 GUIDE_ && has "$d" 3 GUIDE_DB_q7 && ! has "$d" 3 GUIDE_ONBOARD_q7; then
        pass "G-25" "a mid-run reload replaced the rules: request 1 followed the old (start), request 3 the new (validation_failed)"
    else
        fail "G-25" "the reloaded rules were not followed" "req1 onboard=$(has "$d" 1 GUIDE_ONBOARD_q7 && echo y || echo n) req3 db=$(has "$d" 3 GUIDE_DB_q7 && echo y || echo n)"
    fi
else
    fail "G-25" "stub failed to start"
fi

# G-26: agent.batch runs sends on pool threads; every handle gets its start
# guidance exactly once and every delivery has a distinct id.
d="$TEST_TMP/a_batch"
out="$(agent_run "$d" '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]' "" "" '
    let hs = [h, agent.create("worker"), agent.create("worker"), agent.create("worker"), agent.create("worker"), agent.create("worker")]
    let rs = agent.batch(hs, ["a1", "b1", "c1", "d1", "e1", "f1"])
    let rs2 = agent.batch(hs, ["a2", "b2", "c2", "d2", "e2", "f2"])' )"
bat="$(python3 - "$d" <<'PY'
import glob, json, os, sys
d = sys.argv[1]
reqs = [open(p, encoding="utf-8", errors="replace").read() for p in glob.glob(os.path.join(d, "req_*.json"))]
with_g = sum(1 for r in reqs if "GUIDE_ONBOARD_q7" in r)
ev = [json.loads(l) for l in open(os.path.join(d, "tele.jsonl"), encoding="utf-8", errors="replace") if '"GUIDANCE_DELIVERED"' in l]
ids = [e.get("delivery_id") for e in ev]
handles = sorted(set(e.get("handle_id") for e in ev))
print("%d %d %d %d %d" % (len(reqs), with_g, len(ids), len(set(ids)), len(handles)))
PY
)"
if [[ "$out" == *RUN_DONE* ]] && [ "$bat" = "12 6 6 6 6" ]; then
    pass "G-26" "two concurrent batches of 6: each handle's first request carried the guidance once (6 of 12), 6 distinct delivery ids"
else
    fail "G-26" "batch delivery wrong (requests, with guidance, events, distinct ids, handles)" "$bat / $out"
fi

# G-27: a committed proposal is a turn: its paid signal queues guidance for the
# next send. propose itself carries none (the documented slice-1 limit).
d="$TEST_TMP/a_commit"
AGENT_EXTRA='"propose_candidates_max": 1,'
out="$(agent_run "$d" '[{"on": "signal:response_repetition", "from": "playbook#avoid-repetition", "to": ["agent"]}]' "" \
  '"behavioral_sequences": {"enabled": true}, "advisory_escalation": {"enabled": false},
   "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1, "reality_checkpoint": {"enabled": false}},' '
    let a = agent.send(h, "Explain the invoice total.")
    let p = agent.propose(h, "Explain the invoice total.", 1)
    let c = agent.commit(h, p.get("candidates")[0])
    let b = agent.send(h, "And again?")' )"
AGENT_EXTRA=""
if [[ "$out" != *RUN_DONE* ]]; then
    fail "G-27" "propose/commit run did not complete" "$out / $(tail -4 "$d/err.txt")"
elif ! has "$d" 2 GUIDE_REPEAT_q7 && has "$d" 3 GUIDE_REPEAT_q7; then
    pass "G-27" "the commit's repeated reply paid the signal; the next send carried the guidance (propose's request carried none)"
else
    fail "G-27" "commit path did not queue guidance" "propose req2=$(has "$d" 2 GUIDE_REPEAT_q7 && echo y || echo n) send req3=$(has "$d" 3 GUIDE_REPEAT_q7 && echo y || echo n)"
fi

# G-28: the agent channel on the tree-walker (the agent module is shared, but
# that was an assumption until run).
d="$TEST_TMP/a_tw"; mkdir -p "$d"; write_playbook "$d"
if start_stub "$FIXTURE" "$d" >/dev/null 2>&1; then
    STUB_STARTED=1
    cat > "$d/govern.json" <<EOF2
{"version": "5.0", "mode": "enforce", "security": {"sandbox_level": "elevated"},
 $(guidance_json '[{"on": "start", "from": "playbook#onboarding", "to": ["agent"]}]'),
 "agents": {"worker": {"provider": "gemini", "model": "stub-model", "api_base": "http://127.0.0.1:$STUB_PORT",
   "api_key_env": "FAKE_KEY_GUIDE", "max_tokens": 200, "max_turns": 40, "system_prompt": "You maintain the billing service."}}}
EOF2
    printf 'use agent\nmain {\n    let h = agent.create("worker")\n    let r = agent.send(h, "Hello")\n    print("RUN_DONE")\n}\n' > "$d/p.naab"
    out="$( (cd "$d" && timeout 120s "$NAAB" --tree-walk p.naab 2>"$d/err.txt") )"
    stop_stub; STUB_STARTED=0
    if [[ "$out" == *RUN_DONE* ]] && has "$d" 1 GUIDE_ONBOARD_q7; then
        pass "G-28" "the tree-walker delivers the same start guidance"
    else
        fail "G-28" "tree-walker agent channel wrong" "$out / $(tail -3 "$d/err.txt")"
    fi
else
    fail "G-28" "stub failed to start"
fi
fi  # not Windows

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT unmeasurable"
[ "$FAIL_COUNT" -eq 0 ]
