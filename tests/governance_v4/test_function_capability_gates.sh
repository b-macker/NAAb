#!/usr/bin/env bash
# ============================================================
# test_function_capability_gates.sh — the function tier reaches every gate
#
# capabilities.functions shipped wired to ONE gate (checkFilesystemAllowed).
# This covers the rest: NET_CONNECT, SHELL_EXEC, ENV_READ, ENV_WRITE and
# AGENT_SEND, on BOTH engines.
#
# Three things here are not just "more of the same":
#
# FG-07 is a defect this suite found rather than confirmed. The VM DERIVES the
# attribution stack from frames_ at each governed site instead of maintaining
# it, and it only synced inside the stdlib-call path. So a polyglot block was
# attributed to whatever function last made a governed stdlib call: measured, a
# <<shell>> block inside bravo() reported as 'alpha' on the VM while the
# tree-walker (RAII, continuously maintained) correctly said 'bravo'. Wiring
# SHELL_EXEC is what made that visible; the arm pins both engines to 'bravo'.
#
# AGENT_SEND is gated at the agent DISPATCH TABLE, not inside agentSend(),
# because the attribution stack is thread_local and batch/fan_out run
# agentSend() on pool workers where it is EMPTY. Under intersection an empty
# stack is FEWER constraints, so gating deeper would have made a batched send
# strictly more permissive than the same send on the main thread. FG-06 covers
# the gate; the reasoning is why it sits where it does.
#
# FG-09 is the arm that makes the rest mean anything. Every other arm expects a
# REFUSAL, and an arm expecting refusal passes for free whenever the instrument
# is broken — a bad fixture, a config that fails to parse, a program that dies
# before reaching the call. FG-09/FG-10/FG-08 expect SUCCESS and are the only
# arms that can detect that.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAAB="$SCRIPT_DIR/../../build/naab-lang"

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
W="${_SYSTMP}/fncap-gates-$$"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
PASS_COUNT=0; FAIL_COUNT=0; SKIP_COUNT=0; FAILURES=""
pass() { PASS_COUNT=$((PASS_COUNT + 1)); echo -e "  ${GREEN}PASS${NC} [$1] $2"; }
fail() { FAIL_COUNT=$((FAIL_COUNT + 1)); echo -e "  ${RED}FAIL${NC} [$1] $2"; [ -n "${3:-}" ] && echo -e "       ${RED}-> $3${NC}"; FAILURES="${FAILURES}\n  [$1] $2"; }
skip() { SKIP_COUNT=$((SKIP_COUNT + 1)); echo -e "  ${YELLOW}SKIP${NC} [$1] $2"; }

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
echo -e "${CYAN}|  Function capabilities reach every gate, on both engines     |${NC}"
echo -e "${CYAN}+==============================================================+${NC}"
echo ""

ALL_IDS="FG-01 FG-02 FG-03 FG-04 FG-05 FG-06 FG-07 FG-08 FG-09 FG-10 FG-11 FG-12"
if ! command -v python3 >/dev/null 2>&1; then
    for id in $ALL_IDS; do skip "$id" "python3 unavailable (fixture generator)"; done
    echo ""; echo "  Total: 12 | Pass: 0 | Fail: 0 | Skip: 12"; exit 0
fi

mkdir -p "$W"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
echo "data" > "$W/d.txt"

# $1 = capabilities.functions object as JSON, or "" to omit the section.
# NOTE: no allowed_paths, and fixtures never touch govern.json. Both are
# auto-added to blocked_paths, so a fixture that reads the config dies on an
# UNRELATED rule before reaching the call under test — which reads as a refusal
# and passes every refusal-expecting arm. That cost three debugging rounds.
mkcfg() {
    python3 - "$W/govern.json" "$1" "${2:-enforce}" << 'PY'
import json, sys
fns = sys.argv[2]
cfg = {
    "version": "5.0", "mode": sys.argv[3],
    "security": {"sandbox_level": "elevated"},
    "languages": {"allowed": ["shell"]},
    "capabilities": {
        "filesystem": {"mode": "readwrite"},
        "shell": {"enabled": True},
        "network": {"enabled": True},
    },
    "agents": {"w": {"provider": "gemini", "model": "m",
                     "api_key_env": "FK", "system_prompt": "s"}},
}
if fns:
    cfg["capabilities"]["functions"] = json.loads(fns)
json.dump(cfg, open(sys.argv[1], "w"), indent=1)
PY
    python3 -c "import json,sys; json.load(sys.stdin)" < "$W/govern.json" >/dev/null || {
        echo "FIXTURE BROKEN"; return 1; }
}

# run PROG CFG [args...] -> RC, OUT
run() {
    local prog="$1"; shift; local cfg="$1"; shift
    mkcfg "$cfg" >/dev/null || { RC=99; OUT="FIXTURE BROKEN"; return; }
    OUT="$(cd "$W" && FK=x timeout 60s "$NAAB" "$@" "$prog" 2>&1)"; RC=$?
}

