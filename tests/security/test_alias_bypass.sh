#!/usr/bin/env bash
# test_alias_bypass.sh — Dangerous function alias detection tests
#
# Verifies that the governance engine catches variable aliasing of dangerous
# functions (e.g., s = os.system; s("rm")) and Python from-import patterns.
#
# Run: bash tests/security/test_alias_bypass.sh

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB_GOV="$LANG_DIR/build/naab-gov"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"

if [ ! -x "$NAAB_GOV" ]; then
    echo "SKIP: naab-gov not built"
    exit 0
fi

CFG="$TMPDIR/alias_test_config.json"
cat > "$CFG" <<'EOF'
{
    "version": "3.0", "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "code_injection": {"level": "hard"}
    }
}
EOF
trap "rm -f $CFG" EXIT

check_blocked() {
    local desc="$1" lang="$2" code="$3"
    local output
    output=$(echo "$code" | "$NAAB_GOV" check --language "$lang" --config "$CFG" 2>/dev/null)
    if echo "$output" | grep -q '"blocked": true'; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected blocked)"
        echo "        Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

check_not_blocked() {
    local desc="$1" lang="$2" code="$3"
    local output
    output=$(echo "$code" | "$NAAB_GOV" check --language "$lang" --config "$CFG" 2>/dev/null)
    if echo "$output" | grep -q '"blocked": false'; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected NOT blocked)"
        echo "        Got: $(echo "$output" | head -3)"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== Dangerous Function Alias Detection Tests ==="
echo ""

# ─── Python: Direct assignment aliases ──────────────────────────────────

echo "--- Python: Direct assignment aliases ---"

check_blocked "os.system alias via s = os.system; s(cmd)" \
    python 'import os; s = os.system; s("rm")'

check_blocked "os.system alias via fn = os.system; fn(cmd)" \
    python 'import os
fn = os.system
fn("rm -rf /")'

check_blocked "subprocess.Popen alias" \
    python 'import subprocess
run = subprocess.Popen
run(["rm", "-rf", "/"])'

check_blocked "subprocess.call alias" \
    python 'import subprocess
do_it = subprocess.call
do_it("rm", shell=True)'

check_blocked "eval alias" \
    python 'fn = eval
fn("__import__(\"os\").system(\"rm\")")'

check_blocked "exec alias" \
    python 'run_code = exec
run_code("import os; os.system(\"rm\")")'

check_blocked "getattr alias" \
    python 'lookup = getattr
lookup(os, "system")("rm")'

echo ""

# ─── Python: from-import aliases ────────────────────────────────────────

echo "--- Python: from-import aliases ---"

check_blocked "from os import system; system(cmd)" \
    python 'from os import system
system("rm -rf /")'

check_blocked "from os import system as s; s(cmd)" \
    python 'from os import system as s
s("rm -rf /")'

check_blocked "from subprocess import Popen; Popen(cmd)" \
    python 'from subprocess import Popen
Popen(["rm", "-rf", "/"])'

check_blocked "from subprocess import call as do_it; do_it(cmd)" \
    python 'from subprocess import call as do_it
do_it("rm", shell=True)'

check_blocked "from pickle import loads as decode; decode(data)" \
    python 'from pickle import loads as decode
decode(user_data)'

echo ""

# ─── Python: False positive avoidance ───────────────────────────────────

echo "--- Python: False positive avoidance ---"

check_not_blocked "safe code (no alias, no dangerous call)" \
    python 'x = 42
print(x)'

check_not_blocked "re.compile is not dangerous compile" \
    python 'import re
pat = re.compile
pat("[0-9]+")'

check_not_blocked "from os import path (safe function)" \
    python 'from os import path
result = path.join("/tmp", "file.txt")'

check_not_blocked "from os import getcwd (safe function)" \
    python 'from os import getcwd
cwd = getcwd()'

check_not_blocked "safe variable name 'system' with no os.system ref" \
    python 'system = "linux"
print(system)'

echo ""

# ─── Python: Module aliasing ───────────────────────────────────────────

echo "--- Python: Module aliasing (import X as Y) ---"

check_blocked "import os as o; o.system(cmd)" \
    python 'import os as o
o.system("rm -rf /")'

