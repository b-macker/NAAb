#!/usr/bin/env bash
# A185: an environment may raise a check's level, never lower it, and a key it
# cannot apply is named, not dropped.
#
# environments.<name> is a flat map of dotted keys. applyEnvironmentTo() used
# to apply seven of them (mode, three gate switches, three report paths) and
# drop every other key in silence, and a nested object was kept as one key
# holding its text: a production environment's
# `code_quality.no_placeholders.level: "hard"` over an advisory file ran
# advisory, exit 0 (observed on c1312132). Now each key that names a check in
# kEnvironmentChecks (governance_config.cpp) raises its level and turns it on,
# a lower level is ignored with a warning, any other key is warned about, and
# the reload ratchet compares the table's checks on the overlaid rules, so an
# edit that drops a raise mid-run is refused.
#
# Arms:
#   EV-00  CONTROL: no --env, the advisory check warns and the program runs
#   EV-01  dotted key raises no_placeholders to hard: exit 3        (red on c1312132)
#   EV-02  the nested form raises it too                             (red on c1312132)
#   EV-03  an environment turns on a check the file leaves off       (red on c1312132)
#   EV-04  a lower level is ignored, and the warning says so         (red on c1312132)
#   EV-05  a key no environment can change is named in a warning     (red on c1312132)
#   EV-06  a value that is not a level is named, and changes nothing (red on c1312132)
#   EV-07  CONTROL: the environment's mode still raises audit to enforce
#   EV-08  restrictions.dangerous_calls raised: the check reads the flat copy
#          (reconciled), so eval() is refused                         (red on c1312132)
#   RT-01  naab-gov ratchet --env: a reload that drops the raise is refused
#                                                                     (red on c1312132)
#   RT-01c CONTROL: the same file again is accepted
#   RT-03  the same for no_dead_code, a level only the table's comparison sees
#          on the overlaid rules                                  (red on c1312132)
#   RT-02  an ordinary file edit of a declared level prints one violation, not two
#   PIN    every `order level` key in ratchet_classification.txt is in the
#          table or in environment_levels_baseline.txt with a reason, and no
#          baseline entry is stale
#
# Kill table (mutants, run by hand, revert-checked 2026-11-20):
#   E1 applyEnvironmentCheckKeys() not called            -> EV-01..EV-06, EV-08, RT-01
#   E2 no environment object flattened                   -> EV-01..EV-08, RT-01
#   E3 a lower level applied (no levelLoosens test)      -> EV-04
#   E4 the minimum/mirror pass skipped after a raise     -> EV-01..EV-03, EV-08
#   E5 the ratchet loop over the table removed           -> RT-03 (RT-01 survives:
#                                                           an older line compares it)
#   E6 duplicate declared-order messages not dropped     -> RT-02
#   E7 a table entry removed (restrictions.dangerous_calls) -> EV-08
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
W="$(mktemp -d)"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -15 | sed 's/^/      | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] UNMEASURABLE: $2"; SKIP=$((SKIP+1)); }
for b in "$NAAB" "$GOV"; do [ -x "$b" ] || { echo "not built: $b"; exit 1; }; done

TODO_PROG='fn work(x) {
    // TODO: finish this
    return x
}
main {
    print("RAN " + work(1))
}'
EVAL_PROG='main {
    let r = <<javascript
eval("1 + 1")
>>
    print("RAN " + r)
}'

# go ID CONFIG PROGRAM [--env NAME] -> OUT, RC
go() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"; printf '%s\n' "$3" > "$d/p.naab"
    shift 3
    OUT="$(cd "$d" && timeout 60 "$NAAB" run "$@" p.naab 2>&1)"; RC=$?
}
has() { [[ "$OUT" == *"$1"* ]]; }
ADV='"code_quality":{"no_placeholders":{"enabled":true,"level":"advisory"}}'

echo "== environments raise check levels"
go EV00 "{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"code_quality.no_placeholders.level\":\"hard\"}}}" "$TODO_PROG"
if [ "$RC" -eq 0 ] && has "RAN 1"; then ok EV-00 "without --env the advisory check warns and the program runs"; live=1
else bad EV-00 "the program did not run without --env (rc=$RC)" "$OUT"; live=0; fi

