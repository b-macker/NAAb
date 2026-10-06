#!/usr/bin/env bash
# ============================================================
# test_compile_cache_race.sh -- concurrent naab-lang processes compiling the
# same C++ block never crash on, or expose, a half-written cache file
#
# THE FAILURE THIS CATCHES
#
# Both C++ compile caches are shared by every naab-lang process a user runs,
# and both installed their files by writing over the final name:
#
#   * CppExecutor::compileBlock() (use BLOCK-CPP-..., dlopen()ed) renames the
#     compiled .so from TMPDIR into ~/.naab_cpp_cache, and when that rename
#     fails because TMPDIR is another filesystem -- a tmpfs /tmp is the usual
#     case -- fell back to copy_file(overwrite_existing). Two processes
#     compiling the same block: the first dlopen()s the library, the second
#     truncates it in place, and the first dies of SIGBUS on its next page
#     fault. Measured on fecb13e: 7 of 13 iterations failed (six SIGBUS,
#     exit 135; one dlopen "file too short"); the same runs with TMPDIR on
#     HOME's filesystem, where rename() is used, failed 0 of 5.
#   * InlineCodeCache::storeBinary() (inline <<cpp blocks) used
#     copy_file(overwrite_existing) onto ~/.naab/cache/cpp/<hash>.so, so the
#     cached binary was visible at partial sizes for the length of every copy:
#     on fecb13e an observer polling it saw partial sizes in 3 of 3 iterations.
#
# Both now copy into a uniquely named sibling of the destination and rename()
# it into place (include/naab/atomic_file.h).
#
#   CR-00  the two filesystems CR-01 needs exist and differ (else UNMEASURABLE)
#   CR-01  dlopen path: two processes per iteration, TMPDIR on a different
#          filesystem from HOME, a 64 MB block library that the first process
#          keeps touching while the second installs -- every process exits 0
#          and prints its result. CONTROL: the compiler wrapper counts two
#          compiles per iteration, i.e. both processes really raced.
#   CR-02  inline path: two processes per iteration compile the same 64 MB
#          block into one cache while this script polls the cached binary's
#          final name -- every size it sees is the complete size. The poller
#          stands in for a process executing the cached binary (one that
#          loaded metadata.txt); naab-lang itself runs its own copy. CONTROL:
#          the poller saw the file at all, and both processes compiled.
#
# metadata.txt is written the same way and is covered by
# tests/unit/inline_code_cache_test.cpp: a naab-lang script run never writes
# it (the CLI leaves through _exit(), skipping the destructor that saves it),
# so it cannot be raced from here.
#
# Iterations: CR-01 failed 7 of 13 iterations unfixed (~54%), so 8 leave
# roughly a (0.46)^8 = 0.2% chance an unfixed build passes. CR-02 saw partial
# sizes in all 6 unfixed iterations measured, so 3 is plenty. Both counts can
# be raised with CR01_ITERS / CR02_ITERS. CR-02 compares every observed size
# to the final one exactly: both processes compile identical source in
# equal-length mkdtemp paths, and their binaries matched in every run.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
CR01_ITERS="${CR01_ITERS:-8}"
CR02_ITERS="${CR02_ITERS:-3}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "$3" | sed 's/^/       | /'; FAIL=$((FAIL+1)); }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Concurrent compiles of one C++ block share the cache safely ==="

case "$(uname -s)" in
    Linux) ;;
    *) skip "CR-00" "needs Linux (stat -c, /dev/shm) and the POSIX-only C++ executor -- UNMEASURABLE"
       report; exit 0 ;;
esac
if [ ! -x "$NAAB" ]; then
    echo "  naab-lang not built at $NAAB"; exit 1
fi
# CR-01's block library is compiled with clang++, CR-02's inline block with g++.
HAVE_GXX=no; HAVE_CLANG=no
command -v g++ >/dev/null 2>&1 && HAVE_GXX=yes
command -v clang++ >/dev/null 2>&1 && HAVE_CLANG=yes