check_blocked "import subprocess as sp; sp.Popen(cmd)" \
    python 'import subprocess as sp
sp.Popen(["rm", "-rf", "/"])'

check_blocked "module alias + func alias combined" \
    python 'import subprocess as sp
fn = sp.Popen
fn(["rm", "-rf", "/"])'

check_not_blocked "import json as j (safe module)" \
    python 'import json as j
data = j.loads("{}")'

echo ""

# ─── Python: Star imports ──────────────────────────────────────────────

echo "--- Python: Star imports ---"

check_blocked "from os import *; system(cmd)" \
    python 'from os import *
system("rm -rf /")'

check_blocked "from subprocess import *; Popen(cmd)" \
    python 'from subprocess import *
Popen(["rm", "-rf", "/"])'

check_not_blocked "from os import *; safe function" \
    python 'from os import *
cwd = getcwd()'

echo ""

# ─── Python: Multi-import ──────────────────────────────────────────────

echo "--- Python: Multi-import ---"

check_blocked "from os import path, system; system(cmd)" \
    python 'from os import path, system
system("rm -rf /")'

check_blocked "from os import system as s, popen as p; s(cmd)" \
    python 'from os import system as s, popen as p
s("rm -rf /")'

echo ""

# ─── Python: Transitive alias chains ───────────────────────────────────

echo "--- Python: Transitive alias chains ---"

check_blocked "2-level chain: a = os.system; b = a; b(cmd)" \
    python 'import os
a = os.system
b = a
b("rm -rf /")'

check_blocked "3-level chain: a = os.system; b = a; c = b; c(cmd)" \
    python 'import os
a = os.system
b = a
c = b
c("rm -rf /")'

echo ""

# ─── Python: Dict/list indirection ─────────────────────────────────────

echo "--- Python: Dict/list indirection ---"

check_blocked "dict subscript: d[key] = os.system; d[key](cmd)" \
    python 'import os
d = {}
d["fn"] = os.system
d["fn"]("rm -rf /")'

check_blocked "list literal: fns = [os.system]; fns[0](cmd)" \
    python 'import os
fns = [os.system]
fns[0]("rm -rf /")'

check_blocked "dict with subprocess.Popen" \
    python 'import subprocess
handlers = {}
handlers["run"] = subprocess.Popen
handlers["run"](["rm"])'

check_not_blocked "dict with safe values only" \
    python 'handlers = {}
handlers["greet"] = print
handlers["greet"]("hello")'

echo ""

# ─── Python: Reflection access ─────────────────────────────────────────

echo "--- Python: Reflection access ---"

check_blocked "globals() dict access" \
    python 'globals()["eval"]("code")'

check_blocked "locals() dict access" \
    python 'locals()["exec"]("code")'

check_blocked "vars() dict access" \
    python 'vars()["eval"]("code")'

check_blocked "__builtins__ bracket access" \
    python '__builtins__["eval"]("code")'

check_blocked "__builtins__.__dict__ access" \
    python '__builtins__.__dict__["eval"]("code")'

check_blocked "os.__dict__ function extraction" \
    python 'import os
fn = os.__dict__["system"]
fn("rm")'

echo ""

# ─── JavaScript ─────────────────────────────────────────────────────────

echo "--- JavaScript aliases ---"

check_blocked "eval alias in JS" \
    javascript 'const fn = eval; fn("alert(1)")'

check_blocked "Function alias in JS" \
    javascript 'const create = Function; create("return 1")()'

check_not_blocked "safe JS code (no alias)" \
    javascript 'const x = 42; console.log(x)'

echo ""

# ─── Go ─────────────────────────────────────────────────────────────────

echo "--- Go aliases ---"

check_blocked "exec.Command alias in Go" \
    go 'import "os/exec"
fn := exec.Command
fn("rm", "-rf", "/")'

echo ""

# ─── Ruby ───────────────────────────────────────────────────────────────

echo "--- Ruby aliases ---"

check_blocked "system alias in Ruby" \
    ruby 'run = system
run("rm -rf /")'

echo ""

# ─── PHP ────────────────────────────────────────────────────────────────

echo "--- PHP aliases ---"

check_blocked "system alias in PHP" \
    php 'fn = system
fn("rm -rf /")'

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] || exit 1
