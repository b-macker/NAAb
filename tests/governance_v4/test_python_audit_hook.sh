#!/usr/bin/env bash
# A7 fix: the Python polyglot audit hook — the sandbox's runtime call site
# inside CPython.
#
# WHY THIS SUITE EXISTS. Every other I/O path in NAAb asks the sandbox
# (file.read -> canRead, http.get -> canConnect, process.run ->
# canExecuteCommand). A polyglot block had no such call site: once control
# entered CPython there was nowhere to put the question, so polyglot governance
# was source-text-only. That is structurally defeated by reaching an
# already-cached module through the class graph (A7): no import happens, no
# restricted token appears, and the escape reads the filesystem. The audit hook
# supplies the missing call site — it fires at the OPERATION regardless of how
# the module reference was obtained.
#
# THE LOAD-BEARING TEST is AH-02: an OBFUSCATED read (no literal `open(`, no
# `os.`, no `__subclasses__` token) of a denied path. A source-text scanner
# cannot see it, so only a runtime gate can block it. Without AH-02 the suite
# would pass for the pre-existing source scanners and prove nothing about the
# hook. AH-01 (positive control) proves the block runs at all; AH-03 proves the
# hook DECIDES on policy rather than blanket-blocking (an allowed path must
# still read); AH-04 proves the runtime-path carve-out (the interpreter loading
# its own modules must not be denied, or every import breaks under a restrictive
# filesystem policy — and under mode:enforce the sandbox upgrades to `standard`,
# which refuses absolute paths, making the carve-out load-bearing not cosmetic).

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
NAAB="$ROOT/build/naab-lang"
PASS=0; FAIL=0
ok()  { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL [$1] $2"; echo "        $3"; FAIL=$((FAIL+1)); }

if [ ! -x "$NAAB" ]; then echo "naab-lang not built: $NAAB"; exit 1; fi
# Requires the embedded Python executor (a python block must return a value,
# not null). Skip cleanly where Python support is absent.
PVROBE=$(printf 'main { let v = <<python\n1+1\n>>\nprint(string(v)) }\n' > /tmp/_ah_probe.naab; "$NAAB" --no-governance /tmp/_ah_probe.naab 2>/dev/null | tail -1)
if [ "$PVROBE" != "2" ]; then echo "  SKIP: embedded Python executor unavailable (block returned '$PVROBE')"; exit 0; fi

W=$(mktemp -d)
trap 'rm -rf "$W"; rm -f /tmp/_ah_probe.naab' EXIT
echo "audit-hook-allowed-content" > "$W/ok.txt"

# filesystem mode:none — the block's own imports are allowed by the runtime-path
# carve-out, but user I/O outside sys.path is denied.
cat > "$W/deny.json" <<JSON
{ "version":"5.0","mode":"enforce","capabilities":{"filesystem":{"mode":"none"}},
  "languages":{"allowed":["python"],"per_language":{"python":{}}} }
JSON
# filesystem allowing the workdir — an obfuscated read of an allowed path must succeed.
cat > "$W/allow.json" <<JSON
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "languages":{"allowed":["python"],"per_language":{"python":{}}} }
JSON
# filesystem mode:read removes FS_WRITE but keeps FS_READ.
cat > "$W/readonly.json" <<JSON
{ "version":"5.0","mode":"enforce","security":{"sandbox_level":"elevated"},
  "capabilities":{"filesystem":{"mode":"read"}},
  "languages":{"allowed":["python"],"per_language":{"python":{}}} }
JSON

# Obfuscated escape reading an arbitrary path — no literal open(/os./subclasses token.
mk_read() { cat > "$1" <<PY
main {
    let r = <<python
    _sc = getattr("".__class__.__base__, "__subcla" + "sses__")()
    _imp = [c for c in _sc if c.__name__ == "BuiltinImporter"][0]
    _m = getattr(_imp, "load_" + "module")("o" + "s")
    _op = getattr(_m, "op" + "en"); _rd = getattr(_m, "re" + "ad")
    _fd = _op("$2", 0); _out = _rd(_fd, 64); getattr(_m,"cl"+"ose")(_fd)
    _out.decode().strip()
    >>
    print("READ=" + string(r))
}
PY
}

run_ec() { ( cd "$W" && cp "$2" govern.json && "$NAAB" "$1" >/tmp/_ah_out 2>&1 ); echo $?; }

