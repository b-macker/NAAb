#!/usr/bin/env bash
# ============================================================
# test_stub_routing.sh — per-agent response queues in agent_stub.py
#
# WHY
#
# agent_stub.py had ONE global counter, so every agent in a scenario drew from
# one interleaved queue. A scenario cannot then script a failure sequence for
# one agent while another stays clean — which is exactly what living-script_v3
# needs in order to produce drift on a single handle and keep its siblings
# quiet. Routing gives each matched agent its own counter.
#
# EVERY GATE HAS A DEMONSTRATED FAILURE CASE
#
# Five degradations were applied to agent_stub.py one at a time and the suite
# re-run against each. Recorded here rather than in a commit message because
# the next person to touch this needs it, and a commit message is not where
# they will look:
#
#   D1  route matching disabled (`if False`)      -> SR-02 SR-03 SR-05 SR-06
#   D2  routed requests also bump global_idx      -> SR-02 SR-03
#   D3  a routes key shifts the global queue      -> SR-02 SR-03 SR-04
#   D4  last matching key wins, not first         -> SR-05
#   D5  wrap-around instead of last-repeats       -> SR-01 SR-02 SR-04
#   D6  req_N.json opened without encoding=utf-8 -> SR-07 (SR-07c stays green)
#   D7  keys.log written after the body, as before -> SR-08 (SR-08c stays green)
#   D8  append_log() without its lock                -> SR-09 (SR-09c stays green)
#
# Each gate is isolated by at least one row (SR-01/SR-02 by D5, SR-04 by D3,
# SR-05 by D4), so none of them is riding on a neighbour.
#
# SR-04 is worth stating plainly because the obvious version of it is
# worthless: comparing a routed fixture's output against a SEPARATE routeless
# fixture's output proves nothing, since two different fixtures produce
# different output whatever the stub does. What can actually go wrong is
# subtler — the mere PRESENCE of a "routes" key perturbing the unrouted path.
# SR-04 therefore runs a fixture that HAS a route table whose keys match
# nothing in any request body, and requires byte-identical output to the
# routeless baseline. That is the claim every other stub-backed suite depends
# on, and D3 shows it is a real claim rather than a restatement of SR-01.
#
# SR-03 is the other easily-missed half: the global index must NOT advance on
# routed requests, so the one unrouted request still draws G1. A stub that
# routed correctly but also consumed the global queue would pass SR-02 (D2).
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
source "$SCRIPT_DIR/../helpers/stub_platform.sh"
skip_if_no_stub_support
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

# The old form probed `[ -d <termux tmp> ]` first. That probe is unsound off
# Android: the project's own runners created that directory on Linux, so the
# branch was taken on a box where the path is root-owned 755 -- and a non-root
# user got "mktemp: Permission denied". TMPDIR is what Termux actually sets,
# so consulting it needs no Termux-specific branch at all.
_SYSTMP="${TMPDIR:-/tmp}"
TEST_TMP="${_SYSTMP}/stub-routing-$$"
trap 'stop_stub; rm -rf "$TEST_TMP"' EXIT
mkdir -p "$TEST_TMP"

echo "=== agent_stub.py per-agent routing ==="

gate_init "stub-routing"
gate_def SR-01 COMPAT   "routeless fixture keeps global ordering and last-repeats"
gate_def SR-02 ROUTING  "each route consumes its own queue"
gate_def SR-03 ROUTING  "routed requests do not advance the global index"
gate_def SR-04 CONTROL  "a route table matching nothing is byte-identical to no route table"
gate_def SR-05 ROUTING  "first matching key in fixture order wins"
gate_def SR-06 EVIDENCE "routes.log records one decision per request"
gate_def SR-07 COMPAT   "a non-ASCII body is answered and logged intact under a non-UTF-8 locale"
gate_def SR-07c CONTROL "an ASCII body is answered under the same locale"
gate_def SR-08 EVIDENCE "keys.log holds a sequential client's requests in order, none lost at stop"
gate_def SR-08c CONTROL "without a held log line, keys.log holds both requests"
gate_def SR-09 EVIDENCE "every append to a shared log is made under the stub's log lock"
gate_def SR-09c CONTROL "the SR-09 scan flags an unlocked append in a planted copy"

