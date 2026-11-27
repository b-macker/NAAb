#!/usr/bin/env bash
# Scanner/Governance False-Positive Fix Verification Tests
# Fix 1: assigned_never_read — polyglot bindings preserved
# Fix 2: information_disclosure — bare env skip for NAAb
# Fix 3: vcs_secret_extraction — require has_vcs prerequisite
# Fix 4: no_pii — skip all-same-digit credit card matches
# Fix 5: hardcoded_return_value — function-name exemption
#
# Registered in run-all-tests.sh by Bundle 19 PR 4 (A223(b)): it had not been
# since it landed (f01391df), and its behavioural arms could not fail -- each
# asserted only that something was NOT flagged. Now each has a control that
# fires through the same probe (T6c..T10c), and the two whose probe never
# reached the check go through one that does:
#   T7  checkInfoDisclosure() is applied to polyglot blocks, never to a NAAb
#       program's own text, so running a NAAb file could not flag `use env`
#       whatever the exemption did; `naab-gov check --language naab` reaches it.
#   T8  vcs_secret_extraction is a block check the C++ scanner (`--scan`)
#       does not run at all (no output names it); `naab-gov check` reaches it.
# The config is unsigned, so the trust store is isolated (an installed key
# would refuse it at load and every "not flagged" arm would pass on that).
#
# Kill table (mutants of src/scanner/ and src/runtime/governance_checks.cpp,
# run by hand):
#   X1 the binding extraction in stripPolyglotBlocks dropped   -> T6
#   X2 checkInfoDisclosure adds the bare-env pattern for naab  -> T7
#   X3 checkVcsSecretExtraction's has_vcs prerequisite dropped  -> T8
#   X4 checkPii's all-same-digit skip dropped                   -> T9
#   X5 hardcoded_return_value's *default* name exemption gone   -> T10
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
GOV="$SCRIPT_DIR/../../build/naab-gov"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/naab_fp.XXXXXX")"
# A failed mktemp leaves $WORK_DIR EMPTY, and every "$WORK_DIR/x" write below then
# rebases onto the filesystem ROOT. That is how a stray govern.json reached /
# and silently governed every later run on the machine. Fail loudly instead.
[ -n "$WORK_DIR" ] && [ -d "$WORK_DIR" ] || { echo "FATAL: could not create work dir" >&2; exit 1; }
cleanup() { teardown_isolated_trust; rm -rf "$WORK_DIR"; }
trap cleanup EXIT

PASS=0
FAIL=0

