#!/usr/bin/env bash
# test_review_cache_protection.sh -- the governed program may not write the
# engine's cache directory, `.naab_cache/` beside govern.json (A169)
#
# Agent review and the governance voice keep their results in `.naab_cache/`
# and read them back on the next run. The cache's integrity check (F12,
# 29982dc6) is an HMAC keyed by sha256(sha256(govern.json) + ":" +
# sha256(script)) -- inputs a program can know -- and nothing stopped the
# program writing the directory: on 743e4ee7 file.write of
# ".naab_cache/planted.review.json" ran under enforce, Governance: PASS. A
# program that can write the directory can plant a review that found nothing.
# Now every program write to the directory or under it is refused
# (governance.state_files, HARD), on every route that reaches the shared
# decision point; reads stay allowed, and the engine's own cache writes are
# not program writes (test_agent_review_withhold.sh AW-07 and AW-08 see the
# engine write and read its cache with this in place).
#
#   RC-00   CONTROL: an ordinary write beside the cache runs, and so does one
#           into a directory of a similar name -- the gate is not a blanket
#           deny on the project or on the text "naab_cache"
#   RC-01   file.write into an existing .naab_cache/: refused, nothing written
#   RC-02   ... and when .naab_cache/ does not exist yet: refused, not created
#   RC-03   io.write_file into it: refused
#   RC-04   the same write by absolute path: refused
#   RC-05   file.copy of a file INTO the directory (copy's destination
#           resolution): refused
#   RC-06   a write through a symbolic link that points at the directory:
#           refused (the target is canonicalised before the check)
#   RC-07   a write through a HARD link elsewhere to a cache entry: refused
#   RC-07c  CONTROL: a hard-linked file that is not in the cache is writable
#   RC-08   file.delete of a cache entry, with deletion granted: refused
#   RC-08c  CONTROL: deletion really is granted there (an ordinary delete runs)
#   RC-09   embedded Python open(..., "w") into it: refused
#   RC-10   CONTROL: the program can still READ a cache entry
#   RC-11   CONTROL: a .naab_cache/ that is not beside govern.json is an
#           ordinary directory (the protection is the engine's directory only)
#   RC-12   PINNED, not endorsed: a <<sh>> block's write lands -- subprocess
#           languages are outside path policy (CONTRA-013)
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-11-15):
#   the write check removed                        -> RC-01 .. RC-09 red
#   only paths UNDER the directory (not the dir)   -> RC-02 red
#   prefix match without the '/' boundary          -> RC-00 red
#   no hard-link check                             -> RC-07 red
#   reads refused too (mode check dropped)         -> RC-10 red
#   any ".naab_cache" component protected          -> RC-00 RC-11 red
# Red first: on 743e4ee7 RC-01 .. RC-09 fail and every control passes.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/naab_rcache_XXXXXX")"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust          # unsigned configs; exit 3 must mean the gate
cleanup() { teardown_isolated_trust; [ -n "${RC_KEEP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

# mk DIR [SANDBOX] -- a project with an ordinary enforce config
mk() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "${2:-elevated}"},
 "capabilities": {"filesystem": {"mode": "write"}}}
EOF
}
run() {  # DIR PROGRAM -> OUT, RC
    printf '%s\n' "$2" > "$1/p.naab"
    OUT="$(cd "$1" && "$NAAB" run p.naab 2>&1)"; RC=$?
}
named() { grep -q "governance cache is not writable\|Writing into the governance cache is blocked" <<<"$OUT"; }
refused() {  # ID WHAT CONDITION-RC
    if [ "$RC" = 3 ] && named && [ "$3" = 0 ]; then pass "$1" "$2: refused (exit 3, named), nothing written"
    else fail "$1" "$2 should be refused" "rc=$RC named=$(named && echo yes || echo no) $4: $(tail -3 <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi
}

# Embedded Python present? (the audit hook lives there only)
PYD="$TEST_TMP/pyprobe"; mkdir -p "$PYD"; printf '{"version":"5.0","mode":"off"}\n' > "$PYD/govern.json"
run "$PYD" 'main { let x = <<python
1 + 1
>>
  print(x) }'
