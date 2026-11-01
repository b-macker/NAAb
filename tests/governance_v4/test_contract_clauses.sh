#!/usr/bin/env bash
# contracts.<fn>.requires / ensures (Bundle 11) and the null-return contract
# check (A114).
#
# A clause is one NAAb expression in govern.json. requires holds over the
# arguments when the function is entered (after its defaults); ensures over the
# arguments AS THEY WERE ON ENTRY and the returned value, named `result`. The VM
# checks both at the call boundary, at the contract's level (else
# contracts.level). A clause must be pure: the loader refuses one that calls,
# assigns, interpolates or runs a polyglot block (exit 4), the compiler refuses
# one that names anything but a parameter (exit 4), and the VM refuses, while a
# clause runs, a call that is not len/type/typeof and a member read on a value
# that is not a dict -- a parameter can hold a module, and a program can define
# its own `len`.
#
# Every refusal arm is beside a control that the same program runs when the
# clause holds, so a VM that refused every call would fail the controls, and a
# VM that checked nothing would fail the refusals. "BODY_RAN" in a function
# body is the side effect a requires refusal must prevent.
#
# Kill table (each mutant built and run alone, naab-lang and naab-gov both
# rebuilt; the controls stayed green), observed:
#   M1 OP_CONTRACT_ENTRY does nothing          -> RQ-01..08 DF-01 EN-01 EN-03 RG-01 RG-02 AS-01 CB-01 AU-01
#   M2 ensures not evaluated at OP_RETURN      -> EN-01 EN-03
#   M3 ensures reads the parameter slots at return, not the entry snapshot -> EN-02
#   M4 OP_RETURN_NULL skips the return checks (A114) -> NR-01 EN-03
#   M5 a non-boolean clause value counts as holding  -> RQ-06
#   M6 no call guard in callValue while a clause runs -> RG-01
#   M7 no member guard on a module value while a clause runs -> RG-02 (at elevated:
#      under standard the sandbox refused the env read and hid the missing guard)
#   M8 the loader accepts an impure clause     -> PU-10 (the compiler's check still
#      refuses PU-01..PU-05: a second layer)
#   M9 the compiler does not check clause names -> PU-07
#   Not a mutant: the clause's return-taint flag is saved and restored; VM returns
#   carry taint on the taint stack, so TA-01/TA-02 pin only that taint still flows
#   through a contracted function.
#
# Usage: bash tests/governance_v4/test_contract_clauses.sh [path/to/naab-lang]
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-${NAAB:-$SCRIPT_DIR/../../build/naab-lang}}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"
NAAB_GOV="$(dirname "$NAAB")/naab-gov"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS $1"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL $1"; [ -n "${2:-}" ] && printf '%s\n' "$2" | sed 's/^/      /' | head -20; }
skip() { SKIP=$((SKIP+1)); echo "  UNMEASURABLE $1"; }

if [ ! -x "$NAAB" ]; then
    echo "UNMEASURABLE: $NAAB not built"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# run NAME CONFIG PROGRAM [extra args...] -> sets OUT (stdout+stderr) and RC
run() {
    local name="$1" cfg="$2" prog="$3"; shift 3
    local d="$TMP/$name"
    mkdir -p "$d"
    printf '%s\n' "$cfg" > "$d/govern.json"
    printf '%s\n' "$prog" > "$d/p.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" run p.naab "$@" 2>&1)"
    RC=$?
}

has() { grep -qF -- "$1" <<<"$OUT"; }

cfg() {  # cfg LEVEL FUNCTIONS_JSON [EXTRA_TOP_LEVEL_JSON]
    printf '{"version":"3.0","mode":"enforce","contracts":{"level":"%s","functions":%s}%s}' \
        "$1" "$2" "${3:-}"
}

echo "=== contracts.<fn>.requires / ensures ==="

# --- requires ---------------------------------------------------------------
P_SQRT='fn root(n) {
    print("BODY_RAN")
    return n
}
main {
    let r = root(ARG)
    print("AFTER=" + string(r))
}'
C_REQ="$(cfg hard '{"root":{"requires":["n >= 0"]}}')"

