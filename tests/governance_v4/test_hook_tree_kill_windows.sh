#!/usr/bin/env bash
# ============================================================
# test_hook_tree_kill_windows.sh -- a timed-out hook's children die too (Windows)
#
# fireHook() used to end a timed-out hook with TerminateProcess, which kills
# that one process: a hook of `cmd /c ping -n 30 <ip>` "timed out" after 1 s
# while ping ran on for ~29 s. The hook now runs in a job object and a timeout
# terminates the job. POSIX has the same fix (process group) and its own arm,
# test_hooks.sh B5c; that suite is POSIX-only, so this one is Windows-only.
#
#   HT-00  CONTROL: the detector sees a running ping carrying this run's marker
#          address -- without it, "no ping found" below could mean a blind probe
#   HT-01  CONTROL: the hook fired and was killed for its timeout
#   HT-02  no ping with the marker address is left once the run returns
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

PASS=0; FAIL=0; SKIP=0
ok()   { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2"; }
skip_all() { for id in HT-00 HT-01 HT-02; do skip "$id" "$1"; done
             echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0; }

echo "=== A timed-out hook's child processes are killed (Windows) ==="

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;
    *) skip_all "Windows-only (UNMEASURABLE here); POSIX is test_hooks.sh B5c" ;;
esac
[ -x "$NAAB" ] || [ -x "$NAAB.exe" ] || skip_all "naab-lang not built (UNMEASURABLE)"
command -v powershell >/dev/null 2>&1 || skip_all "powershell unavailable -- no process probe (UNMEASURABLE)"

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-hooktree.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust   # the unsigned govern.json below must not meet a populated trust store
IP="127.0.$(( RANDOM % 250 + 2 )).$(( RANDOM % 250 + 2 ))"   # this run's marker
cleanup() {
    # never leave a marker ping behind, whatever the verdict
    powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='PING.EXE'\" | Where-Object { \$_.CommandLine -like '*$IP*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1
    teardown_isolated_trust; rm -rf "$W"
}
trap cleanup EXIT

# marker_pings -> count of running ping.exe whose command line holds $IP.
# Bytes on stdout only; no path crosses into the native process.
marker_pings() {
    powershell -NoProfile -Command "@(Get-CimInstance Win32_Process -Filter \"Name='PING.EXE'\" | Where-Object { \$_.CommandLine -like '*$IP*' }).Count" 2>/dev/null | tr -dc '0-9'
}

# --- HT-00: the probe can see a marker ping -----------------------------------------
cmd //c "ping -n 6 $IP" >/dev/null 2>&1 &
CTL=$!
seen=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
    n=$(marker_pings); [ "${n:-0}" -ge 1 ] && { seen=1; break; }
    sleep 0.5
done
kill "$CTL" 2>/dev/null; wait "$CTL" 2>/dev/null
powershell -NoProfile -Command "Get-CimInstance Win32_Process -Filter \"Name='PING.EXE'\" | Where-Object { \$_.CommandLine -like '*$IP*' } | ForEach-Object { Stop-Process -Id \$_.ProcessId -Force }" >/dev/null 2>&1
if [ "$seen" -eq 1 ]; then
    ok "HT-00" "CONTROL: the process probe sees a ping carrying the marker address"
else
    for id in HT-01 HT-02; do skip "$id" "the process probe cannot see a marker ping (UNMEASURABLE)"; done
    skip "HT-00" "the process probe cannot see a marker ping (UNMEASURABLE)"
    echo ""; echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"; exit 0
fi

# --- HT-01 / HT-02 --------------------------------------------------------------------
cat > "$W/govern.json" <<EOF
{
  "version": "5.0",
  "mode": "enforce",
  "security": { "sandbox_level": "elevated" },
  "languages": { "allowed": ["python"], "require_explicit": true },
  "hooks": {
    "on_violation": { "command": "cmd.exe", "args": ["/c", "ping -n 30 $IP"], "timeout": 1 }
  }
}
EOF
printf 'main {\n  <<shell\n  echo trigger\n  >>\n}\n' > "$W/test.naab"
( cd "$W" && timeout 60 "$NAAB" test.naab > "$W/out.txt" 2> "$W/err.txt" )
case "$(cat "$W/err.txt")" in
    *"Hook killed (timeout)"*) ok "HT-01" "CONTROL: the hook fired and was killed for its timeout" ;;
    *) bad "HT-01" "the hook did not time out -- HT-02 would prove nothing" "$(tail -3 "$W/err.txt" | tr '\n' ' ')" ;;
esac
sleep 1
left=$(marker_pings)
if [ "${left:-0}" -eq 0 ]; then
    ok "HT-02" "no ping started by the hook outlived its timeout"
else
    bad "HT-02" "$left ping process(es) started by the hook are still running after its timeout"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
