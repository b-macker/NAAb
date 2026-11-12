#!/usr/bin/env bash
# test_chain_concurrent.sh -- the telemetry chain under concurrent writers
# (A164), and --verify-telemetry-chain on lines it will not read (A163).
#
# A164: every writer seeds an event's prev_hash from the file's tail. The file
# lock was released before the sealed line was flushed, so the next writer --
# an agent.batch pool thread, or another process sharing the file -- read a
# tail without it, and two events took the same predecessor. An untouched batch
# run then failed --verify-telemetry-chain (observed 7 of 7 runs before the
# fix), which reads exactly like tampering.
#
# Observed at the files: each arm runs real programs and hands the telemetry
# file to the verifier, and CC-02 reads the links itself (no two events may
# share a predecessor), so a verifier that stopped reporting breaks would not
# turn the suite green.
#
# Controls: CC-00 (the same agents sending one at a time verify -- so a red
# batch arm is the batch, not the fixture), CC-00v (the verifier reports a
# break on a file with a line removed, so its "verified" means something),
# CV-00 (an untouched file and an empty one still exit 0).
#
# Mutants (each applied, built, the suite run with CC_ROUNDS=8, the tree
# restored):
#   the deleters unlock before flushing AND the agent      -> CC-01 02 03
#     writer releases the chain lock before its write
#     (the pre-fix shape)
#   the verifier unbounded (0, 0)                           -> CV-01 02
#   a line not read is not counted                          -> CV-01 02
#   unreadable-only file exits 0                            -> CV-03
# Three single mutants SURVIVE on POSIX, and are expected to: the fix has two
# layers and either one closes the window here, because flock already makes
# writers of one file take turns.
#   only the deleters unlock before flushing   (the line is flushed under the
#                                               chain lock anyway)
#   only the chain lock released before the write   (the deleter flushes
#                                               before it unlocks)
#   only the forward made after the chain lock (the file lock is still held)
# The chain-lock layer is what orders threads where there is no flock
# (Windows); no arm runs there (the agent stub does not), so that layer is
# traced, not tested.
#
# CC-04 (four processes, one file) passed on the unfixed build too: it is a
# guard, not a regression arm.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_cc_XXXXXX")
ROUNDS="${CC_ROUNDS:-5}"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

echo "=== the telemetry chain under concurrent writers ==="
if [ ! -x "$NAAB" ]; then echo "  naab-lang not built: UNMEASURABLE, not a pass"; exit 1; fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
SINK_PIDS="$W/sinks.pids"; : > "$SINK_PIDS"
STUB_LOADED=0
cleanup() {
    [ $STUB_LOADED = 1 ] && stop_stub
    while read -r p; do kill "$p" 2>/dev/null; done < "$SINK_PIDS" 2>/dev/null
    teardown_isolated_trust
    if [ -n "${CC_KEEP:-}" ]; then echo "  kept: $W"; else rm -rf "$W"; fi
}
trap cleanup EXIT

# verify DIR FILE -> sets VRC, VOUT (run from DIR: only a relative name crosses)
verify() { VOUT=$(cd "$1" && "$NAAB" --verify-telemetry-chain "$2" 2>&1); VRC=$?; }

# forks DIR FILE -> "events=N shared=K": K events whose prev_hash another
# event also has (0 in a chain; the reader is given bytes on stdin)
forks() {
    python3 -c '
import json, sys, collections
prev = collections.Counter(); n = 0
for line in sys.stdin.buffer:
    try: e = json.loads(line)
    except ValueError: continue
    if isinstance(e, dict) and "prev_hash" in e and "hash" in e:
        n += 1; prev[e["prev_hash"]] += 1
print("events=%d shared=%d" % (n, sum(c - 1 for c in prev.values() if c > 1)))' < "$1/$2"
}

# ---------------------------------------------------------------- A163 arms
# (no agent stub: they run on every platform)
V="$W/v"; mkdir -p "$V"
cat > "$V/govern.json" <<'EOF'
{ "version": "5.0", "mode": "enforce",
  "telemetry": {"enabled": true, "output_file": "tel.jsonl", "tamper_evidence": true} }
EOF
printf 'main {\n  print("ok")\n}\n' > "$V/p.naab"
(cd "$V" && timeout 60 "$NAAB" p.naab >/dev/null 2>&1 </dev/null)
: > "$V/empty.jsonl"
verify "$V" tel.jsonl; r1=$VRC; o1=$VOUT
verify "$V" empty.jsonl; r2=$VRC
if [ -s "$V/tel.jsonl" ] && [ $r1 = 0 ] && [[ "$o1" == *"Chain verified"* ]] && [ $r2 = 0 ]; then
    pass "CV-00" "control: an untouched file verifies, and an empty one still exits 0"
else
    fail "CV-00" "the verifier on untouched input (rc=$r1, empty rc=$r2)" "$o1"
fi

