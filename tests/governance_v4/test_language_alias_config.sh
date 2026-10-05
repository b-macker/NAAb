#!/usr/bin/env bash
# ============================================================
# test_language_alias_config.sh -- a language named by any of its spellings in
# govern.json governs that language
#
# Every check receives a block's CANONICAL language name (<<bash>> is checked as
# "shell"), but the loader stored the names written in govern.json as written.
# So on master `languages.blocked: ["bash"]` never matched <<bash>> and blocked
# nothing; the same for sh, node, js, golang, cs, ts and any capitalised name,
# and per_language / custom-rule / per-agent / codegen lists keyed by an alias
# never applied. #294 briefly added sqlite to that set. Names are now
# canonicalised through the language table when the config loads.
#
# Alias groups come from the binary's own table (naab-gov languages), so a new
# alias is covered the day it is added.
#
#   AL-00  CONTROL: every canonical name in `blocked` blocks its own blocks --
#          the probe can see a block at all
#   AL-01  every alias spelling in `blocked` blocks every spelling of its
#          language (the bypass), and a capitalised spelling does too
#   AL-02  CONTROL: blocking a DIFFERENT language leaves the block alone --
#          without it AL-01 passes for a gate that blocks everything
#   AL-03  every alias spelling in `allowed` admits every spelling of its
#          language (the one permissive change: it used to refuse the very
#          language it named), and AL-03c: allowing a different language
#          still refuses it
#   AL-04  per_language keyed by an alias applies; AL-04c canonical key does
#   AL-05  per_language with two keys for one language is a config error
#          (exit 4) -- keeping either would silently drop the other's rules
#   AL-06  custom_rules[].languages with an alias applies
#   AL-07  real programs, both engines: blocked ["sql"] stops <<sqlite>> and
#          blocked ["sqlite"] stops <<sql>>; AL-07c: an unrelated block list
#          lets the same program run (the executor works)
#   AL-08  codegen.blocked_languages with an alias stops codegen.run; AL-08c
#          control: an unrelated codegen block list lets it run
#   AL-09  runtime_versions pinned under an alias applies, both engines
#          (test_runtime_pin_engines.sh covers pins in depth); AL-09c: an
#          unpinned run passes
#
# Runtime variants (the table's runtime_variants: `node`) are the same
# LANGUAGE as their canonical name but a separate EXECUTOR -- Node.js as a
# subprocess, where `javascript` is in-process QuickJS. Content checks treat
# them as the language (AL-06); allow/block lists treat them as a runtime. So
# they are left out of the AL-01/AL-03 alias groups and tested here instead:
#   AL-10  blocked [variant] blocks <<variant>>, and blocked [language] blocks
#          it too (as before); allowed [variant] admits it; allowed [language]
#          does NOT (on master `allowed: ["javascript"]` ran <<node>>, a
#          subprocess with filesystem reach -- measured)
#   AL-10c CONTROLS: blocked [variant] leaves <<language>> alone (it used to
#          block QuickJS too, breaking a config that allowed it), allowed
#          [language] admits <<language>>, allowed [variant] refuses it
#   AL-11  allowed [variant] + blocked [language] is reported as CONTRA-007;
#          AL-11c allowed [language] + blocked [variant] is not a
#          contradiction -- the config runs (examples/agent_harness uses it)
#   AL-12  codegen.run("node") reaches Node.js, not QuickJS (the shared
#          normaliser would have folded it), and codegen.allowed_languages
#          ["javascript"] refuses it
#   AL-13  an agent role cannot widen languages.allowed: project [sql] + role
#          [python] refuses <<python>> (on master the empty intersection meant
#          "unrestricted" and Python ran -- measured); AL-13c project
#          [sql, python] + role [python] runs it. Both engines.
#
# Unknown names (a typo, or a language NAAb has no table entry for) are a
# config ERROR (exit 4): on master `blocked: ["pyhton"]` loaded, matched no
# block and let Python run -- silent, in the open direction.
#   AL-14  "pyhton" in each of nine language lists: exit 4, the message
#          names the list and suggests "python", the program does not run
#   AL-14c CONTROL: the same nine fixtures with "python" load (no exit 4) --
#          without it AL-14 passes for fixtures that are broken anyway
#   AL-15  a name with no near match ("c") is refused without a guess
#          (Plugin rule languages use the same helper and are not staged here.)
#   AL-16  CONTROL: every canonical name and alias in the table, and its
#          upper-cased spelling, loads -- the check refuses only unknowns
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="$REPO/build/naab-lang"
GOV="$REPO/build/naab-gov"

