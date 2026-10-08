#!/usr/bin/env bash
# ============================================================
# test_shell_off_subprocess_langs.sh -- shell disabled by the project means no
# separate-process language runs; in-process languages are unaffected
#
# WHY THIS EXISTS. Before the gate in LanguageRegistry::getExecutor(), only the
# persistent executors (shell, node, ruby) checked for exec permission, so a
# project with shell disabled still ran php, go, cpp and rust -- and those
# blocks could start commands by absolute path or execve, which the subprocess
# PATH restriction does not cover and RLIMIT_NPROC does not bind as root (nor
# at `elevated` at all). The protection map read those cells TEXT-ONLY or OPEN.
#
# The gate refuses every executor whose runsInProcess() is false. That default
# is the point: a newly registered executor is refused until it shows it is
# hosted in-process. So this suite asks the BINARY for its languages and needs
# a case for each, like test_polyglot_gate_coverage.sh.
#
# WHAT IT MUST NOT DO (the controls). It is keyed on the project's explicit
# decision, NOT on the sandbox level: `standard` (the enforce default) also
# withholds exec, and refusing there would refuse compute-only blocks in every
# project that never disabled shell. SO-06 holds that line. In-process runtimes
# (embedded python, QuickJS, SQLite) keep running -- the engine decides inside
# them -- so SO-L arms expect those to RUN with shell off.
#
#   SO-00  the binary reports its languages
#   SO-01  every registered language has a case here
#   SO-L-* per language: RUNS with shell on (control, else UNMEASURABLE);
#          with shell off, a separate-process language does NOT run and an
#          in-process one DOES
#   SO-02  the bare form "shell": false triggers the gate too
#   SO-03  shell disabled by an `extends` parent triggers it (the merge used to
#          drop it -- see test_capability_mirrors.sh); SO-03c control: a
#          shell-on parent leaves the language running
#   SO-04  the refusal names the cause (not a toolchain failure)
#   SO-05  a mid-run reload that disables shell refuses the next block;
#          SO-05c control: the same program without the swap runs it
#   SO-06  CONTROL: shell ON at the enforce default (`standard`) still runs a
#          compute-only block -- the gate is the operator's decision, not the level
#   SO-07  --lock still works in a shell-off project (it asks versions, it does
#          not execute; it used to go through the execution gate)
#   SO-08  consensus verification treats a refused language as unavailable
#          instead of aborting the program; SO-08c control: it runs with shell on
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
PASS=0; FAIL=0; SKIP=0
ok()   { echo "  PASS [$1] $2"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL [$1] $2"; FAIL=$((FAIL+1)); [ -n "${3:-}" ] && printf '%s\n' "$3" | tail -8 | sed 's/^/       | /'; }
skip() { echo "  SKIP [$1] $2"; SKIP=$((SKIP+1)); }
report() { echo "  Results: $PASS passed, $FAIL failed, $SKIP skipped"; }

echo "=== Shell disabled by the project: separate-process languages refused ==="

if [ ! -x "$NAAB" ]; then
    skip "SO-00" "naab-lang not built -- UNMEASURABLE, not a pass"; report; exit 0
fi

# Unsigned fixtures: isolate trust, or a key in the real store turns every
# probe into an INTEGRITY BLOCK and every "refused" arm passes for free.
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust
source "$REPO/tests/helpers/config_swap.sh"

WDIR="$(mktemp -d)"
trap 'stop_swap_operators; teardown_isolated_trust; rm -rf "$WDIR"' EXIT
cd "$WDIR" || exit 1

# $1 = dir, $2 = mode, $3 = sandbox level, $4 = shell JSON value ("{...}" or bare)
cfg() {
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{
  "version": "4.0",
  "mode": "$2",
  "security": { "sandbox_level": "$3" },
  "codegen": { "enabled": true },
  "capabilities": { "shell": $4, "env_vars": { "read": true } }
}
EOF
}
ON='{ "enabled": true }'
OFF='{ "enabled": false }'

# ---- the language list comes FROM THE BINARY ----------------------------
cfg "$WDIR/list" audit elevated "$ON"
printf 'use codegen\nmain {\n    for l in codegen.supported_languages() { print(l) }\n}\n' > "$WDIR/list/langs.naab"
REGISTERED="$(cd "$WDIR/list" && "$NAAB" langs.naab 2>/dev/null | grep -E '^[a-z]+$' | sort -u)"
REG_COUNT="$(printf '%s\n' "$REGISTERED" | grep -c . || true)"
if [ "${REG_COUNT:-0}" -lt 2 ]; then
    skip "SO-00" "could not enumerate languages from the binary -- UNMEASURABLE"; report; exit 0
fi
ok "SO-00" "binary reports $REG_COUNT registered languages"

# Each snippet writes MARKER (same snippets as test_polyglot_gate_coverage.sh,
# which proves each one runs). SQL cannot write a file and is observed by the
# value it returns.
snippet_for() {  # $1 = language, $2 = marker path
    local m="$2"
    case "$1" in
        bash|sh|shell)      printf 'echo x > %s\n' "$m" ;;
        python)             printf 'open("%s","w").write("x")\n' "$m" ;;
        ruby)               printf 'File.write("%s","x")\n' "$m" ;;
        node|ts|typescript) printf 'require("fs").writeFileSync("%s","x")\n' "$m" ;;
        # QuickJS has no file API in a block; it is observed by its value.
        javascript)         printf '"GATE_" + "EXECUTED"\n' ;;
        php)                printf '<?php\nfile_put_contents("%s","x");\n' "$m" ;;
        rust)               printf 'use std::fs;\nfn main() { fs::write("%s","x").ok(); }\n' "$m" ;;
        go|golang)          printf 'package main\nimport "os"\nfunc main() { os.WriteFile("%s", []byte("x"), 0644) }\n' "$m" ;;
        cpp)                printf '#include <fstream>\nint main() { std::ofstream o("%s"); o << "x"; return 0; }\n' "$m" ;;
        cs|csharp)          printf 'class P { static void Main() { System.IO.File.WriteAllText("%s","x"); } }\n' "$m" ;;
        nim)                printf 'writeFile("%s", "x")\n' "$m" ;;
        zig)                printf 'const std = @import("std");\npub fn main() !void { try std.fs.cwd().writeFile(.{ .sub_path = "%s", .data = "x" }); }\n' "$m" ;;
        julia)              printf 'write("%s", "x")\n' "$m" ;;
        sql|sqlite)         printf "SELECT 'GATE_EXECUTED' AS m\n" ;;
        *)                  return 1 ;;
    esac
}

