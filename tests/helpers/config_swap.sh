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
# The established IN-program route is examples/living-script's operator:
# process.run("mv", ...) then re-sign, through the governed SYS_EXEC path
# (cd70a29b records the stale-.sig race it has to step around). It needs
# capabilities.shell, which these bases switch off, so the swap moves outside.
#
# What these tests model is an operator editing the config of a running
# program, so that is what this is: a background process outside the program.
# The program asks for a swap and waits for it to happen, so the timing stays
# deterministic:
#
#   bash:    start_swap_operator "$run_dir"      # before each naab run
#   python:  _swap("<src>", "<dst>")   # one round trip per call; by
#                                        # convention the .sig goes first
#
# The request travels through marker files in the program's cwd (the run dir),
# which every fixture's path policy permits.

# Operators are tracked in FILES, not a shell variable: callers commonly start
# one inside $( ... ), a subshell whose variables die with it, and an operator
# that outlived its run kept serving the SAME directory. Two operators on one
# request race: a stale one finishing an earlier request deletes the next
# request's files and touches .swap_done, so the program is told a swap
# happened that never did (measured: .sig swapped, govern.json not, about 1
# run in 12 as a non-root user). One operator per directory (a pidfile there),
# plus a registry keyed by the top-level shell's $$ -- unchanged in subshells --
# so cleanup can reach every operator this test started.
_swap_registry() { echo "${TMPDIR:-/tmp}/.naab-swap-ops.$$"; }

_swap_kill() {  # $1 = pid
    kill "$1" 2>/dev/null || true
    # `|| true`: callers may run under `set -e`.
    for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$1" 2>/dev/null || return 0; sleep 0.05; done
    kill -9 "$1" 2>/dev/null || true
}

stop_swap_operators() {
    local reg p
    reg=$(_swap_registry)
    [ -f "$reg" ] || return 0
    while read -r p; do [ -n "$p" ] && _swap_kill "$p"; done < "$reg"
    rm -f "$reg"
}

start_swap_operator() {  # $1 = directory the program runs in
    local d="$1" old
    if [ -f "$d/.swap_operator.pid" ]; then
        read -r old < "$d/.swap_operator.pid" && _swap_kill "$old"
    fi
    rm -f "$d/.swap_req" "$d/.swap_req.ready" "$d/.swap_done"
    (
        end=$((SECONDS + 180))
        while [ "$SECONDS" -lt "$end" ]; do
            if [ -f "$d/.swap_req.ready" ]; then
                # Precaution, not a measured fix. A reload that fires between
                # the two files sees new content under the old .sig and is
                # rejected; that rejection is NOT cached (cd70a29b: the mtime is
                # deliberately left uncached on signature failure so a later
                # valid .sig is retried), so the order is not load-bearing -- it
                # only avoids a spurious "Reload rejected" line. govern.json
                # lands last, by rename (atomic within a directory).
                while IFS=$'\t' read -r s t; do
                    case "$t" in */govern.json|govern.json) ;; *) [ -n "$s" ] && cp "$s" "$t" ;; esac
                done < "$d/.swap_req"
                while IFS=$'\t' read -r s t; do
                    case "$t" in */govern.json|govern.json) cp "$s" "$t.swaptmp" && mv -f "$t.swaptmp" "$t" ;; esac
                done < "$d/.swap_req"
                rm -f "$d/.swap_req" "$d/.swap_req.ready"
                touch "$d/.swap_done"
            fi
            sleep 0.05
        done
    ) >/dev/null 2>&1 &
    echo "$!" > "$d/.swap_operator.pid"
    echo "$!" >> "$(_swap_registry)"
}