# Writes unsigned govern.json files: keep the result independent of whatever
# else populated ~/.naab/trusted-keys.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -6 | sed 's/^/       | /'; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

echo "=== govern.json language names, any spelling ==="
if [ ! -x "$GOV" ] && [ ! -x "$GOV.exe" ]; then
    skip AL-00 "naab-gov not built -- UNMEASURABLE"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip AL-00 "python3 unavailable -- the alias groups cannot be read (UNMEASURABLE)"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

# Alias groups from the binary's own table: "canonical alias1 alias2 ..." per
# line, only for languages that have aliases. Bytes on stdin -- no path crosses
# into python.
# Runtime variants are excluded here and tested in AL-10.
groups="$("$GOV" languages | python3 -c '
import json, sys
for d in json.load(sys.stdin):
    al = [a for a in d["aliases"] if a not in d.get("runtime_variants", [])]
    if al:
        sys.stdout.write(" ".join([d["canonical"]] + al) + "\n")
' | tr -d '\r')"
# "variant canonical alias..." per runtime variant
variants="$("$GOV" languages | python3 -c '
import json, sys
for d in json.load(sys.stdin):
    al = [a for a in d["aliases"] if a not in d.get("runtime_variants", [])]
    for v in d.get("runtime_variants", []):
        sys.stdout.write(" ".join([v, d["canonical"]] + al) + "\n")
' | tr -d '\r')"
if [ -z "$groups" ]; then
    bad AL-00 "the language table has no alias groups -- nothing to test"
    echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 1
fi

# verdict LIST_KIND CONFIG_ENTRY BLOCK_TAG -> BLOCKED / allowed (naab-gov check,
# the same checkPolyglotBlock() naab-lang uses; nothing executed)
verdict() {
    local cfg out
    cfg="{\"mode\":\"enforce\",\"languages\":{\"$1\":[\"$2\"]}}"
    out="$(printf 'x = 1\n' | "$GOV" check --language "$3" --config-string "$cfg" 2>/dev/null)"
    case "$out" in *'"languages.blocked"'*|*'"languages.allowed"'*) echo BLOCKED ;; *) echo allowed ;; esac
}

# --- AL-00 / AL-01 / AL-02 / AL-03 ---
c0=0; f0=""; c1=0; f1=""; c2=0; f2=""; c3=0; f3=""; c3c=0; f3c=""
while read -r canon rest; do
    [ -n "$canon" ] || continue
    names="$canon $rest"
    other="python"; [ "$canon" = "python" ] && other="ruby"
    for tag in $names; do
        [ "$(verdict blocked "$canon" "$tag")" = BLOCKED ] && c0=$((c0+1)) || f0+=" blocked:[$canon]/<<$tag>>"
        [ "$(verdict blocked "$other" "$tag")" = allowed ] && c2=$((c2+1)) || f2+=" blocked:[$other]/<<$tag>>"
        [ "$(verdict allowed "$other" "$tag")" = BLOCKED ] && c3c=$((c3c+1)) || f3c+=" allowed:[$other]/<<$tag>>"
        for entry in $rest $(printf '%s' "$canon" | tr '[:lower:]' '[:upper:]'); do
            [ "$(verdict blocked "$entry" "$tag")" = BLOCKED ] && c1=$((c1+1)) || f1+=" blocked:[$entry]/<<$tag>>"
            [ "$(verdict allowed "$entry" "$tag")" = allowed ] && c3=$((c3+1)) || f3+=" allowed:[$entry]/<<$tag>>"
        done
    done
