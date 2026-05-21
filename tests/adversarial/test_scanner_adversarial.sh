#!/usr/bin/env bash
# test_scanner_adversarial.sh — Adversarial testing of governance scanner obfuscation hardening
#
# Tests ~60 evasion vectors across all 10 polyglot languages:
#   - Python obfuscation (globals, __builtins__, chr chains, base64+exec, ctypes, etc.)
#   - JavaScript obfuscation (atob+eval, Function+fromCharCode, Reflect, dynamic import)
#   - Go/Rust/C++/C#/Ruby/PHP/Nim dangerous calls
#   - Shell obfuscation (hex/octal escapes, source, IFS)
#   - Co-occurrence engine (2-signal advisory, 3-signal block, 4-signal block)
#
# Each test creates a temp .naab file, runs it under governance, and checks the exit code.
# Tests marked SHOULD_BLOCK expect non-zero exit (governance blocks the code).
# Tests marked SHOULD_PASS expect zero exit (no false positive).

set -u

LANG_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB_BIN="$LANG_DIR/build/naab-lang"
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
TMPDIR="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
PASS=0
FAIL=0
TOTAL=0
FAILURES=()

if [ ! -f "$NAAB_BIN" ]; then
    echo "Error: naab-lang not found at $NAAB_BIN"
    echo "Run: cd build && make naab-lang -j4"
    exit 1
fi

# run_test "TEST_NAME" "SHOULD_BLOCK|SHOULD_PASS" "NAAB_CODE"
run_test() {
    local name="$1"
    local expect="$2"
    local code="$3"
    TOTAL=$((TOTAL + 1))

    local tmpfile="$TEST_DIR/.adversarial_test_$$.naab"
    echo "$code" > "$tmpfile"

    local output
    output=$(timeout 15 "$NAAB_BIN" run "$tmpfile" 2>&1)
    local exit_code=$?
    rm -f "$tmpfile"

    if [ "$expect" = "SHOULD_BLOCK" ]; then
        if [ $exit_code -ne 0 ]; then
            PASS=$((PASS + 1))
            # echo "  PASS: $name (blocked, exit=$exit_code)"
        else
            FAIL=$((FAIL + 1))
            FAILURES+=("MISS: $name — expected BLOCK but got PASS")
            echo "  MISS: $name — expected BLOCK but PASSED (evasion succeeded!)"
        fi
    elif [ "$expect" = "SHOULD_PASS" ]; then
        if [ $exit_code -eq 0 ]; then
            PASS=$((PASS + 1))
            # echo "  PASS: $name (allowed, no false positive)"
        else
            FAIL=$((FAIL + 1))
            FAILURES+=("FP:   $name — expected PASS but got BLOCK (false positive)")
            echo "  FP:   $name — expected PASS but got BLOCK (exit=$exit_code)"
            echo "        Output: $(echo "$output" | head -3)"
        fi
    fi
}

echo "================================================================"
echo "  NAAb Adversarial Scanner Tests"
echo "================================================================"
echo ""

# ╔══════════════════════════════════════════════════════════════════╗
# ║ PYTHON OBFUSCATION VECTORS                                      ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Python Obfuscation ---"

run_test "PY-01: globals()[] dict access" "SHOULD_BLOCK" \
'main {
    let x = <<python
x = globals()["__builtins__"]
print(x)
>>
}'

run_test "PY-02: __builtins__ direct access" "SHOULD_BLOCK" \
'main {
    let x = <<python
m = __builtins__.__import__("json")
print(m)
>>
}'

run_test "PY-03: __import__ dynamic import" "SHOULD_BLOCK" \
'main {
    let x = <<python
m = __import__("json")
print(m.dumps({}))
>>
}'

run_test "PY-04: __loader__ module loading" "SHOULD_BLOCK" \
'main {
    let x = <<python
spec = __loader__.find_module("json")
print(spec)
>>
}'

run_test "PY-05: types.ModuleType creation" "SHOULD_BLOCK" \
'main {
    let x = <<python
import types
m = types.ModuleType("fake")
print(m)
>>
}'