pass() { echo "PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "FAIL: $1"; [[ -n "${2:-}" ]] && echo "  $2"; FAIL=$((FAIL+1)); }

# ── Source Verification Tests ─────────────────────────────────────────────────

echo "=== Source Verification ==="

SRC_SCANNER="$SCRIPT_DIR/../../src/scanner/scanner.cpp"
SRC_GOVCHECK="$SCRIPT_DIR/../../src/runtime/governance_checks.cpp"
SRC_LANGNAAB="$SCRIPT_DIR/../../src/scanner/checks_lang_naab.cpp"

# T1: stripPolyglotBlocks preserves binding list content
if grep -q 'binding_pat' "$SRC_SCANNER"; then
    pass "T1: stripPolyglotBlocks has binding extraction regex"
else
    fail "T1: stripPolyglotBlocks missing binding extraction"
fi

# T2: checkInfoDisclosure skips bare \benv\b for naab language
if grep -q 'language != "naab"' "$SRC_GOVCHECK"; then
    pass "T2: checkInfoDisclosure skips bare env for naab"
else
    fail "T2: checkInfoDisclosure missing naab env skip"
fi

# T3: checkVcsSecretExtraction requires has_vcs before checking signals
if grep -q '!has_vcs' "$SRC_GOVCHECK"; then
    pass "T3: checkVcsSecretExtraction has has_vcs prerequisite"
else
    fail "T3: checkVcsSecretExtraction missing has_vcs guard"
fi

# T4: checkPii skips all-same-digit credit card matches
if grep -q 'all_same.*continue' "$SRC_GOVCHECK"; then
    pass "T4: checkPii skips all-same-digit sequences"
else
    fail "T4: checkPii missing all-same-digit skip"
fi

# T5: hardcoded_return_value skips functions named *default*
if grep -q 'func_name.find("default")' "$SRC_LANGNAAB"; then
    pass "T5: hardcoded_return_value has function-name exemption"
else
    fail "T5: hardcoded_return_value missing function-name exemption"
fi

# ── Behavioral Tests ──────────────────────────────────────────────────────────

echo "=== Behavioral Tests ==="

# Minimal govern.json for scanner tests
cat > "$WORK_DIR/govern.json" << 'JSON'
{
    "version": "4.0",
    "mode": "enforce",
    "scanner": { "version": "1.0", "mode": "enforce",
        "code_quality": { "assigned_never_read": { "enabled": true, "level": "advisory" } },
        "lang_naab": { "hardcoded_return_value": { "enabled": true, "level": "advisory" } }
    },
    "restrictions": {
        "information_disclosure": { "enabled": true, "level": "advisory", "block_env_dump": true },
        "vcs_secret_extraction": { "enabled": true, "level": "advisory" }
    },
    "code_quality": {
        "no_pii": { "enabled": true, "level": "advisory", "detect_credit_card": true }
    }
}
JSON

# T6: Polyglot binding — variable used only in <<python[x]>> should NOT be flagged
cat > "$WORK_DIR/t6_binding.naab" << 'NAAB'
use math

fn compute(data) {
    let x = math.abs(data)
    let result = <<python[x]
x * 2
>>
    return result
}

main {
    print(compute(5))
}
NAAB

# One predicate for T6 and its control. The scan prints the rule id and the
# variable on separate lines, so the first version's `assigned_never_read.*x`
# could never match: T6 passed whatever the scanner did (X1 survived it).
x_flagged() { grep -q "Variable 'x' is assigned but never read" <<<"$1"; }
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t6_binding.naab naab 2>&1)
if x_flagged "$OUTPUT"; then
    fail "T6: polyglot binding var 'x' falsely flagged as assigned_never_read" "$OUTPUT"
else
    pass "T6: polyglot binding var 'x' not flagged"
fi
sed 's/let result = <<python\[x\]/let result = <<python/' "$WORK_DIR/t6_binding.naab" > "$WORK_DIR/t6c_unbound.naab"
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t6c_unbound.naab naab 2>&1)
if x_flagged "$OUTPUT"; then
    pass "T6c: CONTROL: the same 'x' NOT bound into the block is flagged"
else
    fail "T6c: CONTROL: an unread, unbound variable was not flagged -- T6 proves nothing" "$OUTPUT"
fi

