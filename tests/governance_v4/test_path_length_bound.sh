#!/usr/bin/env bash
# ============================================================
# test_path_length_bound.sh — a HARD check that takes 17 minutes is not a verdict
#
# checkPathAccess calls std::filesystem::weakly_canonical() on whatever path it
# is handed, and nothing bounded that input. A path is an ordinary runtime
# string, so it can be built by concatenation in a loop.
#
# MEASURED before the fix (VM, capabilities.filesystem configured, sandbox
# standard) — the cost is LINEAR, not superlinear. A quadratic hypothesis was
# raised during the investigation and the data refuted it:
#
#     2,304 B ->   106 ms        9,216 B ->   284 ms
#    36,864 B -> 1,034 ms      147,456 B -> 3,918 ms      (4x in -> ~3.7x time)
#
# ~26us/byte. A 38MB path therefore costs ~17 minutes inside a HARD check;
# observed as a >180s hang with no verdict and no error. Every BOUNDED size did
# return exit 1, so a decision is rendered eventually — this is a slow verdict,
# not a bypassed check, and NOT the SECRET_PATTERNS ReDoS class, which crashed
# inside the check mid-decision rather than completing.
#
# Capping string growth does not fix it: 37.7MB is legal under the 100MB
# MAX_STRING_LENGTH cap, and the probe still timed out after that change. The
# bound has to be on the path itself.
#
#   PB-01  THE FIX. An oversized path is refused, and refused FAST. Both halves
#          matter: exit 3 alone would also be satisfied by a build that blocks
#          after seventeen minutes, which is the behaviour being fixed. The time
#          assertion is the actual subject of this test.
#   PB-02  POSITIVE CONTROL. A normal path must still resolve AND read its file.
#          Without it, PB-01 is equally satisfied by a bound of zero — i.e. by
#          breaking filesystem access entirely, which would "pass" PB-01
#          perfectly.
#   PB-03  BOUNDARY. A path just under the limit is NOT blocked. Distinguishes a
#          real threshold from a check that rejects anything long.
#   PB-04  ENGINE PARITY. The tree-walker must agree. checkPathAccess is shared,
#          so a divergence here means the bound was added to a caller rather
#          than to the check.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; }

[ -x "$NAAB" ] || { echo "  naab-lang not built, skipping"; exit 0; }
command -v python3 >/dev/null 2>&1 || { echo "  python3 unavailable, skipping"; exit 0; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/pathbound-XXXXXX")"
cleanup() { teardown_isolated_trust; rm -rf "$W"; }
trap cleanup EXIT
cd "$W" || exit 1

cat > govern.json <<'JSON'
{ "version":"4.0", "mode":"enforce",
  "security": { "sandbox_level": "standard" },
  "capabilities": { "filesystem": { "mode": "read", "blocked_paths": ["/etc"] } } }
JSON
echo "content" > real.txt

# 36 * 2^20 = ~37.7MB, built by doubling — the shape that produced the hang.
cat > huge.naab <<'X'
use file
main {
    let s = "/tmp/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    let i = 0
    while i < 20 { s = s + s i = i + 1 }
    let r = file.read(s)
}
X
cat > normal.naab <<'X'
use file
main { let c = file.read("real.txt") print("READ_OK") }
X
python3 -c "
p = '/tmp/' + 'a'*7000
open('under.naab','w').write('use file\nmain { let c = file.read(\"%s\") }\n' % p)"

# $1=script $2=flags -> "<exit> <ms>"
timed() {
    local s e
    s=$(date +%s%N)
    ( ulimit -v 3000000 2>/dev/null; timeout 120 "$NAAB" ${2:-} "$1" >/dev/null 2>&1 )
    local ec=$?
    e=$(date +%s%N)
    echo "$ec $(( (e-s)/1000000 ))"
}

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Path length bound: a verdict nobody waits for is not one     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

read -r EC MS <<<"$(timed huge.naab)"
# 30s is far above the fixed cost of BUILDING a 37.7MB string (~200ms observed)
# and far below the ~17 minutes the unbounded check took. Deliberately loose:
# this asserts "bounded", not a benchmark, so a slow CI runner cannot fail it.
if [ "$EC" = "3" ] && [ "$MS" -lt 30000 ]; then
    ok "PB-01" "oversized path refused with exit 3, in ${MS}ms"
elif [ "$EC" = "3" ]; then
    bad "PB-01" "oversized path is refused, but not promptly" \
        "exit 3 after ${MS}ms. The bound must run BEFORE weakly_canonical(), not after — a block that arrives seventeen minutes late is the behaviour this test exists to catch."
else
    bad "PB-01" "oversized path is not refused" \
        "exit $EC after ${MS}ms, expected exit 3. 124 means it timed out — the pre-fix hang."
fi

read -r EC2 _ <<<"$(timed normal.naab)"
if [ "$EC2" = "0" ] && ( cd "$W" && "$NAAB" normal.naab 2>/dev/null | grep -q READ_OK ); then
    ok "PB-02" "POSITIVE CONTROL: a normal path still resolves and reads"
else
    bad "PB-02" "the bound broke ordinary filesystem access" \
        "exit $EC2 reading a file in the working directory. PB-01 passing while this fails means the bound rejects everything, which is not a fix."
fi

read -r EC3 _ <<<"$(timed under.naab)"
if [ "$EC3" != "3" ]; then
    ok "PB-03" "BOUNDARY: a 7000-byte path is not blocked by the bound (exit $EC3)"
else
    bad "PB-03" "a path under the limit is being blocked" \
        "exit 3 on ~7000 bytes, under the 8192 limit. The threshold is wrong, or the check rejects on something other than length."
fi

read -r EC4 _ <<<"$(timed huge.naab --tree-walk)"
if [ "$EC4" = "3" ]; then
    ok "PB-04" "ENGINE PARITY: tree-walker refuses it too"
else
    bad "PB-04" "engines disagree on the oversized path" \
        "VM exit $EC, tree-walk exit $EC4. checkPathAccess is shared, so a divergence means the bound landed in a caller rather than in the check."
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}path length bound: $PASS passed, 0 failed${NC}"
else
    echo -e "${RED}path length bound: $FAIL failed${NC}, $PASS passed"; exit 1
fi
