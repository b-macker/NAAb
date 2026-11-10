#!/usr/bin/env bash
# test_gov_diff.sh -- `naab-gov diff OLD NEW` (Bundle 13)
#
# naab-gov diff reports which requests an edit of govern.json newly allows or
# refuses, and which settings it changes. Requests are judged by the engine's
# own gates through include/naab/policy_probes.h (the probes the ratchet
# property harness judges); settings by the comparisons a mid-run reload makes
# (GovernanceEngine::evaluateReload, as `naab-gov ratchet`). Report only.
#
#   DF-00   CONTROL: a file against itself differs in nothing (exit 0)
#   DF-01   a dropped blocked_read name is reported as newly allowed, for the
#           program's read and for what a child process receives, and under
#           the settings a reload would refuse (exit 1)
#   DF-02   an added blocked path is reported as newly refused, and nothing is
#           reported as newly allowed
#   DF-03   the report agrees with real runs: the variable DF-01 calls
#           blocked -> allowed is refused by `naab-lang` under OLD (exit 3)
#           and printed under NEW; the path DF-02 calls allowed -> blocked is
#           read under OLD and refused under NEW
#   DF-04   the --governance-override column: blocked_read (hard) replaced by
#           an allowlist that omits the name (soft) reads blocked -> blocked
#           without the override and blocked -> allowed with it, as loosened
#   DF-05   each file is judged from its own directory: OLD in a/ blocks the
#           relative entry vault/, NEW in b/ does not, the command run from a
#           third directory
#   DF-05c  CONTROL: the same text in a/ and b/ differs in nothing
#   DF-06   a settings-only change (a raised limit) is reported under
#           settings, with no request changed, exit 1
#   DF-07   --agent-id reaches the judging: a role that loses shell changes
#           the shell request only when that role is selected
#   DF-07b  a role's language list reaches the language request: ruby is
#           newly refused only under the role
#   DF-07c  the settings are judged under the role too: a language the file
#           gains is no loosening under a role that does not allow it
#   DF-08   governance.default_env is applied as a run applies it, and --env
#           overrides it (under an audit environment nothing is refused, so
#           nothing differs); an --env neither file has is an error
#   DF-09   --json carries the requests, the settings and the controls
#   DF-10   usage errors exit 4: one file, a missing file, a file that is not
#           JSON, an unknown option
#   DF-11   the report prints what it does not judge as requests
#   A147    PINNED, open: a blocked_hosts entry written "*.name" matches no
#           host (the matcher compares the entry as a suffix, star included),
#           so the sub-domain it names is not refused; A147c CONTROL: the
#           plain entry refuses it. Found by DF's first trial run. This arm
#           flips when A147 is decided.
#
# Kill table (each mutant rebuilt, this suite re-run; observed 2026-11-10):
#   the working directory is not changed per file      -> DF-02, DF-05, DF-09
#   the override column is not judged                  -> DF-04
#   --agent-id ignored (the agent id not set)          -> DF-07, DF-07b, DF-07c
#   applyAgentRole() skipped                           -> DF-07c (DF-07/07b stay
#       green: the shell and language gates read the role by agent id)
#   governance.default_env ignored                     -> DF-08
#   the unlisted env control dropped from the alphabet -> DF-04
#   the reload evaluation's result not used            -> DF-01, DF-06, DF-09
#   exit 0 when only settings differ                   -> DF-06
#   entries of the NEW file left out of the alphabet   -> DF-02, DF-09
# Red first: every arm fails on master's naab-gov ("unknown command: diff").

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$(cd "$SCRIPT_DIR/../../build" 2>/dev/null && pwd)"
NAAB="${NAAB:-$BUILD/naab-lang}"
GOV="${NAAB_GOV:-$BUILD/naab-gov}"
source "$SCRIPT_DIR/../helpers/trust_setup.sh"

PASS=0; FAIL=0; SKIP=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && echo "       -> $3"; }
skip() { SKIP=$((SKIP+1)); echo "  SKIP [$1] $2 -- UNMEASURABLE"; }
has() { grep -qF -- "$2" <<<"$1"; }

echo "=== naab-gov diff: what a govern.json edit allows and refuses (Bundle 13) ==="
for b in "$NAAB" "$GOV"; do
    if [ ! -x "$b" ] && [ ! -x "$b.exe" ]; then
        echo "  FAIL [setup] $b is not built -- UNMEASURABLE, not a pass"
        exit 1
    fi