# Is python the EMBEDDED interpreter here? Only then is it in-process. Without
# pybind11 the build registers a subprocess python, which the gate must refuse.
cfg "$WDIR/pyprobe" audit elevated "$ON"
printf 'main {\n    let x = <<python\n40 + 2\n>>\n    print(x)\n}\n' > "$WDIR/pyprobe/p.naab"
PY_OUT="$(cd "$WDIR/pyprobe" && timeout 60 "$NAAB" p.naab 2>/dev/null)"
case "$PY_OUT" in *42*) PY_EMBEDDED=1 ;; *) PY_EMBEDDED=0 ;; esac

in_process() {  # the executors that override runsInProcess() to true
    case "$1" in
        javascript|sql|sqlite) return 0 ;;
        python) [ "$PY_EMBEDDED" = 1 ] ;;
        *) return 1 ;;
    esac
}

LAST_OUT=""
# Returns 0 when the block EXECUTED. $1=lang $2=dir (holding govern.json)
run_block() {
    local lang="$1" d="$2" body marker
    marker="$d/marker_${lang}.txt"; rm -f "$marker"
    body="$(snippet_for "$lang" "$marker")" || return 2
    case "$lang" in
        sql|sqlite)
            printf 'main {\n    let r = <<%s\n%s\n>>\n    print(r[0]["m"])\n}\n' "$lang" "$body" > "$d/blk.naab"
            LAST_OUT="$(cd "$d" && timeout 120 "$NAAB" blk.naab 2>&1)"
            [[ "$LAST_OUT" == *GATE_EXECUTED* ]]; return ;;
        javascript)
            printf 'main {\n    let r = <<%s\n%s\n>>\n    print(r)\n}\n' "$lang" "$body" > "$d/blk.naab"
            LAST_OUT="$(cd "$d" && timeout 120 "$NAAB" blk.naab 2>&1)"
            [[ "$LAST_OUT" == *GATE_EXECUTED* ]]; return ;;
    esac
    { echo 'main {'; echo "    <<$lang"; printf '%s\n' "$body"; echo '>>'; echo '}'; } > "$d/blk.naab"
    LAST_OUT="$(cd "$d" && timeout 120 "$NAAB" blk.naab 2>&1)"
    [ -f "$marker" ]
}

