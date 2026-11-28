#!/usr/bin/env bash
# ============================================================
# test_gov_context.sh -- naab-gov context TARGET: what governs a rule, a
# language, an agent role or a file, and why (Bundle 21 PR 4)
#
# Report only: it runs none of the program, fires no hook, writes no
# telemetry. It shows states, levels, where in the extends chain a section is
# set, rationales (through the filter refusals use), what the project's files
# add and whether each took effect, guidance headings and the run history's
# refusals and repairs -- and never a value a check matches or scores against.
#
#   CX-00  no govern.json governs the target: UNMEASURABLE (exit 2)
#   CX-01  a rule: its section, switch, level, the file that sets it, its rationale
#   CX-02  the extends chain: a parent's section is named as set in the parent;
#          set in both, the child is listed first
#   CX-02c CONTROL: the chain itself is listed
#   CX-03  a rule no section governs: UNMEASURABLE (exit 2)
#   CX-04  no value reaches any target's report, text or JSON: banned names,
#          blocked commands, custom rule patterns, key variable names, sanitizer
#          lists, paths, thresholds, golden cases, contract clauses, model names
#   CX-04c CONTROL: the config holds every one of them, and the reports ran
#   CX-05  a language: refused by languages.blocked; per-language settings
#          counted, not listed
#   CX-05c CONTROL: another language reads allowed
#   CX-06  an agent role: its actions and switches
#   CX-07  a file: a blocked path is refused for the program; govern.json is
#          protected; a program's block languages and its contracts' kinds
#   CX-08  project files: a CLAUDE.md ban is listed as having written a rule,
#          without the banned name; a preference is listed with its words
#   CX-09  guidance: a refusal's section is named by heading, its text not shown
#   CX-10  run history: a refusal is listed for its rule and for its file
#   CX-11  writes nothing and fires nothing: the telemetry file is unchanged,
#          a hook's marker is absent
#   CX-11c CONTROL: the same hook fires on a real run of the same project
#   CX-11g CONTROL: the guidance a pull and a refusal select is not empty
#   CX-12  runs none of the program: a file target's main writes nothing
#   CX-13  --json is valid JSON with verdict REPORT
#
# KILL TABLE (each mutant applied alone, naab-lang and naab-gov rebuilt, this
# suite run; observed 2026-11-28 on the default build; the arms that turned red):
#   C1  set_in reads only the loaded file, not the chain -> CX-02
#   C2  a ban's directive shown                        -> CX-08
#   C3  per-language lists listed, not counted         -> CX-04 CX-05
#   C5  guidancePull() records its pull                -> CX-11
#   C6  the language verdict ignores languages.blocked -> CX-05
#   C7  the history filter keeps every rule            -> CX-10
#   C8  writesSection() always false                   -> CX-01 CX-02
#   (C4, the rationale shown unfiltered, is caught by test_rationale_sections.sh
#   RS-03..RS-06, which read it through naab-gov context.)
# ============================================================
set -u

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-${1:-$REPO/build/naab-lang}}"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
TEST_TMP="$(mktemp -d)"
trap 'rm -rf "$TEST_TMP"' EXIT
source "$REPO/tests/helpers/trust_setup.sh"; isolate_trust_store

PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo "  PASS [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo "  FAIL [$1] $2${3:+ -- $3}"; }

echo "=== naab-gov context (Bundle 21 PR 4) ==="
if [ ! -x "$GOV" ] || [ ! -x "$NAAB" ]; then
    echo "  FAIL [CX-00] naab-gov or naab-lang not built -- UNMEASURABLE, not a pass"
    exit 1
fi

ctx() {  # DIR ARGS... -> out.txt (text), rc
    local d="$1"; shift
    (cd "$d" && "$GOV" context "$@" > ctx.txt 2> ctx_err.txt; echo $? > ctx_rc)
}
ctxj() {  # DIR ARGS... -> ctx.json
    local d="$1"; shift
    (cd "$d" && "$GOV" context "$@" --json > ctx.json 2> ctx_err.txt; echo $? > ctx_rc)
}
jq_py() {  # FILE EXPR -> python expression over j
    python3 -I - "$1" "$2" <<'PY'
import json, sys
try:
    j = json.load(open(sys.argv[1], "rb"))
    print(eval(sys.argv[2], {"j": j}))
except Exception as e:
    print("ERROR", type(e).__name__, e)
PY
}

