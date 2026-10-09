#!/usr/bin/env bash
# test_subprocess_env_scrub.sh -- which environment variables reach a CHILD
# process, on every route NAAb starts one by.
#
# capabilities.env_vars.blocked_read stops env.get() (HARD), and the subprocess
# scrub (blocked_subprocess_vars / subprocess_scrub_mode) removes variables from
# child environments. Before this suite, measured on b6d2c62:
#   R  a blocked_read variable reached every child route that ran: <<shell>>,
#      embedded <<python>>, ruby, node, php, go, process.run, async fn.
#   B  setting ANY scrub key made the child start through execve(), which does
#      not search PATH, so bare names (php, go, process.run("printenv")) exited
#      127 -- the hardening broke the languages it was meant to harden.
#   P  setting a scrub key also switched off proxy stripping (network disabled):
#      the containment edits were made to the child's environ, which execve()
#      with a prebuilt envp ignores. The persistent executors (shell, node,
#      ruby) never stripped proxies at all.
#   W  the scrub policy was thread_local and set only on the thread that loaded
#      the config, so a VM async fn's children got no scrubbing.
#   L  for the same reason a mid-run reload never reached the policy.
#
# Every arm reads three variables, each holding a fresh random token:
#   A  NAAB_ES_BR  listed in blocked_read             (the subject of R)
#   B  NAAB_ES_BS  listed in blocked_subprocess_vars  (the subject of B and W)
#   C  NAAB_ES_NO  listed nowhere                      (control: the route ran
#                                                       and its output reached us)
# VIABILITY: before any arm asserts something about a route, the same route
# runs under a config with NO policy, and A and C must both reach its child.
# A route that fails that is UNMEASURABLE here (a toolchain or executor this
# platform lacks -- e.g. Windows has no <<shell>> executor), never a pass and
# never a failure. A route that passes it and then does not run under the
# policy under test is a FAILURE: the policy broke it.
#
# Every external program is handed a NATIVE path (tests/helpers/native_path.sh):
# under MSYS2 naab-lang is a native Windows binary and cannot start /usr/bin/x.
#
# ES_SIMULATE_UNVIABLE=route,route marks routes unviable, to check that the
# suite reports them UNMEASURABLE rather than failing (a test of the test).
#
# Usage: bash tests/security/test_subprocess_env_scrub.sh [path/to/naab-lang]

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="${1:-$SCRIPT_DIR/../../build/naab-lang}"
NAAB="$(cd "$(dirname "$NAAB")" 2>/dev/null && pwd)/$(basename "$NAAB")"

PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS [$1] $2"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL [$1] $2"; FAIL=$((FAIL + 1)); }
skip() { echo "  UNMEASURABLE [$1] $2"; SKIP=$((SKIP + 1)); }

echo "=== Subprocess environment scrubbing: every route a child is started by ==="

if [ ! -x "$NAAB" ]; then
    echo "  FAIL: naab-lang not built at $NAAB (UNMEASURABLE, not a pass)"
    exit 1
fi

TMPBASE="$(mktemp -d "${TMPDIR:-/tmp}/naab_envscrub.XXXXXX")"
# An EMPTY trust store for every arm: the fixtures are unsigned, and on a
# machine with a trusted key installed an unsigned config is an integrity
# block (exit 3) -- which the arms below would read as "route did not run".
source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/config_swap.sh"
source "$SCRIPT_DIR/../helpers/native_path.sh"
setup_isolated_trust
cleanup() { stop_swap_operators; teardown_isolated_trust; rm -rf "$TMPBASE"; }
trap cleanup EXIT

