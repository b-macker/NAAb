#!/usr/bin/env bash
# test_ratchet_property.sh -- "governance only tightens", checked (Bundle 8)
#
# THREE INSTRUMENTS
#
#   1. tests/property/ratchet_property.cpp (build target naab_ratchet_property):
#      generated config pairs over small alphabets, each OLD loaded as a run
#      loads a discovered govern.json (with a role, an environment, a lease and
#      --governance-override drawn at random), NEW written over it with a later
#      mtime and reloadIfChanged() deciding. For every accepted reload no
#      request the engine refused may be allowed, judged by the engine's own
#      gates; a role and a lease must narrow what they are laid over; the CLI's
#      run-limit and sandbox-level flags may not raise what govern.json
#      configures. A failure prints the config pair and the request.
#   2. `naab-gov ratchet OLD NEW`: the seam. It calls evaluateReload(), the
#      function reloadIfChanged() calls, after loading OLD as a run does.
#   3. naab-lang runs that reproduce what the harness found, one per finding,
#      each beside a control that the probe can see the effect at all.
#
#   RP-01  the harness: property and controls hold over the default budget
#   RP-02  the seam agrees with the in-process reload on dumped cases, startup
#          overlays included (role, environment, lease); both outcomes occur
#   RG-01  a loosening is refused (exit 1) and named
#   RG-02  a tightening is accepted (exit 0)
#   RG-03  CONTROL: an unchanged config is accepted with no notice (OLD is
#          loaded with the self-protected paths a run adds, so they compare
#          equal; loading OLD with loadFromFile() alone reported the trust
#          store "now refused" on every reload)
#   RG-04  a NEW that is not JSON, a missing file, an unknown option, an unknown
#          environment and an unknown lease action are config errors (exit 4)
#   RG-05  --json carries the outcome and the violations
#   RG-06  the signature is checked as a reload checks it: with a trusted key
#          installed an unsigned NEW is refused; --ratchet-only judges the
#          ratchet alone; a signed NEW is accepted
#   A94    under enforce a misspelled security.sandbox_level no longer switches
#          the clamp to `standard` off (env.get is refused by the standard
#          sandbox); CONTROLS: "elevated" reads, no level is clamped
#   A95a   an environment whose mode is enforce enforces over a file in mode
#          off; CONTROL: the same file without --env reads the variable
#   A95b   a reload that stops an active engine enforcing is refused, under an
#          environment whose mode is off (file enforce -> off; environment
#          off -> audit); CONTROL: a tightening under that environment is
#          accepted
#   A96a   with env_vars.read false, dropping a blocked_read name (which is
#          still withheld from child processes) is refused; CONTROL: adding
#          one is accepted
#   A96b   detect -> soft is refused (an overridden soft block lets the action
#          through, detect does not); CONTROL: detect -> hard is accepted
#   A97    a lease without ENV_READ keeps the project's blocked_subprocess_vars
#          (HOME stays withheld from a child); CONTROLS: HOME withheld without
#          the lease, and passed under the lease when nothing blocks it
#
# Kill table (each mutant rebuilt, this suite re-run; see the PR):
#   enforceClampApplies back to configured.empty()      -> A94, RP-01 (flag overlays)
#   applyEnvironment does not raise active_             -> A95a
#   evaluateReload without the enforced-mode check      -> A95b, RP-01 (reload)
#   the install sets active_ from the overlaid mode     -> RP-01 (reload)
#   the child-process blocked_read comparison removed   -> A96a, RP-01 (reload)
#   chkLevel ordered by levelToInt() alone               -> A96b, RP-01 (reload)
#   allowlist mode ignores the blocked lists again      -> A97, RP-01 (overlay)
#   naab-gov loads OLD with loadFromFile()              -> RG-03, RP-02
#   naab-gov skips applyAgentRole() / applyEnvironment() -> RP-02
# Red first: RP-01 failed on the code before these fixes, with each of the
# findings above as its counterexamples (seed 1..9 at 300-400 cases).

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$(cd "$SCRIPT_DIR/../../build" 2>/dev/null && pwd)"
NAAB="${NAAB:-$BUILD/naab-lang}"
GOV="${NAAB_GOV:-$BUILD/naab-gov}"
PROP="${NAAB_RATCHET_PROP:-$BUILD/naab_ratchet_property}"

PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }

echo "=== Governance only tightens: the reload ratchet, checked (Bundle 8) ==="
missing=0
for b in "$NAAB" "$GOV" "$PROP"; do
    if [ ! -x "$b" ]; then
        echo "  FAIL [RP-00] $(basename "$b") not built at $b (UNMEASURABLE, not a pass)"
        missing=1
    fi
done
[ "$missing" -eq 0 ] || exit 1

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_ratchetprop_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT
mkdir -p "$W/home"
export HOME="$W/home"
unset NAAB_GOVERN_KEY NAAB_SIGNING_KEY

# ---------------------------------------------------------------- RP-01
CASES="${RATCHET_PROP_CASES:-400}"
SEED="${RATCHET_PROP_SEED:-1}"
RC=0; OUT=$("$PROP" --cases "$CASES" --seed "$SEED" --extends 200 --show 3 --quiet 2>&1) || RC=$?
summary=$(grep -m1 '^ratchet property:' <<<"$OUT")
acc=$(sed -n 's/.* accepted=\([0-9]*\).*/\1/p' <<<"$summary")
ref=$(sed -n 's/.* refused=\([0-9]*\).*/\1/p' <<<"$summary")
if [ "$RC" -eq 0 ] && [ "${acc:-0}" -gt 0 ] && [ "${ref:-0}" -gt 0 ]; then
    pass RP-01 "$summary"
else
    fail RP-01 "the property or a control failed (exit $RC)" "$(grep -E 'COUNTEREXAMPLE|request:|control|UNMEASURABLE|CRASH|ERROR|flag overlays|  sandbox|  timeout|  memory' <<<"$OUT" | head -20)"
fi

