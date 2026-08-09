#!/usr/bin/env bash
# ============================================================
# test_restrictions_enabled_key.sh — "enabled": false that turns a check ON
#
# WHAT WAS FOUND
#
# Six of the twelve restrictions.* sub-blocks enable themselves from the mere
# PRESENCE of the block and never read "enabled" at all. So
#
#     "restrictions": {"crypto": {"enabled": false}}
#
# does not disable the crypto check. It ENABLES it, byte-identically to writing
# true. Omitting the block is the only way to leave one off. The other five
# (vcs_secret_extraction, obfuscation, data_exfiltration, resource_abuse,
# information_disclosure) do read the key and disable properly — so the same
# JSON means opposite things depending on which sibling it is written under,
# and an operator cannot learn the rule from one example.
#
# Found by working the contradiction-check coverage list: CONTRA-010 refused to
# fire on a config written to trip it, because the "restrictions":
# {"code_injection": {"enabled": false}} that was supposed to create the
# contradiction had quietly enabled code_injection instead.
#
# WHAT WAS CHANGED, AND WHAT WAS NOT
#
# Only the silence. The value is still ignored, because honouring it in all
# twelve is a LOOSENING: every config carrying "enabled": false today is being
# enforced and would silently stop being enforced on upgrade. That is the trade
# rejected for default-on secret scanning, in the other direction. So nothing
# about what is enforced changes here; the operator is simply told.
#
# EVERY GATE HAS A DEMONSTRATED FAILURE CASE
#
# Four degradations applied to governance_config.cpp one at a time:
#
#   E1  warning suppressed                        -> RE-01..RE-06
#   E2  warn on block presence, ignoring value    -> RE-08 RE-09
#   E3  warn on a key-honouring sibling too       -> RE-07
#   E4  crypto honours enabled:false              -> RE-10
#
# E2 and E3 are the ones worth keeping. A change that printed the warning for
# every sub-block, or on every presence of an "enabled" key, satisfies
# RE-01..RE-06 completely — those six alone cannot tell a correct warning from
# an indiscriminate one.
#
# E4 is the degradation that IS the rejected fix: making crypto honour the key.
# RE-10 fails under it, which is the point — RE-10 measures engine behaviour
# rather than the presence of a warning string, and it is what would catch
# someone quietly turning this into a loosening later.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/../helpers/gatelib.sh"
# Without an isolated trust store, a stray trusted key in the developer's
# ~/.naab (this container has carried one since Aug 3) makes every unsigned
# govern.json an INTEGRITY BLOCK. Parse-time warnings still print, so the
# warning gates go on passing while nothing executes — which is how RE-10 was
# first observed passing while counting its own warning text.
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust

NAAB="$SCRIPT_DIR/../../build/naab-lang"
if [ ! -x "$NAAB" ]; then
    echo "  test_restrictions_enabled_key.sh: SKIPPED — build/naab-lang not found"
    exit 0
fi

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
W="${_SYSTMP}/restr-enabled-$$"
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
mkdir -p "$W"

echo "=== restrictions.*.enabled: the key that does nothing ==="

gate_init "restrictions-enabled-key"
gate_def RE-01 WARN    "dangerous_calls warns on enabled:false"
gate_def RE-02 WARN    "shell_injection warns on enabled:false"
gate_def RE-03 WARN    "privilege_escalation warns on enabled:false"
gate_def RE-04 WARN    "code_injection warns on enabled:false"
gate_def RE-05 WARN    "crypto warns on enabled:false"
gate_def RE-06 WARN    "imports warns on enabled:false"
gate_def RE-07 CONTROL "a key-honouring sibling stays silent and is really disabled"
gate_def RE-08 CONTROL "no warning when no enabled key is written"
gate_def RE-09 CONTROL "no warning on enabled:true"
gate_def RE-10 TRUTH   "the warning is true — the check still runs"

cat > "$W/t.naab" << 'EOF'
main {
    <<python
import hashlib
hashlib.md5(b"abc").hexdigest()
print("x")
>>
}
EOF

# Runs the binary with the given restrictions JSON, echoes combined output.
run_with() {  # $1 = value of "restrictions"
    cat > "$W/govern.json" << EOF
{"version": "4.0", "restrictions": $1}
EOF
    "$NAAB" --governance-dashboard "$W/t.naab" 2>&1
}