python3 -c 'import sys
n = 200000
sys.stdout.buffer.write(sys.stdin.buffer.read() + b"{\"hash\":\"a\",\"prev_hash\":\"b\",\"x\":" + b"["*n + b"]"*n + b"}\n")' < "$V/tel.jsonl" > "$V/deep.jsonl"
verify "$V" deep.jsonl
if [ $VRC = 1 ] && [[ "$VOUT" == *"NOT READ"* ]]; then
    pass "CV-01" "a line nested 200000 deep: reported NOT READ, exit 1, no crash"
else fail "CV-01" "a deeply nested line (rc=$VRC; 139 is the old segmentation fault)" "$(tail -3 <<<"$VOUT")"; fi

python3 -c 'import sys
sys.stdout.buffer.write(sys.stdin.buffer.read() + b"{\"event_type\":\"X\",\"pad\":\"" + b"a"*(3<<20) + b"\"}\n")' < "$V/tel.jsonl" > "$V/long.jsonl"
verify "$V" long.jsonl
if [ $VRC = 1 ] && [[ "$VOUT" == *"NOT READ"* ]]; then
    pass "CV-02" "a 3 MiB line: reported NOT READ, exit 1"
else fail "CV-02" "an oversized line (rc=$VRC)" "$(tail -3 <<<"$VOUT" | cut -c1-200)"; fi

printf 'not json\n{"also": not json\n' > "$V/junk.jsonl"
verify "$V" junk.jsonl
if [ $VRC = 1 ] && [[ "$VOUT" == *CORRUPT* ]]; then
    pass "CV-03" "a file of only unreadable lines: exit 1, never 0"
else fail "CV-03" "an unreadable-only file (rc=$VRC)" "$VOUT"; fi

# ------------------------------------------- CC-04: two processes, one file
# No stub needed: each process writes its check results and anchors at exit.
X="$W/x"; mkdir -p "$X"
cp "$V/govern.json" "$X/govern.json"
cat > "$X/p.naab" <<'EOF'
main {
  let a = <<python
1 + 1
>>
  let b = <<python
2 + 2
>>
  print("ok")
}
EOF
bad=0; ran=0; detail=""
for i in $(seq 1 "$ROUNDS"); do
    rm -f "$X/tel.jsonl"
    for k in 1 2 3 4; do (cd "$X" && timeout 120 "$NAAB" p.naab >/dev/null 2>&1 </dev/null) & done
    wait
    [ -s "$X/tel.jsonl" ] || continue
    ran=$((ran+1))
    verify "$X" tel.jsonl
    f=$(forks "$X" tel.jsonl)
    if [ $VRC != 0 ] || [[ "$f" != *"shared=0" ]]; then bad=$((bad+1)); detail="round $i: rc=$VRC $f $(grep -m2 BREAK <<<"$VOUT")"; fi
done
if [ $ran = 0 ]; then unm "CC-04" "no round wrote telemetry"
elif [ $bad = 0 ]; then pass "CC-04" "four processes sharing one telemetry file, $ran rounds: every chain verifies"
else fail "CC-04" "$bad of $ran rounds: processes sharing a file broke its chain" "$detail"; fi

# ------------------------------------------------- agent arms (stub-backed)
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
if ! ( skip_if_no_stub_support "test_chain_concurrent.sh (agent arms)" >/dev/null 2>&1; exit 7 ); [ $? = 7 ]; then
    echo "  SKIP: the agent arms (CC-00..CC-03) need the agent stub, which this platform does not run"
    echo ""; echo "  $PASS passed, $FAIL failed, $UNM unmeasurable"
    [ $FAIL -eq 0 ] && [ $UNM -eq 0 ]; exit $?
fi
source "$SCRIPT_DIR/../helpers/stub_launch.sh"; STUB_LOADED=1
export CC_FAKE_KEY="x"

# agent_run DIR EXTRA_TELEMETRY BODY -> runs; sets ARC
agent_run() {
    local d="$1" tel_extra="$2" body="$3"
    mkdir -p "$d"; rm -f "$d/tel.jsonl"
    echo '{"responses": []}' > "$d/fixture.json"
    if ! start_stub "$d/fixture.json" "$d"; then stop_stub; ARC=-1; return; fi
    cat > "$d/govern.json" <<EOF
{ "version": "5.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },
  "telemetry": {"enabled": true, "output_file": "tel.jsonl", "tamper_evidence": true${tel_extra}},
  "behavioral_sequences": {"enabled": true},
  "context_drift": {"enabled": true, "level": "advisory", "check_interval_turns": 1},
  "agents": { "worker": { "provider": "gemini", "model": "stub-model",
      "api_base": "http://127.0.0.1:$STUB_PORT", "api_key_env": "CC_FAKE_KEY",
      "max_tokens": 50 } } }
EOF
    python3 -c 'import json,sys; json.load(sys.stdin)' < "$d/govern.json" 2>/dev/null || { stop_stub; ARC=-2; return; }
    printf 'use agent\nmain {\n  let hs = [agent.create("worker"), agent.create("worker"), agent.create("worker"), agent.create("worker"), agent.create("worker"), agent.create("worker")]\n%s\n  print("RUN_DONE")\n}\n' "$body" > "$d/p.naab"
    (cd "$d" && timeout 120 "$NAAB" p.naab </dev/null > run.out 2>&1); ARC=$?
    stop_stub
    grep -q RUN_DONE "$d/run.out" || ARC=-3
}
BATCH='  let r1 = agent.batch(hs, ["a1", "b1", "c1", "d1", "e1", "f1"])
  let r2 = agent.batch(hs, ["a2", "b2", "c2", "d2", "e2", "f2"])'