run_test "PY-06: chr() chain (3+)" "SHOULD_BLOCK" \
'main {
    let x = <<python
name = chr(111) + chr(115) + chr(46)
print(name)
>>
}'

run_test "PY-07: compile() with eval mode" "SHOULD_BLOCK" \
'main {
    let x = <<python
code = compile("1+1", "", "eval")
print(code)
>>
}'

run_test "PY-08: subprocess.Popen" "SHOULD_BLOCK" \
'main {
    let x = <<python
import subprocess
p = subprocess.Popen(["echo", "hi"])
>>
}'

run_test "PY-09: subprocess with shell=True" "SHOULD_BLOCK" \
'main {
    let x = <<python
import subprocess
subprocess.run("echo hi", shell=True)
>>
}'

run_test "PY-10: ctypes.CDLL loading" "SHOULD_BLOCK" \
'main {
    let x = <<python
import ctypes
lib = ctypes.CDLL("libc.so.6")
>>
}'

run_test "PY-11: ctypes.cdll loading" "SHOULD_BLOCK" \
'main {
    let x = <<python
import ctypes
lib = ctypes.cdll.LoadLibrary("libc.so.6")
>>
}'

run_test "PY-12: getattr dynamic access" "SHOULD_BLOCK" \
'main {
    let x = <<python
obj = getattr(__builtins__, "__import__")
>>
}'

run_test "PY-13: importlib dynamic import" "SHOULD_BLOCK" \
'main {
    let x = <<python
import importlib
m = importlib.import_module("json")
>>
}'

run_test "PY-14: __subclasses__ introspection" "SHOULD_BLOCK" \
'main {
    let x = <<python
classes = object.__subclasses__()
print(len(classes))
>>
}'

run_test "PY-15: setattr alone (co-occurrence only)" "SHOULD_PASS" \
'main {
    let x = <<python
class X: pass
setattr(X, "run", lambda: None)
print("ok")
>>
    print(x)
}'

run_test "PY-16: __mro__ chain walking" "SHOULD_BLOCK" \
'main {
    let x = <<python
chain = str.__mro__
print(chain)
>>
}'

run_test "PY-17: __dict__[] alone (co-occurrence only)" "SHOULD_PASS" \
'main {
    let x = <<python
class X:
    secret = 42
v = X.__dict__["secret"]
print(v)
>>
    print(x)
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ PYTHON CO-OCCURRENCE ENGINE                                      ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Python Co-occurrence ---"

run_test "PY-CO-01: exec + base64 (2 signals = advisory)" "SHOULD_BLOCK" \
'main {
    let x = <<python
import base64
payload = base64.b64decode("cHJpbnQoMSk=")
exec(payload)
>>
}'

run_test "PY-CO-02: exec + base64 + __import__ (3 signals = SOFT)" "SHOULD_BLOCK" \
'main {
    let x = <<python
import base64
payload = base64.b64decode("aW1wb3J0IG9z")
m = __import__("base64")
exec(payload)
>>
}'

run_test "PY-CO-03: eval + chr chain + globals (3 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<python
name = chr(111) + chr(115) + chr(46)
g = globals()["__builtins__"]
eval(name)
>>
}'

run_test "PY-CO-04: compile + bytearray + ctypes (3 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<python
import ctypes
data = bytearray(b"print(1)")
code = compile(data.decode(), "", "exec")
>>
}'

run_test "PY-CO-05: exec + codecs + __loader__ + setattr (4 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<python
import codecs
payload = codecs.decode("cevag(1)", "rot_13")
spec = __loader__
setattr(spec, "x", 1)
exec(payload)
>>
}'

run_test "PY-CO-06: eval + binascii + __builtins__ + __mro__ (4 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<python
import binascii
data = binascii.unhexlify("7072696e742831290a")
b = __builtins__
chain = str.__mro__
eval("1")
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ JAVASCRIPT OBFUSCATION VECTORS                                  ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- JavaScript Obfuscation ---"

run_test "JS-01: dynamic import('child_process')" "SHOULD_BLOCK" \
"main {
    let x = <<javascript
