#!/usr/bin/env bash
# test_reentrant_time.sh -- no non-reentrant time call in the engine (Bundle 9)
#
# gmtime(), localtime(), ctime() and asctime() return a pointer to ONE static
# buffer shared by every thread. The audit logger and the tamper-evident logger
# formatted their timestamps with gmtime() from async fns and agent.batch
# workers, so two entries written at once could carry each other's time;
# ThreadSanitizer reported it. Every caller now uses naab::utcTime()
# (include/naab/utc_time.h). This guard fails on a new call.
#
#   RT-00   CONTROL: a planted gmtime() call is found by the same scan
#   RT-00b  CONTROL: a comment naming gmtime() and a gmtime_r() call are not
#   RT-01   src/ and include/ hold no non-reentrant time call
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PASS=0; FAIL=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -10 | sed 's/^/       | /'; }

# scan PATH... -> the call sites, one per line (comments removed first)
scan() {
    python3 - "$@" <<'PY'
import os, re, sys
CALL = re.compile(r'(?<![\w:])(?:std::)?(gmtime|localtime|ctime|asctime)\s*\(')
def strip(text):
    # remove /* */ and // comments, and string literals, keeping line count
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("/*", i):
            j = text.find("*/", i + 2); j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j)); i = j
        elif text.startswith("//", i):
            j = text.find("\n", i); j = n if j < 0 else j
            i = j
        elif c == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append('""'); i = j + 1
        else:
            out.append(c); i += 1
    return "".join(out)
hits = []
for root in sys.argv[1:]:
    paths = [root] if os.path.isfile(root) else [os.path.join(d, f) for d, _, fs in os.walk(root) for f in fs]
    for p in sorted(paths):
        if not p.endswith((".cpp", ".c", ".h", ".hpp", ".cc")) or p.endswith("utc_time.h"):
            continue
        with open(p, "rb") as f:
            text = f.read().decode("utf-8", "replace")
        for ln, line in enumerate(strip(text).split("\n"), 1):
            if CALL.search(line):
                hits.append("%s:%d" % (p, ln))
sys.stdout.buffer.write(("\n".join(hits) + ("\n" if hits else "")).encode("ascii", "replace"))
PY
}

echo "=== No non-reentrant time call in the engine ==="
T="$(mktemp -d "${TMPDIR:-/tmp}/naab_rt_XXXXXX")"
trap 'rm -rf "$T"' EXIT
printf '#include <ctime>\nvoid f(time_t t) { auto* x = gmtime(&t); (void)x; }\n' > "$T/planted.cpp"
printf '#include <ctime>\n// gmtime() is not thread-safe\nvoid g(time_t t) { struct tm o; gmtime_r(&t, &o); const char* s = "localtime(x)"; (void)s; }\n' > "$T/clean.cpp"

found="$(scan "$T/planted.cpp")"
if grep -q 'planted.cpp:2' <<<"$found"; then pass RT-00 "CONTROL: a planted gmtime() call is found"
else fail RT-00 "CONTROL: the scan did not find a planted gmtime() call" "$found"; fi
found="$(scan "$T/clean.cpp")"
if [ -z "$found" ]; then pass RT-00b "CONTROL: a comment, a string and gmtime_r() are not flagged"
else fail RT-00b "CONTROL: the scan flagged a comment, a string or gmtime_r()" "$found"; fi

found="$(cd "$ROOT" && scan src include)"
if [ -z "$found" ]; then pass RT-01 "src/ and include/ hold no gmtime/localtime/ctime/asctime call (use naab::utcTime)"
else fail RT-01 "non-reentrant time calls (use naab::utcTime from include/naab/utc_time.h)" "$found"; fi

echo ""
echo "Results: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