EMBEDDED_PY=0; [ "$RC" = 0 ] && grep -q '^2$' <<<"$OUT" && EMBEDDED_PY=1
# Shell executor usable here? (the pinned arm)
SHD="$TEST_TMP/shprobe"; mkdir -p "$SHD"; printf '{"version":"5.0","mode":"off"}\n' > "$SHD/govern.json"
run "$SHD" 'main { <<sh
echo SH_OK > sh_marker
>> }'
SHELL_OK=0; [ -f "$SHD/sh_marker" ] && SHELL_OK=1

echo "=== the program may not write .naab_cache/ (A169) ==="

d="$TEST_TMP/c00"; mk "$d"; mkdir -p "$d/.naab_cache"
run "$d" 'use file
main {
    file.write("plain.txt", "FINE")
    file.write(".naab_cache_other.txt", "FINE")
    print("WROTE")
}'
if [ "$RC" = 0 ] && grep -q WROTE <<<"$OUT" && [ "$(cat "$d/plain.txt" 2>/dev/null)" = FINE ] \
        && [ "$(cat "$d/.naab_cache_other.txt" 2>/dev/null)" = FINE ]; then
    pass RC-00 "CONTROL: an ordinary write beside the cache, and one to a similar name, run"
else
    fail RC-00 "ordinary writes should run" "rc=$RC: $(tail -3 <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"
fi

d="$TEST_TMP/c01"; mk "$d"; mkdir -p "$d/.naab_cache"
run "$d" 'use file
main { file.write(".naab_cache/planted.review.json", "{}") }'
refused RC-01 "file.write into .naab_cache/" "$([ -e "$d/.naab_cache/planted.review.json" ] && echo 1 || echo 0)" "written=$([ -e "$d/.naab_cache/planted.review.json" ] && echo yes || echo no)"

d="$TEST_TMP/c02"; mk "$d"
run "$d" 'use file
main { file.write(".naab_cache", "{}") }'
refused RC-02 "creating .naab_cache itself" "$([ -e "$d/.naab_cache" ] && echo 1 || echo 0)" "created=$([ -e "$d/.naab_cache" ] && echo yes || echo no)"

d="$TEST_TMP/c03"; mk "$d"; mkdir -p "$d/.naab_cache"
run "$d" 'use io
main { io.write_file(".naab_cache/io.review.json", "{}") }'
refused RC-03 "io.write_file into .naab_cache/" "$([ -e "$d/.naab_cache/io.review.json" ] && echo 1 || echo 0)" ""