# The whole vocabulary in one program, one function per action.
# The shell function lives in its OWN program, deliberately. A polyglot block
# that cannot run ABORTS the program, and every function after it never
# executes -- so one unusable executor silently took down the FS, NET, ENV and
# AGENT arms too. That is what reddened build-windows: 11 of 12 arms failed
# there while FG-11 passed, because FG-11 is the only one reading the
# per-occurrence message rather than the end-of-run summary.
cat > "$W/all.naab" << 'EOF'
use http
use env
use agent
fn writer()  { file.write("o.txt", "x") }
fn fetcher() { try { let r = http.get("https://127.0.0.1:9/x") } catch (e) { } }
fn reader()  { let v = env.get("HOME") }
fn setter()  { env.set("NAAB_FG", "1") }
fn talker()  { try { let h = agent.create("w") let r = agent.send(h, "hi") } catch (e) { } }
main { writer() fetcher() reader() setter() talker() print("DONE") }
EOF

cat > "$W/shell.naab" << 'EOF'
fn sheller() { let r = <<shell
echo hi
>>
}
main { sheller() print("DONE") }
EOF

RO='{"default": {"allowed_actions": ["FS_READ"]}}'

# saw ACTION FUNC -> is "<func> needs <ACTION>" in the summary?
saw() { case "$OUT" in *"$1 needs $2"*) return 0 ;; *) return 1 ;; esac; }


# Can this platform run a <<shell>> block at all? Asked with the action GRANTED,
# so the only thing that can fail is the executor. Without this the shell arms
# report a governance FAILURE on a platform that simply has no shell executor --
# a broken probe rendering as a finding.
SHELL_OK=0
mkcfg '{"default": {"allowed_actions": ["FS_READ", "SHELL_EXEC"]}}' >/dev/null && {
    probe_out="$(cd "$W" && timeout 60s "$NAAB" shell.naab 2>&1)"
    case "$probe_out" in *DONE*) SHELL_OK=1 ;; esac
}

# --- FG-01..FG-06: every gate, both engines ---------------------------------
declare -a IDS=(FG-01 FG-02 FG-04 FG-05 FG-06)
declare -a FNS=(writer fetcher reader setter talker)
declare -a ACTS=(FS_WRITE NET_CONNECT ENV_READ ENV_WRITE AGENT_SEND)

for i in 0 1 2 3 4; do
    id="${IDS[$i]}"; fn="${FNS[$i]}"; act="${ACTS[$i]}"
    miss=""
    for eng in "" "--tree-walk"; do
        run all.naab "$RO" $eng
        saw "$fn" "$act" || miss="$miss ${eng:-VM}"
    done
    if [ -z "$miss" ]; then
        pass "$id" "$act is gated in '$fn' on both engines"
    else
        fail "$id" "$act was not gated" "engines missing:$miss"
    fi
done


# FG-03: SHELL_EXEC, in its own program so it cannot take the others with it.
if [ "$SHELL_OK" -eq 0 ]; then
    skip "FG-03" "UNMEASURABLE — no usable <<shell>> executor on this platform"
else
    miss=""
    for eng in "" "--tree-walk"; do
        run shell.naab "$RO" $eng
        saw sheller SHELL_EXEC || miss="$miss ${eng:-VM}"
    done
    if [ -z "$miss" ]; then
        pass "FG-03" "SHELL_EXEC is gated in 'sheller' on both engines"
    else
        fail "FG-03" "SHELL_EXEC was not gated" "engines missing:$miss"
    fi
fi

# --- FG-07: stale attribution on the VM (the defect this suite found) -------
cat > "$W/stale.naab" << 'EOF'
fn alpha() { file.read("d.txt") }
fn bravo() { let r = <<shell
echo hi
>>
}
main { alpha() bravo() print("DONE") }
EOF
wrong=""
if [ "$SHELL_OK" -eq 0 ]; then
    skip "FG-07" "UNMEASURABLE — no usable <<shell>> executor on this platform"
    wrong="SKIPPED"
fi
for eng in "" "--tree-walk"; do
    [ "$SHELL_OK" -eq 0 ] && break
    run stale.naab "$RO" $eng
    saw bravo SHELL_EXEC || wrong="$wrong ${eng:-VM}"
    case "$OUT" in *"alpha needs SHELL_EXEC"*) wrong="$wrong ${eng:-VM}(blamed-alpha)" ;; esac
done
if [ "$SHELL_OK" -eq 0 ]; then
    :  # already reported as UNMEASURABLE above
elif [ -z "$wrong" ]; then
    pass "FG-07" "a polyglot block is attributed to its OWN function, not the last stdlib caller"
else
    fail "FG-07" "stale attribution stack" "wrong on:$wrong"
fi

# --- FG-08 CONTROL: granting the action silences it --------------------------
GRANT='{"default": {"allowed_actions": ["FS_READ", "FS_WRITE", "NET_CONNECT", "SHELL_EXEC", "ENV_READ", "ENV_WRITE", "AGENT_SEND"]}}'
bad=""
for eng in "" "--tree-walk"; do
    run all.naab "$GRANT" $eng
    case "$OUT" in *"Undeclared function effects"*) bad="$bad ${eng:-VM}" ;; esac
    case "$OUT" in *DONE*) ;; *) bad="$bad ${eng:-VM}(no-DONE)" ;; esac