# The programs the probes start, as paths the binary can open.
PRINTENV_N=""; SH_N=""
p="$(command -v printenv 2>/dev/null || true)"; [ -n "$p" ] && PRINTENV_N="$(native_path "$p")"
p="$(command -v sh 2>/dev/null || true)";       [ -n "$p" ] && SH_N="$(native_path "$p")"

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------
# mkcfg DIR MODE NETWORK(true|false) POLICY(none|read|scrub|both) [HOOKFILE]
# Written by python (json.dumps) rather than a heredoc, so no shell expansion
# can drop a key's quotes, and read back as JSON before any verdict is trusted.
mkcfg() {
    python3 -I - "$@" "$SH_N" <<'PY'
import json, sys
d, mode, network, policy = sys.argv[1], sys.argv[2], sys.argv[3] == "true", sys.argv[4]
hookfile = sys.argv[5] if len(sys.argv) > 6 else ""
sh = sys.argv[-1]
ev = {"read": True}
if policy in ("read", "both"):
    ev["blocked_read"] = ["NAAB_ES_BR"]
if policy in ("scrub", "both"):
    ev["subprocess_scrub_mode"] = "blocklist"
    ev["blocked_subprocess_vars"] = ["NAAB_ES_BS"]
cfg = {
    "version": "5.0", "mode": mode,
    "languages": {"allowed": ["shell", "python", "ruby", "php", "node", "go"]},
    "capabilities": {"shell": {"enabled": True}, "network": {"enabled": network},
                     "filesystem": {"mode": "read_write"}, "env_vars": ev},
    "security": {"sandbox_level": "elevated"},
}
if hookfile:
    cfg["hooks"] = {"on_complete": {"command": sh,
                                    "args": ["-c", "printenv > " + hookfile],
                                    "timeout": 5}}
with open(d + "/govern.json", "w") as f:
    json.dump(cfg, f, indent=2)
PY
    if ! python3 -I -c 'import json,sys; json.load(sys.stdin)' < "$1/govern.json" 2>/dev/null; then
        echo "  FIXTURE BROKEN: $1/govern.json is not valid JSON"
        exit 1
    fi
}

ECHO3='A=$NAAB_ES_BR B=$NAAB_ES_BS C=$NAAB_ES_NO P=$HTTPS_PROXY'
write_route() { # write_route FILE ROUTE
    local f="$1"
    case "$2" in
    shell)  printf 'main {\n    let r = <<shell\necho "%s"\n>>\n    print("V=" + string(r))\n}\n' "$ECHO3" > "$f" ;;
    python) cat > "$f" <<'EOF'
main {
    let r = <<python
import os
" ".join(k + "=" + os.environ.get(v, "") for k, v in (("A", "NAAB_ES_BR"), ("B", "NAAB_ES_BS"), ("C", "NAAB_ES_NO"), ("P", "HTTPS_PROXY")))
>>
    print("V=" + string(r))
}
EOF
    ;;
    ruby)   cat > "$f" <<'EOF'
main {
    let r = <<ruby
puts "A=#{ENV['NAAB_ES_BR']} B=#{ENV['NAAB_ES_BS']} C=#{ENV['NAAB_ES_NO']} P=#{ENV['HTTPS_PROXY']}"
>>
    print("V=" + string(r))
}
EOF
    ;;
    node)   cat > "$f" <<'EOF'
main {
    let r = <<node
const e = process.env
console.log("A=" + (e.NAAB_ES_BR || "") + " B=" + (e.NAAB_ES_BS || "") + " C=" + (e.NAAB_ES_NO || "") + " P=" + (e.HTTPS_PROXY || ""))
>>
    print("V=" + string(r))
}
EOF
    ;;
    php)    cat > "$f" <<'EOF'
main {
    let r = <<php
echo "A=" . getenv("NAAB_ES_BR") . " B=" . getenv("NAAB_ES_BS") . " C=" . getenv("NAAB_ES_NO") . " P=" . getenv("HTTPS_PROXY");
>>
    print("V=" + string(r))
}
EOF
    ;;
    go)     cat > "$f" <<'EOF'
