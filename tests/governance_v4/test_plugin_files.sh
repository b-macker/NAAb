#!/usr/bin/env bash
# test_plugin_files.sh -- the run's engine judges the compile phase (A116), and
# governance plugin files are the operator's: read without the program's
# gates, never written by the program (A192)
#
# A116. main.cpp builds a tree-walker Interpreter before it chooses the engine,
# and its constructor makes the Interpreter's own engine current -- an engine
# that never loads the config. The run's engine became current only just
# before execute(), so every check reached through GovernanceEngine::getCurrent()
# while the VM compiled asked an inactive engine and passed. Plugins load
# there (the first function-body check calls checkPluginRules()), so a
# plugin's own stdlib calls at compile time ran outside the run's policy:
# observed on c1312132, a plugin rule read a `blocked_read` variable during
# compile and printed it, exit 0. The owner's decision (2026-11-20): make the
# run's engine current before the static phase, and exempt plugin files
# explicitly, as the operator's own files like hooks and guidance sources.
# The exemption covers the reads loadPlugins() makes while it loads a plugin
# (the file and what its top level imports); without it the plugin read would
# now meet the lease, the capability manifest and the path policy, and a
# refused plugin is skipped with a warning -- its rules off.
#
# A192. Nothing stopped the program writing a plugin file: observed on
# c1312132, with shell disabled, file.write over the plugin, and the next run
# executed the eval the plugin refused (exit 3 before, 0 after). Every program
# write to a plugin govern.json names is now refused (governance.plugin_files,
# HARD) at the shared decision point; reads stay allowed.
#
#   PF-00   CONTROL: the plugin loads and fires -- a compliant block runs, an
#           eval block is refused by the plugin's rule
#   PF-01   a plugin rule that reads a `blocked_read` variable while the VM
#           compiles is refused, and the value is not printed
#   PF-01c  CONTROL: the same plugin, the variable not blocked: the plugin ran
#           during compile and printed it
#   PF-02   under a lease granting no action, the plugin still loads and
#           refuses the eval block (its read is exempt)
#   PF-02c  CONTROL: a compliant block runs under the same lease
#   PF-03   a plugin inside a `blocked_paths` directory, importing a helper
#           beside it with `use`, loads and refuses the eval block
#   PF-03c  CONTROL: the program's own read of that plugin is refused (the
#           path policy is in force)
#   PW-00   CONTROL: ordinary writes beside the plugin run, and so does a
#           write to a file of the plugin's name elsewhere
#   PW-00b  CONTROL: with no plugin configured, the same file name is
#           ordinary (the protection is the configuration, not the name)
#   PW-01   file.write over the plugin: refused, unchanged
#   PW-02   file.append to it: refused
#   PW-03   io.write_file over it: refused
#   PW-04   by absolute path: refused
#   PW-05   file.copy of a file of the same name into its directory: refused
#   PW-06   a write through a hard link to it: refused
#   PW-06c  CONTROL: a hard-linked file that is not the plugin is writable
#   PW-07   file.delete of it, with deletion granted: refused
#   PW-07c  CONTROL: deletion is granted there (an ordinary delete runs)
#   PW-08   embedded Python open(..., "w") over it: refused
#   PW-09   CONTROL: the program can read the plugin
#   PW-10   after the refused writes, the next run still refuses the eval
#   PW-11   PINNED, not endorsed: a <<sh>> write lands -- subprocess
#           languages are outside path policy (CONTRA-013)
#
# Kill table (each mutant rebuilt, suite re-run; observed 2026-11-20):
#   PLACEHOLDER
# Red first: on c1312132 PF-01 and PW-01 .. PW-08 fail, every other arm passes.

set -u
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
NAAB="${1:-${NAAB:-$REPO/build/naab-lang}}"
TEST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/naab_plugfiles_XXXXXX")"

PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
unm()  { UNM=$((UNM+1));  echo "  UNMEASURABLE [$1] $2"; }

source "$SCRIPT_DIR/../helpers/trust_setup.sh"
source "$SCRIPT_DIR/../helpers/native_path.sh"   # PW-04 hands the binary an absolute path
setup_isolated_trust          # unsigned configs; exit 3 must mean the gate
cleanup() { teardown_isolated_trust; [ -n "${PF_KEEP:-}" ] || rm -rf "$TEST_TMP"; }
trap cleanup EXIT
[ -x "$NAAB" ] || { echo "naab-lang not built"; exit 1; }

PLUGIN_RULE='{"id":"PF-PLUG","function":"check_no_eval","description":"no eval","level":"hard","trigger":"polyglot_block","languages":["javascript"],"message":"eval() is not allowed here","help":"do not use eval"}'

