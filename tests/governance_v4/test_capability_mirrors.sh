#!/usr/bin/env bash
# ============================================================
# test_capability_mirrors.sh -- a capability set ANYWHERE is the capability
# every gate sees: inherited through `extends`, written in the bare form, or
# changed by a mid-run reload
#
# WHY THIS EXISTS. shell, network and filesystem mode are each stored twice in
# GovernanceRules: a legacy flat field (shell_allowed, network_allowed,
# filesystem_mode) and the v3 structured one (capabilities.shell.enabled, ...).
# Writers and readers each used one copy:
#   - the bare forms ("shell": false, "network": false, "filesystem": "read")
#     set ONLY the flat field;
#   - the `extends` merge set ONLY the structured field;
#   - the sandbox sync, checkShellAllowed(), checkNetworkAllowed() and the
#     filesystem gate read the FLAT one; the reload ratchet reads the
#     structured one.
# Measured before the fix (root, elevated, enforce): a child that `extends` a
# parent with shell.enabled:false ran a shell command (exit 0), and one whose
# parent had network.enabled:false opened a socket to a local listener
# (exit 0), while the parent used directly blocked both (exit 3). A parent in
# the bare form was not inherited at all, and a reload from bare
# "shell": false to bare "shell": true raised no ratchet violation.
# reconcileCapabilityMirrors() now makes the two copies agree on the stricter
# value after every parse and every merge.
#
#   CM-01  extends: parent shell off (object form) -> child's shell refused;
#          CM-01c control: shell-on parent -> runs
#   CM-02  extends: parent shell off (BARE form) -> refused
#   CM-03  extends: parent network off -> python socket never reaches the
#          listener; CM-03c control: network-on parent -> it does
#   CM-04  extends: parent filesystem "read" -> file.write refused;
#          CM-04c control: "write" parent -> file written
#   CM-05  CONTROL: the direct (non-extends) bare forms still block -- the
#          reconcile must not lose the form it did not read before
#   CM-06  reload bare "shell": false -> bare "shell": true is a ratchet
#          violation; CM-06a control: the object-form loosening is rejected
#          too (the detector sees rejections); CM-06b control: a tightening
#          reload is NOT rejected (the arm is not "every reload fails")
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Capability mirrors: extends, bare forms, reload ratchet ==="
if [ ! -x "$NAAB" ]; then
    skip "CM-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
    skip "CM-00" "python3 missing -- UNMEASURABLE"; report; exit 0
fi

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
source "$REPO/tests/helpers/config_swap.sh"
W="$(mktemp -d)"
LP=""
trap 'stop_swap_operators; [ -n "$LP" ] && kill "$LP" 2>/dev/null; teardown_isolated_trust; rm -rf "$W"' EXIT

# $1 dir, $2 parent capabilities JSON (the child extends it and sets none)
ext() {
    mkdir -p "$1"
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "capabilities": %s }\n' "$2" > "$1/parent.json"
    printf '{ "version": "4.0", "extends": "./parent.json", "mode": "enforce",\n  "security": { "sandbox_level": "elevated" } }\n' > "$1/govern.json"
}
direct() {
    mkdir -p "$1"
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "capabilities": %s }\n' "$2" > "$1/govern.json"
}

OUT=""
# Shell probe: an absolute-path command in a <<sh>> block. Returns 0 if it RAN.
shell_ran() {
    local d="$1"; rm -f "$d/marker"
    printf 'main {\n    <<sh\n/usr/bin/touch %s/marker\n>>\n    print("after")\n}\n' "$d" > "$d/s.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" s.naab 2>&1)"
    [ -f "$d/marker" ]
}
# Filesystem probe: file.write relative to the program dir. Returns 0 if written.
fs_wrote() {
    local d="$1"; rm -f "$d/out.txt"
    printf 'use file\nmain {\n    file.write("out.txt", "x")\n    print("after")\n}\n' > "$d/f.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" f.naab 2>&1)"
    [ -f "$d/out.txt" ]
}
# Network probe: embedded python connects to a local listener. Returns 0 if
# the LISTENER saw the connection -- observed at the far end, not inferred.
cat > "$W/listen.py" <<'EOF'
import socket, sys
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", 0)); s.listen(1)
open(sys.argv[1], "w").write(str(s.getsockname()[1])); s.settimeout(20)
try:
    c, _ = s.accept(); open(sys.argv[2], "w").write("ACCEPTED"); c.close()