done <<< "$groups"
[ -z "$f0" ] && ok AL-00 "canonical names block their blocks ($c0 cases)" || bad AL-00 "a canonical name did not block" "$f0"
[ -z "$f1" ] && ok AL-01 "alias and capitalised spellings in blocked block every spelling ($c1 cases)" \
             || bad AL-01 "an alias in languages.blocked does not block its language (bypass)" "$f1"
[ -z "$f2" ] && ok AL-02 "blocking another language leaves every block alone ($c2 cases)" \
             || bad AL-02 "a block list for another language blocked this one" "$f2"
[ -z "$f3" ] && ok AL-03 "alias spellings in allowed admit their language ($c3 cases)" \
             || bad AL-03 "an alias in languages.allowed refuses the language it names" "$f3"
[ -z "$f3c" ] && ok AL-03c "allowing another language still refuses this one ($c3c cases)" \
              || bad AL-03c "an allowlist for another language admitted this one" "$f3c"

# --- AL-10: runtime variants in allow/block lists ---
if [ -z "$variants" ]; then
    bad AL-10 "the language table declares no runtime variant -- node is its own executor (main.cpp registers it apart from QuickJS)"
else
    c10=0; f10=""; c10c=0; f10c=""
    while read -r v canon rest; do
        [ -n "$v" ] || continue
        [ "$(verdict blocked "$v" "$v")" = BLOCKED ] && c10=$((c10+1)) || f10+=" blocked:[$v]/<<$v>>"
        [ "$(verdict allowed "$v" "$v")" = allowed ] && c10=$((c10+1)) || f10+=" allowed:[$v]/<<$v>>"
        for lang in $canon $rest; do
            [ "$(verdict blocked "$lang" "$v")" = BLOCKED ] && c10=$((c10+1)) || f10+=" blocked:[$lang]/<<$v>>"
            [ "$(verdict allowed "$lang" "$v")" = BLOCKED ] && c10=$((c10+1)) || f10+=" allowed:[$lang]/<<$v>>"
            [ "$(verdict blocked "$v" "$lang")" = allowed ] && c10c=$((c10c+1)) || f10c+=" blocked:[$v]/<<$lang>>"
            [ "$(verdict allowed "$canon" "$lang")" = allowed ] && c10c=$((c10c+1)) || f10c+=" allowed:[$canon]/<<$lang>>"
            [ "$(verdict allowed "$v" "$lang")" = BLOCKED ] && c10c=$((c10c+1)) || f10c+=" allowed:[$v]/<<$lang>>"
        done
    done <<< "$variants"
    [ -z "$f10" ] && ok AL-10 "runtime variants: blocked by name or language, admitted only by name ($c10 cases)"                   || bad AL-10 "a runtime variant is governed by the wrong name" "$f10"
    [ -z "$f10c" ] && ok AL-10c "a runtime variant's list entry does not govern its language's other runtime ($c10c cases)"                    || bad AL-10c "a runtime variant entry governed the other runtime" "$f10c"
fi

# --- AL-04 / AL-05: per_language ---
pl() {  # $1 = per_language JSON object; shell code with a banned call
    printf 'rm -rf /tmp/naab_alias_probe\n' | "$GOV" check --language bash \
        --config-string "{\"mode\":\"enforce\",\"languages\":{\"per_language\":$1}}" 2>&1
}
o4="$(pl '{"bash":{"banned_functions":["rm -rf"]}}')"
o4c="$(pl '{"shell":{"banned_functions":["rm -rf"]}}')"
case "$o4c" in *languages.per_language.banned_functions*)
    case "$o4" in *languages.per_language.banned_functions*) ok AL-04 "per_language keyed \"bash\" applies to <<bash>>" ;;
        *) bad AL-04 "per_language keyed by an alias never applies" "$o4" ;; esac ;;
    *) bad AL-04c "per_language keyed \"shell\" did not apply -- probe broken" "$o4c" ;; esac