# ------------------------------------------------------------------
# CX-00 / CX-03
# ------------------------------------------------------------------
d="$TEST_TMP/cx00"; mkdir -p "$d"
ctx "$d" rule:capabilities.network
if [ "$(cat "$d/ctx_rc")" = 2 ] && grep -q "UNMEASURABLE: no govern.json is found" "$d/ctx.txt"; then
    pass "CX-00" "no govern.json governs the target: UNMEASURABLE (exit 2)"
else
    fail "CX-00" "no config did not read UNMEASURABLE" "rc=$(cat "$d/ctx_rc") $(head -c 200 "$d/ctx.txt")"
fi

# ------------------------------------------------------------------
# CX-01 / CX-02 / CX-03
# ------------------------------------------------------------------
d="$TEST_TMP/cx01"; mkdir -p "$d"
cat > "$d/govern.json" <<'EOF'
{"version":"3.0","mode":"enforce",
 "code_quality":{"no_placeholders":{"enabled":true,"level":"hard","rationale":"CX_MARK_PL"}}}
EOF
ctxj "$d" rule:code_quality.no_placeholders
got=$(jq_py "$d/ctx.json" '(j["rules"][0]["section"], j["rules"][0]["enabled"], j["rules"][0]["level"], j["rules"][0]["set_in"], j["rules"][0]["rationale"])')
if [ "$(cat "$d/ctx_rc")" = 0 ] && [ "$got" = "('code_quality.no_placeholders', True, 'hard', ['govern.json'], 'CX_MARK_PL')" ]; then
    pass "CX-01" "a rule: its section, switch, level, the file that sets it and its rationale"
else
    fail "CX-01" "the rule's entry is wrong" "rc=$(cat "$d/ctx_rc") $got"
fi

d="$TEST_TMP/cx02"; mkdir -p "$d/base"
cat > "$d/base/parent.json" <<'EOF'
{"version":"3.0","mode":"enforce",
 "code_quality":{"no_placeholders":{"enabled":true,"level":"hard","rationale":"CX_MARK_PARENT"}},
 "capabilities":{"shell":{"enabled":false,"rationale":"CX_MARK_SHELL"}}}
EOF
cat > "$d/govern.json" <<'EOF'
{"version":"3.0","mode":"enforce","extends":"base/parent.json",
 "capabilities":{"shell":{"enabled":false}}}
EOF
ctxj "$d" rule:code_quality.no_placeholders
a=$(jq_py "$d/ctx.json" 'j["rules"][0]["set_in"]')
ctxj "$d" rule:capabilities.shell
b=$(jq_py "$d/ctx.json" 'j["rules"][0]["set_in"]')
if [ "$a" = "['base/parent.json']" ] && [ "$b" = "['govern.json', 'base/parent.json']" ]; then
    pass "CX-02" "a parent's section is named as set in the parent; set in both, the child first"
else
    fail "CX-02" "set_in does not follow the extends chain" "only-parent=$a both=$b"
fi
if [ "$(jq_py "$d/ctx.json" 'j["config"]["chain"]')" = "['govern.json', 'base/parent.json']" ]; then
    pass "CX-02c" "CONTROL: the chain is listed"
else
    fail "CX-02c" "the chain is not listed" "$(jq_py "$d/ctx.json" 'j["config"]')"
fi
ctx "$TEST_TMP/cx01" rule:no_such_section.thing
if [ "$(cat "$TEST_TMP/cx01/ctx_rc")" = 2 ] && grep -q "no section of govern.json governs a rule named" "$TEST_TMP/cx01/ctx.txt"; then
    pass "CX-03" "a rule no section governs: UNMEASURABLE (exit 2)"
else
    fail "CX-03" "an ungoverned rule was reported" "rc=$(cat "$TEST_TMP/cx01/ctx_rc")"
fi