# ---- SO-01: coverage by construction ------------------------------------
MISSING=""
for lang in $REGISTERED; do snippet_for "$lang" /dev/null >/dev/null 2>&1 || MISSING="$MISSING $lang"; done
if [ -z "$MISSING" ]; then ok "SO-01" "every registered language has a case"
else bad "SO-01" "registered languages with no case (add one):$MISSING"; fi

# ---- SO-L: per language --------------------------------------------------
# Audit mode: the text checks report rather than block, so what is measured is
# the runtime gate alone. Elevated: fork is allowed, so a separate-process
# language that runs here is not being stopped by RLIMIT_NPROC either.
echo "--- per language: control with shell on, then shell off"
VIABLE_SUBPROC=""
for lang in $REGISTERED; do
    snippet_for "$lang" /dev/null >/dev/null 2>&1 || continue
    cfg "$WDIR/on_$lang" audit elevated "$ON"
    if ! run_block "$lang" "$WDIR/on_$lang"; then
        skip "SO-L-$lang" "does not run with shell ON (toolchain or snippet) -- UNMEASURABLE"
        continue
    fi
    cfg "$WDIR/off_$lang" audit elevated "$OFF"
    if in_process "$lang"; then
        if run_block "$lang" "$WDIR/off_$lang"; then
            ok "SO-L-$lang" "in-process: still runs with shell off"
        else
            bad "SO-L-$lang" "in-process language REFUSED with shell off (gate is too wide)" "$LAST_OUT"
        fi
    else
        VIABLE_SUBPROC="$VIABLE_SUBPROC $lang"
        if run_block "$lang" "$WDIR/off_$lang"; then
            bad "SO-L-$lang" "separate-process language RAN with shell disabled" "$LAST_OUT"
        else
            ok "SO-L-$lang" "separate-process: refused with shell off"
        fi
    fi
done

# One viable separate-process language drives the remaining arms. Prefer the
# ones the gate was written for.
PROBE=""
for l in cpp go rust php golang; do case " $VIABLE_SUBPROC " in *" $l "*) PROBE="$l"; break ;; esac; done
if [ -z "$PROBE" ]; then
    for a in SO-02 SO-03 SO-03c SO-04 SO-05 SO-05c SO-06 SO-07 SO-08 SO-08c; do
        skip "$a" "no separate-process language among cpp/go/rust/php runs here -- UNMEASURABLE"
    done
    report; [ $FAIL -eq 0 ] || exit 1; exit 0
fi
echo "--- remaining arms use: $PROBE"

# ---- SO-02: bare form ------------------------------------------------------
cfg "$WDIR/bare" audit elevated "false"
if run_block "$PROBE" "$WDIR/bare"; then bad "SO-02" "$PROBE RAN under \"shell\": false (bare form)" "$LAST_OUT"
else ok "SO-02" "bare \"shell\": false refuses $PROBE"; fi

# ---- SO-03: inherited through extends ----------------------------------------
ext_cfg() {  # $1 = dir, $2 = parent shell value
    mkdir -p "$1"
    cat > "$1/parent.json" <<EOF
{ "version": "4.0", "mode": "audit", "security": { "sandbox_level": "elevated" },
  "capabilities": { "shell": $2 } }
EOF
    cat > "$1/govern.json" <<EOF
{ "version": "4.0", "extends": "./parent.json", "mode": "audit",
  "security": { "sandbox_level": "elevated" } }
EOF
}
ext_cfg "$WDIR/ext_off" "$OFF"
if run_block "$PROBE" "$WDIR/ext_off"; then bad "SO-03" "$PROBE RAN although the extends parent disables shell" "$LAST_OUT"
else ok "SO-03" "shell disabled by an extends parent refuses $PROBE"; fi
ext_cfg "$WDIR/ext_on" "$ON"
if run_block "$PROBE" "$WDIR/ext_on"; then ok "SO-03c" "control: a shell-on parent leaves $PROBE running"
else bad "SO-03c" "control failed: $PROBE refused under a shell-on parent" "$LAST_OUT"; fi