d="$TEST_TMP/c04"; mk "$d"; mkdir -p "$d/.naab_cache"
run "$d" "use file
main { file.write(\"$d/.naab_cache/abs.review.json\", \"{}\") }"
refused RC-04 "an absolute-path write into .naab_cache/" "$([ -e "$d/.naab_cache/abs.review.json" ] && echo 1 || echo 0)" ""

d="$TEST_TMP/c05"; mk "$d"; mkdir -p "$d/.naab_cache"; echo '{}' > "$d/copied.review.json"
run "$d" 'use file
main { file.copy("copied.review.json", ".naab_cache") }'
refused RC-05 "file.copy into .naab_cache/" "$([ -e "$d/.naab_cache/copied.review.json" ] && echo 1 || echo 0)" ""

d="$TEST_TMP/c06"; mk "$d"; mkdir -p "$d/.naab_cache"
if ln -s .naab_cache "$d/lnk" 2>/dev/null && [ -L "$d/lnk" ]; then
    run "$d" 'use file
main { file.write("lnk/through.review.json", "{}") }'
    refused RC-06 "a write through a symbolic link to .naab_cache/" "$([ -e "$d/.naab_cache/through.review.json" ] && echo 1 || echo 0)" ""
else
    unm RC-06 "symbolic links cannot be made here"
fi

d="$TEST_TMP/c07"; mk "$d"; mkdir -p "$d/.naab_cache"; echo ORIGINAL > "$d/.naab_cache/e.review.json"
echo ORIGINAL > "$d/other.txt"
if ln "$d/.naab_cache/e.review.json" "$d/outside.json" 2>/dev/null && ln "$d/other.txt" "$d/other2.txt" 2>/dev/null; then
    run "$d" 'use file
main { file.write("outside.json", "PLANTED") }'
    refused RC-07 "a write through a hard link to a cache entry" "$(grep -q PLANTED "$d/.naab_cache/e.review.json" && echo 1 || echo 0)" ""
    run "$d" 'use file
main { file.write("other2.txt", "FINE") }'
    if [ "$RC" = 0 ] && [ "$(cat "$d/other.txt")" = FINE ]; then pass RC-07c "CONTROL: a hard-linked file outside the cache is writable"
    else fail RC-07c "a hard-linked file outside the cache should be writable" "rc=$RC"; fi
else
    unm RC-07 "hard links cannot be made here"; unm RC-07c "hard links cannot be made here"
fi

d="$TEST_TMP/c08"; mk "$d" unrestricted; mkdir -p "$d/.naab_cache"; echo ORIGINAL > "$d/.naab_cache/d.review.json"
run "$d" 'use file
main { file.write("junk.txt", "x")
    file.delete("junk.txt") }'
if [ "$RC" = 0 ] && [ ! -e "$d/junk.txt" ]; then
    pass RC-08c "CONTROL: deletion is granted here (an ordinary delete runs)"
    run "$d" 'use file
main { file.delete(".naab_cache/d.review.json") }'
    refused RC-08 "file.delete of a cache entry" "$([ -e "$d/.naab_cache/d.review.json" ] && echo 0 || echo 1)" ""
else
    unm RC-08c "deletion not grantable here (rc=$RC)"; unm RC-08 "no granted delete to compare against"
fi

if [ "$EMBEDDED_PY" = 1 ]; then
    d="$TEST_TMP/c09"; mk "$d"; mkdir -p "$d/.naab_cache"
    run "$d" 'main { let x = <<python
with open(".naab_cache/py.review.json", "w") as f:
    f.write("{}")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && [ ! -e "$d/.naab_cache/py.review.json" ]; then pass RC-09 "embedded Python open(w) into .naab_cache/ refused (rc=$RC)"
    else fail RC-09 "embedded Python wrote into the cache" "rc=$RC"; fi
else
    unm RC-09 "embedded Python not available in this build"
fi

d="$TEST_TMP/c10"; mk "$d"; mkdir -p "$d/.naab_cache"; echo CACHED_TEXT > "$d/.naab_cache/r.review.json"
run "$d" 'use file
main { print(file.read(".naab_cache/r.review.json")) }'
if [ "$RC" = 0 ] && grep -q CACHED_TEXT <<<"$OUT"; then pass RC-10 "CONTROL: the program can read a cache entry"
else fail RC-10 "reading the cache should be allowed" "rc=$RC: $(tail -3 <<<"$OUT" | tr '\n' ' ' | cut -c1-240)"; fi

d="$TEST_TMP/c11"; mk "$d"; mkdir -p "$d/sub/.naab_cache"
run "$d" 'use file
main { file.write("sub/.naab_cache/x.json", "FINE") }'
if [ "$RC" = 0 ] && [ "$(cat "$d/sub/.naab_cache/x.json" 2>/dev/null)" = FINE ]; then
    pass RC-11 "CONTROL: a .naab_cache not beside govern.json is an ordinary directory"
else
    fail RC-11 "a .naab_cache elsewhere should be writable" "rc=$RC"
fi

if [ "$SHELL_OK" = 1 ]; then
    d="$TEST_TMP/c12"; mk "$d"; mkdir -p "$d/.naab_cache"
    cat > "$d/govern.json" <<'EOF'
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "unrestricted"},
 "capabilities": {"filesystem": {"mode": "write"}, "shell": {"enabled": true}}}
EOF
    run "$d" 'main { <<sh
echo SH_WROTE > .naab_cache/sh.review.json
>> }'
    if [ -e "$d/.naab_cache/sh.review.json" ]; then
        pass RC-12 "PINNED, not endorsed: a <<sh>> write lands (subprocess languages are outside path policy, CONTRA-013)"
    else
        fail RC-12 "the pinned limit changed: a <<sh>> write no longer lands -- confirm and update this arm" "rc=$RC"
    fi
else
    unm RC-12 "no shell executor here"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