run rq00c "$C_REQ" "${P_SQRT/ARG/4}"
if [ "$RC" -eq 0 ] && has "BODY_RAN" && has "AFTER=4"; then pass "RQ-00c a call that meets requires runs"
else fail "RQ-00c control: requires met must run (rc=$RC)" "$OUT"; fi

run rq01 "$C_REQ" "${P_SQRT/ARG/-1}"
if [ "$RC" -eq 3 ] && has "requires does not hold" && ! has "BODY_RAN" && ! has "AFTER="; then
    pass "RQ-01 a call that breaks requires is refused before the body (hard, exit 3)"
else fail "RQ-01 requires broken must block before the body (rc=$RC)" "$OUT"; fi

run rq02 "$(cfg soft '{"root":{"requires":["n >= 0"]}}')" "${P_SQRT/ARG/-1}"
if [ "$RC" -eq 3 ] && ! has "BODY_RAN"; then pass "RQ-02 soft without override blocks (exit 3)"
else fail "RQ-02 soft requires must block (rc=$RC)" "$OUT"; fi

run rq03 "$(cfg advisory '{"root":{"requires":["n >= 0"]}}')" "${P_SQRT/ARG/-1}"
if [ "$RC" -eq 0 ] && has "requires does not hold" && has "BODY_RAN"; then
    pass "RQ-03 advisory reports the breach and continues"
else fail "RQ-03 advisory must warn and continue (rc=$RC)" "$OUT"; fi

run rq04 "$(cfg detect '{"root":{"requires":["n >= 0"]}}')" 'fn root(n) {
    print("BODY_RAN")
    return n
}
main {
    try {
        let r = root(-1)
        print("AFTER=" + string(r))
    } catch (e) {
        print("CAUGHT")
    }
}'
if [ "$RC" -eq 0 ] && has "CAUGHT" && ! has "BODY_RAN"; then pass "RQ-04 detect is catchable, body not run"
else fail "RQ-04 detect must be catchable (rc=$RC)" "$OUT"; fi

# the per-contract level wins over contracts.level
run rq05 "$(cfg advisory '{"root":{"level":"hard","requires":["n >= 0"]}}')" "${P_SQRT/ARG/-1}"
if [ "$RC" -eq 3 ] && ! has "BODY_RAN"; then pass "RQ-05 the contract's own level is applied"
else fail "RQ-05 per-contract level hard must block (rc=$RC)" "$OUT"; fi

# only `true` holds: a clause that evaluates to a number is a breach
run rq06 "$(cfg hard '{"root":{"requires":["n"]}}')" "${P_SQRT/ARG/4}"
if [ "$RC" -eq 3 ] && has "not true or false" && ! has "BODY_RAN"; then
    pass "RQ-06 a non-boolean clause is a breach, not a pass"
else fail "RQ-06 non-boolean clause must not hold (rc=$RC)" "$OUT"; fi

# a clause that cannot be evaluated is a breach (fail closed)
run rq07 "$(cfg hard '{"first":{"requires":["xs[0] > 0"]}}')" 'fn first(xs) {
    print("BODY_RAN")
    return xs
}
main {
    let r = first([])
    print("AFTER")
}'
if [ "$RC" -eq 3 ] && has "could not be evaluated" && ! has "BODY_RAN"; then
    pass "RQ-07 a clause that cannot be evaluated is a breach"
else fail "RQ-07 unevaluable clause must block (rc=$RC)" "$OUT"; fi

# several clauses: every one must hold; len() is allowed
run rq08 "$(cfg hard '{"pick":{"requires":["len(xs) > 0","i >= 0","i < len(xs)"]}}')" 'fn pick(xs, i) {
    print("BODY_RAN")
    return xs[i]
}
main {
    print("GOT=" + string(pick([7, 8], 1)))
    print("GOT=" + string(pick([7, 8], 2)))
}'
if [ "$RC" -eq 3 ] && has "GOT=8" && ! has "GOT=null" && has "i < len(xs)"; then
    pass "RQ-08 every clause is checked; the failing one is named"