warned_for() {  # $1 = sub-block name ; 0 if the ignored-key warning was printed
    echo "$2" | grep -q "restrictions.$1.enabled\": false has no effect"
}

# Count evidence that the CHECK itself ran, excluding the ignored-key warning.
# The warning names the same rule, so a naive `grep -c restrictions.crypto`
# counts the warning as proof of the thing the warning is complaining about.
# RE-10 was observed passing that way under an INTEGRITY BLOCK, where nothing
# executed at all.
check_hits() {  # $1 = sub-block name, $2 = output
    local n
    n=$(echo "$2" | grep -v 'has no effect' | grep -c "restrictions\.$1" || true)
    echo "${n:-0}"
}

# ------------------------------------------------------------------
# RE-01..RE-06 — every forcing sub-block warns
# ------------------------------------------------------------------
i=0
for sub in dangerous_calls shell_injection privilege_escalation code_injection crypto imports; do
    i=$((i + 1))
    id="RE-0$i"
    out="$(run_with "{\"$sub\": {\"enabled\": false}}")"
    if warned_for "$sub" "$out"; then
        pass "$id" "$sub warns on enabled:false"
    else
        fail "$id" "$sub did not warn that enabled:false is ignored" \
             "an operator writing false gets the check enabled with no notice"
    fi
done

# ------------------------------------------------------------------
# RE-07 — control: a sibling that honours the key must stay silent AND
# must actually be disabled by it. Both halves matter: silence alone would
# also be produced by a check that had been broken into never running.
# ------------------------------------------------------------------
cat > "$W/exfil.naab" << 'EOF'
main {
    <<python
CANARY_EXFIL = 1
print("x")
>>
}
EOF
mk_exfil() {  # $1 = true|false
    cat > "$W/govern.json" << EOF
{"version": "4.0", "restrictions": {"data_exfiltration":
  {"enabled": $1, "patterns": ["CANARY_EXFIL"]}}}
EOF
    "$NAAB" --governance-dashboard "$W/exfil.naab" 2>&1
}
OFF="$(mk_exfil false)"
ON="$(mk_exfil true)"
OFF_HITS=$(check_hits data_exfiltration "$OFF")
ON_HITS=$(check_hits data_exfiltration "$ON")

if warned_for data_exfiltration "$OFF"; then
    fail RE-07 "warned about a sub-block that honours enabled" \
         "the warning is firing indiscriminately"
elif [ "$OFF_HITS" -eq 0 ] && [ "$ON_HITS" -gt 0 ]; then
    pass RE-07 "a key-honouring sibling stays silent and is really disabled"
else
    fail RE-07 "control sibling did not demonstrate a working enabled key" \
         "enabled:false -> $OFF_HITS hits, enabled:true -> $ON_HITS hits (want 0 then >0)"
fi

# ------------------------------------------------------------------
# RE-08 / RE-09 — control: the warning is about the FALSE value, not about
# the block or the key existing.
# ------------------------------------------------------------------
NOKEY="$(run_with '{"crypto": {"weak_hashes": ["md5"]}}')"
if warned_for crypto "$NOKEY"; then
    fail RE-08 "warned when no enabled key was written" \
         "the warning fires on block presence rather than on the value"
else
    pass RE-08 "no warning when no enabled key is written"
fi

TRUEOUT="$(run_with '{"crypto": {"enabled": true}}')"
if warned_for crypto "$TRUEOUT"; then
    fail RE-09 "warned on enabled:true" "the warning does not read the value"
else
    pass RE-09 "no warning on enabled:true"
fi

# ------------------------------------------------------------------
# RE-10 — the substantive claim: the check really does still run, so the
# warning is telling the truth rather than apologising for nothing.
# ------------------------------------------------------------------
FALSE_OUT="$(run_with '{"crypto": {"enabled": false}}')"
ABSENT_OUT="$(run_with '{}')"
F=$(check_hits crypto "$FALSE_OUT")
A=$(check_hits crypto "$ABSENT_OUT")
if [ "$F" -gt 0 ] && [ "$A" -eq 0 ]; then
    pass RE-10 "the warning is true — the check still runs"
else
    fail RE-10 "could not show that enabled:false leaves the check running" \
         "enabled:false -> $F hits, block absent -> $A hits (want >0 then 0)"
fi

gate_print_summary
gate_exit
