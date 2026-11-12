#!/usr/bin/env bash
# test_gov_replay.sh -- naab-gov replay: recorded decisions against their
# recorded evidence (Bundle 14 PR 2).
#
# The subject is a report: per decision MATCH / DIVERGED / UNMEASURABLE, exit 1
# when anything diverged, else 2 when anything is unmeasurable, else 0. Two real
# runs against the agent stub make the evidence (run A: plain chain, adaptive
# baseline on, a reply executed through codegen; run B: a chain sealed with a
# key this suite makes up, baseline off). Every other arm is a COPY of run A's
# files with one thing changed, and asserts the count of the check that should
# notice -- not only the exit status, which a changed line also moves through
# the chain check.
#
# The copies are never re-sealed: no arm builds a chain that verifies over
# edited events. So every edited copy also fails the chain check, and an arm
# about a decision check reads that check's own count.
#
# Controls: RP-00 (the runs happened and hold every kind of event the arms
# edit), RP-01 (the untouched files MATCH on every check -- each DIVERGED arm
# below is that same file with one edit), RP-09c (a short junk line is not what
# RP-09 reports), RP-12 (the config beside the telemetry is one a loader
# refuses).
#
# Arm labels write "unmeasurable" in lower case: run-all-tests.sh lists every
# output line holding the upper-case word as a skip.
#
# The first mutant run (34) left three alive. Two led to RP-03i/03j and RP-14c;
# the third ("a file with nothing to check exits 0") was code no input reaches,
# and was removed.
#
# Mutants (each applied to the tree, the suite run, the tree restored; a
# mutant that leaves the suite green is a missing arm):
#   exit 0 when only UNMEASURABLE                    -> RP-02 08a 08c 09 11a 14a
#   exit 2 when DIVERGED and UNMEASURABLE both       -> RP-07 13 15
#   keyed-without-key reads DIVERGED                 -> RP-08a 08c
#   keyed-without-key reads MATCH                    -> RP-08a 08c
#   the key not taken from the environment           -> RP-08b
#   --hmac-key ignored                               -> RP-08d
#   admissibility: disposition not checked           -> RP-03a
#   admissibility: on_undetermined not applied       -> RP-03i
#   chain: a link break not reported                 -> RP-14c
#   chain: a hash mismatch not reported              -> RP-08e
#   chain: unreadable lines not reported             -> RP-15
#   chain: lines not read not reported               -> RP-09 10
#   admissibility: result vs threshold not checked   -> RP-03b 03c
#   admissibility: baseline state not checked        -> RP-03f
#   admissibility: snapshot not compared             -> RP-03d
#   admissibility: CDD_TURN not compared             -> RP-03e
#   admissibility: no evidence still MATCH           -> RP-03g
#   coherence: sum not compared                      -> RP-04a 04b
#   coherence: adjustments ignored                   -> RP-04d
#   coherence: a row without its lists reads DIVERGED-> RP-04c
#   coherence: non-analysed rows scored              -> RP-04e
#   transcript: reference not looked up              -> RP-05a 05b
#   transcript: no transcript reads MATCH            -> RP-02
#   transcript: an edited entry accepted by its stored hash -> RP-05a
#   provenance: origin not compared                  -> RP-06a
#   provenance: reply-hash rule dropped              -> RP-06c
#   provenance: a missing reply reads MATCH          -> RP-06b
#   line byte bound removed                          -> RP-09
#   nesting bound removed                            -> RP-10
#   report text not made plain                       -> RP-13
#   RunEnd absence reads MATCH                       -> RP-14a
#   a wrong-typed hash field aborts (A163)           -> RP-15
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
GOV="${NAAB_GOV:-$(dirname "$NAAB")/naab-gov}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_rp_XXXXXX")

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -14 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support "test_gov_replay.sh"
source "$SCRIPT_DIR/../helpers/stub_launch.sh"
# Unsigned configs: with a key in the trust store each run would be an
# integrity block and no run would reach an agent.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
cleanup() {
    stop_stub; teardown_isolated_trust
    if [ -n "${RP_KEEP:-}" ]; then echo "  kept: $W"; else rm -rf "$W"; fi
}
trap cleanup EXIT

echo "=== naab-gov replay: recorded decisions against their recorded evidence ==="
if [ ! -x "$NAAB" ] || [ ! -x "$GOV" ]; then
    echo "  naab-lang or naab-gov not built: UNMEASURABLE, not a pass"; exit 1
fi
export RP_FAKE_KEY="x"
# The sealing key of run B: made up here, per run, from parts. Not a real key.
SEAL="rp-$$-$(printf '%s%s' seal "$RANDOM")"