else fail "RQ-08 multi-clause requires (rc=$RC)" "$OUT"; fi

# defaults are filled before requires
run df01 "$(cfg hard '{"d":{"requires":["n == 5"]}}')" 'fn d(n = 5) {
    return n
}
main {
    print("D=" + string(d()))
    print("D=" + string(d(6)))
}'
if [ "$RC" -eq 3 ] && has "D=5" && ! has "D=6"; then pass "DF-01 requires sees the default value"
else fail "DF-01 requires must see defaults (rc=$RC)" "$OUT"; fi

# --- ensures ----------------------------------------------------------------
P_ABS='fn absval(n) {
    if n < 0 { return n }
    return n
}
main {
    print("A=" + string(absval(3)))
    print("A=" + string(absval(-2)))
    print("AFTER")
}'
run en01 "$(cfg hard '{"absval":{"ensures":["result >= 0"]}}')" "$P_ABS"
if [ "$RC" -eq 3 ] && has "A=3" && has "ensures does not hold" && ! has "AFTER"; then
    pass "EN-01 a result that breaks ensures is refused (and one that meets it returns)"
else fail "EN-01 ensures broken must block (rc=$RC)" "$OUT"; fi

# ensures reads the arguments as they were on entry
run en02 "$(cfg hard '{"bump":{"ensures":["result == n + 10"]}}')" 'fn bump(n) {
    n = n + 10
    return n
}
main {
    print("B=" + string(bump(1)))
}'
if [ "$RC" -eq 0 ] && has "B=11"; then pass "EN-02 ensures reads entry arguments, not reassigned ones"
else fail "EN-02 ensures must use the entry arguments (rc=$RC)" "$OUT"; fi

# ensures on a bare `return` (OP_RETURN_NULL)
P_BARE='fn h(x) {
    if x > 0 { return }
    return 1
}
main {
    print("H=" + string(h(0)))
    print("H=" + string(h(1)))
    print("AFTER")
}'
run en03 "$(cfg hard '{"h":{"ensures":["result != null"]}}')" "$P_BARE"
if [ "$RC" -eq 3 ] && has "H=1" && ! has "AFTER"; then pass "EN-03 ensures is checked on a bare return"
else fail "EN-03 ensures on a null return (rc=$RC)" "$OUT"; fi

# --- A114: the output contract on a null return -------------------------------
run nr01 "$(cfg hard '{"h":{"return_not_null":true}}')" "$P_BARE"
if [ "$RC" -eq 3 ] && has "H=1" && ! has "AFTER"; then
    pass "NR-01 return_not_null is checked on a bare return (A114)"
else fail "NR-01 return_not_null must see a bare return (rc=$RC)" "$OUT"; fi

run nr01c "$(cfg hard '{"h":{"return_not_null":true}}')" 'fn h(x) {
    return 1
}
main {
    print("H=" + string(h(1)))
    print("AFTER")
}'
if [ "$RC" -eq 0 ] && has "AFTER"; then pass "NR-01c control: a value return passes return_not_null"
else fail "NR-01c control (rc=$RC)" "$OUT"; fi

# --- purity: refused at load or compile (exit 4) ------------------------------
pure_arm() {  # pure_arm ID CLAUSE_JSON DESC
    run "pu$1" "$(cfg hard "{\"root\":{\"requires\":[$2]}}")" "${P_SQRT/ARG/4}"
    if [ "$RC" -eq 4 ] && ! has "BODY_RAN" && has "contract"; then pass "PU-$1 $3 is refused (exit 4)"
    else fail "PU-$1 $3 must refuse the config (rc=$RC)" "$OUT"; fi
}
pure_arm 01 '"print(n) == null"'  "a call"
pure_arm 02 '"n = 1"'             "an assignment"
pure_arm 03 '"\"${n}\" == \"4\""' "an interpolating string"
pure_arm 04 '"[n].contains(4)"'   "a method call"
pure_arm 05 '"n |> len"'          "a pipeline"
pure_arm 06 '1'                   "a clause that is not text"
pure_arm 07 '"m >= 0"'            "a name that is not a parameter"
pure_arm 08 '"n >= 0 )"'          "trailing text"