# ------------------------------------------------------------------
# CX-04 -- no value reaches any report
# ------------------------------------------------------------------
d="$TEST_TMP/cx04"; mkdir -p "$d/zqpathdir"
cat > "$d/govern.json" <<'EOF'
{"version":"3.0","mode":"enforce",
 "telemetry":{"enabled":true,"output_file":"t.jsonl","tamper_evidence":{"enabled":true,"hmac_key_env":"ZQKEYVAR_CX"}},
 "languages":{"blocked":["ruby"],"per_language":{"python":{"banned_functions":["zqbanfn"],"banned_imports":["zqbanimp"]}}},
 "capabilities":{"shell":{"enabled":true,"blocked_commands":["zqblockcmd"],"rationale":"CX_MARK_SHELL4"},
                 "filesystem":{"mode":"write","blocked_paths":["zqpathdir/"]}},
 "custom_rules":[{"id":"cx_rule","pattern":"zqpattern_cx","language":"naab","level":"advisory","message":"m","rationale":"CX_MARK_CUSTOM"}],
 "taint_tracking":{"enabled":true,"sources":["env.get"],"sinks":["print"],"sanitizers":["zqsanit_"]},
 "context_drift":{"enabled":true,"thresholds":{"semantic_stability_min_overlap":0.777}},
 "contracts":{"level":"hard","functions":{"work":{"must_call":["zqmustcall"],"must_produce":[{"args":["zqgolden"],"expect":"zqexpect"}],"requires":["x > 0"],"rationale":"CX_MARK_CT"}}},
 "agents":{"cx_role":{"provider":"gemini","model":"zqmodel-1","api_key_env":"ZQAPIKEYVAR_CX","allowed_actions":["FS_READ"],"system_prompt":"zqprompt"}}}
EOF
printf 'fn work(x) {\n    return x\n}\nmain {\n    let y = <<python\n1\n    >>\n    print(work(1))\n}\n' > "$d/p.naab"
VALUES="zqbanfn zqbanimp zqblockcmd zqpathdir zqpattern_cx ZQKEYVAR_CX ZQAPIKEYVAR_CX zqsanit_ 0.777 zqmustcall zqgolden zqexpect x > 0 zqmodel-1 zqprompt"
leaked=""
for t in rule:capabilities.shell rule:custom_rules.cx_rule rule:contracts.work.must_call lang:python agent:cx_role file:p.naab; do
    ctx "$d" "$t"; cat "$d/ctx.txt" >> "$d/all.txt"
    ctxj "$d" "$t"; cat "$d/ctx.json" >> "$d/all.txt"
done
for v in zqbanfn zqbanimp zqblockcmd zqpathdir zqpattern_cx ZQKEYVAR_CX ZQAPIKEYVAR_CX zqsanit_ 0.777 zqmustcall zqgolden zqexpect "x > 0" zqmodel-1 zqprompt; do
    grep -qF -- "$v" "$d/all.txt" && leaked="$leaked [$v]"
done
if [ -z "$leaked" ]; then
    pass "CX-04" "no value reaches a rule's, language's, role's or file's report"
else
    fail "CX-04" "a value reached a report" "$leaked"
fi
missing=""
for v in zqbanfn zqblockcmd zqpattern_cx ZQKEYVAR_CX ZQAPIKEYVAR_CX zqsanit_ 0.777 zqgolden zqmodel-1; do
    grep -qF -- "$v" "$d/govern.json" || missing="$missing [$v]"
done
if [ -z "$missing" ] && grep -qF "CX_MARK_SHELL4" "$d/all.txt" && grep -qF "CX_MARK_CUSTOM" "$d/all.txt" && grep -qF "CX_MARK_CT" "$d/all.txt"; then
    pass "CX-04c" "CONTROL: the config holds every value, and the reports ran (their rationales are shown)"
else
    fail "CX-04c" "the probe config or the reports are broken -- CX-04 measures nothing" "missing=$missing"
fi

# ------------------------------------------------------------------
# CX-05 / CX-06 / CX-07
# ------------------------------------------------------------------
ctxj "$d" lang:ruby
r=$(jq_py "$d/ctx.json" '(j["language"]["verdict"], j["language"].get("by"))')
ctxj "$d" lang:python
p=$(jq_py "$d/ctx.json" '(j["language"]["verdict"], j["language"]["settings"])')
if [ "$r" = "('refused', 'languages.blocked')" ] && [ "$p" = "('allowed', ['banned functions: 1 (not listed)', 'banned imports: 1 (not listed)'])" ]; then
    pass "CX-05" "a language: refused by languages.blocked; its settings counted, not listed"
else
    fail "CX-05" "the language report is wrong" "ruby=$r python=$p"
fi
ctxj "$d" lang:javascript
if [ "$(jq_py "$d/ctx.json" 'j["language"]["verdict"]')" = "allowed" ]; then
    pass "CX-05c" "CONTROL: a language no list names reads allowed"
else
    fail "CX-05c" "javascript did not read allowed" "$(jq_py "$d/ctx.json" 'j["language"]')"
fi
ctxj "$d" agent:cx_role
a=$(jq_py "$d/ctx.json" '(j["agent"]["allowed_actions"], j["agent"]["shell_allowed"], j["agent"]["tools_enabled"])')
if [ "$a" = "(['FS_READ'], True, False)" ]; then
    pass "CX-06" "an agent role: its actions and switches"