source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/naab-cacherace-XXXXXX")"
SHM=""
cleanup() { teardown_isolated_trust; rm -rf "$W"; [ -n "$SHM" ] && rm -rf "$SHM"; }
trap cleanup EXIT

# Loading a block library needs an unrestricted sandbox.
cat > "$W/govern.json" <<'EOF'
{
  "version": "4.0",
  "mode": "enforce",
  "security": { "sandbox_level": "unrestricted" }
}
EOF

# Compiler wrappers, first on PATH: count each invocation, then run the real
# compiler. A count of two per iteration is the evidence both processes missed
# the cache and installed -- one compile means the second process loaded the
# first one's result and nothing raced. (The count file's path is written into
# the wrapper rather than passed in the environment: a precaution, since
# SubprocessContainment can scrub a child's environment.)
mkdir -p "$W/wrap"
for tool in g++ clang++; do
    real="$(command -v "$tool")" || continue
    cat > "$W/wrap/$tool" <<EOF
#!/bin/sh
echo x >> "$W/count"
exec "$real" "\$@"
EOF
    chmod +x "$W/wrap/$tool"
done

# ---- CR-00: two filesystems ------------------------------------------------
# HOME holds the cache, TMPDIR the compile; the bug needs them on different
# filesystems. $W is one; /dev/shm (tmpfs) or the build tree is the other.
dev_of() { stat -c %d "$1" 2>/dev/null; }
OTHER=""
for cand in /dev/shm "$REPO/build"; do
    [ -d "$cand" ] && [ -w "$cand" ] || continue
    if [ "$(dev_of "$cand")" != "$(dev_of "$W")" ]; then OTHER="$cand"; break; fi
done
if [ "$HAVE_CLANG" = no ]; then
    skip "CR-00" "clang++ not installed (it compiles block libraries) -- CR-01 UNMEASURABLE here"
elif [ -z "$OTHER" ]; then
    skip "CR-00" "no writable directory on a filesystem other than $W's -- CR-01 UNMEASURABLE here"
else
    SHM="$(mktemp -d "$OTHER/naab-cacherace-XXXXXX")"
    ok "CR-00" "cache on $(stat -f -c %T "$W"), compile dir on $(stat -f -c %T "$SHM") (different devices)"
fi

# ---- CR-01: dlopen()ed block library ----------------------------------------
cat > "$W/block.cpp" <<'EOF'
#include <chrono>
static volatile char big[64 << 20] = {1};
extern "C" int scan(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    long sum = 0;
    while (std::chrono::steady_clock::now() < end)
        for (long i = 0; i < (long)sizeof(big); i += 4096) sum += big[i];
    return sum >= 0 ? 7 : 8;
}
EOF
cat > "$W/block.naab" <<'EOF'
use BLOCK-CPP-CACHERACE as b
main {
    print(b.scan(3000))
}
EOF

if [ -n "$SHM" ]; then
    crashes=""; raced=0
    for i in $(seq 1 "$CR01_ITERS"); do
        H="$W/h1-$i"; T="$SHM/t-$i"
        mkdir -p "$H/.naab/language/blocks/library/cpp" "$T"
        cp "$W/block.cpp" "$H/.naab/language/blocks/library/cpp/BLOCK-CPP-CACHERACE.cpp"
        : > "$W/count"
        for k in 1 2; do
            ( cd "$W" && PATH="$W/wrap:$PATH" HOME="$H" TMPDIR="$T" \
                timeout 120 "$NAAB" --tree-walk block.naab > "$W/out1-$i-$k" 2>&1
              echo $? > "$W/rc1-$i-$k" ) &
        done
        wait
        [ "$(wc -l < "$W/count")" -eq 2 ] && raced=$((raced+1))
        for k in 1 2; do
            rc="$(cat "$W/rc1-$i-$k")"
            out="$(grep -E '^[0-9]+$' "$W/out1-$i-$k" | head -1)"
            if [ "$rc" != 0 ] || [ "$out" != 7 ]; then
                crashes="$crashes