import('child_process').then(m => m.exec('ls'))
>>
}"

run_test "JS-02: process.env access" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var key = process.env.SECRET_KEY
>>
}'

run_test "JS-03: vm.runInNewContext" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var vm = require("vm")
vm.runInNewContext("1+1")
>>
}'

run_test "JS-04: vm.runInThisContext" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var vm = require("vm")
vm.runInThisContext("process")
>>
}'

# JS co-occurrence
run_test "JS-CO-01: eval + atob (2 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var code = atob("YWxlcnQoMSk=")
eval(code)
>>
}'

run_test "JS-CO-02: Function + String.fromCharCode (2 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var code = String.fromCharCode(114,101,116,117,114,110)
var fn = Function(code)
>>
}'

run_test "JS-CO-03: eval + atob + Reflect (3 signals = SOFT)" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var code = atob("YWxlcnQoMSk=")
Reflect.apply(eval, null, [code])
>>
}'

run_test "JS-CO-04: setTimeout + unescape + constructor[] (3 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var code = unescape("%61%6c%65%72%74")
var fn = constructor["constructor"](code)
setTimeout(fn, 0)
>>
}'

run_test "JS-CO-05: Function + decodeURIComponent + Proxy (3 signals)" "SHOULD_BLOCK" \
'main {
    let x = <<javascript
var code = decodeURIComponent("%61%6c%65%72%74")
var p = new Proxy({}, {})
var fn = Function(code)
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ GO DANGEROUS CALLS                                               ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Go ---"

run_test "GO-01: exec.Command" "SHOULD_BLOCK" \
'main {
    let x = <<go
package main
import "os/exec"
func main() {
    cmd := exec.Command("ls", "-la")
    cmd.Run()
}
>>
}'

run_test "GO-02: syscall.Exec" "SHOULD_BLOCK" \
'main {
    let x = <<go
package main
import "syscall"
func main() {
    syscall.Exec("/bin/sh", []string{"sh"}, nil)
}
>>
}'

run_test "GO-03: unsafe.Pointer" "SHOULD_BLOCK" \
'main {
    let x = <<go
package main
import "unsafe"
func main() {
    var x int = 42
    p := unsafe.Pointer(&x)
    _ = p
}
>>
}'

run_test "GO-04: plugin.Open" "SHOULD_BLOCK" \
'main {
    let x = <<go
package main
import "plugin"
func main() {
    p, _ := plugin.Open("evil.so")
    _ = p
}
>>
}'

run_test "GO-05: os.ReadFile (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<go
package main
import "os"
func main() {
    data, _ := os.ReadFile("/etc/passwd")
    _ = data
}
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ RUST DANGEROUS CALLS                                             ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Rust ---"

run_test "RS-01: Command::new" "SHOULD_BLOCK" \
'main {
    let x = <<rust
use std::process::Command;
fn main() {
    Command::new("ls").arg("-la").spawn().unwrap();
}
>>
}'

run_test "RS-02: unsafe block" "SHOULD_BLOCK" \
'main {
    let x = <<rust
fn main() {
    unsafe {
        let p: *const i32 = std::ptr::null();
        let _ = *p;
    }
}
>>
}'

run_test "RS-03: libc FFI" "SHOULD_BLOCK" \
'main {
    let x = <<rust
extern crate libc;
fn main() {
    unsafe { libc::system(b"ls\0".as_ptr() as *const _); }
}
>>
}'

run_test "RS-04: std::fs (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<rust
use std::fs;
fn main() {
    let data = std::fs::read_to_string("/etc/passwd").unwrap();
}
>>
}'

run_test "RS-05: reqwest (network)" "SHOULD_BLOCK" \
'main {
    let x = <<rust
use reqwest;
fn main() {
    let resp = reqwest::blocking::get("http://evil.com").unwrap();
}
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ C++ DANGEROUS CALLS                                              ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- C++ ---"

run_test "CPP-01: system()" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <cstdlib>
int main() {
    system("rm -rf /");
    return 0;
}
>>
}'