else
    fail "CX-06" "the role report is wrong" "$a"
fi
printf 'secret\n' > "$d/zqpathdir/f.txt"
ctxj "$d" file:zqpathdir/f.txt
f1=$(jq_py "$d/ctx.json" '(j["file"]["read"], j["file"]["write"])')
ctxj "$d" file:govern.json
f2=$(jq_py "$d/ctx.json" '(j["file"]["write"], j["file"]["protected_as"])')
ctxj "$d" file:p.naab
f3=$(jq_py "$d/ctx.json" '(j["file"]["blocks"], j["file"]["contracts"])')
if [ "$f1" = "('refused', 'refused')" ] && [ "$f2" = "('refused', ['the governance configuration'])" ] \
   && [ "$f3" = "([{'language': 'python', 'verdict': 'allowed'}], [{'function': 'work', 'kinds': ['must_call', 'must_produce', 'requires']}])" ]; then
    pass "CX-07" "a file: a blocked path refused, govern.json protected, a program's blocks and contract kinds"
else
    fail "CX-07" "the file report is wrong" "blocked=$f1 govern=$f2 program=$f3"
fi

# ------------------------------------------------------------------
# CX-08 -- project files
# ------------------------------------------------------------------
d="$TEST_TMP/cx08"; mkdir -p "$d"
cat > "$d/govern.json" <<'EOF'
{"version":"3.0","mode":"enforce","project_context":{"enabled":true,"enforcement_level":"hard"}}
EOF
printf '# Rules\n\n- Never use zqbannedname() in this project\n- Prefer Go for concurrency\n' > "$d/CLAUDE.md"
ctxj "$d" file:CLAUDE.md
x=$(jq_py "$d/ctx.json" 'sorted((e["category"], e["wrote_rule"], "directive" in e) for e in j["project_context"]["extractions"])')
ctx "$d" file:CLAUDE.md
if echo "$x" | grep -qF "('banned', True, False)" && echo "$x" | grep -qF "('language', False, True)" \
   && ! grep -qF "zqbannedname" "$d/ctx.txt" "$d/ctx.json" && grep -qF "prefer go for concurrency" "$d/ctx.txt"; then
    pass "CX-08" "a CLAUDE.md ban is listed as having written a rule, without its name; a preference with its words"
else
    fail "CX-08" "the project file report is wrong" "$x $(grep -c zqbannedname "$d/ctx.txt")"
fi

# ------------------------------------------------------------------
# CX-09 -- guidance by heading, not text
# ------------------------------------------------------------------
d="$TEST_TMP/cx09"; mkdir -p "$d/docs"
cat > "$d/govern.json" <<'EOF'
{"version":"3.0","mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}},
 "guidance":{"sources":["docs/playbook.md"]}}
EOF
printf '# Playbook\n\n## No placeholders\n\nZQ_GUIDANCE_BODY finish the function before running it.\n' > "$d/docs/playbook.md"
ctxj "$d" rule:code_quality.no_placeholders
g=$(jq_py "$d/ctx.json" '[s["heading"] for s in j["guidance"]["sections"]]')
if [ "$g" = "['No placeholders']" ] && ! grep -qF "ZQ_GUIDANCE_BODY" "$d/ctx.json"; then
    pass "CX-09" "a refusal's guidance section is named by heading; its text is not shown"
else
    fail "CX-09" "the guidance report is wrong" "$g"
fi

# ------------------------------------------------------------------
# CX-10 / CX-11 / CX-11c / CX-12 -- history; writes and runs nothing
# ------------------------------------------------------------------
d="$TEST_TMP/cx10"; mkdir -p "$d"
cat > "$d/govern.json" <<EOF
{"mode":"enforce","security":{"sandbox_level":"elevated"},
 "telemetry":{"enabled":true,"output_file":"tele.jsonl","tamper_evidence":{"enabled":true}},
 "code_quality":{"no_placeholders":{"enabled":true,"level":"hard","rationale":"CX_MARK_HIST"}},
 "guidance":{"sources":["docs/playbook.md"]},
 "hooks":{"on_violation":{"command":"/bin/sh","args":["-c","echo fired >> $d/hook_marker"]}}}
