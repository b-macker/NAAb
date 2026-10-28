#!/usr/bin/env bash
# test_lsp_governance_diag.sh -- the LSP names the rule a governance block cites (A43(d))
#
# naab-lsp runs `naab-lang --lint-only` on a document and turns each governance
# block in its output into a diagnostic. It looked for the label
# "  Rule (govern.json): ", which 7ab3fa77 (2026-06-16, a config-leak fix)
# renamed to "  Rule: " in formatError(), so every governance diagnostic's code
# has read "governance" since. It also split blocks only at a line STARTING with
# "Governance error:", while the CLI prints a HARD block as "Error: Governance
# error: ...", so the block's header was folded into the "[governance] Loaded:"
# banner before it and the message read as the tail of the config's path.
# Observed on master 6055ac3c: code "governance", message "<abs path>/govern.json
# (mode: enforce)\nError: Governance error: Placeholder ...". The writer and the
# LSP now share include/naab/governance_message_format.h.
#
#   LG-00   CONTROL: the LSP answers initialize -- the probe reads its protocol
#   LG-00c  CONTROL: the same document under a config without the rule draws no
#           governance diagnostic -- LG-01's diagnostic is the policy's
#   LG-01   a HARD block (a TODO): code is the rule name
#           code_quality.no_placeholders, message is the block's own first line
#   LG-02   an ADVISORY block: same code, and the code carries no configured
#           value (the Rule line is "<rule> = <value>")
#   LG-03   a whole-source HARD block of another rule (a secret): code is
#           code_quality.no_secrets -- the code is read, not hard-wired
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-10-28):
#   LSP looks for the old "  Rule (govern.json): " label  -> LG-01, LG-02, LG-03 red
#   block splitter matches only at column 0 (no "Error: ") -> LG-01 red (the message;
#                                                            LG-03's code is still read)
#   ruleName() keeps " = <value>"                          -> LG-01, LG-02, LG-03 red
# Red first: on master LG-01, LG-02 and LG-03 fail, LG-00 and LG-00c pass.
# The secret is assembled from fragments: no literal sits in this file.

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LSP="${1:-$SCRIPT_DIR/../../build/naab-lsp}"
LSP="$(cd "$(dirname "$LSP")" && pwd)/$(basename "$LSP")"
PROBE="$SCRIPT_DIR/../helpers/lsp_diag_probe.py"
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_lspgd_XXXXXX")
trap 'rm -rf "$W"' EXIT

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

if [ ! -x "$LSP" ]; then
    echo "  UNMEASURABLE: naab-lsp not built at $LSP (make naab-lsp)"
    exit 1
fi

# probe ID CONFIG PROGRAM -> sets OUT (the probe's JSON lines)
probe() {
    local d="$W/$1"; mkdir -p "$d"
    printf '%s\n' "$2" > "$d/govern.json"
    printf '%s' "$3" > "$d/p.naab"
    OUT=$(cd "$d" && python3 "$PROBE" "$LSP" p.naab 2>&1)
}
# field N KEY -> the KEY of the Nth governance diagnostic (1-based), or ""
field() {
    python3 -c 'import sys, json
rows = [json.loads(l) for l in sys.stdin if l.strip().startswith("{") and "\"init\"" not in l]
n, k = int(sys.argv[1]), sys.argv[2]
print(rows[n-1].get(k, "") if len(rows) >= n else "")' "$1" "$2" <<<"$OUT"
}
count() { grep -c '"code"' <<<"$OUT"; }

TODO_PROG=$'fn f() {\n    // TODO: finish\n    return 1\n}\nmain {\n    print(f())\n}\n'
KEY="AK""IA""TESTTESTTESTTEST"
SECRET_PROG="$(printf 'main {\n    let k = "%s"\n    print(k)\n}\n' "$KEY")"
base='"mode": "enforce", "security": {"sandbox_level": "elevated"}'
HARD="{ $base, \"code_quality\": {\"no_placeholders\": {\"enabled\": true, \"level\": \"hard\"}} }"
ADV="{ $base, \"code_quality\": {\"no_placeholders\": {\"enabled\": true, \"level\": \"advisory\"}} }"
SECRETS="{ $base, \"code_quality\": {\"no_secrets\": {\"enabled\": true, \"level\": \"hard\"}} }"
NONE="{ $base }"

echo "=== LSP governance diagnostics name their rule (A43(d)) ==="

probe lg00 "$NONE" "$TODO_PROG"
if grep -q '"init": true' <<<"$OUT"; then
    pass LG-00 "CONTROL: the LSP answers initialize over stdio"
    LSP_OK=1
else
    unm LG-00 "the LSP did not answer initialize: $(head -3 <<<"$OUT" | tr '\n' ' ')"
    LSP_OK=0
fi

if [ "$LSP_OK" -eq 1 ]; then
    if [ "$(count)" -eq 0 ]; then
        pass LG-00c "CONTROL: without the rule the document draws no governance diagnostic"
    else
        fail LG-00c "a config without the rule should draw no governance diagnostic" "$OUT"
    fi

    probe lg01 "$HARD" "$TODO_PROG"
    code=$(field 1 code); message=$(field 1 message)
    if [ "$code" = "code_quality.no_placeholders" ] && [ "$message" = 'Placeholder "TODO" found in code' ]; then
        pass LG-01 "a HARD block: code is the rule name, message is the block's first line"
    else
        fail LG-01 "a HARD block should name its rule and carry its own message" "$OUT"
    fi

    probe lg02 "$ADV" "$TODO_PROG"
    code=$(field 1 code)
    if [ "$code" = "code_quality.no_placeholders" ]; then
        pass LG-02 "an ADVISORY block: code is the rule name, without the configured value"
    else
        fail LG-02 "an ADVISORY block should name its rule without its value" "$OUT"
    fi

    probe lg03 "$SECRETS" "$SECRET_PROG"
    code=$(field 1 code)
    if [ "$code" = "code_quality.no_secrets" ]; then
        pass LG-03 "a whole-source block of another rule is named code_quality.no_secrets"
    else
        fail LG-03 "a secret block should be named code_quality.no_secrets" "$OUT"
    fi
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