except Exception:
    pass
EOF
net_reached() {
    local d="$1" port
    rm -f "$d/port" "$d/acc"
    python3 "$W/listen.py" "$d/port" "$d/acc" & LP=$!
    for _ in $(seq 1 50); do [ -s "$d/port" ] && break; sleep 0.1; done
    port="$(cat "$d/port" 2>/dev/null)"
    printf 'main {\n    <<python\nimport socket\ns = socket.socket()\ns.settimeout(3)\ns.connect(("127.0.0.1", %s))\ns.close()\n>>\n    print("after")\n}\n' "$port" > "$d/n.naab"
    OUT="$(cd "$d" && timeout 60 "$NAAB" n.naab 2>&1)"
    sleep 0.3; kill "$LP" 2>/dev/null; wait "$LP" 2>/dev/null; LP=""
    [ -f "$d/acc" ]
}

# ---- CM-01 / CM-02: shell -------------------------------------------------
SHELL_OK=0; FS_OK=0
ext "$W/c1c" '{ "shell": { "enabled": true } }'
if shell_ran "$W/c1c"; then
    SHELL_OK=1
    ok "CM-01c" "control: a shell-on parent leaves the shell probe running"
    ext "$W/c1" '{ "shell": { "enabled": false } }'
    if shell_ran "$W/c1"; then bad "CM-01" "shell RAN although the extends parent disables it" "$OUT"
    else ok "CM-01" "shell disabled by an extends parent (object form) is enforced"; fi
    ext "$W/c2" '{ "shell": false }'
    if shell_ran "$W/c2"; then bad "CM-02" "shell RAN although the extends parent disables it (bare form)" "$OUT"
    else ok "CM-02" "shell disabled by an extends parent (bare form) is enforced"; fi
else
    skip "CM-01c" "shell probe does not run even with shell on -- UNMEASURABLE"
    skip "CM-01" "control failed -- UNMEASURABLE"; skip "CM-02" "control failed -- UNMEASURABLE"
fi

# ---- CM-03: network ---------------------------------------------------------
ext "$W/c3c" '{ "network": { "enabled": true } }'
if net_reached "$W/c3c"; then
    ok "CM-03c" "control: a network-on parent lets the probe reach the listener"
    ext "$W/c3" '{ "network": { "enabled": false } }'
    if net_reached "$W/c3"; then bad "CM-03" "connection REACHED the listener although the extends parent disables network" "$OUT"
    else ok "CM-03" "network disabled by an extends parent is enforced"; fi
else
    skip "CM-03c" "network probe never reaches the listener (no embedded python?) -- UNMEASURABLE"
    skip "CM-03" "control failed -- UNMEASURABLE"
fi

# ---- CM-04: filesystem mode --------------------------------------------------
ext "$W/c4c" '{ "filesystem": { "mode": "write" } }'
if fs_wrote "$W/c4c"; then
    FS_OK=1
    ok "CM-04c" "control: a write-mode parent lets file.write through"
    ext "$W/c4" '{ "filesystem": { "mode": "read" } }'
    if fs_wrote "$W/c4"; then bad "CM-04" "file WRITTEN although the extends parent sets read-only" "$OUT"
    else
        case "$OUT" in
            *"Governance error"*|*"not allowed"*|*"read-only"*|*"read only"*) ok "CM-04" "read-only filesystem from an extends parent is enforced by governance" ;;
            *) bad "CM-04" "write refused, but not by governance (the sandbox alone held it)" "$OUT" ;;
        esac
    fi
else
    skip "CM-04c" "file.write does not succeed even in write mode -- UNMEASURABLE"; skip "CM-04" "control failed -- UNMEASURABLE"
fi