# mk DIR [PLUGIN-FILE] [CAPABILITIES-JSON] [SANDBOX] -- an enforce project
# whose plugin refuses a JavaScript block that calls eval(). An empty
# PLUGIN-FILE configures no plugin.
mk() {
    # The default is set apart: a "${3:-{...}}" expansion ends at the first
    # "}" and appended the rest to every config a caller passed (exit 4).
    local dir="$1" pf="${2-plugin.naab}" caps="${3-}" sb="${4:-elevated}"
    [ -n "$caps" ] || caps='{"filesystem": {"mode": "write"}}'
    mkdir -p "$dir"
    local plugins='[]'
    [ -n "$pf" ] && plugins="[{\"file\": \"$pf\", \"rules\": [$PLUGIN_RULE]}]"
    cat > "$dir/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "$sb"},
 "capabilities": $caps,
 "governance_plugins": $plugins}
EOF
    if [ -n "$pf" ]; then
        mkdir -p "$(dirname "$dir/$pf")"
        cat > "$dir/$pf" <<'EOF'
export function check_no_eval(ctx) {
    let code = ctx["code"]
    if code.contains("eval(") {
        return { "pass": false, "message": "eval() is forbidden by project policy" }
    }
    return { "pass": true }
}
EOF
        cp "$dir/$pf" "$dir/plugin.orig"
    fi
}
run() {  # DIR PROGRAM [FLAGS...] -> OUT, RC
    local dir="$1" prog="$2"; shift 2
    printf '%s\n' "$prog" > "$dir/p.naab"
    OUT="$(cd "$dir" && "$NAAB" run "$@" p.naab 2>&1)"; RC=$?
}
excerpt() { tail -3 <<<"$OUT" | tr '\n' ' ' | cut -c1-240; }
P_OK='main {
    let e = <<javascript
String(1 + 1)
>>
    print("JS=" + e)
}'
P_EVAL='main {
    let e = <<javascript
String(eval("1 + 1"))
>>
    print("JS=" + e)
}'
fired() { [ "$RC" = 3 ] && grep -q "PF-PLUG" <<<"$OUT" && ! grep -q "JS=2" <<<"$OUT"; }
ran_ok() { [ "$RC" = 0 ] && grep -q "JS=2" <<<"$OUT"; }

named() { grep -q "governance plugin is not writable\|Writing a governance plugin is blocked" <<<"$OUT"; }
intact() { cmp -s "$1/plugin.naab" "$1/plugin.orig"; }
refused() {  # ID WHAT DIR
    if [ "$RC" = 3 ] && named && intact "$3"; then pass "$1" "$2: refused (exit 3, named), the plugin unchanged"
    else fail "$1" "$2 should be refused" "rc=$RC named=$(named && echo yes || echo no) intact=$(intact "$3" && echo yes || echo no): $(excerpt)"; fi
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

echo "=== A116: the run's engine judges the compile phase; plugin files are exempt ==="

d="$TEST_TMP/f00"; mk "$d"
run "$d" "$P_OK"; ok_rc=$RC; ok_out=$OUT
run "$d" "$P_EVAL"
if [ "$ok_rc" = 0 ] && grep -q "JS=2" <<<"$ok_out" && fired; then
    pass PF-00 "CONTROL: the plugin loads and fires (a compliant block runs, the eval block is refused by PF-PLUG)"
else
    fail PF-00 "the plugin should load and fire" "ok_rc=$ok_rc eval_rc=$RC: $(excerpt)"
fi

# A plugin rule run on every function body (trigger naab_function) that reads
# an environment variable: it runs while the VM compiles. The value is a
# throwaway made here.
PROBE_VAL="pf$RANDOM$RANDOM"
mkenvplug() {  # DIR BLOCKED-JSON-LIST
    mkdir -p "$1"
    cat > "$1/govern.json" <<EOF
{"version": "5.0", "mode": "enforce",
 "security": {"sandbox_level": "elevated"},
 "capabilities": {"env_vars": {"blocked_read": $2}},
 "governance_plugins": [{"file": "plugin.naab", "rules": [
   {"id": "PF-ENV", "function": "check_fn", "description": "reads env", "level": "advisory",
    "trigger": "naab_function", "message": "probe", "help": "probe"}]}]}
EOF
    cat > "$1/plugin.naab" <<'EOF'
use env

export function check_fn(ctx) {
    print("PLUGIN_SAW=" + env.get("NAAB_PF_PROBE"))
    return { "pass": true }
}
EOF
}
P_FN='fn helper(x) {
    return x + 1
}
main {
    print("RAN=" + helper(1))
}'
d="$TEST_TMP/f01c"; mkenvplug "$d" '[]'
printf '%s\n' "$P_FN" > "$d/p.naab"
OUT="$(cd "$d" && NAAB_PF_PROBE="$PROBE_VAL" "$NAAB" run p.naab 2>&1)"; RC=$?
if [ "$RC" = 0 ] && grep -q "PLUGIN_SAW=$PROBE_VAL" <<<"$OUT" && grep -q "RAN=2" <<<"$OUT"; then
    pass PF-01c "CONTROL: with the variable not blocked, the plugin ran during compile and read it"
    d="$TEST_TMP/f01"; mkenvplug "$d" '["NAAB_PF_PROBE"]'
    printf '%s\n' "$P_FN" > "$d/p.naab"
    OUT="$(cd "$d" && NAAB_PF_PROBE="$PROBE_VAL" "$NAAB" run p.naab 2>&1)"; RC=$?
    if [ "$RC" = 3 ] && ! grep -q "$PROBE_VAL" <<<"$OUT" && grep -q "blocked by policy" <<<"$OUT"; then
        pass PF-01 "a plugin's read of a blocked variable during compile is refused (exit 3), the value not printed"
    else
        fail PF-01 "the compile-phase read should meet the run's policy" "rc=$RC printed=$(grep -c "$PROBE_VAL" <<<"$OUT"): $(excerpt)"
    fi