case "$o4c" in *languages.per_language.banned_functions*) ok AL-04c "per_language keyed \"shell\" applies (control)" ;; esac
# Through naab-lang and a govern.json FILE, the way a user meets it: naab-gov's
# inline --config-string path prints its own generic "failed to parse" line in
# place of the loader's message (same exit 4, less said).
W5="$(mktemp -d)"
printf '%s\n' '{"mode":"enforce","languages":{"per_language":{"bash":{"max_lines":5},"shell":{"max_lines":500}}}}' > "$W5/govern.json"
printf 'main {\n    print("RAN")\n}\n' > "$W5/p.naab"
o5="$(cd "$W5" && "$NAAB" p.naab 2>&1)"; rc5=$?
rm -rf "$W5"
case "$o5" in *"more than one entry for the language"*)
    if [ "$rc5" -eq 4 ] && [[ "$o5" != *RAN* ]]; then
        ok AL-05 "two per_language keys for one language: config error (exit 4), program not run"
    else
        bad AL-05 "collision reported but exit $rc5 (expected 4) or the program ran" "$o5"
    fi ;;
  *) bad AL-05 "two per_language keys for one language were accepted (exit $rc5)" "$o5" ;; esac

# --- AL-06: custom_rules ---
o6="$(printf 'FORBIDDEN_CALL()\n' | "$GOV" check --language node --config-string \
    '{"mode":"enforce","custom_rules":[{"id":"CR-ALIAS","pattern":"FORBIDDEN_CALL","languages":["js"],"level":"hard","message":"no"}]}' 2>&1)"
case "$o6" in *CR-ALIAS*) ok AL-06 "custom rule scoped to \"js\" applies to <<node>>" ;;
    *) bad AL-06 "custom rule scoped by an alias never applies" "$o6" ;; esac

# --- AL-07 / AL-08 / AL-09: real programs (SQL runs in-process everywhere) ---
W="$(mktemp -d)"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
run() {  # $1 = dir, $2 = engine flag or "", $3 = file
    (cd "$1" && "$NAAB" ${2:+$2} "$3" --timeout 20 2>&1)
}
mk() {  # $1 = dir, $2 = govern.json, $3 = program
    mkdir -p "$W/$1"; printf '%s\n' "$2" > "$W/$1/govern.json"; printf '%s\n' "$3" > "$W/$1/p.naab"
}
PROG_SQLITE=$'main {\n    let r = <<sqlite\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
PROG_SQL=$'main {\n    let r = <<sql\nSELECT 41 + 1 AS v\n>>\n    print("RAN")\n}'
mk b1 '{"mode":"enforce","languages":{"blocked":["sql"]}}' "$PROG_SQLITE"
mk b2 '{"mode":"enforce","languages":{"blocked":["sqlite"]}}' "$PROG_SQL"
mk b3 '{"mode":"enforce","languages":{"blocked":["ruby"]}}' "$PROG_SQLITE"
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    o="$(run "$W/b3" "$eng" p.naab)"; r=$?
    if [ $r -ne 0 ] || [[ "$o" != *RAN* ]]; then
        skip "AL-07/$e" "the SQL executor did not run under an unrelated block list (exit $r) -- UNMEASURABLE"
        continue
    fi
    ok "AL-07c/$e" "unrelated block list: <<sqlite>> runs"
    o1="$(run "$W/b1" "$eng" p.naab)"; r1=$?
    o2="$(run "$W/b2" "$eng" p.naab)"; r2=$?
    if [ $r1 -eq 3 ] && [[ "$o1" != *RAN* ]] && [ $r2 -eq 3 ] && [[ "$o2" != *RAN* ]]; then
        ok "AL-07/$e" "blocked [sql] stops <<sqlite>>, blocked [sqlite] stops <<sql>> (exit 3)"
    else
        bad "AL-07/$e" "an alias block did not stop the program (exit $r1/$r2)" "$o1"$'\n'"$o2"
    fi