# $1 = dir, $2 = extra telemetry keys, $3 = extra context_drift keys, $4.. = replies
make_run() {
    local d="$1" tel_extra="$2" cdd_extra="$3"; shift 3
    mkdir -p "$d"
    python3 -c 'import json,sys; json.dump({"responses":[{"content":a,"output_tokens":20} for a in sys.argv[2:]]}, open(sys.argv[1],"w",encoding="utf-8"))' "$d/fixture.json" "$@"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; return 1; fi
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "codegen": {"enabled": true},
  "telemetry": {"enabled": true, "output_file": "tel.jsonl", "decision_snapshots": true,
     "tamper_evidence": {"enabled": true${tel_extra}},
     "transcript": {"enabled": true, "output_file": "tr.jsonl"}},
  "behavioral_sequences": {"enabled": true},
  "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1${cdd_extra}},
  "circuit_breaker": {"enabled": true, "output_admissibility": {"enabled": true,
     "threshold": 0.85, "action": "quarantine", "max_quarantine_streak": 0}},
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "system_prompt": "You are a worker.",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "RP_FAKE_KEY",
      "max_tokens": 50 } } }
EOF
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" 2>/dev/null || { stop_stub; return 1; }
    cat > "$d/p.naab" <<'EOF'
use agent
use codegen
main {
  let h = agent.create("worker")
  let r = agent.send(h, "write code")
  let o = codegen.run("python", r["content"])
  let a = agent.send(h, "explain")
  let b = agent.send(h, "explain again")
  let c = agent.send(h, "more")
  let d = agent.send(h, "explain again")
  let e = agent.send(h, "explain again")
  print("RUN_DONE")
}
EOF
    (cd "$d" && timeout 120 "$NAAB" p.naab </dev/null > run.out 2>&1)
    stop_stub
    grep -q RUN_DONE "$d/run.out"
}

SAME="The quick brown fox explains governance in detail today."
A="$W/a"; B="$W/b"
A_OK=1; B_OK=1
make_run "$A" "" "" "print(41+1)" "$SAME" "$SAME" "Another answer about worker tasks and plans." "$SAME" "$SAME" || A_OK=0
RP_SEAL_KEY="$SEAL" make_run "$B" ', "hmac_key_env": "RP_SEAL_KEY"' ', "adaptive_baseline_enabled": false' \
    "print(41+1)" "First answer about apples and pears." "Second answer on rivers and bridges." \
    "Third answer about worker tasks and plans." "Fourth answer on clocks and gears." "Fifth answer about maps." || B_OK=0

# kinds FILE -> "result=pass:N result=fail:N ..." counts the arms depend on
kinds() {
    python3 - "$1" <<'PY'
import json, sys, collections
c = collections.Counter()
for line in open(sys.argv[1], "rb"):
    try: e = json.loads(line)
    except ValueError: continue
    t = e.get("event_type")
    if t == "OUTPUT_ADMISSIBILITY_EVAL":
        c["oa_" + e.get("result", "")] += 1
        if "cdd_snapshot" in e: c["snap"] += 1
    elif t == "CDD_TURN" and e.get("analyzed") == "true":
        c["cdd"] += 1
        if e.get("penalties_detail"): c["cdd_pen"] += 1
    elif t == "TRANSCRIPT_REF": c["ref"] += 1
    elif t in ("CODEGEN_EXEC", "POLYGLOT_EXEC") and e.get("code_attribution") == "model_output_match": c["prov"] += 1
    elif t == "RunEnd": c["end"] += 1
print(" ".join("%s=%d" % kv for kv in sorted(c.items())))
PY
}
has() { [[ " $1 " == *" $2="* ]]; }

# rp DIR [args...]: runs replay --json on DIR/tel.jsonl from inside DIR (no
# path crosses to the binary but relative names). Sets RC and OUT.
rp() {
    local d="$1"; shift
    OUT=$(cd "$d" && timeout 120 "$GOV" replay tel.jsonl --json "$@" 2>&1 </dev/null); RC=$?
}
# cnt CHECK VERDICT -> the count in $OUT ("?" when OUT is not the report)
cnt() {
    python3 -c '
import json, sys
try: j = json.load(sys.stdin)
except ValueError: print("?"); sys.exit(0)
print(j.get("checks", {}).get(sys.argv[1], {}).get(sys.argv[2], "?"))' "$1" "$2" <<<"$OUT"
}
total() {   # total VERDICT over every check
    python3 -c '
import json, sys
try: j = json.load(sys.stdin)
except ValueError: print("?"); sys.exit(0)
print(sum(v.get(sys.argv[1], 0) for v in j.get("checks", {}).values()))' "$1" <<<"$OUT"
}