else
    fail PF-01c "the plugin should run during compile and print the variable" "rc=$RC: $(excerpt)"
    unm PF-01 "the control did not run"
fi

d="$TEST_TMP/f02"; mk "$d"
run "$d" "$P_OK" --lease-grant ""
if ran_ok; then
    pass PF-02c "CONTROL: a compliant block runs under a lease granting no action"
    run "$d" "$P_EVAL" --lease-grant ""
    if fired; then pass PF-02 "under that lease the plugin still loads and refuses the eval block"
    else fail PF-02 "the plugin should load under the lease (its read is the operator's)" "rc=$RC: $(excerpt)"; fi
else
    fail PF-02c "a compliant block should run under the lease" "rc=$RC: $(excerpt)"
    unm PF-02 "the control did not run"
fi

d="$TEST_TMP/f03"; mk "$d" "gov/plugin.naab" '{"filesystem": {"mode": "write", "blocked_paths": ["gov/"]}}'
cat > "$d/gov/helper.naab" <<'EOF'
export fn has_eval(code) {
    return code.contains("eval(")
}
EOF
cat > "$d/gov/plugin.naab" <<'EOF'
use helper

export function check_no_eval(ctx) {
    if helper.has_eval(ctx["code"]) {
        return { "pass": false, "message": "eval() is forbidden by project policy" }
    }
    return { "pass": true }
}
EOF
run "$d" 'use file
main { print(file.read("gov/plugin.naab")) }'
if [ "$RC" = 3 ] && ! grep -q "check_no_eval" <<<"$OUT"; then
    pass PF-03c "CONTROL: the program's own read of the plugin under blocked_paths is refused"
    run "$d" "$P_EVAL"
    if fired; then pass PF-03 "a plugin under blocked_paths, importing a helper beside it, loads and refuses the eval block"
    else fail PF-03 "the plugin should load from a directory the program may not read" "rc=$RC: $(excerpt)"; fi
else
    fail PF-03c "the program's read of a blocked plugin should be refused" "rc=$RC: $(excerpt)"
    unm PF-03 "the control did not run"
fi

echo "=== A192: the program may not write a governance plugin ==="

d="$TEST_TMP/w00"; mk "$d"; mkdir -p "$d/other"
run "$d" 'use file
main {
    file.write("notes.txt", "FINE")
    file.write("other/plugin.naab", "FINE")
    print("WROTE")
}'
if [ "$RC" = 0 ] && grep -q WROTE <<<"$OUT" && [ "$(cat "$d/notes.txt" 2>/dev/null)" = FINE ] \
        && [ "$(cat "$d/other/plugin.naab" 2>/dev/null)" = FINE ] && intact "$d"; then
    pass PW-00 "CONTROL: ordinary writes beside the plugin, and to its name elsewhere, run"
else
    fail PW-00 "ordinary writes should run" "rc=$RC: $(excerpt)"
fi

d="$TEST_TMP/w00b"; mk "$d" ""
run "$d" 'use file
main { file.write("plugin.naab", "FINE")
    print("WROTE") }'
if [ "$RC" = 0 ] && [ "$(cat "$d/plugin.naab" 2>/dev/null)" = FINE ]; then
    pass PW-00b "CONTROL: with no plugin configured, plugin.naab is an ordinary file"
else
    fail PW-00b "an unconfigured plugin.naab should be writable" "rc=$RC: $(excerpt)"
fi

d="$TEST_TMP/w01"; mk "$d"
run "$d" 'use file
main { file.write("plugin.naab", "export function check_no_eval(ctx) {\n    return { \"pass\": true }\n}\n") }'
refused PW-01 "file.write over the plugin" "$d"

d="$TEST_TMP/w02"; mk "$d"
run "$d" 'use file
main { file.append("plugin.naab", "\n") }'
refused PW-02 "file.append to the plugin" "$d"