done
PROG_CG=$'use codegen\nmain {\n    let r = codegen.run("sqlite", "SELECT 1 AS v")\n    print("RAN")\n}'
# enforce upgrades the sandbox to standard, which refuses codegen execution
# outright; elevated lets the governance check be the thing that decides.
mk c1 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard","blocked_languages":["sql"]}}' "$PROG_CG"
mk c2 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard","blocked_languages":["ruby"]}}' "$PROG_CG"
oc2="$(run "$W/c2" "" p.naab)"; rc2=$?
if [ $rc2 -ne 0 ] || [[ "$oc2" != *RAN* ]]; then
    skip AL-08 "codegen.run(\"sqlite\") did not run under an unrelated block list (exit $rc2) -- UNMEASURABLE"
else
    ok AL-08c "unrelated codegen block list: codegen.run(\"sqlite\") runs"
    oc1="$(run "$W/c1" "" p.naab)"; rc1=$?
    if [[ "$oc1" == *"is blocked for dynamic code"* ]] && [[ "$oc1" != *RAN* ]]; then
        ok AL-08 "codegen.blocked_languages [\"sql\"] stops codegen.run(\"sqlite\") (exit $rc1)"
    else
        bad AL-08 "codegen.blocked_languages by an alias did not stop codegen.run (exit $rc1)" "$oc1"
    fi
fi
mk p1 '{"mode":"enforce","runtime_versions":[{"language":"sqlite","required":">=999","level":"hard"}]}' "$PROG_SQL"
mk p2 '{"mode":"enforce"}' "$PROG_SQL"
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    op2="$(run "$W/p2" "$eng" p.naab)"; rp2=$?
    if [ $rp2 -ne 0 ] || [[ "$op2" != *RAN* ]]; then
        skip "AL-09/$e" "<<sql>> did not run (exit $rp2) -- UNMEASURABLE"
        continue
    fi
    ok "AL-09c/$e" "unpinned <<sql>> runs"
    op1="$(run "$W/p1" "$eng" p.naab)"; rp1=$?
    if [[ "$op1" == *runtime_version* ]] && [ $rp1 -eq 3 ]; then
        ok "AL-09/$e" "a runtime_versions pin on \"sqlite\" applies to <<sql>> (exit 3)"
    else
        bad "AL-09/$e" "a runtime pin under an alias never applied (exit $rp1)" "$op1"
    fi
done

# --- AL-11: contradiction detection speaks runtimes too ---
PROG_PRINT=$'main {\n    print("RAN")\n}'
mk k1 '{"mode":"enforce","languages":{"allowed":["node"],"blocked":["javascript"]}}' "$PROG_PRINT"
mk k2 '{"mode":"enforce","languages":{"allowed":["javascript"],"blocked":["node"]}}' "$PROG_PRINT"
ok1="$(run "$W/k1" "" p.naab)"
ok2="$(run "$W/k2" "" p.naab)"; rk2=$?
if [[ "$ok2" == *RAN* ]] && [ $rk2 -eq 0 ] && [[ "$ok2" != *CONTRA-007* ]]; then
    ok AL-11c "allowed [javascript] + blocked [node] is not a contradiction: the program runs"
else
    bad AL-11c "allowed [javascript] + blocked [node] was treated as a contradiction (exit $rk2)" "$ok2"
fi
case "$ok1" in *CONTRA-007*) ok AL-11 "allowed [node] + blocked [javascript] is reported (CONTRA-007)" ;;
    *) bad AL-11 "allowed [node] + blocked [javascript] went unreported" "$ok1" ;; esac