# edit SRC_DIR DST_DIR OP...: copies tel.jsonl and tr.jsonl, applying OPs. Lines
# not named by an op are copied byte for byte; nothing is re-hashed.
#   set:EVENT#k:field=value     set a string field of the k-th EVENT (0-based)
#   setwhere:EVENT:f=v:field=value   ... of the first EVENT whose f is v
#   del:EVENT#k:field           remove a field
#   drop:EVENT#k                remove the line
#   swap:EVENT#k                exchange the line with the one after it
#   dropwhere:EVENT:f=v         remove every EVENT line whose f is v
#   raw:TEXT                    append TEXT as a line
#   tr-set:k:field=value        set a field of the k-th transcript line
#   tr-drop:k                   remove the k-th transcript line
edit() {
    python3 - "$@" <<'PY'
import json, os, sys
src, dst, ops = sys.argv[1], sys.argv[2], sys.argv[3:]
os.makedirs(dst, exist_ok=True)
def load(name):
    p = os.path.join(src, name)
    return open(p, "rb").read().split(b"\n") if os.path.exists(p) else []
tel, tr = load("tel.jsonl"), load("tr.jsonl")
def index(event, pred):
    n = 0
    for i, raw in enumerate(tel):
        try: e = json.loads(raw)
        except ValueError: continue
        if e.get("event_type") == event and pred(e, n):
            return i, e
        if e.get("event_type") == event: n += 1
    sys.exit("edit: no such event: %s" % event)
def enc(e): return json.dumps(e).encode("utf-8")
for op in ops:
    kind, _, rest = op.partition(":")
    if kind in ("set", "del", "drop"):
        spec, _, arg = rest.partition(":")
        ev, _, k = spec.partition("#")
        i, e = index(ev, lambda e, n: n == int(k or 0))
        if kind == "drop": tel[i] = None
        elif kind == "del": e.pop(arg, None); tel[i] = enc(e)
        else:
            f, _, v = arg.partition("="); e[f] = v; tel[i] = enc(e)
    elif kind == "swap":
        ev, _, k = rest.partition("#")
        i, e = index(ev, lambda e, n: n == int(k or 0))
        tel[i], tel[i + 1] = tel[i + 1], tel[i]
    elif kind == "setwhere":
        ev, cond, arg = rest.split(":", 2)
        cf, _, cv = cond.partition("=")
        i, e = index(ev, lambda e, n: e.get(cf) == cv)
        f, _, v = arg.partition("="); e[f] = v; tel[i] = enc(e)
    elif kind == "delwhere":
        ev, cond, arg = rest.split(":", 2)
        cf, _, cv = cond.partition("=")
        i, e = index(ev, lambda e, n: e.get(cf) == cv)
        e.pop(arg, None); tel[i] = enc(e)
    elif kind == "dropwhere":
        ev, cond = rest.split(":", 1)
        cf, _, cv = cond.partition("=")
        hit = 0
        for i, raw in enumerate(tel):
            if raw is None: continue
            try: e = json.loads(raw)
            except ValueError: continue
            if e.get("event_type") == ev and e.get(cf) == cv: tel[i] = None; hit += 1
        if not hit: sys.exit("edit: nothing to drop: %s" % op)
    elif kind == "raw":
        while tel and tel[-1] == b"": tel.pop()
        tel.append(rest.encode("utf-8")); tel.append(b"")
    elif kind == "tr-set":
        k, _, arg = rest.partition(":")
        e = json.loads(tr[int(k)]); f, _, v = arg.partition("="); e[f] = v
        tr[int(k)] = json.dumps(e, separators=(",", ":")).encode("utf-8")
    elif kind == "tr-drop":
        tr[int(rest)] = None
    else:
        sys.exit("edit: unknown op " + op)
open(os.path.join(dst, "tel.jsonl"), "wb").write(b"\n".join(x for x in tel if x is not None))
if tr: open(os.path.join(dst, "tr.jsonl"), "wb").write(b"\n".join(x for x in tr if x is not None))
PY
}

# expect ARM LABEL RC CHECK VERDICT OP N: the exit status, and one count.
expect() {
    local arm="$1" label="$2" want_rc="$3" check="$4" verdict="$5" op="$6" n="$7"
    local got; got=$(cnt "$check" "$verdict")
    local ok=1
    [ "$RC" = "$want_rc" ] || ok=0
    case "$op" in
        eq) [ "$got" = "$n" ] || ok=0 ;;
        ge) { [ "$got" != "?" ] && [ "$got" -ge "$n" ]; } || ok=0 ;;
    esac
    if [ $ok = 1 ]; then pass "$arm" "$label"
    else fail "$arm" "$label (rc=$RC want $want_rc; $check.$verdict=$got want $op $n)" "$OUT"; fi
}

KA=""; KB=""
[ $A_OK = 1 ] && KA=$(kinds "$A/tel.jsonl")
[ $B_OK = 1 ] && KB=$(kinds "$B/tel.jsonl")
echo "  run A: $KA"
echo "  run B: $KB"