# ------------------------------------------------------------------
# A minimal client: POST a body, print the model's text on one line.
# ------------------------------------------------------------------
cat > "$TEST_TMP/post.py" << 'PYEOF'
import json, sys, urllib.request
port = sys.argv[1]
out = []
for body in sys.argv[2:]:
    req = urllib.request.Request("http://127.0.0.1:%s/v1" % port,
                                 data=body.encode("utf-8"),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10) as r:
        d = json.load(r)
    parts = d["candidates"][0]["content"]["parts"]
    out.append(parts[0].get("text", "") or "-")
print(" ".join(out))
PYEOF

# The bodies. Tokens stand in for what the v3 harness plants in each agent's
# system_prompt; the seventh carries neither.
BODY_A='{"contents":[{"parts":[{"text":"TOKEN_ALPHA work"}]}]}'
BODY_B='{"contents":[{"parts":[{"text":"TOKEN_BRAVO work"}]}]}'
BODY_N='{"contents":[{"parts":[{"text":"no token here"}]}]}'
SEQ=("$BODY_A" "$BODY_B" "$BODY_A" "$BODY_B" "$BODY_A" "$BODY_B" "$BODY_N")

# ------------------------------------------------------------------
# Fixtures
# ------------------------------------------------------------------
cat > "$TEST_TMP/routed.json" << 'EOF'
{
  "routes": {
    "TOKEN_ALPHA": {"responses": [{"content": "A1"}, {"content": "A2"}, {"content": "A3"}]},
    "TOKEN_BRAVO": {"responses": [{"content": "B1"}, {"content": "B2"}]}
  },
  "responses": [{"content": "G1"}, {"content": "G2"}]
}
EOF

cat > "$TEST_TMP/flat.json" << 'EOF'
{"responses": [{"content": "G1"}, {"content": "G2"}, {"content": "G3"}, {"content": "G4"}]}
EOF

# Same global queue as flat.json, plus a route table whose keys appear in no
# request body. The 30 suites that never heard of routing depend on this being
# indistinguishable from flat.json.
cat > "$TEST_TMP/unmatched.json" << 'EOF'
{
  "routes": {
    "TOKEN_ZULU":   {"responses": [{"content": "Z1"}]},
    "TOKEN_YANKEE": {"responses": [{"content": "Y1"}]}
  },
  "responses": [{"content": "G1"}, {"content": "G2"}, {"content": "G3"}, {"content": "G4"}]
}
EOF

# Both tokens in one route table, ALPHA declared first; the request carries
# BRAVO's token before ALPHA's, so a body-order match would pick BRAVO.
cat > "$TEST_TMP/ambiguous.json" << 'EOF'
{
  "routes": {
    "TOKEN_ALPHA": {"responses": [{"content": "FIRST"}]},
    "TOKEN_BRAVO": {"responses": [{"content": "SECOND"}]}
  }
}
EOF

run_seq() {  # $1=fixture  $2=state subdir ; echoes the response line
    local fx="$1" dir="$TEST_TMP/$2"
    mkdir -p "$dir"
    start_stub "$fx" "$dir" || return 1
    python3 "$TEST_TMP/post.py" "$STUB_PORT" "${SEQ[@]}" 2>"$dir/client.err"
    local rc=$?
    stop_stub
    return $rc
}

# ------------------------------------------------------------------
# SR-01 / SR-04 — the routeless baseline, which is also the control
# ------------------------------------------------------------------
FLAT_OUT="$(run_seq "$TEST_TMP/flat.json" flat)"
FLAT_RC=$?
FLAT_EXPECT="G1 G2 G3 G4 G4 G4 G4"

if [ $FLAT_RC -ne 0 ]; then
    skip SR-01 "routeless fixture keeps global ordering" "stub or client failed"
else
    if [ "$FLAT_OUT" = "$FLAT_EXPECT" ]; then
        pass SR-01 "routeless fixture keeps global ordering and last-repeats"
    else
        fail SR-01 "routeless fixture changed behaviour" \
             "expected '$FLAT_EXPECT', got '$FLAT_OUT'"
    fi