# --- AL-12: codegen keeps node on Node.js ---
if ! command -v node >/dev/null 2>&1; then
    skip AL-12 "node not installed -- UNMEASURABLE"
else
    PROG_CGN=$'use codegen\nmain {\n    let r = codegen.run("node", "require(\'fs\').writeFileSync(\'cg_node.txt\', \'x\')")\n    print("RAN")\n}'
    mk n1 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard","allowed_languages":["node"]}}' "$PROG_CGN"
    mk n2 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"codegen":{"enabled":true,"level":"hard","allowed_languages":["javascript"]}}' "$PROG_CGN"
    on1="$(run "$W/n1" "" p.naab)"; rn1=$?
    if [ -f "$W/n1/cg_node.txt" ]; then
        ok AL-12 "codegen.run(\"node\") ran on Node.js (require('fs') wrote its file)"
    else
        bad AL-12 "codegen.run(\"node\") did not reach Node.js (exit $rn1)" "$on1"
    fi
    on2="$(run "$W/n2" "" p.naab)"; rn2=$?
    if [ ! -f "$W/n2/cg_node.txt" ] && [[ "$on2" == *"is not allowed for dynamic code"* ]]; then
        ok AL-12c "codegen.allowed_languages [javascript] refuses codegen.run(\"node\") (exit $rn2)"
    else
        bad AL-12c "allowing QuickJS admitted Node.js through codegen (exit $rn2)" "$on2"
    fi
fi

# --- AL-13: an agent role narrows languages.allowed, never widens it ---
PROG_PY=$'main {\n    let r = <<python\n1 + 1\n>>\n    print("RAN")\n}'
mk r1 '{"mode":"enforce","languages":{"allowed":["sql"]},"agents":{"r":{"allowed_languages":["python"]}}}' "$PROG_PY"
mk r2 '{"mode":"enforce","languages":{"allowed":["sql","python"]},"agents":{"r":{"allowed_languages":["python"]}}}' "$PROG_PY"
for eng in "" --tree-walk; do
    e=${eng:-vm}; e=${e#--}
    or2="$(cd "$W/r2" && "$NAAB" ${eng:+$eng} --agent-id r p.naab --timeout 20 2>&1)"; rr2=$?
    if [ $rr2 -ne 0 ] || [[ "$or2" != *RAN* ]]; then
        bad "AL-13c/$e" "a role inside the project allow list could not run <<python>> (exit $rr2)" "$or2"
        continue
    fi
    ok "AL-13c/$e" "project [sql, python] + role [python]: <<python>> runs"
    or1="$(cd "$W/r1" && "$NAAB" ${eng:+$eng} --agent-id r p.naab --timeout 20 2>&1)"; rr1=$?
    if [ $rr1 -eq 3 ] && [[ "$or1" != *RAN* ]]; then
        ok "AL-13/$e" "project [sql] + role [python]: <<python>> refused (exit 3)"
    else
        bad "AL-13/$e" "an agent role widened languages.allowed (exit $rr1)" "$or1"
    fi
done

# --- AL-14 / AL-14c / AL-15: unknown names are refused ---
# One fixture per list; NAME is substituted. Each must LOAD with "python".
fixture() {  # $1 = list id, $2 = language name
    local n="\"$2\""
    case "$1" in
        allowed)    echo "{\"mode\":\"enforce\",\"languages\":{\"allowed\":[$n]}}" ;;
        blocked)    echo "{\"mode\":\"enforce\",\"languages\":{\"blocked\":[$n]}}" ;;
        per_lang)   echo "{\"mode\":\"enforce\",\"languages\":{\"per_language\":{$n:{\"max_lines\":500}}}}" ;;
        custom)     echo "{\"mode\":\"enforce\",\"custom_rules\":[{\"id\":\"X\",\"pattern\":\"ZZZ_NEVER\",\"languages\":[$n],\"level\":\"advisory\",\"message\":\"m\"}]}" ;;
        cg_allow)   echo "{\"mode\":\"enforce\",\"codegen\":{\"enabled\":true,\"allowed_languages\":[$n]}}" ;;
        cg_block)   echo "{\"mode\":\"enforce\",\"codegen\":{\"enabled\":true,\"blocked_languages\":[$n]}}" ;;
        ag_allow)   echo "{\"mode\":\"enforce\",\"agents\":{\"r\":{\"allowed_languages\":[$n]}}}" ;;
        ag_block)   echo "{\"mode\":\"enforce\",\"agents\":{\"r\":{\"blocked_languages\":[$n]}}}" ;;
        pin)        echo "{\"mode\":\"enforce\",\"runtime_versions\":[{\"language\":$n,\"required\":\">=0\"}]}" ;;
    esac
}
LISTS="allowed blocked per_lang custom cg_allow cg_block ag_allow ag_block pin"
f14=""; f14c=""; c14=0
for L in $LISTS; do
    mk "u_$L" "$(fixture "$L" pyhton)" "$PROG_PRINT"
    mk "k_$L" "$(fixture "$L" python)" "$PROG_PRINT"
    ou="$(run "$W/u_$L" "" p.naab)"; ru=$?
    ok_="$(run "$W/k_$L" "" p.naab)"; rk=$?
    [ $rk -ne 4 ] || f14c+=" $L(rc=4: ${ok_##*Error: })"
    if [ $ru -eq 4 ] && [[ "$ou" == *'unknown language name "pyhton"'* ]] \
       && [[ "$ou" == *'Did you mean "python"'* ]] && [[ "$ou" != *RAN* ]]; then
        c14=$((c14+1))
    else
        f14+=" $L(rc=$ru)"
    fi
