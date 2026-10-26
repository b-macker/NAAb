#!/usr/bin/env bash
# ============================================================
# test_malformed_config.sh -- a govern.json that is FOUND and does not load
# refuses the run, whatever the flags (A84)
#
# --no-governance waives only the "no govern.json found" requirement (A78). A
# config that fails to load read as "not found": b998cf26 (naab-29 BUG 3) made
# loadFromFile() return false on a parse error instead of throwing, to stop a
# crash, and every caller then took the not-found branch. So the refusal said
# no govern.json existed, and with --no-governance the program ran with none of
# the policy beside it. Measured before the fix (observed, this branch and
# master's binary at abeec476): a config that blocks reading NAABPROBE but does
# not parse, run with --no-governance, printed the value and exited 0, on both
# engines and for each shape below. REST /check answered 200 "blocked": false
# beside it, and `naab-gov check` gave a verdict from built-in checks alone.
#
#   MC-00   CONTROL: the same policy, well formed, blocks the read (exit 3)
#           with and without --no-governance -- the probe can see a block
#   MC-01   a parse error: refused, exit 4, nothing ran, with and without
#           --no-governance, on the VM and under --tree-walk
#   MC-02   an empty govern.json: the same
#   MC-03   an extends that does not resolve: the same
#   MC-04   an array over the element cap: the same
#   MC-05   the refusal says the config was found and did not load, not that
#           none was found
#   MC-06   CONTROL: with no govern.json found, --no-governance still runs the
#           program (the waiver is not removed) -- UNMEASURABLE when a
#           govern.json sits above the temp directory
#   MC-07   CONTROL: a signed config edited after signing stays an INTEGRITY
#           BLOCK, exit 3, with and without the flag -- not turned into exit 4
#   MC-08   REST /check beside a config that does not parse: 400, no verdict;
#           MC-08c CONTROL: beside the well-formed config, a verdict (200)
#   MC-09   naab-gov check beside it: exit 4; MC-09c CONTROL: with no config
#           found, check still runs its built-in checks (exit 0)
#   MC-10   naab-gov lint beside it names a config that did not load
#
# Kill table (each mutant rebuilt, this suite run):
#   the main.cpp refusal removed                  MC-01..04 (--no-governance), MC-05
#   its g_governance_hard_block half removed      MC-07
#   the /check config_ok test removed             MC-08
#   the naab-gov check test removed               MC-09
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$SCRIPT_DIR/../../build}"
NAAB="${NAAB:-$BUILD_DIR/naab-lang}"
GOV="${NAAB_GOV:-$BUILD_DIR/naab-gov}"

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -8 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo "=== a govern.json that does not load ==="
if ! grep -qi naab <<<"$("$NAAB" --version 2>/dev/null)"; then
    echo "  FAIL [SETUP] $NAAB does not identify as naab-lang" >&2; exit 1
fi

# An unsigned govern.json with a key in the trust store is an INTEGRITY BLOCK:
# isolate the store so the result does not depend on this machine's keys.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-malcfg-XXXXXX")"
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; wait "$p" 2>/dev/null; done
    teardown_isolated_trust; rm -rf "$W"
}
trap cleanup EXIT

BR='"capabilities": { "env_vars": { "blocked_read": ["NAABPROBE"] } }'
PROG='use env
main { print("P=" + string(env.get("NAABPROBE"))) }'
mk() { mkdir -p "$W/$1"; printf '%s' "$2" > "$W/$1/govern.json"; printf '%s\n' "$PROG" > "$W/$1/p.naab"; }
mk good     "{ \"version\": \"5.0\", \"mode\": \"enforce\", \"security\": { \"sandbox_level\": \"elevated\" }, $BR }"
mk parse    "{ \"version\": \"5.0\", \"mode\": \"enforce\", $BR, "
mk empty    ""
mk extends  "{ \"version\": \"5.0\", \"mode\": \"enforce\", \"extends\": \"./missing/govern.json\", $BR }"
BIG="$(python3 -c "print(','.join('\"x%d\"' % i for i in range(20000)))")"
mk arraycap "{ \"version\": \"5.0\", \"mode\": \"enforce\", $BR, \"languages\": { \"blocked\": [$BIG] } }"