go EV01 "{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"code_quality.no_placeholders.level\":\"hard\"}}}" "$TODO_PROG" --env production
if [ "$live" -ne 1 ]; then skip EV-01 "EV-00 did not run"
elif [ "$RC" -eq 3 ] && ! has "RAN 1" && has "code_quality.no_placeholders"; then ok EV-01 "a dotted level key raises the check to hard (exit 3)"
else bad EV-01 "the raise did not apply (rc=$RC)" "$OUT"; fi

go EV02 "{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"code_quality\":{\"no_placeholders\":{\"level\":\"hard\"}}}}}" "$TODO_PROG" --env production
if [ "$RC" -eq 3 ] && ! has "RAN 1"; then ok EV-02 "the nested form raises the check too (exit 3)"
else bad EV-02 "the nested form did not apply (rc=$RC)" "$OUT"; fi

go EV03 '{"mode":"enforce","environments":{"production":{"code_quality.no_placeholders.level":"hard"}}}' "$TODO_PROG" --env production
if [ "$RC" -eq 3 ] && ! has "RAN 1"; then ok EV-03 "an environment turns on a check the file leaves off"
else bad EV-03 "the check was not turned on (rc=$RC)" "$OUT"; fi

go EV04 '{"mode":"enforce","code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}},"environments":{"production":{"code_quality.no_placeholders.level":"advisory"}}}' "$TODO_PROG" --env production
if [ "$RC" -eq 3 ] && ! has "RAN 1" && has "can only raise a level"; then ok EV-04 "a lower level is ignored, with a warning (exit 3)"
else bad EV-04 "a lower level: rc=$RC, expected exit 3 and the warning" "$OUT"; fi

go EV05 "{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"limits.timeout.global\":\"5\"}}}" "$TODO_PROG" --env production
if [ "$RC" -eq 0 ] && has "sets limits.timeout.global, which an environment cannot change"; then ok EV-05 "an unappliable key is named in a warning"
else bad EV-05 "no warning naming limits.timeout.global (rc=$RC)" "$OUT"; fi

go EV06 "{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"code_quality.no_placeholders.level\":\"strict\"}}}" "$TODO_PROG" --env production
if [ "$RC" -eq 0 ] && has "RAN 1" && has "is not an enforcement level"; then ok EV-06 "a value that is not a level is named and changes nothing"
else bad EV-06 "a non-level value: rc=$RC" "$OUT"; fi

go EV07 '{"mode":"audit","code_quality":{"no_placeholders":{"enabled":true,"level":"hard"}},"environments":{"production":{"mode":"enforce"}}}' "$TODO_PROG" --env production
if [ "$RC" -eq 3 ] && ! has "RAN 1"; then ok EV-07 "the environment's mode still raises audit to enforce"
else bad EV-07 "mode enforce over audit did not refuse (rc=$RC)" "$OUT"; fi

go EV08c '{"mode":"enforce","security":{"sandbox_level":"elevated"},"restrictions":{"dangerous_calls":{"enabled":true,"level":"advisory"}}}' "$EVAL_PROG"
if [ "$RC" -ne 0 ] || ! has "RAN 2"; then skip EV-08 "the JavaScript block does not run here (rc=$RC)"
else
    go EV08 '{"mode":"enforce","security":{"sandbox_level":"elevated"},"restrictions":{"dangerous_calls":{"enabled":true,"level":"advisory"}},"environments":{"production":{"restrictions.dangerous_calls.level":"hard"}}}' "$EVAL_PROG" --env production
    if [ "$RC" -eq 3 ] && ! has "RAN 2"; then ok EV-08 "restrictions.dangerous_calls raised: eval() is refused (exit 3)"
    else bad EV-08 "the dangerous-call raise did not reach the check (rc=$RC)" "$OUT"; fi
fi