done
[ -z "$f14c" ] && ok AL-14c "every fixture loads with a known name (9 lists)" \
               || bad AL-14c "a fixture fails to load even with \"python\" -- AL-14 would prove nothing" "$f14c"
[ -z "$f14" ] && ok AL-14 "\"pyhton\" is refused with a suggestion in every list ($c14 lists, exit 4)" \
              || bad AL-14 "an unknown language name was accepted, or refused without the suggestion" "$f14"
mk u_c "$(fixture blocked c)" "$PROG_PRINT"
o15="$(run "$W/u_c" "" p.naab)"; r15=$?
if [ $r15 -eq 4 ] && [[ "$o15" == *'unknown language name "c"'* ]] && [[ "$o15" != *"Did you mean"* ]] \
   && [[ "$o15" == *"no language by that name"* ]]; then
    ok AL-15 "a name with no near match (\"c\") is refused without a guess (exit 4)"
else
    bad AL-15 "\"c\" was accepted or given a guess (exit $r15)" "$o15"
fi

# --- AL-16: every known spelling loads (naab-gov's loader; rc 4 = refused) ---
names16="$("$GOV" languages | python3 -c '
import json, sys
for d in json.load(sys.stdin):
    for n in [d["canonical"]] + d["aliases"]:
        sys.stdout.write(n + "\n" + n.upper() + "\n")
' | tr -d '\r')"
c16=0; f16=""
while read -r n; do
    [ -n "$n" ] || continue
    printf 'x = 1\n' | "$GOV" check --language python \
        --config-string "{\"mode\":\"enforce\",\"languages\":{\"blocked\":[\"$n\"]}}" >/dev/null 2>&1
    [ $? -ne 4 ] && c16=$((c16+1)) || f16+=" $n"
done <<< "$names16"
[ -z "$f16" ] && [ $c16 -gt 0 ] && ok AL-16 "every table name and alias, either case, loads ($c16 spellings)" \
              || bad AL-16 "a real language name was refused as unknown" "$f16"

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
