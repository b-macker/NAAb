#!/usr/bin/env python3
"""Does the BSD stdlib event table classify every function of every module it covers?

include/naab/bsd_stdlib_events.h is the one place the VM learns what a stdlib
call is to behavioural sequence detection. A function the table does not list
records no event and is never pre-checked, whatever it does -- which is how
env.get_all (the same code branch as env.list) and env.set (the same branch as
env.set_var) went unseen (docs/open-investigations.md A22). This compares each
covered module's hasFunction() names, read from its source, with the table:

  MISSING  the module accepts the name and the table does not list it
  STALE    the table lists a name the module does not accept

Exit 0 when both are empty, 1 otherwise, 2 when the probe itself is broken
(a covered module with no known source, or a hasFunction() that yields no
names) -- a broken probe must not read as "nothing missing".

Reads source text, stdlib only; prints ASCII.
"""
import argparse
import os
import re
import sys

# Covered module -> (source file under --src, class whose hasFunction() lists
# its functions). A module added to kCoveredModules needs an entry here, or
# the probe reports itself broken.
MODULE_SOURCES = {
    "env": ("src/stdlib/env_impl.cpp", "EnvModule"),
    "file": ("src/stdlib/file_impl.cpp", "FileModule"),
    "io": ("src/stdlib/stdlib.cpp", "IOModule"),
    "csv": ("src/stdlib/csv_impl.cpp", "CsvModule"),
    "http": ("src/stdlib/http_impl.cpp", "HTTPModule"),
    "process": ("src/stdlib/process_impl.cpp", "ProcessModule"),
    "crypto": ("src/stdlib/crypto_impl.cpp", "CryptoModule"),
    "path": ("src/stdlib/path_impl.cpp", "PathModule"),
    "log": ("src/stdlib/log_impl.cpp", "LogModule"),
    "agent": ("src/stdlib/agent_impl.cpp", "AgentModule"),
}

ROW = re.compile(r'(?:vmEvent|noEvent)\(\s*"(\w+)"\s*,\s*"(\w+)"'
                 r'|\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*StdlibEventSource::')


def emit(line):
    # Bytes, not print(): a native Windows python3 writes CRLF from print(),
    # and the coverage suite matches whole lines.
    sys.stdout.buffer.write((line + "\n").encode("ascii", "replace"))


def read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def table_rows(header_text):
    rows = []
    for m in ROW.finditer(header_text):
        mod = m.group(1) or m.group(3)
        fn = m.group(2) or m.group(4)
        rows.append((mod, fn))
    return rows


def covered_modules(header_text):
    m = re.search(r'kCoveredModules\[\]\s*=\s*\{([^}]*)\}', header_text)
    if not m:
        return None
    return re.findall(r'"(\w+)"', m.group(1))


def has_function_names(src_text, cls):
    """String literals in the body of `bool CLS::hasFunction(...) const {`."""
    m = re.search(r'bool\s+' + re.escape(cls) + r'::hasFunction\s*\([^)]*\)\s*const\s*\{', src_text)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(src_text) and depth:
        c = src_text[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        i += 1
    return set(re.findall(r'"([A-Za-z_][A-Za-z_0-9]*)"', src_text[m.end():i - 1]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--src", default=".", help="repository root (default: .)")
    ap.add_argument("--header", default=None,
                    help="table header (default: SRC/include/naab/bsd_stdlib_events.h)")
    a = ap.parse_args()
    header = a.header or os.path.join(a.src, "include/naab/bsd_stdlib_events.h")
    htext = read(header)
    mods = covered_modules(htext)
    rows = table_rows(htext)
    if not mods or not rows:
        emit("UNMEASURABLE: no kCoveredModules list or no table rows in %s" % header)
        return 2

    listed = {}
    dups = []
    for mod, fn in rows:
        listed.setdefault(mod, set())
        if fn in listed[mod]:
            dups.append("%s.%s" % (mod, fn))
        listed[mod].add(fn)

    broken, missing, stale = [], [], []
    for mod in mods:
        if mod not in MODULE_SOURCES:
            broken.append("%s: no known source (add it to MODULE_SOURCES)" % mod)
            continue
        rel, cls = MODULE_SOURCES[mod]
        path = os.path.join(a.src, rel)
        if not os.path.exists(path):
            broken.append("%s: %s does not exist" % (mod, rel))
            continue
        names = has_function_names(read(path), cls)
        if not names:
            broken.append("%s: %s::hasFunction not found or lists no names" % (mod, cls))
            continue
        have = listed.get(mod, set())
        missing += ["%s.%s" % (mod, n) for n in sorted(names - have)]
        stale += ["%s.%s" % (mod, n) for n in sorted(have - names)]
        emit("%-8s %3d functions, %3d listed" % (mod, len(names), len(have & names)))

    for mod in sorted(set(listed) - set(mods)):
        stale += ["%s.%s (module not in kCoveredModules)" % (mod, n) for n in sorted(listed[mod])]

    for b in broken:
        emit("BROKEN   " + b)
    for d in dups:
        emit("DUPLICATE " + d)
    for m in missing:
        emit("MISSING  " + m)
    for s in stale:
        emit("STALE    " + s)
    if broken:
        emit("UNMEASURABLE: the probe could not read every covered module")
        return 2
    if missing or stale or dups:
        emit("FAIL: %d missing, %d stale, %d duplicate" % (len(missing), len(stale), len(dups)))
        return 1
    emit("OK: every function of %d covered modules is classified" % len(mods))
    return 0


if __name__ == "__main__":
    sys.exit(main())