# AH-01 POSITIVE CONTROL: a normal python block that imports + computes runs to completion.
cat > "$W/ctl.naab" <<'PY'
main { let r = <<python
import hashlib, base64, json
hashlib.sha256(b"x").hexdigest()[:8]
>>
print("OK=" + string(r)) }
PY
EC=$(run_ec ctl.naab deny.json)
if [ "$EC" = "0" ] && grep -q "OK=" /tmp/_ah_out; then
    ok "AH-01" "POSITIVE CONTROL: a python block that imports stdlib still runs under filesystem mode:none (carve-out live)"
else
    bad "AH-01" "legitimate imports broke under the hook" "exit $EC; $(tail -2 /tmp/_ah_out)"
fi

# AH-02 THE FIX: obfuscated escape read of a DENIED path must be blocked at the syscall.
mk_read "$W/deny_read.naab" "/etc/hostname"
EC=$(run_ec deny_read.naab deny.json)
if [ "$EC" != "0" ] && ! grep -q "READ=" /tmp/_ah_out && grep -qi "denied by sandbox policy" /tmp/_ah_out; then
    ok "AH-02" "obfuscated escape read of a denied path is blocked by the runtime hook (no source token to catch)"
else
    bad "AH-02" "the escape read a denied path OR was blocked by the wrong gate" "exit $EC; $(tail -3 /tmp/_ah_out)"
fi

# AH-03 the hook DECIDES on policy: obfuscated read of an ALLOWED path must succeed.
mk_read "$W/allow_read.naab" "$W/ok.txt"
EC=$(run_ec allow_read.naab allow.json)
if [ "$EC" = "0" ] && grep -q "READ=audit-hook-allowed-content" /tmp/_ah_out; then
    ok "AH-03" "CONTROL: obfuscated read of an ALLOWED path succeeds (hook enforces policy, not a blanket block)"
else
    bad "AH-03" "the hook blocked a permitted read" "exit $EC; $(tail -3 /tmp/_ah_out)"
fi

# AH-04 carve-out negative control: the SAME denied config must NOT deny the
# interpreter's own module loads — proven by AH-01 running clean under deny.json,
# re-asserted here as the explicit claim so a future change that drops the
# carve-out fails with this name rather than silently breaking imports.
EC=$(run_ec ctl.naab deny.json)
if [ "$EC" = "0" ]; then
    ok "AH-04" "runtime-path carve-out: interpreter module loads under sys.path are not denied under mode:none"
else
    bad "AH-04" "carve-out missing — interpreter cannot load its own modules under a restrictive fs policy" "exit $EC; $(tail -3 /tmp/_ah_out)"
fi

# AH-05 os.open WRITE via the escape must be adjudicated as a WRITE. os.open
# carries mode=None and the O_* access mode in integer flags; classifying by the
# (absent) mode string alone would check canRead and let a write through on a
# read-allowed/write-denied policy. Obfuscated so no source gate sees os.open.
cat > "$W/wr.naab" <<'NAABW'
main {
    let r = <<python
    _sc = getattr("".__class__.__base__, "__subcla" + "sses__")()
    _imp = [c for c in _sc if c.__name__ == "BuiltinImporter"][0]
    _m = getattr(_imp, "load_" + "module")("o" + "s")
    _op = getattr(_m, "op" + "en")
    _fl = getattr(_m, "O_WR" + "ONLY") | getattr(_m, "O_CR" + "EAT")
    _fd = _op("__WRPATH__", _fl, 420)
    getattr(_m, "wr" + "ite")(_fd, b"x"); getattr(_m,"cl"+"ose")(_fd)
    "WROTE"
    >>
    print("W=" + string(r))
}
NAABW
sed -i "s#__WRPATH__#$W/evil.txt#" "$W/wr.naab"
EC=$(run_ec wr.naab readonly.json)
if [ "$EC" != "0" ] && [ ! -f "$W/evil.txt" ] && grep -qi "denied by sandbox policy" /tmp/_ah_out; then
    ok "AH-05" "os.open WRITE via the escape is adjudicated as a write and denied under filesystem mode:read (integer-flags path)"
else
    bad "AH-05" "a low-level os.open write was allowed under a read-only fs policy" "exit $EC; evil.txt: $([ -f "$W/evil.txt" ] && echo exists || echo absent); $(tail -2 /tmp/_ah_out)"
fi

echo
echo "python audit hook: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