# T7: NAAb's `env` is a stdlib module, not the shell's dump-everything command,
# so a NAAb block naming it is not information_disclosure. Asked of the check
# itself (naab-gov check), which a NAAb program's own text never reaches.
# rules LANG CODE -> the rules naab-gov check reports, "ERROR" if it gave no JSON
rules() {
    local out
    out=$(printf '%s\n' "$2" | "$GOV" check --language "$1" --config "$WORK_DIR/govern.json" 2>/dev/null)
    printf '%s' "$out" | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
except ValueError:
    print("ERROR"); sys.exit(0)
print(" ".join(sorted({v.get("rule", "") for v in d.get("violations", [])})))'
}
# `use env` names the module bare: the dump pattern is \benv\b not followed
# by a dot, so env.get_args() alone could never match it (X2 survived that).
R=$(rules naab 'use env
let args = env.get_args()')
if [ "$R" != "ERROR" ] && ! grep -q 'information_disclosure' <<<"$R"; then
    pass "T7: NAAb's env module is not information_disclosure"
else
    fail "T7: NAAb env flagged, or no JSON" "$R"
fi
R=$(rules shell 'env')
R2=$(rules naab 'let a = os.environ')
if grep -q 'information_disclosure' <<<"$R" && grep -q 'information_disclosure' <<<"$R2"; then
    pass "T7c: CONTROL: a shell block's bare env, and os.environ even in NAAb, are flagged"
else
    fail "T7c: CONTROL: the check did not fire (shell: $R; naab os.environ: $R2) -- T7 proves nothing"
fi

# T8: reading a file and hashing it, with no VCS command, is not
# vcs_secret_extraction (the has_vcs prerequisite). A block check, asked of
# naab-gov check; the C++ scanner `--scan` ran before does not run it.
# Three of the four signals and not the VCS one -- history (a .log file),
# search (open/read), secret (key) -- so only the has_vcs prerequisite keeps
# it unflagged: three with a secret is an advisory finding (X3 survived the
# first fixture, which had two).
R=$(rules python 'content = open("audit.log").read()
key = hashlib.sha256(content.encode()).hexdigest()')
if [ "$R" != "ERROR" ] && ! grep -q 'vcs_secret_extraction' <<<"$R"; then
    pass "T8: file read + key without git not flagged"
else
    fail "T8: flagged without git, or no JSON" "$R"
fi
R=$(rules python 'import subprocess
out = subprocess.run(["git", "log", "-p"], capture_output=True).stdout
key = out.decode()')
if grep -q 'vcs_secret_extraction' <<<"$R"; then
    pass "T8c: CONTROL: the same with git log -p is flagged"
else
    fail "T8c: CONTROL: git log -p was not flagged -- T8 proves nothing" "$R"
fi

# T9: "0000000000000000" should NOT trigger no_pii credit card
cat > "$WORK_DIR/t9_zeros.naab" << 'NAAB'
fn get_genesis() {
    let hash = "0000000000000000"
    return hash
}

main {
    print(get_genesis())
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" t9_zeros.naab 2>&1)
if echo "$OUTPUT" | grep -qi 'Credit Card\|no_pii'; then
    fail "T9: all-zero string falsely triggered credit card PII" "$OUTPUT"
else
    pass "T9: all-zero string not flagged as credit card"
fi
# A test card number, built from fragments so the repository never holds it.
CARD="4111""1111""1111""1111"
printf 'fn get_card() {\n    let c = "%s"\n    return c\n}\n\nmain {\n    print(get_card())\n}\n' "$CARD" > "$WORK_DIR/t9c_card.naab"
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" t9c_card.naab 2>&1)
if grep -qi 'Credit Card\|no_pii' <<<"$OUTPUT"; then
    pass "T9c: CONTROL: a card-shaped number is flagged by the same run"
else
    fail "T9c: CONTROL: no_pii did not fire -- T9 proves nothing" "$OUTPUT"
fi

# T10: fn get_defaults() returning hardcoded dict should NOT be flagged
cat > "$WORK_DIR/t10_defaults.naab" << 'NAAB'
fn get_defaults() {
    return { "enabled": true, "retries": 0 }
}

main {
    let cfg = get_defaults()
    print(cfg)
}
NAAB

OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t10_defaults.naab naab 2>&1)
if echo "$OUTPUT" | grep -q 'hardcoded_return_value'; then
    fail "T10: get_defaults() falsely flagged for hardcoded_return_value" "$OUTPUT"
else
    pass "T10: get_defaults() not flagged"
fi
sed 's/get_defaults/get_settings/g' "$WORK_DIR/t10_defaults.naab" > "$WORK_DIR/t10c_settings.naab"
OUTPUT=$(cd "$WORK_DIR" && "$NAAB" --scan t10c_settings.naab naab 2>&1)
if grep -q 'hardcoded_return_value' <<<"$OUTPUT"; then
    pass "T10c: CONTROL: the same function under another name is flagged"
else
    fail "T10c: CONTROL: hardcoded_return_value did not fire -- T10 proves nothing" "$OUTPUT"
fi

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