run_test "CPP-02: popen()" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <cstdio>
int main() {
    FILE* f = popen("ls", "r");
    pclose(f);
    return 0;
}
>>
}'

run_test "CPP-03: dlopen()" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <dlfcn.h>
int main() {
    void* h = dlopen("libevil.so", RTLD_LAZY);
    return 0;
}
>>
}'

run_test "CPP-04: inline assembly" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
int main() {
    __asm__("int $0x80");
    return 0;
}
>>
}'

run_test "CPP-05: socket() (network)" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <sys/socket.h>
int main() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    return 0;
}
>>
}'

run_test "CPP-06: fopen() (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <cstdio>
int main() {
    FILE* f = fopen("/etc/passwd", "r");
    fclose(f);
    return 0;
}
>>
}'

run_test "CPP-07: fstream (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <fstream>
int main() {
    std::ifstream f("/etc/passwd");
    return 0;
}
>>
}'

run_test "CPP-08: exec() family" "SHOULD_BLOCK" \
'main {
    let x = <<cpp
#include <unistd.h>
int main() {
    execl("/bin/sh", "sh", NULL);
    return 0;
}
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ C# DANGEROUS CALLS                                               ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- C# ---"

run_test "CS-01: Process.Start" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
using System.Diagnostics;
class Program {
    static void Main() {
        Process.Start("cmd.exe", "/c dir");
    }
}
>>
}'

run_test "CS-02: unsafe block" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
class Program {
    static void Main() {
        unsafe {
            int* p = null;
        }
    }
}
>>
}'

run_test "CS-03: DllImport" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
using System.Runtime.InteropServices;
class Program {
    [DllImport("kernel32")]
    static extern void ExitProcess(int code);
}
>>
}'

run_test "CS-04: Reflection.Emit" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
using System.Reflection.Emit;
class Program {
    static void Main() {
        var ab = Reflection.Emit.AssemblyBuilder.DefineDynamicAssembly(null, 0);
    }
}
>>
}'

run_test "CS-05: System.Net (network)" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
using System.Net;
class Program {
    static void Main() {
        System.Net.WebClient wc = new System.Net.WebClient();
    }
}
>>
}'

run_test "CS-06: HttpClient (network)" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
class Program {
    static void Main() {
        var client = new HttpClient();
    }
}
>>
}'

run_test "CS-07: System.IO (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<csharp
class Program {
    static void Main() {
        System.IO.File.ReadAllText("/etc/passwd");
    }
}
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ RUBY DANGEROUS CALLS                                             ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Ruby ---"

run_test "RB-01: system()" "SHOULD_BLOCK" \
'main {
    let x = <<ruby
system("ls -la")
>>
}'

run_test "RB-02: eval()" "SHOULD_BLOCK" \
'main {
    let x = <<ruby
eval("puts 42")
>>
}'

run_test "RB-03: instance_eval" "SHOULD_BLOCK" \
'main {
    let x = <<ruby
obj = Object.new
obj.instance_eval { puts 42 }
>>
}'

run_test "RB-04: send() dynamic dispatch" "SHOULD_BLOCK" \
'main {
    let x = <<ruby
send(:puts, "hello")
>>
}'

run_test "RB-05: File.read (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<ruby
data = File.read("/etc/passwd")
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ PHP DANGEROUS CALLS                                              ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- PHP ---"

run_test "PHP-01: system()" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php system("ls"); ?>
>>
}'

run_test "PHP-02: exec()" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php exec("whoami"); ?>
>>
}'

run_test "PHP-03: shell_exec()" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php $out = shell_exec("id"); ?>
>>
}'

run_test "PHP-04: passthru()" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php passthru("ls -la"); ?>
>>
}'

run_test "PHP-05: eval()" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php eval("echo 42;"); ?>
>>
}'

run_test "PHP-06: curl_init (network)" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php $ch = curl_init("http://evil.com"); ?>
>>
}'

run_test "PHP-07: fopen (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php $f = fopen("/etc/passwd", "r"); ?>
>>
}'

run_test "PHP-08: file_get_contents (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<php
<?php $data = file_get_contents("/etc/passwd"); ?>
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ NIM DANGEROUS CALLS                                              ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Nim ---"

