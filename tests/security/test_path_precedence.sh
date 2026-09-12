#!/usr/bin/env bash
# ============================================================
# test_path_precedence.sh -- which list wins when both match
#
# WHY THIS EXISTS. An allowed entry and a blocked entry can both match the same
# path. checkPathAccess() answered that twice, with two different answers: the
# capabilities layer let ANY allowed entry cancel EVERY blocked entry, while the
# agent-role overlay applied its blocked list first and unconditionally. The
# capabilities comment claimed "specific allow beats broad deny", but nothing in
# the code compared specificity.
#
# The consequence was not theoretical. addGovernanceProtectedPaths() appends
# govern.json, its .sig and the trusted-keys directory to blocked_paths so that
# a NAAb program cannot rewrite the governance it runs under. Measured on
# c5a645f: a project whose config said allowed_paths ["."] -- the most ordinary
# entry there is -- could read its own govern.json, and the same config with
# allowed_paths empty could not. One broad allow disabled the self-protection.
#
# Precedence now lives in decidePathAccess(): longest matching prefix wins, ties
# deny, and the agent overlay stays deny-first because a role narrows the
# project policy and must never widen it.
#
# WHAT EACH ASSERTION IS FOR. PP-01, PP-03 and PP-05 are the fix. PP-02 is the
# reason the fix is longest-prefix rather than deny-first: the "allowed ./data
# beats blocked /" pattern is documented and real configs use it, so a change
# that broke it would be a regression wearing a security badge. PP-00 and PP-04
# are the controls without which the blocking assertions prove nothing -- an
# arm that refuses every read passes PP-01, PP-03 and PP-05 for free, and this
# campaign has already shipped one suite (entry-point parity) whose one-
# directional assertions certified exactly that.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
LAST_OUTPUT=""
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  {
    echo "  FAIL [$1] $2"
    # An assertion that cannot show why it failed gets believed over the code.
    # This suite runs on a Windows job nobody has locally, so a bare FAIL costs
    # a full CI round trip to learn nothing.
    if [ -n "${LAST_OUTPUT:-}" ]; then
        echo "$LAST_OUTPUT" | sed 's/^/        | /' | head -12
    fi
    FAIL=$((FAIL+1))
}
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Path policy precedence ==="

if [ ! -x "$NAAB" ]; then
    skip "PP-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

# Every probe writes an UNSIGNED govern.json. With any key in the real trust
# store that is an INTEGRITY BLOCK (exit 3) before path policy is consulted, and
# every "blocked" assertion below would pass for the wrong reason.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

W="$(mktemp -d)"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

mkdir -p "$W/data"
echo "SECRET"  > "$W/secret.txt"
echo "OK"      > "$W/data/ok.txt"

# EVERY PATH IN THIS SUITE IS RELATIVE, and that is load-bearing rather than
# stylistic. An absolute path written by the shell is in the shell's vocabulary:
# under MSYS2 `mktemp -d` yields /tmp/xxx while the native binary canonicalises
# its file argument to C:/..., so the config entry and the path being judged
# never share a prefix and even the permitted read is refused. Relative entries
# go through NAAb's own canonicaliser on both sides, so the comparison is
# between two things resolved the same way on every platform. It is also how
# real configs are written, which makes PP-03 ("." voiding self-protection) the
# realistic case rather than a contrived one. The script cd's into $W for every
# run, so "." is the fixture directory.
#
# sandbox_level elevated on purpose: with "mode": "enforce" and no level, the
# runtime upgrades unrestricted -> standard, which blocks absolute paths in the
# sandbox layer. Every assertion here would then be measuring the sandbox
# instead of the path policy it names.
#
# The agents block is always emitted rather than appended with ${var:+...}.
# Inside a heredoc that expansion eats the double quotes around the key it is
# inserting, so `"agents":` was written as `agents:` and every agent probe died
# on a config error instead of reaching a path decision. PP-07 caught it; PP-06
# had been "passing" on a refusal that had nothing to do with path policy.
cfg() {  # $1 = allowed_paths JSON array, $2 = blocked_paths JSON array, $3 = agents object
    local agents="${3:-}"
    [ -n "$agents" ] || agents='{}'
    cat > "$W/govern.json" <<EOF
{
  "version": "4.0",
  "meta": { "mode": "enforce" },
  "security": { "sandbox_level": "elevated" },
  "capabilities": {
    "filesystem": { "mode": "readwrite", "allowed_paths": $1, "blocked_paths": $2 }
  },
  "agents": $agents
}
EOF
    # A malformed fixture exits 4 and every assertion below reads as "refused".
    if command -v python3 >/dev/null 2>&1; then
        python3 -c "import json,sys; json.load(open('$W/govern.json'))" 2>/dev/null \
            || { bad "PP-CFG" "generated govern.json is not valid JSON -- fixture is broken"; }
    fi
}

# Prints "read" if the program read the file, "refused" otherwise. The marker is
# the read itself, not an exit code: a governance block and an unrelated crash
# both exit non-zero, and only one of them is the thing under test.
attempt() {  # $1 = path to read, $2... = extra naab args
    local target="$1"; shift
    cat > "$W/p.naab" <<EOF
main {
  let c = file.read("$target")
  print("PATH_READ_OK")
}
EOF
    # Captured, not piped. Under `set -o pipefail`, `naab | grep -q` reports
    # FAILURE when grep matches: grep exits early, naab takes SIGPIPE, and the
    # pipeline status becomes the signal. Every read would then read as refused
    # and every blocking assertion here would pass for the wrong reason.
    local out
    out=$( (cd "$W" && timeout 60s "$NAAB" "$@" p.naab) 2>&1 )
    LAST_OUTPUT="$out"
    case "$out" in
        *PATH_READ_OK*) echo "read" ;;
        *)              echo "refused" ;;
    esac
}