fi

# SR-04 — an unmatched route table must not perturb the unrouted path.
UNM_OUT="$(run_seq "$TEST_TMP/unmatched.json" unmatched)"
UNM_RC=$?
if [ $FLAT_RC -ne 0 ] || [ $UNM_RC -ne 0 ]; then
    skip SR-04 "unmatched route table is inert" "stub or client failed"
elif [ "$UNM_OUT" = "$FLAT_OUT" ] && [ "$FLAT_OUT" = "$FLAT_EXPECT" ]; then
    pass SR-04 "a route table matching nothing is byte-identical to no route table"
else
    fail SR-04 "an unmatched route table changed the unrouted path" \
         "flat='$FLAT_OUT' unmatched='$UNM_OUT' (both should be '$FLAT_EXPECT')"
fi

# ------------------------------------------------------------------
# SR-02 / SR-03 — routing
# ------------------------------------------------------------------
ROUTED_OUT="$(run_seq "$TEST_TMP/routed.json" routed)"
ROUTED_RC=$?
# A: A1 A2 A3 (three sends)   B: B1 B2 B2 (last repeats)   unrouted: G1
ROUTED_EXPECT="A1 B1 A2 B2 A3 B2 G1"

if [ $ROUTED_RC -ne 0 ]; then
    skip SR-02 "each route consumes its own queue" "stub or client failed"
    skip SR-03 "routed requests do not advance the global index" "stub or client failed"
else
    if [ "$ROUTED_OUT" = "$ROUTED_EXPECT" ]; then
        pass SR-02 "each route consumes its own queue"
    else
        fail SR-02 "route queues interleaved or mis-ordered" \
             "expected '$ROUTED_EXPECT', got '$ROUTED_OUT'"
    fi

    # The seventh request is the only unrouted one. G1 means the global index
    # sat still through six routed requests; G2 or a default means it did not.
    LAST="${ROUTED_OUT##* }"
    if [ "$LAST" = "G1" ]; then
        pass SR-03 "routed requests do not advance the global index"
    else
        fail SR-03 "global index advanced on routed requests" \
             "unrouted request got '$LAST', expected the first global response G1"
    fi
fi

# ------------------------------------------------------------------
# SR-05 — determinism when a body carries two tokens
# ------------------------------------------------------------------
AMB_DIR="$TEST_TMP/amb"; mkdir -p "$AMB_DIR"
if start_stub "$TEST_TMP/ambiguous.json" "$AMB_DIR"; then
    BOTH='{"contents":[{"parts":[{"text":"TOKEN_BRAVO then TOKEN_ALPHA"}]}]}'
    AMB_OUT="$(python3 "$TEST_TMP/post.py" "$STUB_PORT" "$BOTH" 2>/dev/null)"
    stop_stub
    if [ "$AMB_OUT" = "FIRST" ]; then
        pass SR-05 "first matching key in fixture order wins"
    else
        fail SR-05 "ambiguous body did not resolve to the first declared route" \
             "expected 'FIRST', got '$AMB_OUT'"
    fi
else
    skip SR-05 "first matching key wins" "stub failed to start"
fi

# ------------------------------------------------------------------
# SR-06 — the routing decision is recorded, because mis-routing is silent
# ------------------------------------------------------------------
RLOG="$TEST_TMP/routed/routes.log"
if [ $ROUTED_RC -ne 0 ]; then
    skip SR-06 "routes.log records one decision per request" "routed run failed"
elif [ ! -f "$RLOG" ]; then
    fail SR-06 "routes.log was not written" "expected $RLOG"
else
    LINES=$(wc -l < "$RLOG" | tr -d ' ')
    NALPHA=$(count_matches 'TOKEN_ALPHA' "$RLOG")
    NBRAVO=$(count_matches 'TOKEN_BRAVO' "$RLOG")
    NNONE=$(count_matches ' -$' "$RLOG")
    if [ "$LINES" -eq 7 ] && [ "$NALPHA" -eq 3 ] && [ "$NBRAVO" -eq 3 ] && [ "$NNONE" -eq 1 ]; then
        pass SR-06 "routes.log records one decision per request"
    else
        fail SR-06 "routes.log does not account for every request" \
             "lines=$LINES alpha=$NALPHA bravo=$NBRAVO unrouted=$NNONE (want 7/3/3/1)"
    fi