done

setup_isolated_trust
W="$(mktemp -d "${TMPDIR:-/tmp}/govdiff-XXXXXX")"
trap 'rm -rf "$W"; teardown_isolated_trust' EXIT
cd "$W" || exit 1

# diff ID OLD NEW [args]: sets OUT (stdout) and RC. Stderr (load warnings) is
# kept apart in ERR.
diff_run() {
    OUT="$("$GOV" diff "$@" 2>"$W/err.txt")"; RC=$?
    ERR="$(cat "$W/err.txt")"
}
# The lines of one report group ("newly allowed", "newly refused", "a reload
# would refuse", "other changes"), up to the next group or blank line.
group() {
    awk -v t="$2" 'index($0, t) { on = 1; next }
                   on && (/^  [a-z]/ || /^$/) { on = 0 }
                   on { print }' <<<"$1"
}

BASE='"version":"3.0","mode":"enforce","security":{"sandbox_level":"elevated"}'

# --- DF-00 -------------------------------------------------------------
mkdir -p same && cat > same/govern.json <<EOF
{$BASE,"capabilities":{"env_vars":{"blocked_read":["DF_TOKEN"]},"filesystem":{"mode":"write","blocked_paths":["vault/"]}}}
EOF
cp same/govern.json same/copy.json
diff_run same/govern.json same/copy.json
if [ "$RC" = 0 ] && has "$OUT" "no request is judged differently" &&
   has "$OUT" "no setting the reload ratchet compares differs"; then
    pass DF-00 "CONTROL: a file against its copy differs in nothing (exit 0)"
else
    fail DF-00 "a file against its copy reported a difference (exit $RC)" "$(head -12 <<<"$OUT")"
fi

# --- DF-01 / DF-02 / DF-03 ----------------------------------------------
mkdir -p env && cat > env/govern.json <<EOF
{$BASE,"capabilities":{"env_vars":{"blocked_read":["DF_TOKEN"]},"filesystem":{"mode":"write"}}}
EOF
cat > env/new.json <<EOF
{$BASE,"capabilities":{"env_vars":{"blocked_read":[]},"filesystem":{"mode":"write","blocked_paths":["vault/"]}}}
EOF
diff_run env/govern.json env/new.json
ALLOWED="$(group "$OUT" "newly allowed or less refused")"
REFUSED="$(group "$OUT" "newly refused or more refused")"
WOULD="$(group "$OUT" "a reload would refuse")"
if [ "$RC" = 1 ] && has "$ALLOWED" "env.read:DF_TOKEN  blocked -> allowed" &&
   has "$ALLOWED" "env.child:DF_TOKEN  blocked -> allowed" && has "$WOULD" "DF_TOKEN"; then
    pass DF-01 "a dropped blocked_read name is newly allowed (read and child), and a reload would refuse it"
else
    fail DF-01 "the dropped blocked_read name was not reported as newly allowed (exit $RC)" "$(head -14 <<<"$OUT")"
fi
if has "$REFUSED" "fs.read:vault/naab-diff-probe  allowed -> blocked" &&
   has "$REFUSED" "fs.write:vault/naab-diff-probe  allowed -> blocked" && ! has "$ALLOWED" "fs."; then
    pass DF-02 "an added blocked path is newly refused, and no path is newly allowed"
else
    fail DF-02 "the added blocked path was not reported as newly refused" "$(head -14 <<<"$OUT")"
fi