main {
    let r = <<go
package main
import ("fmt"; "os")
func main() { fmt.Print("A=" + os.Getenv("NAAB_ES_BR") + " B=" + os.Getenv("NAAB_ES_BS") + " C=" + os.Getenv("NAAB_ES_NO") + " P=" + os.Getenv("HTTPS_PROXY")) }
>>
    print("V=" + string(r))
}
EOF
    ;;
    # process.run by FULL path: the subject here is the environment, not PATH
    # lookup (that is Group B, which uses the bare name on purpose).
    printenv) printf 'use process\nmain {\n    let r = process.run("%s", [])\n    print("V=" + r["stdout"])\n}\n' "$PRINTENV_N" > "$f" ;;
    printenv_bare) printf 'use process\nmain {\n    let r = process.run("printenv", [])\n    print("EXIT=" + string(r["exit_code"]) + " V=" + r["stdout"])\n}\n' > "$f" ;;
    sh_c)   printf 'use process\nmain {\n    let r = process.run("%s", ["-c", "echo %s"])\n    print("V=" + r["stdout"])\n}\n' "$SH_N" "$ECHO3" > "$f" ;;
    async_run) printf 'use process\nasync fn f() {\n    let r = process.run("%s", [])\n    return r["stdout"]\n}\nmain {\n    let fut = f()\n    let s = await fut\n    print("V=" + s)\n}\n' "$PRINTENV_N" > "$f" ;;
    async_shell) printf 'async fn f() {\n    let r = <<shell\necho "%s"\n>>\n    return string(r)\n}\nmain {\n    let fut = f()\n    let s = await fut\n    print("V=" + s)\n}\n' "$ECHO3" > "$f" ;;
    env_get) printf 'use env\nmain {\n    let v = env.get("NAAB_ES_BR")\n    print("V=" + string(v))\n}\n' > "$f" ;;
    *) echo "unknown route $2"; exit 1 ;;
    esac
}

# run_arm DIR ROUTE ENGINE(vm|tw) -> sets OUT, RC, HAS_A, HAS_B, HAS_C, HAS_P, LOADED
TA="" TB="" TC="" TP=""
token() { printf '%s%s' "$1" "$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')"; }
run_arm() {
    local dir="$1" route="$2" engine="$3" flags=()
    [ "$engine" = tw ] && flags=(--tree-walk)
    write_route "$dir/p.naab" "$route"
    TA="$(token TA)"; TB="$(token TB)"; TC="$(token TC)"; TP="$(token TP)"
    OUT="$(cd "$dir" && NAAB_ES_BR="$TA" NAAB_ES_BS="$TB" NAAB_ES_NO="$TC" HTTPS_PROXY="$TP" \
           timeout 120 "$NAAB" "${flags[@]}" p.naab --timeout 90 2>&1)"
    RC=$?
    HAS_A=0; HAS_B=0; HAS_C=0; HAS_P=0; LOADED=0
    case "$OUT" in *"$TA"*) HAS_A=1;; esac
    case "$OUT" in *"$TB"*) HAS_B=1;; esac
    case "$OUT" in *"$TC"*) HAS_C=1;; esac
    case "$OUT" in *"$TP"*) HAS_P=1;; esac
    loaded_own "$dir" "$OUT" && LOADED=1
}
# loaded_own DIR TEXT: TEXT has a "[governance] Loaded:" line naming DIR's own
# govern.json. Matched on DIR's basename (mktemp-random, so unique) with every
# backslash turned into a slash, NOT on DIR's full path: a native Windows build
# prints the path it built itself (D:\...\arm.X\govern.json), which is neither
# the MSYS path the shell holds nor cygpath -m's D:/... form. Comparing full
# paths there read every config as not loaded (build-windows on 1870d482:
# R-00 and four route arms FAILED while the policy demonstrably applied).
loaded_own() {
    local want="/$(basename "$1")/govern.json (mode:" line
    while IFS= read -r line; do
        line="${line//\\//}"
        case "$line" in *"[governance] Loaded: "*"$want"*) return 0 ;; esac
    done <<< "$2"
    return 1
}
# The probe's own control, on both path vocabularies and a CRLF line ending,
# plus a sibling directory that must NOT match: a probe that cannot fire, or
# fires on anything, would turn every arm below into a verdict about itself.
_lo_ok=1
loaded_own /x/arm.Q1 '[governance] Loaded: /x/arm.Q1/govern.json (mode: enforce)' || _lo_ok=0
loaded_own /x/arm.Q1 $'[governance] Loaded: D:\\a\\x\\arm.Q1\\govern.json (mode: enforce)\r' || _lo_ok=0
loaded_own /x/arm.Q1 '[governance] Loaded: /x/arm.Q2/govern.json (mode: enforce)' && _lo_ok=0
if [ "$_lo_ok" != 1 ]; then
    echo "  FAIL: the loaded-config probe fails its own control -- every verdict below would be about the probe"
    exit 1