# run DIR [ARGS...] -> OUT RC
run() { local d="$1"; shift; OUT="$(cd "$W/$d" && NAABPROBE=leaked-value timeout 30 "$NAAB" run "$@" p.naab 2>&1)"; RC=$?; }
leaked() { grep -q 'P=leaked-value' <<<"$OUT"; }

# --- MC-00: the probe sees a block ------------------------------------------------
c0=1
for fl in "" "--no-governance"; do
    run good $fl
    { [ "$RC" = 3 ] && ! leaked; } || { c0=0; bad "MC-00" "the well-formed policy should block the read (${fl:-no flag})" "rc=$RC $OUT"; }
done
[ "$c0" = 1 ] && ok "MC-00" "control: the well-formed policy blocks the read, with and without --no-governance (exit 3)"

# --- MC-01..04: each shape, both engines, both flag settings ------------------------
n=1
for shape in parse empty extends arraycap; do
    id="MC-0$n"; n=$((n+1)); all=1; detail=""
    for eng in "" "--tree-walk"; do
        for fl in "" "--no-governance"; do
            run "$shape" $eng $fl
            if [ "$RC" != 4 ] || leaked; then
                all=0; detail+="${eng:-vm} ${fl:-(no flag)}: rc=$RC $(leaked && echo LEAKED)"$'\n'
            fi
        done
    done
    if [ "$all" = 1 ]; then ok "$id" "$shape: refused with exit 4 and nothing ran (VM and --tree-walk, with and without --no-governance)"
    else bad "$id" "$shape: a govern.json that does not load must refuse the run" "$detail"; fi
done

# --- MC-05: the message -----------------------------------------------------------
run parse --no-governance
if grep -q 'found but could not be loaded' <<<"$OUT" && ! grep -q 'No govern.json found' <<<"$OUT"; then
    ok "MC-05" "the refusal says the config was found and did not load"
else
    bad "MC-05" "the refusal should name a config that did not load, not a missing one" "$OUT"
fi

# --- MC-06: the waiver still works ------------------------------------------------
has_config_above() {
    local d="$1"
    while :; do
        [ -f "$d/govern.json" ] && return 0
        [ "$d" = "/" ] && return 1
        d="$(dirname "$d")"
    done
}
mkdir -p "$W/none"; printf '%s\n' 'main { print("NOCFG-RAN") }' > "$W/none/p.naab"
if has_config_above "$W/none"; then
    unm "MC-06" "a govern.json sits above $W -- no config-free directory to run from"
else
    OUT="$(cd "$W/none" && timeout 30 "$NAAB" run --no-governance p.naab 2>&1)"; RC=$?
    if [ "$RC" = 0 ] && grep -q 'NOCFG-RAN' <<<"$OUT"; then
        ok "MC-06" "control: with no govern.json found, --no-governance still runs the program"
    else
        bad "MC-06" "the no-config waiver should still run the program" "rc=$RC $OUT"
    fi
fi

# --- MC-07: an integrity failure keeps exit 3 ----------------------------------------
# Its own trust store: a trusted key makes every other arm's unsigned config an
# integrity block.
T2="$W/trust-signed"; mkdir -p "$T2" "$W/signed"
printf '%s' "{ \"version\": \"5.0\", \"mode\": \"enforce\", \"security\": { \"sandbox_level\": \"elevated\" }, $BR }" > "$W/signed/govern.json"
printf '%s\n' "$PROG" > "$W/signed/p.naab"
NAAB_TRUST_STORE_DIR="$T2" "$NAAB" --keygen "$W/key.pem" >/dev/null 2>&1
NAAB_TRUST_STORE_DIR="$T2" "$NAAB" --trust-key "$W/key.pem.pub" >/dev/null 2>&1
( cd "$W/signed" && NAAB_TRUST_STORE_DIR="$T2" NAAB_SIGNING_KEY="$W/key.pem" "$NAAB" --sign-governance >/dev/null 2>&1 )
OUT="$(cd "$W/signed" && NAAB_TRUST_STORE_DIR="$T2" NAABPROBE=leaked-value timeout 30 "$NAAB" run p.naab 2>&1)"; RC=$?
if [ ! -f "$W/signed/govern.json.sig" ] || [ "$RC" != 3 ]; then
    unm "MC-07" "the signed config did not run as signed (rc=$RC) -- cannot stage an edit after signing"