SERIAL='  for h in hs {
    let r = agent.send(h, "one")
  }
  for h in hs {
    let r = agent.send(h, "two")
  }'

agent_run "$W/s" "" "$SERIAL"
verify "$W/s" tel.jsonl
if [ "$ARC" = 0 ] && [ $VRC = 0 ] && [[ "$(forks "$W/s" tel.jsonl)" == *"shared=0" ]]; then
    pass "CC-00" "control: the same six agents sending one at a time leave a chain that verifies"
else
    fail "CC-00" "the sequential control (run rc=$ARC, verify rc=$VRC) -- the batch arms below would be void" "$VOUT $(tail -3 "$W/s/run.out" 2>/dev/null)"
    echo ""; echo "  $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 1
fi

# the verifier's "verified" must be able to say otherwise: one line removed
python3 -c 'import sys
lines = sys.stdin.buffer.read().split(b"\n"); del lines[len(lines)//2]
sys.stdout.buffer.write(b"\n".join(lines))' < "$W/s/tel.jsonl" > "$W/s/cut.jsonl"
verify "$W/s" cut.jsonl
if [ $VRC = 1 ] && [[ "$VOUT" == *BREAK* ]]; then pass "CC-00v" "control: the verifier reports a removed line"
else fail "CC-00v" "the verifier accepted a file with a line removed (rc=$VRC)" "$VOUT"; fi

bad=0; forked=0; detail=""
for i in $(seq 1 "$ROUNDS"); do
    agent_run "$W/b$i" "" "$BATCH"
    if [ "$ARC" != 0 ]; then bad=$((bad+1)); detail="round $i: the run did not finish (rc=$ARC)"; continue; fi
    verify "$W/b$i" tel.jsonl
    [ $VRC = 0 ] || { bad=$((bad+1)); detail="round $i: $(grep -m2 -A2 BREAK <<<"$VOUT") $(tail -1 <<<"$VOUT")"; }
    [[ "$(forks "$W/b$i" tel.jsonl)" == *"shared=0" ]] || forked=$((forked+1))
done
if [ $bad = 0 ]; then pass "CC-01" "two batches of six, $ROUNDS runs: every untouched chain verifies"
else fail "CC-01" "$bad of $ROUNDS batch runs left a chain that does not verify" "$detail"; fi
if [ $forked = 0 ]; then pass "CC-02" "no two events of a batch run share a predecessor"
else fail "CC-02" "$forked of $ROUNDS batch runs hold events sharing a predecessor" "$(forks "$W/b1" tel.jsonl)"; fi

# CC-03: what the webhook receives is the chain too, in the file's order
SK="$W/k"; mkdir -p "$SK"; : > "$SK/recv.jsonl"
(cd "$SK" && exec python3 "$REPO/tests/helpers/webhook_sink.py" port recv.jsonl) >/dev/null 2>&1 &
echo "$!" >> "$SINK_PIDS"
for _ in $(seq 1 50); do [ -s "$SK/port" ] && break; sleep 0.1; done
if [ ! -s "$SK/port" ]; then
    unm "CC-03" "the webhook sink did not start"
else
    agent_run "$SK" ", \"webhook_url\": \"http://127.0.0.1:$(cat "$SK/port")/\", \"forward_batch_size\": 1" "$BATCH"
    sleep 1
    cmp=$(python3 - "$SK" <<'PY'
import json, sys
d = sys.argv[1]
def hashes(name):
    out = []
    for line in open(d + "/" + name, "rb"):
        try: e = json.loads(line)
        except ValueError: continue
        if isinstance(e, dict) and "hash" in e: out.append(e["hash"])
    return out
f, r = hashes("tel.jsonl"), hashes("recv.jsonl")
print("file=%d received=%d same_order=%s" % (len(f), len(r), f == r))
PY
)
    if [ "$ARC" = 0 ] && [[ "$cmp" == *"same_order=True" ]] && [[ "$cmp" != "file=0 "* ]]; then
        pass "CC-03" "a batch run's forwarded events arrive in the file's order ($cmp)"
    else fail "CC-03" "the forwarded stream of a batch run (run rc=$ARC): $cmp" "$(tail -3 "$SK/run.out" 2>/dev/null)"; fi
fi

echo ""
echo "  $PASS passed, $FAIL failed, $UNM unmeasurable"
[ $FAIL -eq 0 ] && [ $UNM -eq 0 ]