fi
show() { printf '%s\n' "$OUT" | grep -vE '^\[governance\] Warning|^$' | head -8 | sed 's/^/        /'; }
newdir() { local d; d="$(mktemp -d "$TMPBASE/arm.XXXXXX")"; d="$(cd "$d" && pwd -P)"; echo "$d"; }

# viable ROUTE ENGINE: the route carries A and C to its child under a config
# with no policy (cached). Sets WHY when it does not.
declare -A VIABLE=()
WHY=""
viable() {
    local key="$1/$2" d
    case ",${ES_SIMULATE_UNVIABLE:-}," in *",$1,"*) WHY="simulated"; return 1;; esac
    if [ -z "${VIABLE[$key]+x}" ]; then
        d="$(newdir)"; mkcfg "$d" enforce true none
        run_arm "$d" "$1" "$2"
        if [ "$HAS_A" = 1 ] && [ "$HAS_C" = 1 ]; then VIABLE[$key]=1; else VIABLE[$key]="rc=$RC"; fi
    fi
    [ "${VIABLE[$key]}" = 1 ] && return 0
    WHY="${VIABLE[$key]}"; return 1
}
unviable() { skip "$1" "route '$2' does not carry the variables here even with no policy ($WHY) -- executor or program unavailable"; }

# ---------------------------------------------------------------------------
# Group R: a blocked_read variable reaches no child process
# ---------------------------------------------------------------------------
echo ""
echo "--- R: blocked_read reaches no child (enforce, elevated, shell on) ---"

D="$(newdir)"; mkcfg "$D" enforce true read
run_arm "$D" env_get vm
if [ "$RC" = 3 ] && [ "$HAS_A" = 0 ] && [ "$LOADED" = 1 ]; then
    pass R-00 "env.get() of the blocked variable is refused (exit 3) -- the policy is live in this config"
else
    fail R-00 "control: env.get() not refused (rc=$RC, value seen=$HAS_A, own config loaded=$LOADED)"; show
fi

for route in shell python ruby node php go printenv sh_c; do
    for engine in vm tw; do
        id="R-$route/$engine"
        if ! viable "$route" "$engine"; then unviable "$id" "$route"; continue; fi
        D="$(newdir)"; mkcfg "$D" enforce true read
        run_arm "$D" "$route" "$engine"
        if [ "$HAS_C" != 1 ] || [ "$LOADED" != 1 ]; then
            fail "$id" "route ran with no policy but not with blocked_read set (rc=$RC, own config loaded=$LOADED)"; show
        elif [ "$HAS_A" = 1 ]; then
            fail "$id" "blocked_read variable reached the child"
        else
            pass "$id" "blocked_read variable withheld; unlisted variable still passed"
        fi
    done
done

# A VM async fn runs on its own thread.
for route in async_run async_shell; do
    id="R-$route/vm"
    if ! viable "$route" vm; then unviable "$id" "$route"; continue; fi
    D="$(newdir)"; mkcfg "$D" enforce true read
    run_arm "$D" "$route" vm
    if [ "$HAS_C" != 1 ]; then
        fail "$id" "route ran with no policy but not with blocked_read set (rc=$RC)"; show
    elif [ "$HAS_A" = 1 ]; then
        fail "$id" "blocked_read variable reached a child started from an async fn"
    else
        pass "$id" "blocked_read variable withheld from an async fn's child"
    fi
done