else
    printf ' ' >> "$W/signed/govern.json"   # one byte after signing: still valid JSON
    all=1; detail=""
    for fl in "" "--no-governance"; do
        OUT="$(cd "$W/signed" && NAAB_TRUST_STORE_DIR="$T2" NAABPROBE=leaked-value timeout 30 "$NAAB" run $fl p.naab 2>&1)"; RC=$?
        if [ "$RC" != 3 ] || leaked || ! grep -q 'INTEGRITY BLOCK' <<<"$OUT"; then all=0; detail+="${fl:-(no flag)}: rc=$RC $OUT"$'\n'; fi
    done
    if [ "$all" = 1 ]; then ok "MC-07" "control: a signed config edited after signing stays an INTEGRITY BLOCK (exit 3)"
    else bad "MC-07" "an integrity failure must keep exit 3" "$detail"; fi
fi

# --- MC-08: REST /check -------------------------------------------------------------
if ! command -v curl >/dev/null 2>&1; then
    unm "MC-08" "curl not available"
else
    # start DIR -> PORT; 1 if the server did not come up
    start() {
        PORT=$((20000 + RANDOM % 9000))
        ( cd "$W/$1" && exec "$NAAB" api "$PORT" --api-key k-mc >"$W/$1.api.log" 2>&1 ) &
        PIDS+=("$!")
        for _ in $(seq 1 100); do
            curl -s "http://127.0.0.1:$PORT/health" >/dev/null 2>&1 && return 0
            sleep 0.1
        done
        return 1
    }
    check() {
        RESP="$(curl -s --max-time 30 -o "$W/check.body" -w '%{http_code}' -X POST "http://127.0.0.1:$1/api/v1/check" \
            -H "Authorization: Bearer k-mc" -H "Content-Type: application/json" \
            -d '{"code": "import os\nos.system(\"id\")", "language": "python"}')"
        BODY="$(cat "$W/check.body" 2>/dev/null)"
    }
    if start parse; then
        check "$PORT"
        if [ "$RESP" = 400 ] && grep -q 'could not be loaded' <<<"$BODY" && ! grep -q '"blocked"' <<<"$BODY"; then
            ok "MC-08" "REST /check beside a config that does not parse answers 400 with no verdict"
        else
            bad "MC-08" "REST /check reported a verdict beside a config that did not load" "http=$RESP $BODY"
        fi
    else
        unm "MC-08" "the API server did not start beside the malformed config"
    fi
    if start good; then
        check "$PORT"
        if [ "$RESP" = 200 ] && grep -q '"blocked"' <<<"$BODY"; then
            ok "MC-08c" "control: beside the well-formed config, /check answers with a verdict"
        else
            bad "MC-08c" "/check should give a verdict beside a well-formed config" "http=$RESP $BODY"
        fi
    else
        unm "MC-08c" "the API server did not start beside the well-formed config"
    fi
fi

# --- MC-09 / MC-10: naab-gov ---------------------------------------------------------
if [ ! -x "$GOV" ]; then
    unm "MC-09" "naab-gov not built at $GOV"
else
    OUT="$(cd "$W/parse" && printf 'x = 1\n' | timeout 30 "$GOV" check --language python 2>&1)"; RC=$?
    if [ "$RC" = 4 ] && grep -q 'could not be loaded' <<<"$OUT"; then
        ok "MC-09" "naab-gov check beside a config that does not parse refuses (exit 4)"
    else
        bad "MC-09" "naab-gov check gave a verdict beside a config that did not load" "rc=$RC $OUT"
    fi
    if has_config_above "$W/none"; then
        unm "MC-09c" "a govern.json sits above $W -- no config-free directory to run from"
    else
        OUT="$(cd "$W/none" && printf 'x = 1\n' | timeout 30 "$GOV" check --language python 2>&1)"; RC=$?
        if [ "$RC" = 0 ]; then ok "MC-09c" "control: with no config found, naab-gov check still runs its built-in checks"
        else bad "MC-09c" "naab-gov check should run with no config found" "rc=$RC $OUT"; fi
    fi
    OUT="$(cd "$W/parse" && timeout 30 "$GOV" lint p.naab 2>&1)"; RC=$?
    if [ "$RC" = 4 ] && grep -q 'could not be loaded' <<<"$OUT"; then
        ok "MC-10" "naab-gov lint names a config that did not load"
    else
        bad "MC-10" "naab-gov lint should name a config that did not load" "rc=$RC $OUT"
    fi
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
