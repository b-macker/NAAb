#!/usr/bin/env bash
# ============================================================
# test_env_action_matrix.sh — ENV_READ / ENV_WRITE in allowed_actions
#
# WHY
#
# The per-agent action matrix enforced FS_READ, FS_WRITE, NET_CONNECT,
# SHELL_EXEC, AGENT_SEND and TOOL_EXEC. ENV_READ and ENV_WRITE had ZERO
# occurrences in it, so no allowed_actions list could express "this role may not
# read environment variables" -- the primary credential-exfiltration surface, and
# the thing capabilities.env_vars governs globally. This is phase 2 of
# docs/plan-function-effects.md; it is useful to per-agent roles now, before
# function scope exists.
#
# THE COMPATIBILITY DECISION, which is the whole design of this change:
#
# Enforcement is OPT-IN -- it applies only when the matrix already mentions an
# ENV_* action. A matrix written before these existed opted into an allowlist
# over a SIX-action vocabulary; enforcing a seventh retroactively denies
# something those configs never had the option to permit. Measured: every
# shipped config using the matrix (living-script_v2's twelve roles) lists no
# ENV_* action, so a non-opt-in version would break all of them. This follows
# the shell_content_allowed convention -- a new field must not change existing
# behaviour.
#
# A config expresses the restriction by listing ENV_WRITE and not ENV_READ.
#
#   EM-01  no matrix at all -> unaffected
#   EM-02  BACKWARD COMPAT: a matrix without any ENV_* action -> unaffected.
#          This is the arm that would have caught the non-opt-in version, which
#          broke every shipped config that uses the matrix
#   EM-03  opting in and listing ENV_READ -> permitted
#   EM-04  opting in WITHOUT ENV_READ -> blocked (the restriction is expressible)
#   EM-05  the same for ENV_WRITE on env.set_var
#   EM-06  POSITIVE CONTROL: the global capabilities.env_vars.read switch still
#          blocks independently, so EM-01/02 passing cannot mean "env checks are
#          simply off". Note it refuses via the SANDBOX (that key syncs to
#          capabilities and drops SYS_ENV), not via checkEnvVarRead -- the outer
#          gate masks the inner one, and the first draft of this arm asserted
#          the wrong message and failed for that reason alone.
#   Every arm runs on BOTH engines.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-envmatrix-$$"
cleanup(){ teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"
[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

printf 'main {\n  let v = env.get("HOME")\n  print("READ_OK")\n}\n' > "$W/read.naab"
printf 'main {\n  env.set_var("NAAB_T", "1")\n  print("WRITE_OK")\n}\n' > "$W/write.naab"

# python writes to stdout, the shell redirects (see test_shell_path_handoff.sh)
cfg() {  # $1 = JSON array of actions, $2 = optional env_vars object JSON
    python3 -c "
import json,sys
acts=json.loads(sys.argv[1])
d={'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
   'agents':{'worker':{'provider':'gemini','model':'m','api_key_env':'K'}}}
if acts: d['agents']['worker']['allowed_actions']=acts
if sys.argv[2]: d.setdefault('capabilities',{})['env_vars']=json.loads(sys.argv[2])
json.dump(d, sys.stdout)
" "$1" "${2:-}" > "$W/govern.json"
}
verdict() {  # $1 = script, $2 = engine flag
    local o; o=$( cd "$W" && timeout 60 "$NAAB" ${2:-} --agent-id worker "$1" 2>&1 )
    case "$o" in
        *READ_OK*|*WRITE_OK*)            echo allowed ;;
        *"does not include ENV_READ"*)   echo blocked_read ;;
        *"does not include ENV_WRITE"*)  echo blocked_write ;;
        # capabilities.env_vars.read:false syncs to the sandbox, which drops
        # SYS_ENV and refuses BEFORE checkEnvVarRead's message -- the outer gate
        # masking the inner one. Either refusal proves env access is still
        # gated, which is all this control claims.
        *"disabled by policy"*|*"denied by sandbox"*|*"SYS_ENV capability required"*)
                                         echo blocked_global ;;
        *)                               echo "other" ;;
    esac
}

echo -e "${CYAN}=== Group EM: ENV_READ / ENV_WRITE in the action matrix ===${NC}"

for eng in "VM:" "tree-walk:--tree-walk"; do
    label="${eng%%:*}"; flag="${eng##*:}"

    cfg '[]'
    [ "$(verdict read.naab "$flag")" = "allowed" ] \
      && ok  "EM-01/$label" "no matrix -> unaffected" \
      || bad "EM-01/$label" "absence of a matrix must not restrict" "got: $(verdict read.naab "$flag")"

    cfg '["AGENT_SEND","FS_READ"]'
    got=$(verdict read.naab "$flag")
    [ "$got" = "allowed" ] \
      && ok  "EM-02/$label" "BACKWARD COMPAT: legacy matrix without ENV_* unaffected" \
      || bad "EM-02/$label" "a pre-existing matrix must not start blocking env reads" \
             "got: $got — this breaks every shipped config using the matrix"

    cfg '["AGENT_SEND","ENV_READ"]'
    [ "$(verdict read.naab "$flag")" = "allowed" ] \
      && ok  "EM-03/$label" "opted in and listed -> permitted" \
      || bad "EM-03/$label" "listing ENV_READ must permit reads" "got: $(verdict read.naab "$flag")"

    cfg '["AGENT_SEND","ENV_WRITE"]'
    [ "$(verdict read.naab "$flag")" = "blocked_read" ] \
      && ok  "EM-04/$label" "opted in without ENV_READ -> blocked" \
      || bad "EM-04/$label" "the restriction must be expressible" "got: $(verdict read.naab "$flag")"

    cfg '["AGENT_SEND","ENV_READ"]'
    [ "$(verdict write.naab "$flag")" = "blocked_write" ] \
      && ok  "EM-05/$label" "ENV_WRITE enforced the same way" \
      || bad "EM-05/$label" "ENV_WRITE must be enforced too" "got: $(verdict write.naab "$flag")"

    cfg '[]' '{"read": false}'
    [ "$(verdict read.naab "$flag")" = "blocked_global" ] \
      && ok  "EM-06/$label" "POSITIVE CONTROL: the global switch still blocks" \
      || bad "EM-06/$label" "env checks must still be live" \
             "got: $(verdict read.naab "$flag") — EM-01/02 may be passing vacuously"
done

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