# --- RP-00: the instrument ------------------------------------------------
READY=1
for k in oa_undetermined oa_fail snap cdd cdd_pen ref prov end; do has "$KA" "$k" || READY=0; done
has "$KB" oa_pass || READY=0
if [ $A_OK = 1 ] && [ $B_OK = 1 ] && [ $READY = 1 ]; then
    pass "RP-00" "both runs finished and hold every kind of event the arms edit"
else
    fail "RP-00" "the fixture runs are not what the arms need -- every verdict below would be void" \
         "A_OK=$A_OK B_OK=$B_OK A: $KA B: $KB $(tail -5 "$A/run.out" 2>/dev/null)"
    echo "  $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi
REFS=$(sed -n 's/.* ref=\([0-9]*\).*/\1/p' <<<" $KA ")

# --- RP-01: the untouched run matches on every check -----------------------
rp "$A" --transcript tr.jsonl
ok=1; [ "$RC" = 0 ] || ok=0
for c in chain admissibility coherence transcript provenance; do
    m=$(cnt $c match); [ "$m" != "?" ] && [ "$m" -ge 1 ] || ok=0
done
[ "$(total diverged)" = 0 ] && [ "$(total unmeasurable)" = 0 ] || ok=0
if [ $ok = 1 ]; then pass "RP-01" "an untouched run with its transcript: every check MATCH, exit 0"
else fail "RP-01" "an untouched run does not MATCH (rc=$RC)" "$OUT"; fi

VOUT=$(cd "$A" && "$NAAB" --verify-telemetry-chain tel.jsonl 2>&1); VRC=$?
if [ $VRC = 0 ] && [[ "$VOUT" == *"Chain verified"* ]]; then
    pass "RP-01b" "--verify-telemetry-chain agrees on the same file, in its old words"
else fail "RP-01b" "the chain verifier's answer changed (rc=$VRC)" "$VOUT"; fi

TOUT=$(cd "$A" && "$GOV" replay tel.jsonl --transcript tr.jsonl 2>&1); TRC=$?
if [ $TRC = 0 ] && [[ "$TOUT" == *"Result: MATCH"* ]] && [[ "$TOUT" == *"recorded decisions against their recorded evidence"* ]] && [[ "$TOUT" == *"Not checked:"* ]]; then
    pass "RP-01c" "the text report says what it is, what it did not check, and MATCH"
else fail "RP-01c" "the text report (rc=$TRC)" "$TOUT"; fi

# --- RP-02: evidence not given is UNMEASURABLE, exit 2 ----------------------
rp "$A"
if [ "$RC" = 2 ] && [ "$(cnt transcript unmeasurable)" = "$REFS" ] && [ "$(cnt transcript match)" = 0 ] && [ "$(total diverged)" = 0 ]; then
    pass "RP-02" "no transcript given: every reference unmeasurable, exit 2, nothing diverged"
else fail "RP-02" "no transcript given (rc=$RC, refs=$REFS)" "$OUT"; fi

# --- RP-03: admissibility ---------------------------------------------------
edit "$A" "$W/3a" "setwhere:OUTPUT_ADMISSIBILITY_EVAL:result=fail:disposition=admitted"
rp "$W/3a" --transcript tr.jsonl
expect RP-03a "a failed decision recorded as admitted" 1 admissibility diverged eq 1

edit "$A" "$W/3b" "setwhere:OUTPUT_ADMISSIBILITY_EVAL:result=fail:result=pass" \
                   "setwhere:OUTPUT_ADMISSIBILITY_EVAL:result=pass:baseline_state=complete" \
                   "setwhere:OUTPUT_ADMISSIBILITY_EVAL:result=pass:disposition=admitted"
rp "$W/3b" --transcript tr.jsonl
expect RP-03b "a pass recorded below the threshold" 1 admissibility diverged eq 1

# coherence, snapshot and the turn's CDD_TURN all moved above the threshold
# together, so only the result-against-threshold rule can notice
T3=$(python3 - "$A/tel.jsonl" <<'PY'
import json, sys
for line in open(sys.argv[1], "rb"):
    e = json.loads(line)
    if e.get("event_type") == "OUTPUT_ADMISSIBILITY_EVAL" and e.get("result") == "fail":
        print(e["turn"]); break
PY
)
edit "$A" "$W/3c" "setwhere:OUTPUT_ADMISSIBILITY_EVAL:result=fail:coherence=0.9500" \
                   "delwhere:OUTPUT_ADMISSIBILITY_EVAL:coherence=0.9500:cdd_snapshot" \
                   "setwhere:CDD_TURN:turn=$T3:coherence=0.9500"
rp "$W/3c" --transcript tr.jsonl
expect RP-03c "a fail recorded above the threshold" 1 admissibility diverged eq 1

