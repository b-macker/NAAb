#!/bin/bash
# SessionStart hook: prepare a fresh remote container to build and test NAAb.
# Installs the build dependencies that base images commonly lack:
#   - git submodules (external/abseil-cpp, fmt, spdlog, tomlplusplus, ...)
#   - libcurl dev headers (package manager + http stdlib)
#   - pybind11 (embedded Python executor — without it <<python>> expression
#     blocks evaluate to null and ~10 tests XFAIL as missing-executor)
# Idempotent: every step checks before acting, so cached containers skip fast.
set -euo pipefail

# Only needed on remote (Claude Code on the web) containers.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "$CLAUDE_PROJECT_DIR"

# 1. Git submodules (external/ trees are empty on fresh shallow clones)
if [ ! -f external/fmt/CMakeLists.txt ]; then
  git submodule update --init --depth 1
fi

# 2. libcurl dev headers (cmake hard-requires curl/curl.h via naab_packages)
if ! [ -f /usr/include/x86_64-linux-gnu/curl/curl.h ] && ! [ -f /usr/include/curl/curl.h ]; then
  APT="apt-get"
  command -v sudo >/dev/null 2>&1 && [ "$(id -u)" != "0" ] && APT="sudo -n apt-get"
  $APT update -qq || true
  DEBIAN_FRONTEND=noninteractive $APT install -y -qq libcurl4-openssl-dev
fi

# 3. pybind11 for the embedded Python executor (cmake probes `python3 -m pybind11`)
if ! python3 -c "import pybind11" >/dev/null 2>&1; then
  python3 -m pip install --quiet pybind11 \
    || python3 -m pip install --quiet --break-system-packages pybind11
fi

echo "NAAb session setup complete (submodules, libcurl-dev, pybind11)."