if viable printenv vm; then
    # The withheld variable is NAMED once, and its value never printed.
    D="$(newdir)"; mkcfg "$D" enforce true read
    run_arm "$D" printenv vm
    n="$(printf '%s\n' "$OUT" | grep -c 'Withheld.*NAAB_ES_BR' || true)"
    if [ "$HAS_C" = 1 ] && [ "$n" = 1 ] && [ "$HAS_A" = 0 ]; then
        pass R-notice "one stderr notice names the withheld variable; its value appears nowhere"
    else
        fail R-notice "expected exactly one notice naming NAAB_ES_BR and no value (notices=$n, value seen=$HAS_A, ran=$HAS_C)"; show
    fi

    # Audit mode observes and does not enforce: env.get() is not refused there,
    # so children are not scrubbed of blocked_read either. Pinned as a decision.
    D="$(newdir)"; mkcfg "$D" audit true read
    run_arm "$D" env_get vm
    AUDIT_ENVGET_RC="$RC"; AUDIT_ENVGET_A="$HAS_A"
    run_arm "$D" printenv vm
    if [ "$AUDIT_ENVGET_RC" = 0 ] && [ "$AUDIT_ENVGET_A" = 1 ] && [ "$HAS_C" = 1 ] && [ "$HAS_A" = 1 ]; then
        pass R-audit "audit mode: env.get() and children both see the variable (blocked_read is enforced only in enforce mode)"
    else
        fail R-audit "audit mode: env.get rc=$AUDIT_ENVGET_RC seen=$AUDIT_ENVGET_A; child ran=$HAS_C seen=$HAS_A"; show
    fi

    # Hooks are commands the OPERATOR configured, not program code: a
    # blocked_read variable stays available to them (a hook may need the token a
    # script must not read). The hook writes its environment to a file.
    D="$(newdir)"; HOOKOUT="$D/hook_env.txt"; mkcfg "$D" enforce true read "$(native_path "$HOOKOUT")"
    run_arm "$D" printenv vm
    if [ ! -f "$HOOKOUT" ]; then
        skip R-hook "on_complete hook did not run in this configuration (no hook output) -- cannot measure"
    else
        HOOK_ENV="$(cat "$HOOKOUT")"
        case "$HOOK_ENV" in
            *"$TA"*) if [ "$HAS_A" = 0 ] && [ "$HAS_C" = 1 ]; then
                         pass R-hook "the operator's hook still receives the variable the program's child was denied"
                     else
                         fail R-hook "hook saw the variable, but the program's child result is wrong (child saw=$HAS_A, ran=$HAS_C)"
                     fi ;;
            *) fail R-hook "the operator's hook lost a blocked_read variable" ;;
        esac
    fi

    # mode "off": the subprocess lists still apply (they did before, set at load
    # whatever the mode), and blocked_read does not (env.get() is not enforced).
    # Loosening the first would be a silent regression for any config that keeps
    # its scrub lists while governance is switched off.
    D="$(newdir)"; mkcfg "$D" off true both
    run_arm "$D" printenv vm
    if [ "$HAS_C" != 1 ]; then
        fail S-off "route did not run under mode off (rc=$RC)"; show
    elif [ "$HAS_B" = 1 ]; then
        fail S-off "mode off: a blocked_subprocess_vars variable reached the child"
    elif [ "$HAS_A" != 1 ]; then
        fail S-off "mode off: the blocked_read variable was withheld, though env.get() is not enforced in mode off"
    else
        pass S-off "mode off: the subprocess list still scrubs; blocked_read is not applied (as env.get())"
    fi
else
    for id in R-notice R-audit R-hook S-off; do unviable "$id" printenv; done
fi

# ---------------------------------------------------------------------------
# Group B: a scrub policy must not break PATH lookup
# ---------------------------------------------------------------------------
echo ""
echo "--- B: bare command names still resolve when a scrub policy is set ---"

if viable printenv_bare vm; then
    D="$(newdir)"; mkcfg "$D" enforce true scrub
    run_arm "$D" printenv_bare vm
    case "$OUT" in *"EXIT=0 "*) bexit=0;; *) bexit=1;; esac
    if [ "$bexit" = 0 ] && [ "$HAS_C" = 1 ] && [ "$HAS_B" = 0 ]; then
        pass B-01 'process.run("printenv") (bare name) runs under a scrub policy, and the scrubbed variable is gone'
    else
        fail B-01 "bare-name process.run under a scrub policy (exit0=$((1-bexit)), ran=$HAS_C, scrubbed var seen=$HAS_B)"; show
    fi
else
    unviable B-01 printenv_bare
fi