# The same two files, run. OLD and NEW each as their own project's govern.json.
mkdir -p run_old run_new run_old/vault run_new/vault
cp env/govern.json run_old/govern.json
cp env/new.json run_new/govern.json
echo "VAULT_CONTENT" > run_old/vault/naab-diff-probe
echo "VAULT_CONTENT" > run_new/vault/naab-diff-probe
cat > read_env.naab <<'EOF'
use env
main {
    print("ENV=" + env.get("DF_TOKEN"))
}
EOF
cat > read_file.naab <<'EOF'
use file
main {
    print("FILE=" + file.read("vault/naab-diff-probe"))
}
EOF
run_in() {  # run_in DIR PROGRAM -> ROUT, RRC
    cp "$2" "$1/prog.naab"
    ROUT="$(cd "$1" && DF_TOKEN=token-value "$NAAB" prog.naab 2>&1)"; RRC=$?
}
run_in run_old read_env.naab;  e_old_rc=$RRC; e_old="$ROUT"
run_in run_new read_env.naab;  e_new_rc=$RRC; e_new="$ROUT"
run_in run_old read_file.naab; f_old_rc=$RRC; f_old="$ROUT"
run_in run_new read_file.naab; f_new_rc=$RRC; f_new="$ROUT"
if [ "$e_old_rc" = 3 ] && ! has "$e_old" "ENV=token-value" &&
   [ "$e_new_rc" = 0 ] && has "$e_new" "ENV=token-value" &&
   [ "$f_old_rc" = 0 ] && has "$f_old" "FILE=VAULT_CONTENT" &&
   [ "$f_new_rc" = 3 ] && ! has "$f_new" "FILE=VAULT_CONTENT"; then
    pass DF-03 "real runs agree: the variable is refused under OLD and read under NEW; the file read under OLD and refused under NEW"
else
    fail DF-03 "a real run disagrees with the report" \
        "env old rc=$e_old_rc new rc=$e_new_rc; file old rc=$f_old_rc new rc=$f_new_rc"
fi

# --- DF-04 -------------------------------------------------------------
mkdir -p ovr && cat > ovr/govern.json <<EOF
{$BASE,"capabilities":{"env_vars":{"blocked_read":["DF_TOKEN"]}}}
EOF
cat > ovr/new.json <<EOF
{$BASE,"capabilities":{"env_vars":{"allowed_read":["HOME"]}}}
EOF
diff_run ovr/govern.json ovr/new.json
ALLOWED="$(group "$OUT" "newly allowed or less refused")"
REFUSED="$(group "$OUT" "newly refused or more refused")"
if has "$ALLOWED" "env.read:DF_TOKEN  blocked -> blocked  (with --governance-override: blocked -> allowed)" &&
   has "$REFUSED" "env.read:NAAB_DIFF_UNLISTED  allowed -> blocked  (with --governance-override: allowed -> allowed)"; then
    pass DF-04 "hard -> soft reads blocked -> blocked, and blocked -> allowed with the override; the allowlist refuses the unlisted control"
else
    fail DF-04 "the override column or the unlisted control is missing" "$(head -12 <<<"$OUT")"
fi

# --- DF-05 / DF-05c ------------------------------------------------------
mkdir -p a b third
cat > a/govern.json <<EOF
{$BASE,"capabilities":{"filesystem":{"mode":"write","blocked_paths":["vault/"]}}}
EOF
cat > b/govern.json <<EOF
{$BASE,"capabilities":{"filesystem":{"mode":"write"}}}
EOF
OUT="$(cd third && "$GOV" diff ../a/govern.json ../b/govern.json 2>/dev/null)"; RC=$?
ALLOWED="$(group "$OUT" "newly allowed or less refused")"
if [ "$RC" = 1 ] && has "$ALLOWED" "fs.read:vault/naab-diff-probe  blocked -> allowed"; then
    pass DF-05 "each file is judged from its own directory (run from a third one)"
else
    fail DF-05 "a relative entry was not judged in its file's directory (exit $RC)" "$(head -10 <<<"$OUT")"
fi
cp a/govern.json b/same.json
OUT="$(cd third && "$GOV" diff ../a/govern.json ../b/same.json 2>/dev/null)"; RC=$?
if [ "$RC" = 0 ] && has "$OUT" "no request is judged differently"; then
    pass DF-05c "CONTROL: the same text in two directories differs in nothing"
else
    fail DF-05c "the same text in two directories reported a difference (exit $RC)" "$(head -10 <<<"$OUT")"
fi

# --- DF-06 -------------------------------------------------------------
mkdir -p lim && cat > lim/govern.json <<EOF
{$BASE,"limits":{"timeout":{"global":30}}}
EOF
cat > lim/new.json <<EOF
{$BASE,"limits":{"timeout":{"global":60}}}
EOF
diff_run lim/govern.json lim/new.json
WOULD="$(group "$OUT" "a reload would refuse")"
if [ "$RC" = 1 ] && has "$OUT" "no request is judged differently" &&
   has "$WOULD" "limits.timeout.global: 30 -> 60"; then
    pass DF-06 "a settings-only change is reported under settings, exit 1"
else
    fail DF-06 "a raised limit was not reported as a setting a reload would refuse (exit $RC)" "$(head -10 <<<"$OUT")"
