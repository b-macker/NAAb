#!/usr/bin/env bash
# Commit test script fixes for V-GOV-007 fail-closed default.
# All test scripts that write naab scripts to temp dirs in $HOME
# now create a govern.json (mode:off) to satisfy require_governance.

set -euo pipefail
cd "$(dirname "$0")"

echo "=== Making test scripts executable ==="
chmod +x tests/cli/test_cli_flags.sh
chmod +x tests/cli/test_gc_flags.sh
chmod +x tests/cli/test_pipe_mode.sh
chmod +x tests/cli/test_governance_exit_codes.sh
chmod +x tests/property/test_governance_transparency.sh
chmod +x tests/security/test_vm_gc_rt008.sh

echo ""
echo "=== Verifying test fixes ==="
echo ""

echo "--- CLI Flag Tests ---"
bash tests/cli/test_cli_flags.sh 2>&1 | tail -3

echo ""
echo "--- GC Flag Tests ---"
bash tests/cli/test_gc_flags.sh 2>&1 | tail -3

echo ""
echo "--- Pipe Mode Tests ---"
bash tests/cli/test_pipe_mode.sh 2>&1 | tail -3

echo ""
echo "--- Governance Exit Code Tests ---"
bash tests/cli/test_governance_exit_codes.sh 2>&1 | tail -3

echo ""
echo "--- Governance Transparency (property) ---"
bash tests/property/test_governance_transparency.sh 2>&1 | tail -3

echo ""
echo "--- VM GC RT008 ---"
bash tests/security/test_vm_gc_rt008.sh 2>&1 | tail -3

echo ""
echo "=== Committing ==="
git add \
    tests/cli/test_cli_flags.sh \
    tests/cli/test_gc_flags.sh \
    tests/cli/test_pipe_mode.sh \
    tests/cli/test_governance_exit_codes.sh \
    tests/property/test_governance_transparency.sh \
    tests/security/test_vm_gc_rt008.sh \
    run-all-tests.sh \
    commit-test-fixes-r9.sh

git commit -m "$(cat <<'EOF'
fix(tests): update test scripts for V-GOV-007 fail-closed default

All test scripts that write naab scripts to temp dirs in $HOME now
create a govern.json (mode:off) in those dirs, satisfying the new
fail-closed require_governance default without affecting test intent.

- test_cli_flags.sh, test_gc_flags.sh, test_pipe_mode.sh: add govern.json to temp dir
- test_governance_exit_codes.sh: T1/T2 use --no-governance (explicitly testing no-governance behavior)
- test_governance_transparency.sh: add govern.json (mode:off) to TMPDIR
- test_vm_gc_rt008.sh: add govern.json (mode:off) to WORK_DIR

Result: 376 pass, 40 error-behavior, 16 missing-executor, 0 unexpected (432 total)

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

echo ""
echo "Done. Test fixes committed."