for route in php go; do
    id="B-$route"
    if ! viable "$route" vm; then unviable "$id" "$route"; continue; fi
    D="$(newdir)"; mkcfg "$D" enforce true scrub
    run_arm "$D" "$route" vm
    if [ "$HAS_C" = 1 ] && [ "$HAS_B" = 0 ]; then
        pass "$id" "$route block runs under a scrub policy, and the scrubbed variable is gone"
    else
        fail "$id" "$route block under a scrub policy (ran=$HAS_C, scrubbed var seen=$HAS_B, rc=$RC)"; show
    fi
done

# ---------------------------------------------------------------------------
# Group P: proxy stripping (network disabled) holds with and without a scrub
# policy, on both spawn paths
# ---------------------------------------------------------------------------
echo ""
echo "--- P: proxy variables are stripped from children when network is disabled ---"

p_arm() { # p_arm ID ROUTE NETWORK POLICY EXPECT_PROXY(0|1) DESC
    if ! viable "$2" vm; then unviable "$1" "$2"; return; fi
    local D; D="$(newdir)"; mkcfg "$D" enforce "$3" "$4"
    run_arm "$D" "$2" vm
    if [ "$HAS_C" != 1 ]; then
        fail "$1" "route ran with no policy but not in this configuration (rc=$RC)"; show
    elif [ "$HAS_P" = "$5" ]; then
        pass "$1" "$6"
    else
        fail "$1" "$6 -- proxy seen=$HAS_P, expected $5"
    fi
}
p_arm P-00 printenv true  scrub 1 "control: network on, process.run child sees HTTPS_PROXY"
p_arm P-01 printenv false scrub 0 "network off + scrub policy: process.run child has no HTTPS_PROXY"
p_arm P-01b printenv false none 0 "network off, no scrub policy: process.run child has no HTTPS_PROXY"
p_arm P-02 shell   true  none  1 "control: network on, <<shell>> child sees HTTPS_PROXY"
p_arm P-03 shell   false none  0 "network off: <<shell>> (persistent executor) child has no HTTPS_PROXY"

# ---------------------------------------------------------------------------
# Group W: the scrub policy reaches worker threads
# ---------------------------------------------------------------------------
echo ""
echo "--- W: a VM async fn's children are scrubbed like the main thread's ---"

for route in async_run async_shell; do
    id="W-$route"
    if ! viable "$route" vm; then unviable "$id" "$route"; continue; fi
    D="$(newdir)"; mkcfg "$D" enforce true scrub
    run_arm "$D" "$route" vm
    if [ "$HAS_C" != 1 ]; then
        fail "$id" "route ran with no policy but not with a scrub policy (rc=$RC)"; show
    elif [ "$HAS_B" = 1 ]; then
        fail "$id" "blocked_subprocess_vars variable reached a child started from an async fn"
    else
        pass "$id" "blocked_subprocess_vars variable scrubbed on the async fn's thread"
    fi
done

# ---------------------------------------------------------------------------
# Group L: a mid-run reload reaches the next child
# ---------------------------------------------------------------------------
# The program starts under a config with no env policy, an EXTERNAL operator
# swaps in a signed one that adds blocked_read and blocked_subprocess_vars,
# env.get() triggers the reload (checkEnvVarRead calls reloadIfChanged), and
# the next process.run must see the new policy. B isolates the reload: the
# old thread_local copy was set once at load, so a reload never reached it.
echo ""
echo "--- L: a mid-run reload reaches the next child ---"
IS_WINDOWS=0
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) IS_WINDOWS=1 ;; esac
[ -n "${WINDIR:-}" ] && IS_WINDOWS=1
if [ "$IS_WINDOWS" = 1 ]; then
    # Decided in 0304af3a and followed by every reload suite since: Windows
    # file locking prevents replacing govern.json under a running program.
    # build-windows on 1870d482 reached the second spawn unscrubbed with no
    # "Reload rejected" -- consistent with the swap never landing, which
    # this arm could not tell apart from a reload that did not apply.
    skip L-01 "mid-run file swap requires POSIX file semantics"
elif ! viable printenv vm; then
    unviable L-01 printenv