fi

# --- DF-07 -------------------------------------------------------------
mkdir -p role && cat > role/govern.json <<EOF
{$BASE,"agents":{"worker":{"shell_allowed":true}}}
EOF
cat > role/new.json <<EOF
{$BASE,"agents":{"worker":{"shell_allowed":false}}}
EOF
diff_run role/govern.json role/new.json
no_role="$OUT"
diff_run role/govern.json role/new.json --agent-id worker
REFUSED="$(group "$OUT" "newly refused or more refused")"
if has "$no_role" "no request is judged differently" && has "$REFUSED" "shell  allowed -> blocked"; then
    pass DF-07 "--agent-id reaches the judging (shell changes only under the role)"
else
    fail DF-07 "the role did not reach the judging" "$(head -8 <<<"$OUT")"
fi

# --- DF-07b ------------------------------------------------------------
# A role's language list, like its shell switch, is read by the gate from the
# agent id (a mutant that skips applyAgentRole() leaves this arm green; DF-07c
# is the arm that needs it).
mkdir -p rolelang && cat > rolelang/govern.json <<EOF
{$BASE,"languages":{"allowed":["python","ruby"]},"agents":{"worker":{"allowed_languages":["python","ruby"]}}}
EOF
cat > rolelang/new.json <<EOF
{$BASE,"languages":{"allowed":["python","ruby"]},"agents":{"worker":{"allowed_languages":["python"]}}}
EOF
diff_run rolelang/govern.json rolelang/new.json
no_role="$OUT"
diff_run rolelang/govern.json rolelang/new.json --agent-id worker
REFUSED="$(group "$OUT" "newly refused or more refused")"
if ! has "$no_role" "lang:ruby" && has "$REFUSED" "lang:ruby  allowed -> blocked" && ! has "$REFUSED" "lang:python"; then
    pass DF-07b "the role's language list reaches the language request (ruby refused only under the role)"
else
    fail DF-07b "the role's language list did not reach the judging" "$(head -8 <<<"$OUT")"
fi

# --- DF-07c ------------------------------------------------------------
# The settings half is the reload decision, and a reload lays the role over
# the new rules: a language the file gains is no loosening under a role that
# does not allow it (test_ratchet_property.sh RG-07's case).
mkdir -p rolefile && cat > rolefile/govern.json <<EOF
{$BASE,"languages":{"allowed":["python","ruby"]},"agents":{"worker":{"allowed_languages":["python"]}}}
EOF
cat > rolefile/new.json <<EOF
{$BASE,"languages":{"allowed":["python","ruby","php"]},"agents":{"worker":{"allowed_languages":["python"]}}}
EOF
diff_run rolefile/govern.json rolefile/new.json
no_role="$(group "$OUT" "a reload would refuse")"
diff_run rolefile/govern.json rolefile/new.json --agent-id worker
with_role="$(group "$OUT" "a reload would refuse")"
if has "$no_role" "php" && ! has "$with_role" "php"; then
    pass DF-07c "the settings are judged under the role too (the file's new language is no loosening under it)"
else
    fail DF-07c "the role did not reach the settings comparison" "without: $no_role | with: $with_role"
fi

# --- DF-08 -------------------------------------------------------------
mkdir -p envs
cat > envs/govern.json <<EOF
{"version":"3.0","mode":"audit","security":{"sandbox_level":"elevated"},
 "governance":{"default_env":"prod"},
 "environments":{"prod":{"mode":"enforce"},"dev":{}},
 "capabilities":{"env_vars":{"blocked_read":["DF_TOKEN"]}}}
EOF
cat > envs/new.json <<EOF
{"version":"3.0","mode":"audit","security":{"sandbox_level":"elevated"},
 "governance":{"default_env":"prod"},
 "environments":{"prod":{"mode":"enforce"},"dev":{}},
 "capabilities":{"env_vars":{"blocked_read":[]}}}
EOF
diff_run envs/govern.json envs/new.json
dflt="$(group "$OUT" "newly allowed or less refused")"
diff_run envs/govern.json envs/new.json --env dev
dev="$OUT"
diff_run envs/govern.json envs/new.json --env nosuch
if has "$dflt" "env.read:DF_TOKEN  blocked -> allowed" &&
   has "$dev" "no request is judged differently" && [ "$RC" = 4 ] && has "$ERR" "nosuch"; then
    pass DF-08 "default_env is applied (enforce: blocked), --env overrides it (audit refuses nothing), an unknown --env is an error"