iteration $i process $k: exit $rc$( [ "$rc" = 135 ] && echo ' (SIGBUS)'), output '$out'
$(grep -v '^\[governance\]' "$W/out1-$i-$k" | grep -m2 .)"
            fi
        done
        rm -rf "$H" "$T"
    done
    if [ "$raced" -eq 0 ]; then
        bad "CR-01" "CONTROL: no iteration compiled twice -- the processes never raced, so the clean result proves nothing"
    elif [ -n "$crashes" ]; then
        bad "CR-01" "a process sharing the block-library cache failed ($raced of $CR01_ITERS iterations raced)" "$crashes"
    else
        ok "CR-01" "$((CR01_ITERS * 2)) processes, $raced of $CR01_ITERS iterations compiled twice, none failed"
    fi
fi

# ---- CR-02: inline block, cached binary -------------------------------------
cat > "$W/inline.naab" <<'EOF'
main {
    let v = <<cpp
static volatile char big[64 << 20] = {1};
big[0] + 6
>>
    print(v)
}
EOF

partial_total=0; seen_total=0; raced=0; failures=""
[ "$HAVE_GXX" = yes ] || CR02_ITERS=0
for i in $(seq 1 "$CR02_ITERS"); do
    H="$W/h2-$i"; T="$W/t2-$i"; mkdir -p "$H" "$T"
    : > "$W/count"; : > "$W/sizes-$i"; rm -f "$W/stop"
    ( while [ ! -e "$W/stop" ]; do
          for f in "$H"/.naab/cache/cpp/*.so; do
              [ -e "$f" ] && stat -c %s "$f" >> "$W/sizes-$i" 2>/dev/null
          done
      done ) &
    poller=$!
    pids=""
    for k in 1 2; do
        ( cd "$W" && PATH="$W/wrap:$PATH" HOME="$H" TMPDIR="$T" \
            timeout 120 "$NAAB" inline.naab > "$W/out2-$i-$k" 2>&1
          echo $? > "$W/rc2-$i-$k" ) &
        pids="$pids $!"
    done
    wait $pids
    touch "$W/stop"; wait "$poller"
    [ "$(wc -l < "$W/count")" -eq 2 ] && raced=$((raced+1))
    final="$(stat -c %s "$H"/.naab/cache/cpp/*.so 2>/dev/null | head -1)"
    seen="$(wc -l < "$W/sizes-$i")"
    partial="$(awk -v f="${final:-0}" '$1 != f' "$W/sizes-$i" | wc -l)"
    seen_total=$((seen_total + seen)); partial_total=$((partial_total + partial))
    [ "$partial" -gt 0 ] && failures="$failures
iteration $i: $partial of $seen observations were not the final size $final, e.g. $(awk -v f="$final" '$1 != f' "$W/sizes-$i" | sort -n | uniq | head -3 | tr '\n' ' ')"
    for k in 1 2; do
        rc="$(cat "$W/rc2-$i-$k")"
        out="$(grep -E '^[0-9]+$' "$W/out2-$i-$k" | head -1)"
        if [ "$rc" != 0 ] || [ "$out" != 7 ] || grep -q 'Failed to cache' "$W/out2-$i-$k"; then
            failures="$failures
iteration $i process $k: exit $rc, output '$out'
$(grep -v '^\[governance\]' "$W/out2-$i-$k" | grep -m2 .)"
        fi
    done
    rm -rf "$H" "$T"
done
if [ "$HAVE_GXX" = no ]; then
    skip "CR-02" "g++ not installed (it compiles inline blocks) -- UNMEASURABLE"
elif [ "$raced" -eq 0 ] || [ "$seen_total" -eq 0 ]; then
    bad "CR-02" "CONTROL: $raced iterations compiled twice and the poller saw the cached binary $seen_total times -- nothing was measured"
elif [ -n "$failures" ]; then
    bad "CR-02" "the cached binary was observable half-written, or a process failed" "$failures"
else
    ok "CR-02" "$seen_total observations of the cached binary over $CR02_ITERS iterations ($raced raced), all complete"
fi

report
[ "$FAIL" -eq 0 ]
