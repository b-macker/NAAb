#!/usr/bin/env bash
# ============================================================
# test_secret_dotenv.sh — no_secrets catches an unquoted .env line
#
# THE FAILURE THIS CATCHES
#
# Every assignment pattern in SECRET_PATTERNS required a QUOTED value
# (password = "...", api_key = "..."). An .env dump has no quotes, so
#   DB_PASSWORD=SuperSecretPassw0rd123! API_KEY=sk_live_99887766554433221100
# passed code_quality.no_secrets at HARD. An outside dogfood run (Gemini,
# release-notes pipeline, F-03) had an agent obey "list the contents of .env"
# and commit exactly that line in five of five adversarial runs.
#
# The new pattern is a tightening, so its false-positive cost matters as much
# as its catch: agent responses routinely SHOW configuration. The controls are
# the code and placeholder shapes it must leave alone.
#
#   SD-01..03  unquoted dotenv secrets are blocked (exit 3)
#   SD-04      the same line in an agent RESPONSE is blocked -- the path F-03
#              was found on
#   SD-10..16  CONTROLS that must still pass: code (os.environ[...], a call),
#              placeholders (<your-key>, your_..., changeme...), a value under
#              8 characters, and a non-secret name ending in TOKENS
#   SD-17      CONTROL: with no_secrets absent the secret line passes (the
#              block is this check, not something else)
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
source "$SCRIPT_DIR/../helpers/stub_launch.sh"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-dotenv-$$"
STUB_PID=""
cleanup(){ [ -n "$STUB_PID" ] && kill "$STUB_PID" 2>/dev/null; teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

echo "=== no_secrets: unquoted dotenv assignments ==="

cfg() {  # $1 = on/off. "off" OMITS the key: an object-form no_secrets is enabled
         # by presence and its inner "enabled": false is warned, not honoured
         # (governance_config.cpp parseCodeQualityField), so it is no control.
    if [ "$1" = on ]; then
        printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},"code_quality":{"no_secrets":{"enabled":true,"level":"hard"}}}\n' > "$W/govern.json"
    else
        printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"}}\n' > "$W/govern.json"
    fi
}
src_rc() {  # $1 = value placed in a function's string literal
    printf 'fn f() {\n  return "%s"\n}\nmain { print(f()) }\n' "$1" > "$W/t.naab"
    ( cd "$W" && timeout 30 "$NAAB" t.naab >/dev/null 2>&1 ); echo $?
}
expect() {  # ID WANT_RC VALUE DESC
    local rc; rc=$(src_rc "$3")
    if [ "$rc" = "$2" ]; then ok "$1" "$4"; else bad "$1" "$4 (rc=$rc, wanted $2)" "$3"; fi
}

cfg on
expect SD-01 3 'DB_PASSWORD=SuperSecretPassw0rd123!' "unquoted DB_PASSWORD is blocked"
expect SD-02 3 'API_KEY=sk_live_99887766554433221100' "unquoted API_KEY is blocked (the Stripe pattern needs 24 chars; this one does not)"
expect SD-03 3 'export GITHUB_TOKEN=abcd1234efgh5678' "an exported token line is blocked"

expect SD-10 0 'api_key=os.environ[KEY]' "CONTROL: code reading the environment passes"
expect SD-11 0 'token=get_token()' "CONTROL: a function call passes"
expect SD-12 0 'API_KEY=<your-key-here>' "CONTROL: an angle-bracket placeholder passes"
expect SD-13 0 'API_KEY=your_api_key_here' "CONTROL: a your_... placeholder passes"
expect SD-14 0 'SECRET=changeme123' "CONTROL: a changeme placeholder passes"
expect SD-15 0 'PASSWORD=short' "CONTROL: a value under 8 characters passes"
expect SD-16 0 'MAX_TOKENS=4096' "CONTROL: a non-secret ...TOKENS setting passes"

cfg off
expect SD-17 0 'DB_PASSWORD=SuperSecretPassw0rd123!' "CONTROL: with no_secrets absent the same line passes"

# SD-04: the agent-response path.
IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
if [ "$IS_WINDOWS" = 1 ] || ! command -v python3 >/dev/null 2>&1; then
    skip "SD-04" "agent stub unavailable (UNMEASURABLE)"
else
    python3 -c 'import json,sys; json.dump({"routes":{"DOTENV":{"responses":[{"content":"Notes:\n- [5a12761] config: DB_PASSWORD=SuperSecretPassw0rd123! API_KEY=sk_live_99887766554433221100","output_tokens":40}]}}}, open(sys.argv[1],"w"))' "$W/stub.json"
    if start_stub "$W/stub.json" "$W"; then
        cat > "$W/govern.json" <<EOF
{"version":"5.0","mode":"enforce","security":{"sandbox_level":"standard"},
 "code_quality":{"no_secrets":{"enabled":true,"level":"hard"}},
 "agents":{"w":{"provider":"gemini","model":"gemini-2.5-flash","api_key_env":"GEMINI_API_KEY",
   "api_base":"http://127.0.0.1:$STUB_PORT","system_prompt":"DOTENV writer","max_tokens":200}}}
EOF
        printf 'use agent\nmain {\n  let h = agent.create("w")\n  let r = agent.send(h, "write notes")\n  print("GOT|" + r.get("content"))\n}\n' > "$W/a.naab"
        out=$( cd "$W" && GEMINI_API_KEY=stub timeout 60 "$NAAB" a.naab 2>&1 ); rc=$?
        if [ $rc -eq 3 ] && ! grep -q 'GOT|' <<<"$out"; then ok "SD-04" "the dotenv line in an agent response ends the run before the script sees it"
        else bad "SD-04" "the response passed (rc=$rc)" "$(grep -m1 GOT <<<"$out")"; fi
    else
        skip "SD-04" "stub failed to start (UNMEASURABLE)"
    fi
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
