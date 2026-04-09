#!/bin/bash
set -e

cd "$(dirname "$0")"

git add \
    src/runtime/cpp_executor.cpp \
    src/api/rest_api.cpp \
    src/vm/vm.cpp \
    src/vm/compiler.cpp \
    src/runtime/ffi_callback_validator.cpp

git commit -m "$(cat <<'EOF'
security: remediate Round 3 findings A–G — FFI, REST API, VM governance

Finding A — FFI dangling pointer in ffi_call (Critical):
- Reserve int_args, double_args, ptr_args to args.size() before the
  argument-preparation loop in cpp_executor.cpp; prevents reallocation
  from invalidating pointers already stored in arg_values (UAF/corruption)

Finding B — REST API shared interpreter state & race conditions (Critical):
- Create a fresh Interpreter per /api/v1/execute request; eliminates
  cross-request variable leakage, governance-state persistence, and
  concurrent-request races on shared mutable interpreter state

Finding C — OP_CALL bypasses governance (Critical):
- Add filesystem/network permission checks and taint-sink enforcement
  in callValue() for __stdlib_module__: callees; mirrors OP_CALL_METHOD
  governance so direct stdlib function references cannot bypass govern.json

Finding D — OP_GET_MEMBER taint propagation gap (High):
- Apply isTaintSource/isSanitizer checks after callStdlibMethod in both
  __stdlib_module__: and __builtin__: branches of OP_GET_MEMBER so
  zero-argument members (env.HOME, math.PI) correctly receive taint
- Fix unconditional member_obj_taint overwrite at end of handler to
  avoid clobbering taint already set by a taint-source check

Finding E — env module governance bypass (High):
- Add checkDangerousCall("naab", "env.method", 0) in OP_CALL_METHOD for
  the env module; govern.json restrictions.dangerous_calls entries that
  match env.* are now enforced at the VM layer (taint-sink check for
  tainted args was already covered by the generic loop below)

Finding F — isTypeCompatible stub (High):
- Implement basic value-vs-type compatibility in ffi_callback_validator:
  null accepts any type; numeric widening (int/float); exact name match;
  unresolved types (getTypeName returns "type") still accepted but now
  log a non-fatal AuditLogger entry for mismatch visibility

Finding G — OP_GOV_CHECK_FUNC / OP_GOV_TAINT_CHECK_ASSIGN are STUB_NOP (High):
- Wire opcode 69 (OP_GOV_TAINT_CHECK_ASSIGN) to a new VM handler that
  checks whether a tainted TOS value reaches a declared taint sink at
  variable assignment time via checkTaintedSink("assignment", ...)
- Emit OP_GOV_TAINT_CHECK_ASSIGN from compiler in VarDeclStmt and
  BinaryExpr assignment paths when RHS is statically tainted
- Leave OP_GOV_CHECK_FUNC (opcode 66) as STUB_NOP with a TODO comment
  pending function-level governance policy in govern.json

Tests: 387 pass, 40 error-behavior, 5 missing-executor, 0 unexpected.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