fi

# ------------------------------------------------------------------
# SR-07 / SR-07c -- the stub under a locale whose encoding is not UTF-8
# ------------------------------------------------------------------
# Python opens a file in the LOCALE's encoding unless told otherwise: cp1252
# under MSYS2, ASCII under a bare C locale (with PEP 538/540 coercion off).
# The stub wrote each request body that way, so a body carrying a character
# the locale cannot encode -- the governance voice's prompt quotes violation
# messages with their check marks -- raised UnicodeEncodeError, which the
# handler's `except OSError` does not catch: the request file was left empty
# and the connection closed with no reply. Observed on build-windows (#345:
# the voice suite's stub got one request per arm and answered none) and
# reproduced here. Every open() in agent_stub.py now names UTF-8.
NONASCII='{"contents":[{"parts":[{"text":"Example: \u2717 Blocked \u2713 Allowed"}]}]}'
NONASCII_CHAR=$(printf '\xe2\x9c\x97')
loc_seq() {  # $1=state subdir, then bodies; echoes the response line
    local dir="$TEST_TMP/$1"; shift
    mkdir -p "$dir"
    PYTHONUTF8=0 PYTHONCOERCECLOCALE=0 LC_ALL=C LANG=C start_stub "$TEST_TMP/flat.json" "$dir" || return 1
    python3 "$TEST_TMP/post.py" "$STUB_PORT" "$@" 2>"$dir/client.err"
    local rc=$?
    stop_stub
    return $rc
}
ASCII_OUT="$(loc_seq loc_ascii "$BODY_N")"; ASCII_RC=$?
if [ $ASCII_RC -eq 0 ] && [ "$ASCII_OUT" = "G1" ]; then
    pass SR-07c "an ASCII body is answered under a non-UTF-8 locale"
else
    fail SR-07c "the stub did not answer an ASCII body under a non-UTF-8 locale" \
         "rc=$ASCII_RC out='$ASCII_OUT' $(head -c 300 "$TEST_TMP/loc_ascii/client.err" 2>/dev/null)"
fi
# The JSON escape keeps this file ASCII; the stub decodes it to the real
# characters, which is what it must then write.
NA_OUT="$(loc_seq loc_nonascii "$(printf '%s' "$NONASCII" | python3 -c 'import json,sys; print(json.dumps(json.load(sys.stdin), ensure_ascii=False))')")"; NA_RC=$?
if [ $NA_RC -eq 0 ] && [ "$NA_OUT" = "G1" ] && grep -qF "$NONASCII_CHAR" "$TEST_TMP/loc_nonascii/req_1.json" 2>/dev/null; then
    pass SR-07 "a non-ASCII body is answered and its request file holds it, under a non-UTF-8 locale"
else
    fail SR-07 "a non-ASCII body broke the stub under a non-UTF-8 locale" \
         "rc=$NA_RC out='$NA_OUT' req_1=$(wc -c < "$TEST_TMP/loc_nonascii/req_1.json" 2>/dev/null) bytes; $(grep -a -m1 'Error' "$TEST_TMP/loc_nonascii/stub.log" 2>/dev/null)"
fi