done
if [ -z "$bad" ]; then
    pass "FG-08" "control: declaring all seven actions silences every gate and the program runs"
else
    fail "FG-08" "granting the actions did not silence the gates" "problems:$bad"
fi

# --- FG-09 CONTROL: the tier is opt-in --------------------------------------
# Without this, an implementation that gated everything unconditionally would
# pass FG-01..FG-07 and look correct.
bad=""
for eng in "" "--tree-walk"; do
    run all.naab "" $eng
    case "$OUT" in *"Undeclared function effects"*) bad="$bad ${eng:-VM}" ;; esac
    case "$OUT" in *DONE*) ;; *) bad="$bad ${eng:-VM}(no-DONE)" ;; esac
done
if [ -z "$bad" ]; then
    pass "FG-09" "control: no capabilities.functions section gates nothing anywhere"
else
    fail "FG-09" "the tier fired without being configured" "problems:$bad"
fi

# --- FG-10 CONTROL: an unrestricted frame still narrows nothing --------------
# Only 'writer' is named and there is no "default", so every other function is
# unrestricted and must pass every NEW gate untouched.
bad=""
for eng in "" "--tree-walk"; do
    run all.naab '{"writer": {"allowed_actions": ["FS_READ"]}}' $eng
    saw writer FS_WRITE || bad="$bad ${eng:-VM}(writer-not-gated)"
    for other in fetcher sheller reader setter talker; do
        case "$OUT" in *"$other needs"*) bad="$bad ${eng:-VM}($other)" ;; esac
    done
done
if [ -z "$bad" ]; then
    pass "FG-10" "control: a frame with no entry and no default narrows nothing at the new gates"
else
    fail "FG-10" "unlisted frames were constrained" "problems:$bad"
fi

# --- FG-11: the level applies to the new gates too ---------------------------
# The level was built against the filesystem gate; it has to carry to the rest.
bad=""
for eng in "" "--tree-walk"; do
    run all.naab '{"level": "hard", "default": {"allowed_actions": ["FS_READ", "FS_WRITE"]}}' $eng
    [ "$RC" -eq 3 ] || bad="$bad ${eng:-VM}(rc=$RC)"
    case "$OUT" in *"NET_CONNECT"*) ;; *) bad="$bad ${eng:-VM}(not-net)" ;; esac
done
if [ -z "$bad" ]; then
    pass "FG-11" "level hard blocks at a non-filesystem gate on both engines"
else
    fail "FG-11" "the level did not carry to the new gates" "problems:$bad"
fi

# --- FG-12: mode "off" silences the tier on both engines ---------------------
# isActive() is false only for govern.json mode:"off", and the engine treats
# that as "governance renders no verdict". The function tier postdates the work
# that made that consistent across engines (tests/security/test_governance_
# authority.sh), so nothing else pins it for these gates. Checked with the
# strictest possible declaration -- hard level, an entry permitting NOTHING --
# because a weaker fixture would pass even if the tier ignored mode entirely.
bad=""
for eng in "" "--tree-walk"; do
    mkcfg '{"level": "hard", "default": {"allowed_actions": []}}' off >/dev/null \
        || { bad="$bad ${eng:-VM}(fixture)"; continue; }
    out="$(cd "$W" && FK=x timeout 60s "$NAAB" $eng all.naab 2>&1)"; rc=$?
    [ "$rc" -eq 0 ] || bad="$bad ${eng:-VM}(rc=$rc)"
    case "$out" in *"Undeclared"*) bad="$bad ${eng:-VM}(still-gating)" ;; esac
    case "$out" in *DONE*) ;; *) bad="$bad ${eng:-VM}(no-DONE)" ;; esac
done
# Positive control: the SAME declaration under enforce must block, or FG-12
# would pass for a build where the tier never worked at all.
for eng in "" "--tree-walk"; do
    run all.naab '{"level": "hard", "default": {"allowed_actions": []}}' $eng
    [ "$RC" -eq 3 ] || bad="$bad ${eng:-VM}(enforce-rc=$RC)"
done
if [ -z "$bad" ]; then
    pass "FG-12" "mode \"off\" silences the tier on both engines, and enforce still blocks"
else
    fail "FG-12" "the tier disagreed with mode:off" "problems:$bad"
fi

echo ""
echo -e "${CYAN}+==============================================================+${NC}"
TOTAL=$((PASS_COUNT + FAIL_COUNT + SKIP_COUNT))
echo -e "  Total: $TOTAL | ${GREEN}Pass: $PASS_COUNT${NC} | ${RED}Fail: $FAIL_COUNT${NC} | ${YELLOW}Skip: $SKIP_COUNT${NC}"
if [ "$FAIL_COUNT" -gt 0 ]; then
    echo -e "${RED}Failures:${NC}$FAILURES"
    exit 1
fi
exit 0
