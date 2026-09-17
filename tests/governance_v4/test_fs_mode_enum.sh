#!/usr/bin/env bash
# ============================================================
# test_fs_mode_enum.sh — an unrecognized filesystem mode must not mean "write"
#
# THE FAILURE THIS CATCHES
#
# capabilities.filesystem.mode is a closed enum whose DEFAULT is its most
# permissive member. checkFilesystemAllowed() tested only for "none" and
# "read", so every other string fell through to full write access -- silently,
# under "Governance: PASS". Measured before the fix:
#
#     mode="readonly"   -> ALLOWED write
#     mode="read-only"  -> ALLOWED write
#     mode="raed"       -> ALLOWED write
#
# "readonly" is the most natural spelling of the restriction an operator wants,
# and it granted the opposite of it. That is worse than an inert key: an inert
# key gives you nothing, this gives you write access while reading as protection.
# fsModeRank() shared the fallback, so the mid-run ratchet agreed.
#
# WHY REFUSE RATHER THAN WARN-AND-DEFAULT
#
# A13 handled the same shape on security.sandbox_level by warning and declining
# to apply the value. That does not work here: the default IS "write", so
# warn-and-default is still fail-open. An uninterpretable policy value is a
# config error (exit 4).
#
# WHY "read_write" IS ACCEPTED
#
# Three shipped example configs use it. It is not in the enum, so it reached the
# gate through the same fall-through -- but the intent is unambiguous, and
# refusing it would break working configs to no benefit. It is now an explicit
# alias for "write" rather than an accident.
#
#   FM-01..02  LIVE CONTROLS: "none" and "read" still block a write
#   FM-03      LIVE CONTROL: "write" still ALLOWS -- the only arm that can
#              reveal a masked probe. Every other arm here expects a refusal,
#              and a refusal is what a broken probe produces for free.
#   FM-04      "read_write" still allows (shipped configs keep working)
#   FM-05..07  typos are REFUSED at load, not silently granted
#   FM-08      the legacy string form is refused too (second parse site)
#   FM-09      NEGATIVE CONTROL: omitting the key entirely must still load
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS=0; FAIL=0; FAILURES=""
ok()  { PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }

W="${TMPDIR:-/tmp}/naab-fsmode-$$"
cleanup(){ rm -rf "$W"; }
trap cleanup EXIT
mkdir -p "$W"

[ -x "$NAAB" ] || { echo "naab-lang not built at $NAAB"; exit 1; }

printf 'main {\n  file.write("out.txt", "DATA")\n  print("WROTE")\n}\n' > "$W/t.naab"

# sandbox_level elevated is deliberate: with mode:enforce the sandbox upgrades to
# "standard" and denies the write BEFORE governance is consulted, so every arm
# reads as blocked and the suite passes without testing the gate at all.
write_cfg() {  # $1 = json fragment for capabilities.filesystem
    python3 -c "
import json,sys
json.dump({'version':'1.0','mode':'enforce',
 'security':{'sandbox_level':'elevated'},
 'capabilities':{'filesystem':json.loads(sys.argv[1])}}, open(sys.argv[2],'w'))
" "$1" "$W/govern.json"
}

verdict() {
    rm -f "$W/out.txt"
    local out rc
    out=$( cd "$W" && timeout 60 "$NAAB" t.naab 2>&1 ); rc=$?
    case "$out" in
        *WROTE*)                                  echo "allowed" ;;
        *"Unrecognized filesystem access mode"*)  echo "refused:$rc" ;;
        *"not allowed"*)                          echo "blocked" ;;
        *"denied by sandbox"*)                    echo "sandbox-masked" ;;
        *)                                        echo "unmeasurable:$rc" ;;
    esac
}

echo -e "${CYAN}=== Group FM: filesystem.mode enum ===${NC}"

for pair in "none:blocked:FM-01" "read:blocked:FM-02" "write:allowed:FM-03" "read_write:allowed:FM-04"; do
    m="${pair%%:*}"; rest="${pair#*:}"; want="${rest%%:*}"; id="${rest##*:}"
    write_cfg "{\"mode\":\"$m\"}"
    got=$(verdict)
    if [ "$got" = "$want" ]; then ok "$id" "mode=\"$m\" -> $want"
    else bad "$id" "mode=\"$m\" expected $want" "got: $got"; fi
done

i=5
for m in readonly read-only raed; do
    write_cfg "{\"mode\":\"$m\"}"
    got=$(verdict)
    if [ "$got" = "refused:4" ]; then ok "FM-0$i" "mode=\"$m\" refused at load (exit 4)"
    else bad "FM-0$i" "mode=\"$m\" must be refused, not interpreted" "got: $got"; fi
    i=$((i+1))
done

# Second parse site: capabilities.filesystem as a bare string.
write_cfg '"readonly"'
got=$(verdict)
if [ "$got" = "refused:4" ]; then ok "FM-08" "legacy string form refused too"
else bad "FM-08" "legacy string form must be refused" "got: $got"; fi

# NEGATIVE CONTROL. Refusing unknown values must not refuse an ABSENT key --
# without this, a validator that rejected everything would pass FM-05..08.
python3 -c "
import json
json.dump({'version':'1.0','mode':'enforce','security':{'sandbox_level':'elevated'},
 'capabilities':{'filesystem':{'allowed_paths':[]}}}, open('$W/govern.json','w'))
"
got=$(verdict)
if [ "$got" = "allowed" ]; then ok "FM-09" "absent mode still loads and uses the default"
else bad "FM-09" "omitting the key must not be a config error" "got: $got"; fi

echo
if [ $FAIL -eq 0 ]; then
    echo -e "${GREEN}=== Results: $PASS passed, 0 failed ===${NC}"; exit 0
else
    echo -e "${RED}=== Results: $PASS passed, $FAIL failed ===${NC}"; echo -e "$FAILURES"; exit 1
fi