# a passing coherence changed to another passing one: only the snapshot differs
# (the turn's CDD_TURN is moved with it)
edit "$A" "$W/3d" "set:OUTPUT_ADMISSIBILITY_EVAL#0:coherence=0.9900" "set:CDD_TURN#0:coherence=0.9900"
rp "$W/3d" --transcript tr.jsonl
expect RP-03d "a coherence its own snapshot does not record" 1 admissibility diverged eq 1

# ... and with the snapshot removed, only the CDD_TURN differs
edit "$A" "$W/3e" "set:OUTPUT_ADMISSIBILITY_EVAL#0:coherence=0.9900" "del:OUTPUT_ADMISSIBILITY_EVAL#0:cdd_snapshot"
rp "$W/3e" --transcript tr.jsonl
expect RP-03e "a coherence the turn's CDD_TURN does not record" 1 admissibility diverged eq 1

edit "$A" "$W/3f" "set:OUTPUT_ADMISSIBILITY_EVAL#0:result=pass"
rp "$W/3f" --transcript tr.jsonl
expect RP-03f "a pass recorded while the baseline is calibrating" 1 admissibility diverged eq 1

# no snapshot and no CDD_TURN for the turn: the decision's evidence is absent
T0=$(python3 - "$A/tel.jsonl" <<'PY'
import json, sys
for line in open(sys.argv[1], "rb"):
    e = json.loads(line)
    if e.get("event_type") == "OUTPUT_ADMISSIBILITY_EVAL": print(e["turn"]); break
PY
)
edit "$A" "$W/3g" "del:OUTPUT_ADMISSIBILITY_EVAL#0:cdd_snapshot" "dropwhere:CDD_TURN:turn=$T0"
rp "$W/3g" --transcript tr.jsonl
if [ "$(cnt admissibility unmeasurable)" = 1 ] && [ "$(cnt admissibility diverged)" = 0 ]; then
    pass "RP-03g" "a decision with no snapshot and no CDD_TURN is unmeasurable, not MATCH"
else fail "RP-03g" "a decision without evidence" "$OUT"; fi

# an undetermined decision's disposition follows on_undetermined
edit "$A" "$W/3i" "set:OUTPUT_ADMISSIBILITY_EVAL#0:on_undetermined=quarantine"
rp "$W/3i" --transcript tr.jsonl
expect RP-03i "undetermined under a quarantine policy, recorded as admitted" 1 admissibility diverged eq 1
edit "$A" "$W/3j" "set:OUTPUT_ADMISSIBILITY_EVAL#0:on_undetermined=block" "set:OUTPUT_ADMISSIBILITY_EVAL#0:disposition=quarantined"
rp "$W/3j" --transcript tr.jsonl
expect RP-03j "undetermined under a block policy, recorded as quarantined" 1 admissibility diverged eq 1

rp "$B" --transcript tr.jsonl --hmac-key "$SEAL"
PASSES=$(sed -n 's/.* oa_pass=\([0-9]*\).*/\1/p' <<<" $KB ")
if [ "$RC" = 0 ] && [ "$(cnt admissibility match)" -ge "$PASSES" ] 2>/dev/null; then
    pass "RP-03h" "determined passes (baseline off) MATCH"
else fail "RP-03h" "run B's passes (rc=$RC)" "$OUT"; fi

# --- RP-04: coherence -------------------------------------------------------
PEN_TURN=$(python3 - "$A/tel.jsonl" <<'PY'
import json, sys
for line in open(sys.argv[1], "rb"):
    e = json.loads(line)
    if e.get("event_type") == "CDD_TURN" and e.get("penalties_detail"): print(e["turn"]); break
PY
)
edit "$A" "$W/4a" "setwhere:CDD_TURN:turn=$PEN_TURN:penalties_detail=circular=0.0100"
rp "$W/4a" --transcript tr.jsonl
expect RP-04a "a penalty amount that does not add up" 1 coherence diverged eq 1

edit "$A" "$W/4b" "setwhere:CDD_TURN:turn=$PEN_TURN:coherence=0.9900" \
                   "setwhere:OUTPUT_ADMISSIBILITY_EVAL:turn=$PEN_TURN:coherence=0.9900"
rp "$W/4b" --transcript tr.jsonl
expect RP-04b "a coherence the listed penalties do not give" 1 coherence diverged ge 1

edit "$A" "$W/4c" "delwhere:CDD_TURN:turn=$PEN_TURN:coherence_adjustments"
rp "$W/4c" --transcript tr.jsonl
if [ "$(cnt coherence unmeasurable)" = 1 ] && [ "$(cnt coherence diverged)" = 0 ]; then
    pass "RP-04c" "a row that does not record its adjustments is unmeasurable, not DIVERGED"
else fail "RP-04c" "a row without coherence_adjustments" "$OUT"; fi

# an adjustment is part of the sum: a healing credit nothing else accounts for
edit "$A" "$W/4d" "setwhere:CDD_TURN:turn=$PEN_TURN:coherence_adjustments=natural_healing=+0.0500"
rp "$W/4d" --transcript tr.jsonl
expect RP-04d "an adjustment that does not add up" 1 coherence diverged eq 1