# ---- SO-04: the refusal is the gate, not some other failure ------------------
run_block "$PROBE" "$WDIR/off_$PROBE" >/dev/null 2>&1
case "$LAST_OUT" in
    *"$PROBE execution denied by sandbox"*) ok "SO-04" "refusal names the cause" ;;
    *) bad "SO-04" "refusal did not come from the sandbox gate" "$LAST_OUT" ;;
esac

# ---- SO-06: the level is not the trigger --------------------------------------
# Compute-only, observed by the value it returns: at standard in enforce mode a
# file write is refused by other checks, which would read as the gate firing.
cfg "$WDIR/std" enforce standard "$ON"
printf 'main {\n    let x = <<%s\n40 + 2\n>>\n    print(x)\n}\n' "$PROBE" > "$WDIR/std/v.naab"
LAST_OUT="$(cd "$WDIR/std" && timeout 120 "$NAAB" v.naab 2>&1)"
case "$LAST_OUT" in
    *"governance disables running commands"*)
        bad "SO-06" "the gate fired at standard with shell ON (it must key on policy, not level)" "$LAST_OUT" ;;
    *42*) ok "SO-06" "control: shell on at the enforce default still runs a compute-only $PROBE block" ;;
    *)    skip "SO-06" "$PROBE expression does not evaluate at standard here -- UNMEASURABLE" ;;
esac

# ---- SO-07: --lock in a shell-off project ------------------------------------
cfg "$WDIR/lock" audit elevated "$OFF"
printf 'main {\n    print("ok")\n}\n' > "$WDIR/lock/h.naab"
LOCK_OUT="$(cd "$WDIR/lock" && timeout 60 "$NAAB" h.naab --lock 2>&1)"; LRC=$?
if [ $LRC -eq 0 ] && [ -f "$WDIR/lock/.naab/naab.lock" ]; then
    ok "SO-07" "--lock writes the lockfile in a shell-off project"
else
    bad "SO-07" "--lock failed in a shell-off project (exit $LRC)" "$LOCK_OUT"
fi

# ---- SO-08: a refused language is UNAVAILABLE to consensus verification ------
# polyglot_optimization.verification re-runs a block's result in other
# languages and asked getExecutor() whether each was available. The gate makes
# that THROW for a refused language, and the throw escaped: an in-process
# python block in a shell-off project aborted the program. Tree-walker only
# (the VM does not run this verification). SO-08c is the control that the
# verification really runs (a shell-on project prints the agreement line).
vcfg() {  # $1 = dir, $2 = shell enabled
    mkdir -p "$1"
    printf '{"version":"4.0","mode":"enforce","security":{"sandbox_level":"elevated"},"capabilities":{"shell":{"enabled":%s}},"polyglot_optimization":{"enabled":true,"verification":{"enabled":true,"consensus_languages":["python","%s"]}}}\n' "$2" "$VLANG" > "$1/govern.json"
    printf 'main {\n    let x = <<python\nsum([1, 2, 3])\n>>\n    print("SUM=" + x)\n}\n' > "$1/v.naab"
}
# The verifier must be a language with a verification template (rust and go
# have one; cpp and php do not, and verification silently does not run).
VLANG=""
for l in rust go; do case " $VIABLE_SUBPROC " in *" $l "*) VLANG="$l"; break ;; esac; done
if [ "$PY_EMBEDDED" != 1 ] || [ -z "$VLANG" ]; then
    skip "SO-08c" "no embedded python or no rust/go verifier -- UNMEASURABLE"; skip "SO-08" "no embedded python or no rust/go verifier -- UNMEASURABLE"
