#!/bin/bash
# SessionStart hook: prepare a fresh remote container to build and test NAAb.
# Installs the build dependencies that base images commonly lack:
#   - git submodules (external/abseil-cpp, fmt, spdlog, tomlplusplus, ...)
#   - libcurl dev headers (package manager + http stdlib)
#   - pybind11 (embedded Python executor — without it <<python>> expression
#     blocks evaluate to null and ~10 tests XFAIL as missing-executor)
# Idempotent: every step checks before acting, so cached containers skip fast.
#
# Then, in every session (local too), one line saying whether the code map
# (tools/codemap) can answer here and how fresh it is. Hook output reaches the
# session's context, so a session learns before relying on the tool whether the
# tool is working; it builds nothing (an index takes about 2 minutes, coverage
# about 45). Whatever goes wrong there, the hook still exits 0.
set -euo pipefail

codemap_status() {
  local dir="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
  local line=""
  if command -v python3 >/dev/null 2>&1; then
    if command -v timeout >/dev/null 2>&1; then
      line="$(timeout 20 python3 "$dir/tools/codemap/codemap.py" --repo "$dir" status 2>/dev/null)" || line=""
    else
      line="$(python3 "$dir/tools/codemap/codemap.py" --repo "$dir" status 2>/dev/null)" || line=""
    fi
  fi
  case "$line" in
    codemap:*) printf '%s\n' "$line" ;;
    *) echo "codemap: status UNMEASURABLE (the status query failed or python3 is missing) -- do every investigation gate by hand." ;;
  esac
}

# The installs are only needed on remote (Claude Code on the web) containers.
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  codemap_status || true
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
codemap_status || true