echo "== the reload ratchet compares the overlaid levels (naab-gov ratchet)"
# ratchet ID OLD NEW [args] -> OUT, RC
ratchet() {
    local d="$W/$1"; mkdir -p "$d/a" "$d/b"
    printf '%s\n' "$2" > "$d/a/govern.json"; printf '%s\n' "$3" > "$d/b/govern.json"
    shift 3
    OUT="$(cd "$d" && "$GOV" ratchet a/govern.json b/govern.json "$@" 2>&1)"; RC=$?
}
RAISED="{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{\"code_quality.no_placeholders.level\":\"hard\"}}}"
DROPPED="{\"mode\":\"enforce\",$ADV,\"environments\":{\"production\":{}}}"
ratchet RT01c "$RAISED" "$RAISED" --env production
if [ "$RC" -eq 0 ]; then ok RT-01c "the same file reloaded is accepted"; rt_live=1
else bad RT-01c "an unchanged reload was refused (rc=$RC)" "$OUT"; rt_live=0; fi
ratchet RT01 "$RAISED" "$DROPPED" --env production
if [ "$rt_live" -ne 1 ]; then skip RT-01 "RT-01c did not accept"
elif [ "$RC" -eq 1 ] && has "code_quality.no_placeholders.level: hard -> advisory"; then ok RT-01 "dropping the environment's raise mid-run is refused"
else bad RT-01 "dropping the raise: rc=$RC, expected a refusal naming the level" "$OUT"; fi

# RT-03: a check only the table's comparison covers on the overlaid rules
# (no_dead_code's level is otherwise compared on the raw file text, where an
# environment's raise does not appear). RT-01's no_placeholders is also
# compared by an older line of the ratchet, so RT-01 alone cannot show the
# table loop is there (the E5 mutant survived it).
ratchet RT03 '{"mode":"enforce","code_quality":{"no_dead_code":{"enabled":true,"level":"advisory"}},"environments":{"production":{"code_quality.no_dead_code.level":"hard"}}}' \
             '{"mode":"enforce","code_quality":{"no_dead_code":{"enabled":true,"level":"advisory"}},"environments":{"production":{}}}' --env production
if [ "$rt_live" -ne 1 ]; then skip RT-03 "RT-01c did not accept"
elif [ "$RC" -eq 1 ] && has "code_quality.no_dead_code.level: hard -> advisory"; then ok RT-03 "dropping an environment's raise of a level the file text cannot show is refused"
else bad RT-03 "rc=$RC, expected a refusal naming no_dead_code's level" "$OUT"; fi

ratchet RT02 '{"mode":"enforce","code_quality":{"no_dead_code":{"enabled":true,"level":"hard"}}}' \
             '{"mode":"enforce","code_quality":{"no_dead_code":{"enabled":true,"level":"advisory"}}}'
n="$(grep -c 'violation: code_quality.no_dead_code.level:' <<<"$OUT")"
if [ "$RC" -eq 1 ] && [ "$n" -eq 1 ]; then ok RT-02 "a file edit of a declared level is refused with one violation line"
else bad RT-02 "rc=$RC, $n violation lines (expected exit 1 and one)" "$OUT"; fi

echo "== the table is pinned against the declared level keys"
TABLE="$(awk '/ENVIRONMENT CHECK TABLE BEGIN/{f=1;next} /ENVIRONMENT CHECK TABLE END/{f=0} f' "$REPO/src/runtime/governance_config.cpp" \
         | grep -oE '"[a-z_.]+"' | tr -d '"' | sort -u)"
ORDER="$(grep -E '^[^#[:space:]]+[[:space:]]+order level' "$REPO/tests/governance_v4/ratchet_classification.txt" | awk '{print $1}' | sort -u)"
BASE="$(grep -vE '^(#|$)' "$REPO/tests/governance_v4/environment_levels_baseline.txt" | cut -f1 | sort -u)"
nt="$(grep -c . <<<"$TABLE")"; no="$(grep -c . <<<"$ORDER")"
if [ "$nt" -lt 40 ] || [ "$no" -lt 30 ]; then
    bad PIN "the probe read $nt table entries and $no order-level keys (it cannot read the sources)"
else
    missing=""; stale=""
    while read -r k; do
        [ -z "$k" ] && continue
        base="${k%.level}"
        if grep -qxF "$k" <<<"$TABLE" || grep -qxF "$base" <<<"$TABLE" || grep -qxF "$k" <<<"$BASE"; then continue; fi
        missing="$missing $k"
    done <<<"$ORDER"
    while read -r k; do
        [ -z "$k" ] && continue
        if ! grep -qxF "$k" <<<"$ORDER" || grep -qxF "$k" <<<"$TABLE" || grep -qxF "${k%.level}" <<<"$TABLE"; then stale="$stale $k"; fi
    done <<<"$BASE"
    if [ -z "$missing$stale" ]; then ok PIN "all $no order-level keys are in the table ($nt entries) or the baseline"
    else bad PIN "unclassified:${missing:- none}; stale baseline:${stale:- none}"; fi
fi

echo
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