EOF
mkdir -p "$d/docs"
printf '# Playbook\n\n## Python\n\nPython blocks keep to the standard library.\n\n## No placeholders\n\nFinish the function.\n' > "$d/docs/playbook.md"
printf 'fn compute(x) {\n    // TODO: finish this\n    return x * 2\n}\nmain {\n    print(compute(3))\n}\n' > "$d/t.naab"
(cd "$d" && timeout 120s "$NAAB" run t.naab > run.txt 2>&1; echo $? > run_rc)
ctxj "$d" rule:code_quality.no_placeholders
h1=$(jq_py "$d/ctx.json" '[(x["rule"], x["function"]) for x in j["history"]["remembered"]]')
ctxj "$d" file:t.naab
h2=$(jq_py "$d/ctx.json" '[(x["rule"], x["function"]) for x in j["history"]["remembered"]]')
ctxj "$d" rule:capabilities.network
h3=$(jq_py "$d/ctx.json" 'len(j["history"].get("remembered", []))')
if [ "$(cat "$d/run_rc")" = 3 ] && [ "$h1" = "[('code_quality.no_placeholders', 'compute')]" ] && [ "$h1" = "$h2" ] && [ "$h3" = 0 ]; then
    pass "CX-10" "a refusal is listed for its rule and its file, and not for another rule"
else
    fail "CX-10" "the history report is wrong" "run_rc=$(cat "$d/run_rc") rule=$h1 file=$h2 other=$h3"
fi
if [ -s "$d/hook_marker" ]; then
    pass "CX-11c" "CONTROL: the hook fires on a real run of the project"
else
    fail "CX-11c" "the hook did not fire on a real run -- CX-11 measures nothing" "$(grep -m2 -i hook "$d/run.txt")"
fi
rm -f "$d/hook_marker"
ctxj "$d" lang:python
pg=$(jq_py "$d/ctx.json" 'len(j["guidance"]["sections"])')
ctxj "$d" rule:code_quality.no_placeholders
rg=$(jq_py "$d/ctx.json" 'len(j["guidance"]["sections"])')
if [ "$pg" -ge 1 ] 2>/dev/null && [ "$rg" -ge 1 ] 2>/dev/null; then
    pass "CX-11g" "CONTROL: the language pull and the refusal's guidance each return a section"
else
    fail "CX-11g" "no guidance section was selected -- CX-11 cannot see a pull that writes" "pull=$pg refusal=$rg"
fi
before=$(sha256sum "$d/tele.jsonl" | cut -d' ' -f1)
for t in rule:code_quality.no_placeholders file:t.naab lang:python; do ctx "$d" "$t"; ctxj "$d" "$t"; done
after=$(sha256sum "$d/tele.jsonl" | cut -d' ' -f1)
if [ "$before" = "$after" ] && [ ! -e "$d/hook_marker" ]; then
    pass "CX-11" "context writes no telemetry and fires no hook"
else
    fail "CX-11" "context wrote telemetry or fired a hook" "tele changed: $([ "$before" = "$after" ] && echo no || echo yes) hook: $([ -e "$d/hook_marker" ] && echo yes || echo no)"
fi
d="$TEST_TMP/cx12"; mkdir -p "$d"
echo '{"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"}}' > "$d/govern.json"
printf 'use file\nmain {\n    file.write("ran_marker.txt", "ran")\n}\n' > "$d/w.naab"
ctx "$d" file:w.naab
if [ "$(cat "$d/ctx_rc")" = 0 ] && [ ! -e "$d/ran_marker.txt" ]; then
    pass "CX-12" "a file target's program is not run"
else
    fail "CX-12" "the program ran, or the report failed" "rc=$(cat "$d/ctx_rc")"
fi
(cd "$d" && "$NAAB" run w.naab > /dev/null 2>&1)
[ -e "$d/ran_marker.txt" ] || fail "CX-12c" "the probe program writes nothing when run -- CX-12 measures nothing"

# ------------------------------------------------------------------
# CX-13 -- JSON
# ------------------------------------------------------------------
ctxj "$TEST_TMP/cx01" rule:code_quality.no_placeholders
if [ "$(jq_py "$TEST_TMP/cx01/ctx.json" 'j["verdict"]')" = "REPORT" ]; then
    pass "CX-13" "--json is valid JSON with verdict REPORT"
else
    fail "CX-13" "--json is not a valid report" "$(head -c 200 "$TEST_TMP/cx01/ctx.json")"
fi

echo ""
echo "Results: $PASS_COUNT passed, $FAIL_COUNT failed, $SKIP_COUNT skipped"
[ "$FAIL_COUNT" -eq 0 ]