else
    L_TRUST="$NAAB_TRUST_STORE_DIR"
    export NAAB_TRUST_STORE_DIR="$(mktemp -d "$TMPBASE/trust.XXXXXX")"
    LD="$(newdir)"; LN="$(newdir)"
    "$NAAB" --keygen "$TMPBASE/l-key.pem" >/dev/null 2>&1
    "$NAAB" --trust-key "$TMPBASE/l-key.pem.pub" >/dev/null 2>&1
    mkcfg "$LD" enforce true none
    mkcfg "$LN" enforce true both
    (cd "$LD" && NAAB_SIGNING_KEY="$TMPBASE/l-key.pem" "$NAAB" --sign-governance >/dev/null 2>&1)
    (cd "$LN" && NAAB_SIGNING_KEY="$TMPBASE/l-key.pem" "$NAAB" --sign-governance >/dev/null 2>&1)
    if [ ! -f "$LD/govern.json.sig" ] || [ ! -f "$LN/govern.json.sig" ]; then
        skip L-01 "could not sign the reload fixtures -- cannot measure"
    else
        cat > "$LD/p.naab" <<EOF
use env
use process
main {
    let r1 = process.run("$PRINTENV_N", [])
    print("BEFORE_RELOAD " + r1["stdout"])
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
_swap("$LN/govern.json.sig", "$LD/govern.json.sig")
_swap("$LN/govern.json", "$LD/govern.json")
print("SWAPPED_OK")
>>
    let h = env.get("HOME")
    let r2 = process.run("$PRINTENV_N", [])
    print("AFTER_RELOAD " + r2["stdout"])
}
EOF
        start_swap_operator "$LD"
        TA="$(token TA)"; TB="$(token TB)"; TC="$(token TC)"
        OUT="$(cd "$LD" && NAAB_ES_BR="$TA" NAAB_ES_BS="$TB" NAAB_ES_NO="$TC" \
               timeout 120 "$NAAB" p.naab --timeout 90 2>&1)"
        stop_swap_operators
        BEFORE="${OUT%%AFTER_RELOAD*}"
        case "$OUT" in *AFTER_RELOAD*) AFTER="${OUT#*AFTER_RELOAD}" ;; *) AFTER="" ;; esac
        has() { case "$1" in *"$2"*) echo 1;; *) echo 0;; esac; }
        if [ "$(has "$BEFORE" "$TC")" != 1 ]; then
            fail L-01 "the first spawn did not run, though the route is viable"; show
        elif [ ! -f "$LD/.swap_done" ] || ! cmp -s "$LD/govern.json" "$LN/govern.json"; then
            # The swap is driven from a <<python>> block and an external operator;
            # where that machinery does not work the reload is never staged --
            # and the program can reach its second spawn anyway, so reaching it
            # proves nothing. Only a swap whose result is on disk is measured.
            skip L-01 "the config swap did not happen in this environment -- cannot stage the reload"
        elif [ -z "$AFTER" ]; then
            fail L-01 "the config was swapped but the program never reached its second spawn"; show
        elif [ "$(has "$BEFORE" "$TA")" != 1 ] || [ "$(has "$BEFORE" "$TB")" != 1 ]; then
            fail L-00 "control: before the reload the child should see both variables (A=$(has "$BEFORE" "$TA") B=$(has "$BEFORE" "$TB"))"
        else
            pass L-00 "control: before the reload the child sees both variables"
            case "$OUT" in *"Reload rejected"*) fail L-01 "the tightening reload was rejected"; show ;;
            *)
                if [ "$(has "$AFTER" "$TB")" = 0 ] && [ "$(has "$AFTER" "$TA")" = 0 ]; then
                    pass L-01 "after the reload the next child is scrubbed of both (subprocess list and blocked_read)"
                else
                    fail L-01 "after the reload: blocked_subprocess_vars seen=$(has "$AFTER" "$TB"), blocked_read seen=$(has "$AFTER" "$TA")"
                fi ;;
            esac
        fi
    fi
    export NAAB_TRUST_STORE_DIR="$L_TRUST"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP unmeasurable"
[ "$FAIL" -eq 0 ]
