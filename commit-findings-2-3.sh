#!/bin/bash
set -e

cd "$(dirname "$0")"

# Rebuild naab-gov to ensure it's up to date
echo "Building naab-gov..."
cmake --build build --target naab-gov -j4

git add \
    include/naab/governance.h \
    src/runtime/governance_engine.cpp \
    src/runtime/governance_checks.cpp \
    src/runtime/polyglot_async_executor.cpp

git commit -m "$(cat <<'EOF'
security: remediate findings 2 & 3 — polyglot pattern DB and async sandbox context

Finding 2 — incomplete polyglot filesystem pattern DB:
- Add FILESYSTEM_IMPORT_PATTERNS covering Python open(), io.open(), pathlib,
  os.listdir/walk/remove/unlink/rename/makedirs/path, shutil, glob, Node.js
  fs module, and shell rm -r/-f
- Add GovernanceEngine::checkFilesystemImports() mirroring checkNetworkImports()
  pattern; enforces only when capabilities.filesystem is restricted (not the
  default "write" mode — no regression for existing configs)
- Call checkFilesystemImports() in checkPolyglotBlock() immediately after
  checkNetworkImports()

Finding 3 — thread_local sandbox context lost in async worker threads:
- Before each ThreadPool::enqueue() call in polyglot_async_executor.cpp,
  capture the calling thread's active SandboxConfig by value
- Install a ScopedSandbox from the snapshot as the first action inside each
  worker lambda, so checkFileSandbox() sees a non-null getCurrent() and
  enforces access control on async/parallel polyglot blocks
- Applied uniformly to all 6 executeAsync overloads: Python, JavaScript,
  C++, Rust, C#, Shell

Tests: 387 pass, 40 error-behavior, 5 missing-executor, 0 unexpected.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"

git push