# a row marked not analysed re-shows old state and is not scored: marking the
# edited row of 4a that way must remove its divergence (and unsettle the next)
edit "$W/4a" "$W/4e" "setwhere:CDD_TURN:turn=$PEN_TURN:analyzed=false"
rp "$W/4e" --transcript tr.jsonl
M_A=$(cd "$A" && "$GOV" replay tel.jsonl --json | python3 -c 'import json,sys; print(json.load(sys.stdin)["checks"]["coherence"]["match"])')
got=$(( $(cnt coherence match) + $(cnt coherence diverged) + $(cnt coherence unmeasurable) ))
if [ "$got" = $((M_A - 1)) ]; then pass "RP-04e" "a row not analysed is not scored"
else fail "RP-04e" "rows scored: $got, run A scores $M_A" "$OUT"; fi

# --- RP-05: transcript ------------------------------------------------------
edit "$A" "$W/5a" "tr-set:1:agent=someone-else"
rp "$W/5a" --transcript tr.jsonl
if [ "$RC" = 1 ] && [ "$(cnt transcript diverged)" = 2 ] && [ "$(cnt chain diverged)" = 0 ]; then
    pass "RP-05a" "an edited transcript entry: it fails its own hash and its reference finds nothing"
else fail "RP-05a" "an edited transcript entry (rc=$RC)" "$OUT"; fi

edit "$A" "$W/5b" "tr-drop:1"
rp "$W/5b" --transcript tr.jsonl
if [ "$RC" = 1 ] && [ "$(cnt transcript diverged)" = 1 ] && [ "$(cnt transcript match)" = $((REFS - 1)) ]; then
    pass "RP-05b" "a removed transcript entry: its reference DIVERGED, the others MATCH"
else fail "RP-05b" "a removed transcript entry (rc=$RC)" "$OUT"; fi

# --- RP-06: provenance ------------------------------------------------------
ZERO=$(printf '%064d' 0)
edit "$A" "$W/6a" "set:CODEGEN_EXEC#0:origin_request_sha256=$ZERO"
rp "$W/6a" --transcript tr.jsonl
expect RP-06a "an origin no AGENT_RESPONSE recorded under that request" 1 provenance diverged eq 1

edit "$A" "$W/6b" "drop:AGENT_RESPONSE#0"
rp "$W/6b" --transcript tr.jsonl
if [ "$(cnt provenance unmeasurable)" -ge 1 ] 2>/dev/null && [ "$(cnt provenance diverged)" = 0 ] && [ "$(cnt provenance match)" = 0 ]; then
    pass "RP-06b" "the reply's AGENT_RESPONSE missing: unmeasurable, not MATCH"
else fail "RP-06b" "a missing AGENT_RESPONSE" "$OUT"; fi

edit "$A" "$W/6c" "set:CODEGEN_EXEC#0:code_sha256=$ZERO"
rp "$W/6c" --transcript tr.jsonl
expect RP-06c "code recorded as the whole reply under another hash" 1 provenance diverged eq 1

# --- RP-07: DIVERGED outranks UNMEASURABLE ----------------------------------
rp "$W/3a"
if [ "$RC" = 1 ] && [ "$(total unmeasurable)" -ge 1 ] && [ "$(total diverged)" -ge 1 ]; then
    pass "RP-07" "something DIVERGED and something unmeasurable: exit 1"
else fail "RP-07" "exit precedence (rc=$RC)" "$OUT"; fi

# --- RP-08: a chain sealed with a key ---------------------------------------
rp "$B" --transcript tr.jsonl
if [ "$RC" = 2 ] && [ "$(cnt chain unmeasurable)" = 1 ] && [ "$(cnt chain diverged)" = 0 ] && [ "$(cnt chain match)" = 0 ]; then
    pass "RP-08a" "a keyed chain without its key: unmeasurable, exit 2"
else fail "RP-08a" "a keyed chain without its key (rc=$RC)" "$OUT"; fi

OUT=$(cd "$B" && NAAB_TELEMETRY_HMAC_KEY="$SEAL" timeout 120 "$GOV" replay tel.jsonl --json --transcript tr.jsonl 2>&1 </dev/null); RC=$?
if [ "$RC" = 0 ] && [ "$(cnt chain match)" = 1 ]; then
    pass "RP-08b" "with the key where --verify-telemetry-chain reads it: MATCH, exit 0"
else fail "RP-08b" "the key from the environment (rc=$RC)" "$OUT"; fi
VOUT=$(cd "$B" && NAAB_TELEMETRY_HMAC_KEY="$SEAL" "$NAAB" --verify-telemetry-chain tel.jsonl 2>&1); VRC=$?
if [ $VRC = 0 ]; then pass "RP-08b2" "--verify-telemetry-chain reads the same key the same way"
else fail "RP-08b2" "the chain verifier with the key (rc=$VRC)" "$VOUT"; fi

