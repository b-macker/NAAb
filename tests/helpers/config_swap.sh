#!/usr/bin/env bash
# config_swap.sh -- an EXTERNAL operator that swaps govern.json under a running program
#
# Mid-run reload and ratchet tests need govern.json replaced while a NAAb
# program runs. They used to do it from inside the program, with a <<python>>
# block calling shutil.copy() onto govern.json. That only worked through a
# hole: govern.json, its .sig and the trusted keys are auto-protected
# (addGovernanceProtectedPaths), and the embedded Python audit hook did not
# consult the path policy. Now that it does, a Python write to govern.json is
# denied -- correctly -- and a <<shell>> cp is no substitute either, because
# several of these suites test LOOSENING shell, so their base config has it off.
#
# What these tests model is an operator editing the config of a running
# program, so that is what this is: a background process outside the program.
# The program asks for a swap and waits for it to happen, so the timing stays
# deterministic:
#
#   bash:    start_swap_operator "$run_dir"      # before each naab run
#   python:  _swap("<src>", "<dst>")             # see SWAP_PY_PRELUDE
#
# The request travels through marker files in the program's cwd (the run dir),
# which every fixture's path policy permits.

SWAP_OPERATOR_PIDS=""

stop_swap_operators() {
    local p
    # `|| true`: callers may run under `set -e`, and wait reports the kill (143).
    for p in $SWAP_OPERATOR_PIDS; do kill "$p" 2>/dev/null || true; wait "$p" 2>/dev/null || true; done
    SWAP_OPERATOR_PIDS=""
}

start_swap_operator() {  # $1 = directory the program runs in
    local d="$1"
    stop_swap_operators
    rm -f "$d/.swap_req" "$d/.swap_req.ready" "$d/.swap_done"
    (
        end=$((SECONDS + 180))
        while [ "$SECONDS" -lt "$end" ]; do
            if [ -f "$d/.swap_req.ready" ]; then
                while IFS=$'\t' read -r s t; do
                    [ -n "$s" ] && cp "$s" "$t"
                done < "$d/.swap_req"
                rm -f "$d/.swap_req" "$d/.swap_req.ready"
                touch "$d/.swap_done"
            fi
            sleep 0.05
        done
    ) >/dev/null 2>&1 &
    SWAP_OPERATOR_PIDS="$!"
}