# The loader is the only layer that sees a clause on a function the program
# never defines (the compiler checks clauses as it compiles their function).
run pu10 "$(cfg hard '{"ghost":{"requires":["print(n) == null"]}}')" "${P_SQRT/ARG/4}"
if [ "$RC" -eq 4 ] && ! has "BODY_RAN"; then pass "PU-10 an impure clause is refused at load, function or not"
else fail "PU-10 the loader must refuse an impure clause on any function (rc=$RC)" "$OUT"; fi

# A generator's body runs when it is iterated, not called: a clause there
# would check nothing, so it is refused.
run pu11 "$(cfg hard '{"count_up":{"requires":["n >= 0"]}}')" 'fn count_up(n) {
    for i in 0..n {
        yield i
    }
}
main {
    for x in count_up(2) { print(x) }
}'
if [ "$RC" -eq 4 ] && has "generator"; then pass "PU-11 a clause on a generator is refused"
else fail "PU-11 generator clause must be refused (rc=$RC)" "$OUT"; fi

run pu09 "$(cfg hard '{"root":{"ensures":["result >= 0"]}}')" 'fn root(result) {
    return result
}
main { print(root(1)) }'
if [ "$RC" -eq 4 ]; then pass "PU-09 ensures on a function with a parameter named result is refused"
else fail "PU-09 parameter named result (rc=$RC)" "$OUT"; fi

# --- runtime guards -------------------------------------------------------------
# A program's own `len` cannot run inside a clause.
run rg01 "$(cfg hard '{"root":{"requires":["len(xs) > 0"]}}')" 'fn len(x) {
    print("USER_LEN_RAN")
    return 5
}
fn root(xs) {
    print("BODY_RAN")
    return xs
}
main {
    root([1])
    print("AFTER")
}'
if [ "$RC" -eq 3 ] && ! has "USER_LEN_RAN" && ! has "BODY_RAN"; then
    pass "RG-01 a clause cannot call a program-defined len"
elif [ "$RC" -eq 4 ] || has "cannot define"; then
    skip "RG-01 the program could not define len (rc=$RC)"
else fail "RG-01 a shadowed len must not run in a clause (rc=$RC)" "$OUT"; fi

# A parameter holding a module: reading its member would CALL a function.
# At elevated the environment read itself is allowed (RG-02e), so only the
# clause guard can stop it; under the default standard sandbox the read is
# refused anyway and this arm could not tell the guard was there.
ENVOK=',"security":{"sandbox_level":"elevated"}'
run rg02e "$(cfg hard '{}' "$ENVOK")" 'fn probe(m) {
    let e = m.get_all
    print("ENV_TYPE=" + type(e))
    return 1
}
main {
    probe(env)
}'
if [ "$RC" -eq 0 ] && has "ENV_TYPE=dict"; then pass "RG-02e control: m.get_all on env reads the environment here"
else fail "RG-02e control: the module member read must work outside a clause (rc=$RC)" "$OUT"; fi

run rg02 "$(cfg hard '{"probe":{"requires":["m.get_all != null"]}}' "$ENVOK")" 'fn probe(m) {
    print("BODY_RAN")
    return 1
}
main {
    probe(env)
    print("AFTER")
}'
if [ "$RC" -eq 3 ] && ! has "BODY_RAN" && has "could not be evaluated"; then
    pass "RG-02 a clause cannot read a module member (it would call env.get_all)"
else fail "RG-02 module member read in a clause must be refused (rc=$RC)" "$OUT"; fi

run rg02c "$(cfg hard '{"probe":{"requires":["m.k == 1"]}}')" 'fn probe(m) {
    print("BODY_RAN")
    return 1
}
main {
    probe({"k": 1})
    print("AFTER")
}'
if [ "$RC" -eq 0 ] && has "BODY_RAN" && has "AFTER"; then pass "RG-02c control: a dict member read is allowed"
else fail "RG-02c control: dict member in a clause (rc=$RC)" "$OUT"; fi