# ---------------------------------------------------------------- RP-02
DUMP="$W/dump"
RC=0; "$PROP" --cases 120 --seed 7 --dump "$DUMP" 120 --quiet >/dev/null 2>&1 || RC=$?
agree=0; disagree=0; n_acc=0; n_ref=0; n_overlay=0; n_unknown=0; first_bad=""
for d in "$DUMP"/case-*; do
    [ -f "$d/meta.json" ] || continue
    # meta: accepted role env lease lease_actions -- read with the shell, no path crosses to python
    meta=$(python3 -c 'import json,sys; m=json.load(sys.stdin); print(int(m["accepted"]), int(m["env_known"]), int(m["role"]), m["env"] or "-", int(m["lease"]), ",".join(m["lease_actions"]))' < "$d/meta.json")
    read -r m_acc m_known m_role m_env m_lease m_actions <<<"$meta"
    args=()
    [ "$m_role" = 1 ] && args+=(--agent-id worker)
    [ "$m_env" != "-" ] && args+=(--env "$m_env")
    [ "$m_lease" = 1 ] && args+=(--lease-grant "${m_actions:-}")
    [ ${#args[@]} -gt 0 ] && n_overlay=$((n_overlay+1))
    grc=0; (cd "$d" && "$GOV" ratchet govern.json new.json "${args[@]}" >/dev/null 2>&1) || grc=$?
    want=1; [ "$m_acc" = 1 ] && want=0
    if [ "$m_known" = 0 ]; then
        want=4   # naab-gov refuses an environment the old config lacks; a run warns
        n_unknown=$((n_unknown+1))
    elif [ "$m_acc" = 1 ]; then n_acc=$((n_acc+1)); else n_ref=$((n_ref+1)); fi
    if [ "$grc" -eq "$want" ]; then agree=$((agree+1)); else
        disagree=$((disagree+1)); [ -z "$first_bad" ] && first_bad="$(basename "$d"): reload accepted=$m_acc, naab-gov exit $grc (${args[*]:-no overlay})"
    fi
done
if [ "$RC" -eq 0 ] && [ "$disagree" -eq 0 ] && [ "$n_acc" -ge 10 ] && [ "$n_ref" -ge 10 ] && [ "$n_overlay" -ge 10 ]; then
    pass RP-02 "naab-gov ratchet agrees with the reload on $agree cases ($n_acc accepted, $n_ref refused, $n_overlay with startup overlays, $n_unknown naming an environment the old config lacks)"
else
    fail RP-02 "seam and reload disagree, or the sample is one-sided (harness exit $RC)" \
        "agree=$agree disagree=$disagree accepted=$n_acc refused=$n_ref overlays=$n_overlay ${first_bad}"
fi

# ---------------------------------------------------------------- RG arms
C="$W/cli"; mkdir -p "$C"
cfg() { printf '%s' "$2" > "$C/$1"; }
ratchet() { RC=0; OUT=$(cd "$C" && "$GOV" ratchet "$@" 2>/dev/null) || RC=$?; }
has() { grep -q -F -- "$1" <<<"$OUT"; }

cfg govern.json '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"blocked_read":["NAAB_RP_SECRET"]}}}'
cfg loose.json '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"blocked_read":[]}}}'
cfg tight.json '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"blocked_read":["NAAB_RP_SECRET","NAAB_RP_OTHER"]}}}'
cfg same.json '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"blocked_read":["NAAB_RP_SECRET"]}}}'
printf '{not json' > "$C/bad.json"

ratchet govern.json loose.json
if [ "$RC" -eq 1 ] && has "REFUSED" && has "'NAAB_RP_SECRET' blocked -> allowed"; then
    pass RG-01 "a loosening is refused and named"
else fail RG-01 "expected exit 1 naming NAAB_RP_SECRET" "rc=$RC $OUT"; fi

ratchet govern.json tight.json
if [ "$RC" -eq 0 ] && has "ACCEPTED" && has "'NAAB_RP_OTHER' allowed -> blocked"; then
    pass RG-02 "a tightening is accepted, with its notice"
else fail RG-02 "expected exit 0 with the tightening notice" "rc=$RC $OUT"; fi

ratchet govern.json same.json
if [ "$RC" -eq 0 ] && has "ACCEPTED" && ! has "notice:"; then
    pass RG-03 "CONTROL: an unchanged config is accepted with no notice"
else fail RG-03 "an unchanged config should compare equal" "rc=$RC $OUT"; fi

bad=""
ratchet govern.json bad.json;            [ "$RC" -eq 4 ] || bad+=" not-json=$RC"
ratchet govern.json missing.json;        [ "$RC" -eq 4 ] || bad+=" missing=$RC"
ratchet govern.json tight.json --frob;   [ "$RC" -eq 4 ] || bad+=" option=$RC"
ratchet govern.json tight.json --env nope; [ "$RC" -eq 4 ] || bad+=" env=$RC"
ratchet govern.json tight.json --lease-grant FS_READ,BOGUS; [ "$RC" -eq 4 ] || bad+=" lease=$RC"
ratchet govern.json;                     [ "$RC" -eq 4 ] || bad+=" one-file=$RC"
if [ -z "$bad" ]; then pass RG-04 "config errors exit 4 (not JSON, missing, option, environment, lease action, one file)"
else fail RG-04 "expected exit 4 for each config error" "$bad"; fi

ratchet govern.json loose.json --json
if [ "$RC" -eq 1 ] && python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["outcome"]=="ratchet_refused" and d["violations"] and d["signature_checked"] is True' <<<"$OUT" 2>/dev/null; then
    pass RG-05 "--json carries outcome, violations and the signature flag"
else fail RG-05 "unexpected --json output" "rc=$RC $OUT"; fi

# RG-06: a trusted key installed -> unsigned NEW refused; signed NEW accepted.
S="$W/signed"; mkdir -p "$S"
cp "$C/govern.json" "$S/govern.json"; cp "$C/tight.json" "$S/tight.json"
"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
(cd "$S" && NAAB_SIGNING_KEY="$W/k.pem" "$NAAB" --sign-governance >/dev/null 2>&1) || true
rc_unsigned=0; out_unsigned=$(cd "$S" && "$GOV" ratchet govern.json tight.json 2>/dev/null) || rc_unsigned=$?
rc_only=0; (cd "$S" && "$GOV" ratchet govern.json tight.json --ratchet-only >/dev/null 2>&1) || rc_only=$?
mkdir -p "$S/n"; cp "$S/tight.json" "$S/n/govern.json"
(cd "$S/n" && NAAB_SIGNING_KEY="$W/k.pem" "$NAAB" --sign-governance >/dev/null 2>&1) || true
rc_signed=0; (cd "$S" && "$GOV" ratchet govern.json n/govern.json >/dev/null 2>&1) || rc_signed=$?
if [ -f "$S/govern.json.sig" ] && [ "$rc_unsigned" -eq 1 ] && grep -q "signature" <<<"$out_unsigned" \
   && [ "$rc_only" -eq 0 ] && [ "$rc_signed" -eq 0 ]; then
    pass RG-06 "unsigned NEW refused under a trusted key; --ratchet-only and a signed NEW accepted"
else
    fail RG-06 "signature handling" "sig=$([ -f "$S/govern.json.sig" ] && echo y || echo n) unsigned=$rc_unsigned only=$rc_only signed=$rc_signed $out_unsigned"
fi
teardown_isolated_trust; setup_isolated_trust   # the remaining arms run unsigned

# ---------------------------------------------------------------- findings
R="$W/run"; mkdir -p "$R"
naab() { RC=0; OUT=$(cd "$R" && NAAB_RP_SECRET=rp-token-7781 timeout 30 "$NAAB" run "$@" 2>&1) || RC=$?; }

# A94: env.get("HOME") is refused by the standard sandbox (SYS_ENV).
printf 'use env\nmain {\n    let v = env.get("HOME")\n    print("V=[" + string(v) + "]")\n}\n' > "$R/e.naab"
level_arm() {  # $1 = security.sandbox_level JSON value or empty
    local sec=""
    [ -n "$1" ] && sec=",\"security\":{\"sandbox_level\":$1}"
    printf '{"version":"5.0","mode":"enforce"%s}' "$sec" > "$R/govern.json"
    naab e.naab
}
level_arm '"bogus"'; bogus_rc=$RC; bogus_read=$(grep -c 'V=\[/' <<<"$OUT")
level_arm '"elevated"'; elev_read=$(grep -c 'V=\[/' <<<"$OUT")
level_arm ''; none_viol=$(grep -c 'SANDBOX VIOLATION' <<<"$OUT")
if [ "$bogus_read" -eq 0 ] && [ "$bogus_rc" -ne 0 ] && [ "$elev_read" -eq 1 ] && [ "$none_viol" -ge 1 ]; then
    pass A94 "a misspelled sandbox_level is clamped to standard under enforce (controls: elevated reads, none is clamped)"
else
    fail A94 "sandbox level under enforce" "bogus: rc=$bogus_rc read=$bogus_read; elevated read=$elev_read; none violation=$none_viol"
fi

# A95a: file mode off, environment x enforce, blocked_read the variable.
printf 'use env\nmain {\n    let v = env.get("NAAB_RP_SECRET")\n    print("V=[" + string(v) + "]")\n}\n' > "$R/s.naab"
printf '{"version":"5.0","mode":"off","security":{"sandbox_level":"elevated"},"capabilities":{"env_vars":{"blocked_read":["NAAB_RP_SECRET"]}},"environments":{"x":{"mode":"enforce"}}}' > "$R/govern.json"
naab --env x s.naab; env_rc=$RC; env_read=$(grep -c 'rp-token-7781' <<<"$OUT")
naab s.naab; plain_read=$(grep -c 'rp-token-7781' <<<"$OUT")
if [ "$env_rc" -eq 3 ] && [ "$env_read" -eq 0 ] && [ "$plain_read" -eq 1 ]; then
    pass A95a "--env with mode enforce over a mode-off file enforces (control: without it the variable is read)"
else
    fail A95a "environment mode enforce over a mode-off file" "with --env: rc=$env_rc read=$env_read; without: read=$plain_read"
fi

# A95b: reloads under an environment whose mode is off.
cfg govern.json '{"version":"5.0","mode":"enforce","environments":{"dev":{"mode":"off"}}}'
cfg off.json    '{"version":"5.0","mode":"off","environments":{"dev":{"mode":"off"}}}'
cfg audit.json  '{"version":"5.0","mode":"enforce","environments":{"dev":{"mode":"audit"}}}'
cfg more.json   '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"blocked_read":["NAAB_RP_SECRET"]}},"environments":{"dev":{"mode":"off"}}}'
ratchet govern.json off.json --env dev;   off_rc=$RC;   off_named=$(grep -c "mode: enforced enforce -> off" <<<"$OUT")
ratchet govern.json audit.json --env dev; aud_rc=$RC;   aud_named=$(grep -c "mode: enforced enforce -> audit" <<<"$OUT")
ratchet govern.json more.json --env dev;  more_rc=$RC
if [ "$off_rc" -eq 1 ] && [ "$off_named" -eq 1 ] && [ "$aud_rc" -eq 1 ] && [ "$aud_named" -eq 1 ] && [ "$more_rc" -eq 0 ]; then
    pass A95b "reloads that stop an active engine enforcing are refused (control: a tightening is accepted)"
else
    fail A95b "reload under an environment whose mode is off" "off: rc=$off_rc named=$off_named; audit: rc=$aud_rc named=$aud_named; tightening: rc=$more_rc"
fi

# A96a: env_vars.read false, blocked_read dropped / added.
cfg govern.json '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"read":false,"blocked_read":["NAAB_RP_SECRET"]}}}'
cfg drop.json   '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"read":false,"blocked_read":[]}}}'
cfg add.json    '{"version":"5.0","mode":"enforce","capabilities":{"env_vars":{"read":false,"blocked_read":["NAAB_RP_SECRET","NAAB_RP_OTHER"]}}}'
ratchet govern.json drop.json; drop_rc=$RC; drop_named=$(grep -c "child-process read" <<<"$OUT")
ratchet govern.json add.json;  add_rc=$RC
if [ "$drop_rc" -eq 1 ] && [ "$drop_named" -ge 1 ] && [ "$add_rc" -eq 0 ]; then
    pass A96a "with read false, dropping a blocked_read name is refused (control: adding one is accepted)"
else
    fail A96a "blocked_read under read false" "drop: rc=$drop_rc named=$drop_named; add: rc=$add_rc"
fi

# A96b: detect -> soft refused, detect -> hard accepted.
cfg govern.json '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"enabled":true,"level":"detect"}}}'
cfg soft.json   '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"enabled":true,"level":"soft"}}}'
cfg hard.json   '{"version":"5.0","mode":"enforce","code_quality":{"no_secrets":{"enabled":true,"level":"hard"}}}'
ratchet govern.json soft.json; soft_rc=$RC; soft_named=$(grep -c "detect -> soft" <<<"$OUT")
ratchet govern.json hard.json; hard_rc=$RC
if [ "$soft_rc" -eq 1 ] && [ "$soft_named" -ge 1 ] && [ "$hard_rc" -eq 0 ]; then
    pass A96b "detect -> soft is refused (control: detect -> hard is accepted)"
else
    fail A96b "level order under an override" "soft: rc=$soft_rc named=$soft_named; hard: rc=$hard_rc"
fi

# A97: a lease keeps the project's blocked_subprocess_vars. The child is
# printenv by full path; HOME is one of the names allowlist mode always passes.
PE="$(command -v printenv || true)"
if [ -z "$PE" ]; then
    echo "  SKIP [A97] UNMEASURABLE: no printenv on this machine"
else
    printf 'use process\nmain {\n    let r = process.run("%s", ["HOME"])\n    print("V=[" + r["stdout"] + "]")\n}\n' "$PE" > "$R/c.naab"
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"env_vars":{"blocked_subprocess_vars":["HOME"]}}}' > "$R/govern.json"
    naab --lease-grant SHELL_EXEC c.naab; lease_seen=$(grep -c 'V=\[/' <<<"$OUT"); lease_ran=$(grep -c 'V=\[' <<<"$OUT")
    naab c.naab; plain_seen=$(grep -c 'V=\[/' <<<"$OUT")
    printf '{"version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"}}' > "$R/govern.json"
    naab --lease-grant SHELL_EXEC c.naab; open_seen=$(grep -c 'V=\[/' <<<"$OUT")
    if [ "$lease_ran" -eq 1 ] && [ "$lease_seen" -eq 0 ] && [ "$plain_seen" -eq 0 ] && [ "$open_seen" -eq 1 ]; then
        pass A97 "a lease keeps HOME withheld (controls: withheld without the lease, passed under it when nothing blocks it)"
    else
        fail A97 "child environment under a lease" "lease: ran=$lease_ran seen=$lease_seen; no lease: seen=$plain_seen; lease, no list: seen=$open_seen"
    fi
fi

echo ""
echo "Ratchet property: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