# ------------------------------------------------------------------
# SR-08 / SR-08c -- keys.log against the reply it records
# ------------------------------------------------------------------
# The stub wrote a request's keys.log line AFTER sending its body, so a
# sequential client could send request 2 before request 1 was logged, and a
# stub stopped after the last reply could lose a line outright. Seen under one
# CPU (tests/tsan/schedule_perturb.sh): test_concurrent_key_rotation.sh CK-11
# read request 2's key on line 1. log_hold_ms on the first response holds that
# request's logging step for 300 ms; the line must still come first, and both
# lines must be there once the stub is stopped right after the client returns.
log_seq() {  # $1=state subdir, $2=fixture; echoes "<n of line 1> <line count>"
    local dir="$TEST_TMP/$1"
    mkdir -p "$dir"
    start_stub "$2" "$dir" || return 1
    python3 "$TEST_TMP/post.py" "$STUB_PORT" "$BODY_N" "$BODY_N" >/dev/null 2>"$dir/client.err"
    local rc=$?
    stop_stub
    echo "$(awk 'NR==1{print $1}' "$dir/keys.log" 2>/dev/null) $(wc -l < "$dir/keys.log" 2>/dev/null | tr -d ' ')"
    return $rc
}
cat > "$TEST_TMP/log_hold.json" << 'EOF'
{"responses": [{"content": "G1", "log_hold_ms": 300}, {"content": "G2"}]}
EOF
LC_OUT="$(log_seq log_plain "$TEST_TMP/flat.json")"; LC_RC=$?
if [ $LC_RC -eq 0 ] && [ "$LC_OUT" = "1 2" ]; then
    pass SR-08c "without a held log line, keys.log holds both requests in order"
else
    fail SR-08c "keys.log did not hold two ordered lines even without a hold" "rc=$LC_RC line1/count='$LC_OUT'"
fi
LH_OUT="$(log_seq log_held "$TEST_TMP/log_hold.json")"; LH_RC=$?
if [ $LH_RC -eq 0 ] && [ "$LH_OUT" = "1 2" ]; then
    pass SR-08 "a held log line is still written first, and none is lost when the stub is stopped"
else
    fail SR-08 "keys.log lost or reordered request 1's line" "rc=$LH_RC line1/count='$LH_OUT' (want '1 2')"
fi

# ------------------------------------------------------------------
# SR-09 / SR-09c -- shared-log appends are serialised
# ------------------------------------------------------------------
# Two handler threads append to keys.log at once whenever two requests are held
# together, and on Windows an append is not atomic between file handles: both
# lines land at one offset and one is lost (build-windows, #354: one of the two
# concurrent first calls missing from keys.log in 3 of 3 runs). POSIX O_APPEND is
# atomic, so no run on Linux can show the race; this arm reads the stub's syntax
# tree instead: every open() in append mode must sit inside `with self.log_lock`.
unlocked_appends() {  # $1 = a copy of agent_stub.py; prints each unlocked append's line
    python3 - "$1" <<'PYEOF'
import ast, sys
tree = ast.parse(open(sys.argv[1], encoding="utf-8").read())
def is_append_open(n):
    return (isinstance(n, ast.Call) and getattr(n.func, "id", "") == "open"
            and len(n.args) >= 2 and isinstance(n.args[1], ast.Constant) and n.args[1].value == "a")
def under_log_lock(stack):
    for w in stack:
        for item in w.items:
            e = item.context_expr
            if isinstance(e, ast.Attribute) and e.attr == "log_lock":
                return True
    return False
def walk(node, stack):
    for child in ast.iter_child_nodes(node):
        if is_append_open(child) and not under_log_lock(stack):
            print(child.lineno)
        walk(child, stack + [child] if isinstance(child, ast.With) else stack)
walk(tree, [])
PYEOF
}
STUB_SRC="$SCRIPT_DIR/../helpers/agent_stub.py"
UNLOCKED="$(unlocked_appends "$STUB_SRC")"
APPENDS="$(grep -c '"a", encoding' "$STUB_SRC")"
if [ -z "$UNLOCKED" ] && [ "$APPENDS" -ge 1 ]; then
    pass SR-09 "every append to a shared log ($APPENDS site) is under the log lock"
else
    fail SR-09 "an append to a shared log is not under the log lock" "unlocked at line(s): $UNLOCKED; append sites: $APPENDS"
fi
sed 's/^            with self.log_lock:$/            if True:/' "$STUB_SRC" > "$TEST_TMP/stub_unlocked.py"
if [ -n "$(unlocked_appends "$TEST_TMP/stub_unlocked.py")" ]; then
    pass SR-09c "the scan flags the append once the lock is removed from a copy"
else
    fail SR-09c "the scan did not flag an unlocked append in the planted copy -- SR-09 measures nothing"
fi

gate_print_summary
gate_exit