else
    fail DF-08 "the environment did not reach the judging" "default: $dflt | unknown rc=$RC"
fi

# --- DF-09 -------------------------------------------------------------
diff_run env/govern.json env/new.json --json
if command -v python3 >/dev/null 2>&1; then
    verdict="$(python3 -c '
import json, sys
d = json.load(sys.stdin)
r = {x["request"]: x for x in d["requests"]}
ok = (r["env.read:DF_TOKEN"]["change"] == "loosened"
      and r["env.read:DF_TOKEN"]["before"] == "blocked"
      and r["env.read:DF_TOKEN"]["after"] == "allowed"
      and r["fs.read:vault/naab-diff-probe"]["change"] == "tightened"
      and any("DF_TOKEN" in s for s in d["settings"]["loosened"])
      and d["unlisted_controls"]["env"] == "NAAB_DIFF_UNLISTED"
      and d["probes"] > 20 and len(d["not_probed"]) >= 1)
sys.stdout.write("ok" if ok else "bad")
' <<<"$OUT" 2>&1)"
    if [ "$RC" = 1 ] && [ "$verdict" = "ok" ]; then
        pass DF-09 "--json carries the requests, the settings and the controls"
    else
        fail DF-09 "--json is missing a field (exit $RC)" "$verdict"
    fi
else
    skip DF-09 "python3 is not installed, so the JSON cannot be read"
fi

# --- DF-10 -------------------------------------------------------------
echo "not json {" > bad.json
bad=""
"$GOV" diff env/govern.json >/dev/null 2>&1;                      [ $? = 4 ] || bad="$bad one-file"
"$GOV" diff env/govern.json nosuch.json >/dev/null 2>&1;          [ $? = 4 ] || bad="$bad missing"
"$GOV" diff env/govern.json bad.json >/dev/null 2>&1;             [ $? = 4 ] || bad="$bad not-json"
"$GOV" diff env/govern.json env/new.json --frob >/dev/null 2>&1;  [ $? = 4 ] || bad="$bad option"
if [ -z "$bad" ]; then
    pass DF-10 "usage errors exit 4 (one file, a missing file, not JSON, an unknown option)"
else
    fail DF-10 "a usage error did not exit 4" "$bad"
fi

# --- DF-11 -------------------------------------------------------------
diff_run env/govern.json env/new.json
if has "$OUT" "Not judged as requests:" && has "$OUT" "taint" && has "$OUT" "NAAB_DIFF_UNLISTED"; then
    pass DF-11 "the report prints what it does not judge as requests"
else
    fail DF-11 "the report does not say what it leaves out"
fi

# --- A147 (pinned, open) ---------------------------------------------------
mkdir -p wild plain
cat > wild/govern.json <<EOF
{$BASE,"capabilities":{"network":{"enabled":true,"blocked_hosts":["*.blocked.invalid"]}}}
EOF
cat > plain/govern.json <<EOF
{$BASE,"capabilities":{"network":{"enabled":true,"blocked_hosts":["blocked.invalid"]}}}
EOF
cat > host.naab <<'EOF'
use http
main {
    try {
        let r = http.get("http://sub.blocked.invalid/")
        print("ANSWERED")
    } catch (e) {
        print("CAUGHT")
    }
}
EOF
run_in wild host.naab;  w_out="$ROUT"
run_in plain host.naab; p_out="$ROUT"
if has "$p_out" "SANDBOX VIOLATION" && has "$p_out" "CAUGHT"; then
    pass A147c "CONTROL: the plain entry blocked.invalid refuses sub.blocked.invalid"
    if has "$w_out" "CAUGHT" && ! has "$w_out" "SANDBOX VIOLATION"; then
        pass A147 "PINNED (open): the entry *.blocked.invalid does not refuse sub.blocked.invalid"
    else
        fail A147 "the wildcard entry no longer behaves as pinned -- update the A147 record and this arm" \
            "$(grep -E 'SANDBOX|CAUGHT|ANSWERED' <<<"$w_out" | head -3)"
    fi
else
    skip A147c "the plain entry's refusal was not observed, so the wildcard arm has no control"
    skip A147 "no control"
fi

echo ""
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
[ "$FAIL" -eq 0 ]