# --- other call routes ----------------------------------------------------------
run as01 "$C_REQ" 'async fn root(n) {
    print("BODY_RAN")
    return n
}
main {
    let r = await root(-1)
    print("AFTER")
}'
if [ "$RC" -eq 3 ] && ! has "BODY_RAN" && ! has "AFTER"; then pass "AS-01 requires is checked in an async fn"
else fail "AS-01 async fn requires (rc=$RC)" "$OUT"; fi

run cb01 "$(cfg hard '{"cmp":{"requires":["type(a) == \"int\"","type(b) == \"int\""]}}')" 'fn cmp(a, b) {
    return a - b
}
main {
    let xs = [3, 1, 2]
    print("S=" + string(xs.sorted(cmp)))
    let ys = [3, "x", 2]
    print("T=" + string(ys.sorted(cmp)))
    print("AFTER")
}'
if [ "$RC" -eq 3 ] && has "S=[1, 2, 3]" && ! has "AFTER"; then pass "CB-01 requires is checked on a callback call"
else fail "CB-01 callback requires (rc=$RC)" "$OUT"; fi

# --- taint still flows through a contracted function ---------------------------
TAINT=',"taint_tracking":{"enabled":true,"level":"hard","sources":["env.get"],"sinks":["file.write"]},"security":{"sandbox_level":"elevated"},"capabilities":{"filesystem":{"mode":"write"},"env_vars":{"read":true}}'
P_TAINT='fn id(x) {
    return x
}
main {
    let v = id(env.get("HOME"))
    file.write("out.txt", v)
    print("WROTE")
}'
run ta01 "$(cfg hard '{}' "$TAINT")" "$P_TAINT"
TA01_RC=$RC
if [ "$RC" -eq 3 ] && ! has "WROTE"; then pass "TA-01 control: the taint sink blocks without a contract"
else fail "TA-01 control: taint must block (rc=$RC)" "$OUT"; fi
run ta02 "$(cfg hard '{"id":{"requires":["x != null"],"ensures":["result == x"]}}' "$TAINT")" "$P_TAINT"
if [ "$RC" -eq 3 ] && ! has "WROTE" && [ "$TA01_RC" -eq 3 ]; then
    pass "TA-02 a contracted function does not launder taint"
else fail "TA-02 taint through a contracted function (rc=$RC)" "$OUT"; fi

# --- audit mode records and continues ------------------------------------------
run au01 '{"version":"3.0","mode":"audit","contracts":{"level":"hard","functions":{"root":{"requires":["n >= 0"]}}}}' "${P_SQRT/ARG/-1}"
if [ "$RC" -eq 0 ] && has "BODY_RAN" && has "requires does not hold"; then
    pass "AU-01 audit mode reports the breach and runs on"
else fail "AU-01 audit mode (rc=$RC)" "$OUT"; fi

# --- the reload ratchet: contracts are fixed for the run ------------------------
if [ -x "$NAAB_GOV" ]; then
    # Relative names: under MSYS a native naab-gov cannot open a /tmp path.
    mkdir -p "$TMP/rt"
    printf '%s\n' "$(cfg hard '{"root":{"requires":["n >= 0"]}}')" > "$TMP/rt/old.json"
    printf '%s\n' "$(cfg hard '{"root":{}}')" > "$TMP/rt/new.json"
    OUT="$(cd "$TMP/rt" && "$NAAB_GOV" ratchet old.json new.json 2>&1)"; RC=$?
    if [ "$RC" -eq 1 ]; then pass "RT-01 a reload that removes requires is refused"
    else fail "RT-01 removing requires mid-run must be refused (rc=$RC)" "$OUT"; fi
    cp "$TMP/rt/old.json" "$TMP/rt/same.json"
    OUT="$(cd "$TMP/rt" && "$NAAB_GOV" ratchet old.json same.json 2>&1)"; RC=$?
    if [ "$RC" -eq 0 ]; then pass "RT-01c control: the same contract reloads"
    else fail "RT-01c control (rc=$RC)" "$OUT"; fi
else
    skip "RT-01 naab-gov not built"
fi

echo
echo "contract clauses: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ] && [ "$SKIP" -eq 0 ]
