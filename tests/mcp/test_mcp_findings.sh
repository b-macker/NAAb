#!/usr/bin/env bash
# test_mcp_findings.sh -- `naab-lang mcp` tells the caller why (A43(b)(c))
#
# The arms (MF-00..MF-04) and what each defect was are in mcp_findings_arms.py.
# Red first, observed with master 6055ac3c's mcp_server.cpp: MF-01, MF-02 and
# MF-03 fail (a blocked propose answered findings []; 80 lines of program
# stderr pushed every governance line out of findings), and so do MF-04c and
# MF-04 -- there propose never read stdout, so the parse error reached the
# caller by no route: MF-04 guards the route this fix adds, it is not a leak
# found on master. MF-00, MF-02c and MF-03c pass.
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   propose's findings from stderr only (the pre-fix shape) -> MF-01, MF-02, MF-04c, MF-04 red
#   findingsFrom keeps the first 60 lines (the pre-fix cap) -> MF-03 red
#   propose without withholdFindings()                      -> MF-04 red
#   naab_run without withholdFindings()                     -> test_mcp_stdio.sh MC-18e red
#
# POSIX only, like the server's worker spawning: elsewhere UNMEASURABLE.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" && pwd)/$(basename "$NAAB")"

if [ "$(uname -s)" != "Linux" ]; then
    echo "  UNMEASURABLE [MF-*] the MCP server's workers are POSIX in this slice"
    exit 0
fi
if [ ! -x "$NAAB" ]; then
    echo "  FAIL [MF-pre] naab-lang not built at $NAAB"
    exit 1
fi

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_mcpf_XXXXXX")
trap 'teardown_isolated_trust; rm -rf "$W"' EXIT

echo "=== MCP findings carry the reason (A43(b)(c)) ==="
OUT=$(timeout 300 python3 "$SCRIPT_DIR/mcp_findings_arms.py" "$NAAB" "$W/arms" 2>&1)
RC=$?
sed 's/^/  /' <<<"$OUT"
if ! grep -q '^MF: ' <<<"$OUT"; then
    echo "  FAIL [MF-run] the arms did not finish (rc=$RC)"
    exit 1
fi
if grep -q '^SKIP \[' <<<"$OUT"; then
    exit 1
fi
[ "$RC" -eq 0 ]