# ---- CM-05: direct bare forms (no extends) keep working ----------------------
# An absence assertion: meaningful only when the same probes were shown to run
# (CM-01c, CM-04c). Against an interpreter that runs nothing it would pass.
if [ "$SHELL_OK" = 1 ] && [ "$FS_OK" = 1 ]; then
    direct "$W/c5s" '{ "shell": false }'
    direct "$W/c5f" '{ "filesystem": "read" }'
    r5=""
    shell_ran "$W/c5s" && r5="$r5 shell"
    fs_wrote "$W/c5f" && r5="$r5 filesystem"
    if [ -z "$r5" ]; then ok "CM-05" "direct bare forms (shell, filesystem) still block"
    else bad "CM-05" "a direct bare form no longer blocks:$r5" "$OUT"; fi
else
    skip "CM-05" "the shell or filesystem probe never ran (CM-01c/CM-04c) -- UNMEASURABLE"
fi

# ---- CM-06: reload ratchet sees the bare form --------------------------------
"$NAAB" --keygen "$W/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$W/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$W/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }
# $1 = name, $2 = start capabilities, $3 = next capabilities. Sets OUT.
reload_case() {
    local d="$W/rl_$1" n="$W/rl_$1_next"
    mkdir -p "$d" "$n"
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "capabilities": %s }\n' "$2" > "$d/govern.json"
    printf '{ "version": "4.0", "mode": "enforce", "security": { "sandbox_level": "elevated" },\n  "capabilities": %s }\n' "$3" > "$n/govern.json"
    sign_dir "$d"; sign_dir "$n"
    if [ ! -f "$d/govern.json.sig" ] || [ ! -f "$n/govern.json.sig" ]; then OUT="__NOSIGN__"; return; fi
    cat > "$d/r.naab" <<EOF
use env
main {
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1)
_swap("$n/govern.json.sig", "$d/govern.json.sig")
_swap("$n/govern.json", "$d/govern.json")
"swapped"
>>
    print(s)
    let h = env.get("HOME")
    print("REACHED_END")
}
EOF
    start_swap_operator "$d"
    OUT="$(cd "$d" && timeout 120 "$NAAB" r.naab 2>&1)"
}
ENV_ON='"env_vars": { "read": true }'
reload_case obj "{ \"shell\": { \"enabled\": false }, $ENV_ON }" "{ \"shell\": { \"enabled\": true }, $ENV_ON }"
if [ "$OUT" = "__NOSIGN__" ]; then
    skip "CM-06a" "signing unavailable -- UNMEASURABLE"; skip "CM-06" "signing unavailable -- UNMEASURABLE"; skip "CM-06b" "signing unavailable -- UNMEASURABLE"
else
    case "$OUT" in
        *"Reload rejected: ratchet violation"*"capabilities.shell.enabled"*) ok "CM-06a" "control: an object-form shell loosening is rejected by the ratchet" ;;
        *) bad "CM-06a" "control failed: the object-form loosening was not reported as a ratchet violation" "$OUT" ;;
    esac
    reload_case bare "{ \"shell\": false, $ENV_ON }" "{ \"shell\": true, $ENV_ON }"
    case "$OUT" in
        *"Reload rejected: ratchet violation"*"capabilities.shell.enabled"*) ok "CM-06" "a bare-form shell loosening is rejected by the ratchet" ;;
        *) bad "CM-06" "bare \"shell\": false -> true was NOT rejected" "$OUT" ;;
    esac
    reload_case tight "{ \"shell\": false, \"network\": true, $ENV_ON }" "{ \"shell\": false, \"network\": false, $ENV_ON }"
    case "$OUT" in
        *"Reload rejected"*) bad "CM-06b" "control failed: a TIGHTENING reload was rejected" "$OUT" ;;
        *REACHED_END*) ok "CM-06b" "control: a tightening reload is not rejected" ;;
        *) bad "CM-06b" "control: program did not finish" "$OUT" ;;
    esac
fi
unset NAAB_SIGNING_KEY

report
[ $FAIL -eq 0 ] || exit 1
exit 0