rp "$B" --transcript tr.jsonl --hmac-key "not-$SEAL"
if [ "$RC" = 2 ] && [ "$(cnt chain unmeasurable)" = 1 ] && [ "$(cnt chain match)" = 0 ]; then
    pass "RP-08c" "with another key: unmeasurable, exit 2"
else fail "RP-08c" "a wrong key (rc=$RC)" "$OUT"; fi

rp "$B" --transcript tr.jsonl --hmac-key "$SEAL"
if [ "$RC" = 0 ] && [ "$(cnt chain match)" = 1 ]; then pass "RP-08d" "--hmac-key gives the key too"
else fail "RP-08d" "--hmac-key (rc=$RC)" "$OUT"; fi

# one edited event in a keyed chain, verified with the key: DIVERGED, not the
# keyed-without-key reading
edit "$B" "$W/8e" "set:CODEGEN_EXEC#0:language=other"
rp "$W/8e" --transcript tr.jsonl --hmac-key "$SEAL"
expect RP-08e "one edited event in a keyed chain, with the key" 1 chain diverged eq 1

# --- RP-09 / RP-10: bounds on what is read ----------------------------------
mkdir -p "$W/9"; cp "$A/tel.jsonl" "$A/tr.jsonl" "$W/9/"
python3 -c 'import sys; open(sys.argv[1],"ab").write(b"\n{\"event_type\":\"X\",\"pad\":\"" + b"a"*(3<<20) + b"\"}\n")' "$W/9/tel.jsonl"
rp "$W/9" --transcript tr.jsonl
NR=$(python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("lines_not_read"))
except ValueError: print("?")' <<<"$OUT")
if [ "$RC" = 2 ] && [ "$NR" = 1 ] && [ "$(total diverged)" = 0 ]; then
    pass "RP-09" "a 3 MiB line is not read: counted, unmeasurable, exit 2"
else fail "RP-09" "an oversized line (rc=$RC, not read=$NR)" "$(head -c 600 <<<"$OUT")"; fi

edit "$A" "$W/9c" 'raw:{"event_type":"X","pad":"aaaa"}'
rp "$W/9c" --transcript tr.jsonl
if [ "$RC" = 0 ]; then pass "RP-09c" "control: a short unchained line of the same shape changes nothing"
else fail "RP-09c" "the control line (rc=$RC)" "$OUT"; fi

