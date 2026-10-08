#!/usr/bin/env bash
# python_shell_off.sh -- is a <<python>> block refused BY DESIGN under a
# shell-off config in this build?
#
# A project that disables shell refuses every language whose runtime is a
# separate process (LanguageRegistry::getExecutor(), shell_disabled_by_policy).
# Embedded Python (pybind11) runs in-process and is NOT refused. Without
# pybind11 -- every Windows build, and Linux built without it -- python is a
# subprocess executor and IS refused. That was decided deliberately (fail
# closed): the separate python3 has no in-process hook and can start commands
# the engine never sees.
#
# Suites that use Python as a VEHICLE under a shell-off config (to drive a
# mid-run config swap, or to show that something else still runs) cannot
# measure their subject on such builds, and their arms fail -- or, worse, pass
# on a program that never ran. Source this file and call:
#
#   python_refused_under_shell_off "$NAAB"   # 0 = refused by design here
#
# It answers 0 ONLY when a shell-ON control writes its marker AND the shell-OFF
# run is refused with the gate's own message. Any other outcome (python broken,
# trust store rejecting the probe, a different error) answers 1, so a genuinely
# broken python is never excused by this. The probe runs against its OWN empty
# trust store (NAAB_TRUST_STORE_DIR), so its unsigned configs are never an
# INTEGRITY BLOCK and the answer cannot depend on the caller's keys or on
# whatever populated ~/.naab/trusted-keys on this machine.
#
# The marker is a RELATIVE path: on Windows the subprocess python is a native
# build that cannot open an MSYS /tmp/... path. It runs in the program's cwd.

python_refused_under_shell_off() {
    local naab="$1" d out rc=1 s
    d="$(mktemp -d 2>/dev/null)" || return 1
    mkdir -p "$d/on" "$d/off"
    printf '{"version":"4.0","mode":"audit","capabilities":{"shell":{"enabled":true}}}\n' > "$d/on/govern.json"
    printf '{"version":"4.0","mode":"audit","capabilities":{"shell":{"enabled":false}}}\n' > "$d/off/govern.json"
    mkdir -p "$d/trust"
    for s in on off; do
        printf 'main {\n    <<python\nopen("marker", "w").write("x")\n>>\n}\n' > "$d/$s/p.naab"
    done
    (cd "$d/on" && NAAB_TRUST_STORE_DIR="$d/trust" timeout 60 "$naab" p.naab >/dev/null 2>&1) || true
    out="$(cd "$d/off" && NAAB_TRUST_STORE_DIR="$d/trust" timeout 60 "$naab" p.naab 2>&1)" || true
    if [ -f "$d/on/marker" ] && [ ! -f "$d/off/marker" ]; then
        case "$out" in
            *"python execution denied by sandbox"*"governance disables running commands"*) rc=0 ;;
        esac
    fi
    rm -rf "$d"
    return $rc
}

# The skip reason, worded once so every suite says the same thing.
PYTHON_SHELL_OFF_REASON="UNMEASURABLE - python is a separate process in this build and a shell-off config refuses it by design (embedded Python is not refused)"