run_test "NIM-01: execProcess()" "SHOULD_BLOCK" \
'main {
    let x = <<nim
import osproc
let output = execProcess("ls")
echo output
>>
}'

run_test "NIM-02: startProcess()" "SHOULD_BLOCK" \
'main {
    let x = <<nim
import osproc
let p = startProcess("bash")
>>
}'

run_test "NIM-03: {.importc.} pragma" "SHOULD_BLOCK" \
'main {
    let x = <<nim
proc c_system(cmd: cstring): cint {.importc: "system", header: "<stdlib.h>".}
discard c_system("ls")
>>
}'

run_test "NIM-04: {.emit.} pragma" "SHOULD_BLOCK" \
'main {
    let x = <<nim
{.emit: "system(\"ls\");".}
>>
}'

run_test "NIM-05: readFile (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<nim
let data = readFile("/etc/passwd")
echo data
>>
}'

run_test "NIM-06: writeFile (filesystem)" "SHOULD_BLOCK" \
'main {
    let x = <<nim
writeFile("/tmp/evil.txt", "pwned")
>>
}'

run_test "NIM-07: HttpClient (network)" "SHOULD_BLOCK" \
'main {
    let x = <<nim
import httpclient
let client = newHttpClient()
>>
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ SHELL OBFUSCATION                                                ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- Shell ---"

run_test "SH-01: source command" "SHOULD_BLOCK" \
'main {
    let x = <<shell
source /etc/profile
echo "loaded"
>>
}'

run_test "SH-02: IFS manipulation" "SHOULD_BLOCK" \
'main {
    let x = <<shell
IFS=/ cmd args
>>
}'

run_test "SH-03: hex escape in dollar-quote" "SHOULD_BLOCK" \
"main {
    let x = <<shell
cmd=\$'\\x72\\x6d'
>>
}"

run_test "SH-04: octal escape in dollar-quote" "SHOULD_BLOCK" \
"main {
    let x = <<shell
cmd=\$'\\162\\155'
>>
}"

# ╔══════════════════════════════════════════════════════════════════╗
# ║ FALSE POSITIVE CHECKS (should PASS)                              ║
# ╚══════════════════════════════════════════════════════════════════╝
echo "--- False Positive Checks ---"

run_test "FP-01: Python legitimate math" "SHOULD_PASS" \
'main {
    let x = <<python
result = 2 + 3
print(result)
>>
    print(x)
}'

run_test "FP-02: Python single chr() (not a chain)" "SHOULD_PASS" \
'main {
    let x = <<python
c = chr(65)
print(c)
>>
    print(x)
}'

run_test "FP-03: Python base64 without exec" "SHOULD_PASS" \
'main {
    let x = <<python
import base64
data = base64.b64encode(b"hello")
print(data.decode())
>>
    print(x)
}'

run_test "FP-04: JS simple arithmetic" "SHOULD_PASS" \
'main {
    let x = <<javascript
var result = 2 + 3
result
>>
    print(x)
}'

run_test "FP-05: Python dict access (not __dict__[])" "SHOULD_PASS" \
'main {
    let x = <<python
d = {"key": "value"}
v = d["key"]
print(v)
>>
    print(x)
}'

# ╔══════════════════════════════════════════════════════════════════╗
# ║ SUMMARY                                                          ║
# ╚══════════════════════════════════════════════════════════════════╝
echo ""
echo "================================================================"
echo "  Adversarial Scanner Results"
echo "================================================================"
echo "  Total:  $TOTAL"
echo "  Pass:   $PASS"
echo "  Fail:   $FAIL"
echo "================================================================"

if [ ${#FAILURES[@]} -gt 0 ]; then
    echo ""
    echo "  Failures:"
    for f in "${FAILURES[@]}"; do
        echo "    $f"
    done
fi

echo ""
if [ $FAIL -eq 0 ]; then
    echo "  ALL TESTS PASSED"
    exit 0
else
    echo "  $FAIL TEST(S) FAILED"
    exit 1
fi