else
    vcfg "$WDIR/ver_on" true
    VON="$(cd "$WDIR/ver_on" && timeout 120 "$NAAB" --tree-walk v.naab 2>&1)"
    case "$VON" in
        *agree*) case "$VON" in *SUM=6*) ok "SO-08c" "control: consensus verification runs with shell on" ;;
                                 *) skip "SO-08c" "verification ran but the block did not print -- UNMEASURABLE"; VON="" ;; esac ;;
        *) skip "SO-08c" "verification did not run here -- UNMEASURABLE"; VON="" ;;
    esac
    if [ -n "$VON" ]; then
        vcfg "$WDIR/ver_off" false
        VOFF="$(cd "$WDIR/ver_off" && timeout 120 "$NAAB" --tree-walk v.naab 2>&1)"; VRC=$?
        case "$VOFF" in
            *SUM=6*) ok "SO-08" "shell off: the block still runs; the refused verifier is skipped" ;;
            *) bad "SO-08" "shell off: consensus verification aborted an allowed python block (exit $VRC)" "$VOFF" ;;
        esac
    else
        skip "SO-08" "control did not run -- UNMEASURABLE"
    fi
fi

# ---- SO-05: mid-run reload (LAST: it installs a trusted key, after which
# every unsigned fixture above would be an INTEGRITY BLOCK) --------------------------------------------------
# The reload site is one of the four places that act on shell-off. A signed
# shell-on config is swapped for a signed shell-off one by an EXTERNAL operator
# (tests/helpers/config_swap.sh); env.get() triggers reloadIfChanged().
"$NAAB" --keygen "$WDIR/k.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/k.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/k.pem"
sign_dir() { (cd "$1" && "$NAAB" --sign-governance >/dev/null 2>&1); }
reload_prog() {  # $1 = dir, $2 = do_swap (1/0)
    local d="$1" marker="$1/marker_reload.txt" swap_line=""
    rm -f "$marker"
    [ "$2" = 1 ] && swap_line="_swap(\"$WDIR/rl_next/govern.json.sig\", \"$d/govern.json.sig\")
_swap(\"$WDIR/rl_next/govern.json\", \"$d/govern.json\")"
    cat > "$d/r.naab" <<EOF
use env
main {
    let s = <<python
import os, time
def _swap(src, dst):
    if os.path.exists(".swap_done"):
        os.remove(".swap_done")
    with open(".swap_req", "w") as f:
        f.write(src + "\t" + dst + "\n")
    open(".swap_req.ready", "w").close()
    for _ in range(1200):
        if os.path.exists(".swap_done"):
            return
        time.sleep(0.05)
    raise RuntimeError("config swap operator did not respond")
time.sleep(1)
$swap_line
"swapped"
>>
    print(s)
    let h = env.get("HOME")
    <<$PROBE
$(snippet_for "$PROBE" "$marker")
>>
    print("REACHED_END")
}
EOF
    start_swap_operator "$d"
    LAST_OUT="$(cd "$d" && timeout 180 "$NAAB" r.naab 2>&1)"
    [ -f "$marker" ]
}
cfg "$WDIR/rl_next" audit elevated "$OFF"; sign_dir "$WDIR/rl_next"
cfg "$WDIR/rl" audit elevated "$ON"; sign_dir "$WDIR/rl"
if [ ! -f "$WDIR/rl/govern.json.sig" ] || [ ! -f "$WDIR/rl_next/govern.json.sig" ] || [ "$PY_EMBEDDED" != 1 ]; then
    skip "SO-05" "signing or embedded python unavailable -- UNMEASURABLE"
    skip "SO-05c" "signing or embedded python unavailable -- UNMEASURABLE"
else
    if reload_prog "$WDIR/rl" 1; then
        bad "SO-05" "$PROBE RAN after a reload disabled shell" "$LAST_OUT"
    else
        case "$LAST_OUT" in
            *"$PROBE execution denied by sandbox"*) ok "SO-05" "a reload that disables shell refuses the next $PROBE block" ;;
            *) bad "SO-05" "$PROBE did not run, but not because of the gate (reload never applied?)" "$LAST_OUT" ;;
        esac
    fi
    cfg "$WDIR/rl" audit elevated "$ON"; sign_dir "$WDIR/rl"
    if reload_prog "$WDIR/rl" 0; then ok "SO-05c" "control: without the swap the same program runs $PROBE"
    else bad "SO-05c" "control failed: $PROBE refused with no reload" "$LAST_OUT"; fi
fi
unset NAAB_SIGNING_KEY

report
[ $FAIL -eq 0 ] || exit 1
exit 0