# --- PP-00 usability -------------------------------------------------------
# Without this, an arm that refuses everything scores four passes below.
cfg '["."]' '[]'
R=$(attempt "data/ok.txt")
if [ "$R" = "read" ]; then
    ok "PP-00" "a permitted read succeeds (probe is usable)"
else
    bad "PP-00" "permitted read was refused -- suite is UNMEASURABLE, not passing"
    report; exit 1
fi

# --- PP-01 the inversion ---------------------------------------------------
cfg '["."]' '["./secret.txt"]'
R=$(attempt "secret.txt")
if [ "$R" = "refused" ]; then
    ok "PP-01" "a blocked file beats an allowed entry naming its directory"
else
    bad "PP-01" "broad allow cancelled a specific block (F9): got $R"
fi

# --- PP-02 the pattern that must survive -----------------------------------
cfg '["./data"]' '["."]'
R=$(attempt "data/ok.txt")
if [ "$R" = "read" ]; then
    ok "PP-02" "an allowed subdirectory still beats a blocked root"
else
    bad "PP-02" "longest-prefix broke the documented allow-narrow pattern: got $R"
fi

# --- PP-03 govern.json self-protection -------------------------------------
# The blocked entry here is not in the config: addGovernanceProtectedPaths()
# adds it. PP-03b is its positive control -- if self-protection did not block
# govern.json even with no allowed_paths, PP-03 would be testing nothing.
cfg '[]' '[]'
R=$(attempt "govern.json")
if [ "$R" = "refused" ]; then
    ok "PP-03b" "self-protection blocks govern.json with no allowed_paths (control)"
else
    bad "PP-03b" "self-protection is not active at all: got $R"
fi

cfg '["."]' '[]'
R=$(attempt "govern.json")
if [ "$R" = "refused" ]; then
    ok "PP-03" "allowed_paths [\".\"] does not void govern.json self-protection"
else
    bad "PP-03" "a broad allow disabled self-protection: got $R"
fi

# --- PP-04 the allowlist still denies --------------------------------------
cfg '["./data"]' '[]'
R=$(attempt "secret.txt")
if [ "$R" = "refused" ]; then
    ok "PP-04" "a path outside a non-empty allowlist is refused (control)"
else
    bad "PP-04" "allowlist no longer denies non-members: got $R"
fi

# --- PP-05 ties deny -------------------------------------------------------
cfg '["./secret.txt"]' '["./secret.txt"]'
R=$(attempt "secret.txt")
if [ "$R" = "refused" ]; then
    ok "PP-05" "equally specific allow and block: the block wins"
else
    bad "PP-05" "a tie resolved to allow -- precedence is not fail-closed: got $R"
fi

# --- PP-06 the agent overlay is unchanged ----------------------------------
# Deny-first, so a MORE specific agent allowed_path still loses to a broader
# agent blocked_path. This is the asymmetry the change preserves on purpose; if
# decidePathAccess() were applied in LongestPrefixWins mode here it would be a
# loosening, and this assertion is what would catch that.
AGENTS="{ \"w\": { \"provider\": \"gemini\", \"model\": \"m\", \"api_key_env\": \"K\",
      \"blocked_paths\": [\".\"], \"allowed_paths\": [\"./data\"] } }"
cfg "[]" "[]" "$AGENTS"
R=$(attempt "data/ok.txt" --agent-id w)
if [ "$R" = "refused" ]; then
    ok "PP-06" "agent overlay stays deny-first (broad role block beats narrow role allow)"
else
    bad "PP-06" "agent overlay was loosened to longest-prefix: got $R"
fi

# --- PP-07 the overlay control ---------------------------------------------
# Without this, PP-06 passes on a build where --agent-id refuses every read.
AGENTS="{ \"w\": { \"provider\": \"gemini\", \"model\": \"m\", \"api_key_env\": \"K\",
      \"blocked_paths\": [\"./secret.txt\"], \"allowed_paths\": [\".\"] } }"
cfg "[]" "[]" "$AGENTS"
R=$(attempt "data/ok.txt" --agent-id w)
if [ "$R" = "read" ]; then
    ok "PP-07" "the same role reads a path it permits (control for PP-06)"
else
    bad "PP-07" "role refuses everything -- PP-06 is UNMEASURABLE: got $R"
fi

# --- PP-08/PP-09 an empty entry names no path ------------------------------
# weakly_canonical("") resolves to "", which used to reach prefix.back() -- UB
# on an empty string, and on this platform it read as "matches everything".
# Both directions are pinned because defining it moved both: an empty allow no
# longer grants, and an empty block no longer denies.
cfg '[""]' '[]'
R=$(attempt "data/ok.txt")
if [ "$R" = "refused" ]; then
    ok "PP-08" "an empty allowed_paths entry grants nothing"
else
    bad "PP-08" "an empty allow entry still matches: got $R"
fi

cfg '[]' '[""]'
R=$(attempt "data/ok.txt")
if [ "$R" = "read" ]; then
    ok "PP-09" "an empty blocked_paths entry denies nothing"
else
    bad "PP-09" "an empty block entry still matches: got $R"
fi

report
[ "$FAIL" -eq 0 ] || exit 1
exit 0