mkdir -p "$W/10"; cp "$A/tel.jsonl" "$A/tr.jsonl" "$W/10/"
python3 -c 'import sys; n=100000; open(sys.argv[1],"ab").write(b"\n{\"hash\":\"a\",\"prev_hash\":\"b\",\"x\":" + b"["*n + b"]"*n + b"}\n")' "$W/10/tel.jsonl"
rp "$W/10" --transcript tr.jsonl
NR=$(python3 -c 'import json,sys
try: print(json.load(sys.stdin).get("lines_not_read"))
except ValueError: print("?")' <<<"$OUT")
if [ "$RC" = 2 ] && [ "$NR" = 1 ]; then
    pass "RP-10" "a line nested 100000 deep is not parsed: counted, exit 2, no crash"
else fail "RP-10" "a deeply nested line (rc=$RC, not read=$NR)" "$(head -c 600 <<<"$OUT")"; fi

# --- RP-11: nothing to check, and usage -------------------------------------
mkdir -p "$W/11"; : > "$W/11/tel.jsonl"
rp "$W/11"
if [ "$RC" = 2 ]; then pass "RP-11a" "an empty file: exit 2, never 0"
else fail "RP-11a" "an empty file (rc=$RC)" "$OUT"; fi

# the same events with their hash fields removed: an unsealed file
mkdir -p "$W/11b"; cp "$A/tr.jsonl" "$W/11b/"
python3 - "$A/tel.jsonl" "$W/11b/tel.jsonl" <<'PY'
import json, sys
with open(sys.argv[2], "wb") as out:
    for line in open(sys.argv[1], "rb"):
        e = json.loads(line); e.pop("hash", None); e.pop("prev_hash", None)
        out.write(json.dumps(e).encode("utf-8") + b"\n")
PY
rp "$W/11b" --transcript tr.jsonl
if [ "$RC" = 2 ] && [ "$(cnt chain unmeasurable)" = 1 ] && [ "$(cnt chain match)" = 0 ]; then
    pass "RP-11b" "telemetry with no chain: the chain is unmeasurable, exit 2"
else fail "RP-11b" "unsealed telemetry (rc=$RC)" "$OUT"; fi

(cd "$W/11" && "$GOV" replay no-such-file.jsonl >/dev/null 2>&1); r1=$?
(cd "$W/11" && "$GOV" replay tel.jsonl --frobnicate >/dev/null 2>&1); r2=$?
(cd "$W/11" && "$GOV" replay >/dev/null 2>&1); r3=$?
if [ $r1 = 4 ] && [ $r2 = 4 ] && [ $r3 = 4 ]; then pass "RP-11c" "a missing file, an unknown option, no file: exit 4"
else fail "RP-11c" "usage errors: $r1 $r2 $r3 (want 4 4 4)"; fi

# --- RP-12: it loads no govern.json -----------------------------------------
mkdir -p "$W/12"; cp "$A/tel.jsonl" "$A/tr.jsonl" "$W/12/"
printf '{ "mode": "enforce", this is not json\n' > "$W/12/govern.json"
printf 'main {\n  print("x")\n}\n' > "$W/12/p.naab"
(cd "$W/12" && "$GOV" lint p.naab >/dev/null 2>&1); LRC=$?
rp "$W/12" --transcript tr.jsonl
if [ $LRC = 0 ]; then unm "RP-12" "the control did not hold: lint accepted the broken govern.json"
elif [ "$RC" = 0 ]; then
    pass "RP-12" "beside a govern.json that does not load (lint: exit $LRC), the report is unchanged"
else fail "RP-12" "the report beside a broken govern.json (rc=$RC)" "$OUT"; fi

# --- RP-13: text from the file reaches the report plain ---------------------
python3 - "$A" "$W/13" <<'PY'
import json, os, sys
src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
data = open(os.path.join(src, "tel.jsonl"), "rb").read().rstrip(b"\n")
ev = {"event_type": "OUTPUT_ADMISSIBILITY_EVAL", "run_id": "r\u001b[31m\n  Result: MATCH",
      "handle_id": "9\u001b[2J\nResult: MATCH", "turn": "1", "result": "fail",
      "coherence": "0.9000", "threshold": "0.5000", "action": "quarantine",
      "disposition": "quarantined", "pulse_override": "false"}
open(os.path.join(dst, "tel.jsonl"), "wb").write(data + b"\n" + json.dumps(ev).encode() + b"\n")
PY
TOUT=$(cd "$W/13" && "$GOV" replay tel.jsonl 2>&1); TRC=$?
NRES=$(grep -c 'Result: ' <<<"$TOUT")
if [ $TRC = 1 ] && [ "$NRES" = 1 ] && [[ "$TOUT" != *$'\033'* ]] && [[ "$TOUT" == *"Result: DIVERGED"* ]]; then
    pass "RP-13" "escape bytes and line breaks in a field do not reach the report"
else fail "RP-13" "untrusted text in the report (rc=$TRC, Result lines=$NRES)" "$(sed -n l <<<"$TOUT" | tail -8)"; fi

# --- RP-14: a run with its end or its middle missing ------------------------
edit "$A" "$W/14a" "drop:RunEnd#0"
rp "$W/14a" --transcript tr.jsonl
if [ "$RC" = 2 ] && [ "$(cnt chain unmeasurable)" = 1 ] && [ "$(cnt chain match)" = 0 ]; then
    pass "RP-14a" "a run with no RunEnd: unmeasurable, exit 2"
else fail "RP-14a" "a run with no RunEnd (rc=$RC)" "$OUT"; fi

edit "$A" "$W/14b" "drop:PROMPT_SCAN#0"
rp "$W/14b" --transcript tr.jsonl
expect RP-14b "an event removed from the middle of the chain" 1 chain diverged eq 1

# two events exchanged: every hash still verifies and the count is right, so
# only the links can notice
edit "$A" "$W/14c" "swap:PROMPT_SCAN#0"
rp "$W/14c" --transcript tr.jsonl
expect RP-14c "two events exchanged in place" 1 chain diverged eq 1

# --- RP-15: a hash field of the wrong type (A163) ----------------------------
mkdir -p "$W/15"
printf '{"hash":1,"prev_hash":"x","event_type":"RunStart"}\n' > "$W/15/tel.jsonl"
rp "$W/15"
VOUT=$(cd "$W/15" && "$NAAB" --verify-telemetry-chain tel.jsonl 2>&1); VRC=$?
if [ "$RC" = 1 ] && [ "$(cnt chain diverged)" = 1 ] && [ $VRC -lt 126 ] && [[ "$VOUT" == *CORRUPT* ]]; then
    pass "RP-15" "a hash field that is not a string: DIVERGED here, CORRUPT in the verifier, no abort"
else fail "RP-15" "a wrong-typed hash field (replay rc=$RC, verifier rc=$VRC)" "$OUT $VOUT"; fi

echo ""
echo "  $PASS passed, $FAIL failed, $UNM unmeasurable"
[ $FAIL -eq 0 ] && [ $UNM -eq 0 ]