d="$TEST_TMP/w03"; mk "$d"
run "$d" 'use io
main { io.write_file("plugin.naab", "x") }'
refused PW-03 "io.write_file over the plugin" "$d"

d="$TEST_TMP/w04"; mk "$d"
# The path is written INTO the program, so it must be in the binary's
# vocabulary (native_path.sh).
run "$d" "use file
main { file.write(\"$(native_path "$d")/plugin.naab\", \"x\") }"
refused PW-04 "an absolute-path write over the plugin" "$d"

d="$TEST_TMP/w05"; mk "$d" "gov/plugin.naab"; cp "$d/gov/plugin.naab" "$d/plugin.orig"
mkdir -p "$d/src"; echo "PLANTED" > "$d/src/plugin.naab"
run "$d" 'use file
main { file.copy("src/plugin.naab", "gov") }'
if [ "$RC" = 3 ] && named && cmp -s "$d/gov/plugin.naab" "$d/plugin.orig"; then
    pass PW-05 "file.copy of a same-named file into the plugin's directory: refused, the plugin unchanged"
else
    fail PW-05 "file.copy into the plugin's directory should be refused" "rc=$RC: $(excerpt)"
fi

d="$TEST_TMP/w06"; mk "$d"; echo ORIGINAL > "$d/other.txt"
if ln "$d/plugin.naab" "$d/alias.naab" 2>/dev/null && ln "$d/other.txt" "$d/other2.txt" 2>/dev/null; then
    run "$d" 'use file
main { file.write("alias.naab", "PLANTED") }'
    refused PW-06 "a write through a hard link to the plugin" "$d"
    run "$d" 'use file
main { file.write("other2.txt", "FINE") }'
    if [ "$RC" = 0 ] && [ "$(cat "$d/other.txt")" = FINE ]; then pass PW-06c "CONTROL: a hard-linked file that is not the plugin is writable"
    else fail PW-06c "a hard-linked ordinary file should be writable" "rc=$RC"; fi
else
    unm PW-06 "hard links cannot be made here"; unm PW-06c "hard links cannot be made here"
fi

d="$TEST_TMP/w07"; mk "$d" plugin.naab '{"filesystem": {"mode": "write"}}' unrestricted
run "$d" 'use file
main { file.write("junk.txt", "x")
    file.delete("junk.txt") }'
if [ "$RC" = 0 ] && [ ! -e "$d/junk.txt" ]; then
    pass PW-07c "CONTROL: deletion is granted here (an ordinary delete runs)"
    run "$d" 'use file
main { file.delete("plugin.naab") }'
    refused PW-07 "file.delete of the plugin" "$d"
else
    unm PW-07c "deletion not grantable here (rc=$RC)"; unm PW-07 "no granted delete to compare against"
fi

if [ "$EMBEDDED_PY" = 1 ]; then
    d="$TEST_TMP/w08"; mk "$d"
    run "$d" 'main { let x = <<python
with open("plugin.naab", "w") as f:
    f.write("planted")
"done"
>>
  print(x) }'
    if [ "$RC" != 0 ] && intact "$d"; then pass PW-08 "embedded Python open(w) over the plugin refused (rc=$RC)"
    else fail PW-08 "embedded Python wrote the plugin" "rc=$RC intact=$(intact "$d" && echo yes || echo no)"; fi
else
    unm PW-08 "embedded Python not available in this build"
fi

d="$TEST_TMP/w09"; mk "$d"
run "$d" 'use file
main { print(file.read("plugin.naab")) }'
if [ "$RC" = 0 ] && grep -q "check_no_eval" <<<"$OUT"; then pass PW-09 "CONTROL: the program can read the plugin"
else fail PW-09 "reading the plugin should be allowed" "rc=$RC: $(excerpt)"; fi

# The attack the protection is for: a refused write, then the next run.
d="$TEST_TMP/w01"
run "$d" "$P_EVAL"
if fired && intact "$d"; then pass PW-10 "after the refused writes, the next run still refuses the eval block"
else fail PW-10 "the plugin's rule should still refuse the eval block" "rc=$RC: $(excerpt)"; fi

if [ "$SHELL_OK" = 1 ]; then
    d="$TEST_TMP/w11"; mk "$d" plugin.naab '{"filesystem": {"mode": "write"}, "shell": {"enabled": true}}' unrestricted
    run "$d" 'main { <<sh
echo "# sh" >> plugin.naab
>> }'
    if ! intact "$d"; then
        pass PW-11 "PINNED, not endorsed: a <<sh>> write lands (subprocess languages are outside path policy, CONTRA-013)"
    else
        fail PW-11 "the pinned limit changed: a <<sh>> write no longer lands -- confirm and update this arm" "rc=$RC"
    fi
else
    unm PW-11 "no shell executor here"
fi

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ]
