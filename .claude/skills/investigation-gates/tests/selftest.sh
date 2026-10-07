#!/usr/bin/env bash
# selftest.sh -- positive controls for the investigation-gates scripts.
#
# Each arm reports PASS / FAIL / UNMEASURABLE. The M arms are MUTANTS: a
# deliberately broken copy of a script must make its control go red, or the
# control cannot fail ("A new test that passes first try is suspect").
#
# Hermetic: builds its own throwaway git repository; touches nothing in the
# working tree. No `set -e`/`pipefail`, and no `| grep -q` (CLAUDE.md Gotchas):
# output is captured into variables and matched with `case`.

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
S="$HERE/../scripts"
FX="$HERE/fixtures"
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS + 1)); echo "PASS  $1"; }
fail() { FAIL=$((FAIL + 1)); echo "FAIL  $1"; [ -n "${2:-}" ] && printf '      %s\n' "${2:0:400}"; }
unm()  { UNM=$((UNM + 1));   echo "UNMEASURABLE  $1"; }
has()  { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

PY=$(command -v python3 || true)
GIT=$(command -v git || true)
WORK=$(mktemp -d 2>/dev/null) || { echo "UNMEASURABLE  mktemp failed"; exit 2; }
trap 'rm -rf "$WORK"' EXIT

# ---------------------------------------------------------------- write-up lint
if [ -z "$PY" ]; then
  unm "L*: python3 not found"
else
  bad=$("$PY" "$S/writeup_lint.py" "$FX/bad_writeup.md" 2>&1)
  for rule in green-suite action-claimed single-cause absence-claim fix-claim \
              confidence-word done-claim unlabelled-figure \
              no-blind-spots no-error-direction no-retrace-count no-adversarial-pass; do
    if has "$bad" "[$rule]"; then pass "L-01 lint flags [$rule] on the bad fixture"
    else fail "L-01 lint flags [$rule] on the bad fixture" "$bad"; fi
  done
  clean=$("$PY" "$S/writeup_lint.py" "$FX/clean_writeup.md" 2>&1)
  if has "$clean" "0 line flag(s), 0 missing"; then pass "L-02 clean fixture raises nothing (suppressions work)"
  else fail "L-02 clean fixture raises nothing" "$clean"; fi
  : > "$WORK/empty.md"
  empty=$("$PY" "$S/writeup_lint.py" "$WORK/empty.md" 2>&1); st=$?
  if [ $st -eq 2 ] && has "$empty" "UNMEASURABLE"; then pass "L-03 empty input is UNMEASURABLE, not clean"
  else fail "L-03 empty input is UNMEASURABLE" "exit=$st $empty"; fi

  # M-L: a lint with one rule's pattern disabled must lose that flag.
  sed 's/(pushed|merged|committed|deployed|published)/(zzz_never_matches)/' "$S/writeup_lint.py" > "$WORK/lint_mutant.py"
  mut=$("$PY" "$WORK/lint_mutant.py" "$FX/bad_writeup.md" 2>&1)
  if has "$mut" "[action-claimed]"; then fail "M-L mutant lint (rule disabled) still flags [action-claimed] -- L-01 cannot fail" "$mut"
  elif cmp -s "$S/writeup_lint.py" "$WORK/lint_mutant.py"; then unm "M-L mutation did not apply (pattern text changed?)"
  else pass "M-L disabling a rule removes its flag (L-01 can go red)"; fi
fi

# ---------------------------------------------------------------- flake sizing
if [ -z "$PY" ]; then
  unm "F*: python3 not found"
else
  f=$("$PY" "$S/flake_runs.py" --observed 1/12 --runs 15 2>&1)
  if has "$f" "0.271" && has "$f" "NOT EVIDENCE"; then pass "F-01 reproduces the method's figure: (11/12)^15 = 0.271, not evidence"
  else fail "F-01 1/12 then 15 clean runs" "$f"; fi
  f=$("$PY" "$S/flake_runs.py" --observed 0/20 --runs 5 2>&1); st=$?
  if [ $st -eq 2 ] && has "$f" "UNMEASURABLE"; then pass "F-02 no observed failure is UNMEASURABLE"
  else fail "F-02 0/20" "exit=$st $f"; fi
  f=$("$PY" "$S/flake_runs.py" --observed 1/12 --runs 300 2>&1)
  if has "$f" "long enough"; then pass "F-03 success control: 300 clean runs after 1/12 counts as evidence"
  else fail "F-03 success control" "$f"; fi
fi

# ---------------------------------------------------------------- history pass
if [ -z "$GIT" ]; then
  unm "H*: git not found"
else
  R="$WORK/repo"
  g() { git -C "$R" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }
  mkdir -p "$R" && git init -q "$R"
  printf 'int alpha_token = 1;\n' > "$R/a.c";               g add a.c >/dev/null; g commit -q -m "add alpha"
  printf '# Fixture\n- alpha_token: kept on purpose, see the reload rule\n' > "$R/CLAUDE.md"
  g add CLAUDE.md >/dev/null; g commit -q -m "decide: keep reload rule"
  printf 'int other = 2;\nint alpha_token = 1;\n' > "$R/a.c"; g add a.c >/dev/null; g commit -q -m "touch a.c"
  printf 'int beta_token = 3;\n' > "$R/b.c";                g add b.c >/dev/null; g commit -q -m "add beta"

  h=$(cd "$R" && bash "$S/history_pass.sh" -g "reload rule" alpha_token 2>&1); st=$?
  if [ $st -eq 0 ] && has "$h" "HITS 1" && has "$h" "add alpha" && has "$h" "decide: keep reload rule"; then
    pass "H-01 full history: pickaxe finds the adding commit, --grep finds the decision"
    if has "$h" "CLAUDE.md:2:"; then pass "H-01b decision-doc search finds the CLAUDE.md line"
    else fail "H-01b decision-doc search" "$h"; fi
  else fail "H-01 full history" "exit=$st $h"; fi

  h=$(cd "$R" && bash "$S/history_pass.sh" never_existed_xyz 2>&1); st=$?
  if [ $st -eq 0 ] && has "$h" "NONE" && has "$h" "absent at HEAD"; then pass "H-02 full history: a string never committed is NONE"
  else fail "H-02 NONE on full history" "exit=$st $h"; fi

  C="$WORK/shallow"
  git clone -q --depth 1 "file://$R" "$C" 2>/dev/null
  if [ "$(git -C "$C" rev-parse --is-shallow-repository 2>/dev/null)" != "true" ]; then
    unm "H-03 could not make a shallow clone here"
  else
    h=$(cd "$C" && bash "$S/history_pass.sh" alpha_token 2>&1); st=$?
    # The pickaxe section must end UNMEASURABLE; the decision-doc section may
    # legitimately have HITS (it reads the current tree), so do not test for
    # "no HITS anywhere".
    pick=${h#*"-- git log -S"}; pick=${pick%%"== decision docs"*}
    if [ $st -eq 2 ] && has "$h" "SHALLOW CLONE" && has "$pick" "shallow-boundary artifact" \
       && has "$pick" "UNMEASURABLE: no hits, but history is truncated" && ! has "$pick" "HITS"; then
      pass "H-03 shallow clone: boundary 'origin' is set aside and the result is UNMEASURABLE"
    else fail "H-03 shallow clone" "exit=$st $h"; fi
  fi

  h=$(cd "$WORK" && bash "$S/history_pass.sh" alpha_token 2>&1); st=$?
  if [ $st -eq 2 ] && has "$h" "not inside a git repository"; then pass "H-04 outside a repository is UNMEASURABLE"
  else fail "H-04 outside a repository" "exit=$st $h"; fi

  h=$(cd "$R" && bash "$S/history_pass.sh" -p b.c alpha_token 2>&1); st=$?
  if [ $st -eq 0 ] && has "$h" "SCOPED" && ! has "$h" "probe is broken"; then
    pass "H-05 a scoped NONE is labelled SCOPED and does not trip the broken-probe control"
  else fail "H-05 scoped query" "exit=$st $h"; fi

  # M-H: sabotage the pickaxe so it can never match; the built-in control
  # (string present at HEAD => some commit added it) must fire.
  sed 's/-S"\$id"/-S"${id}__sabotaged"/' "$S/history_pass.sh" > "$WORK/hp_mutant.sh"
  if cmp -s "$S/history_pass.sh" "$WORK/hp_mutant.sh"; then
    unm "M-H mutation did not apply (pickaxe line changed?)"
  else
    h=$(cd "$R" && bash "$WORK/hp_mutant.sh" alpha_token 2>&1); st=$?
    if [ $st -eq 2 ] && has "$h" "probe is broken"; then pass "M-H a sabotaged pickaxe is caught by the built-in control"
    else fail "M-H sabotaged pickaxe went undetected" "exit=$st $h"; fi
  fi

  # H-06: git warnings on stderr must not be parsed as commit hits. A non-root
  # CI job cannot read $HOME/.config/git and git warns to stderr on every call;
  # merged with 2>&1 those lines were counted as hits (shallow UNMEASURABLE
  # hidden, broken-probe control inflated past zero). Reproduced on ANY user by
  # a `git` shim that prints a warning to stderr, then forwards to the real git.
  # With the fix (separate streams) the result is identical to the clean run.
  if [ -n "$GIT" ]; then
    mkdir -p "$WORK/shim"
    { printf '#!/usr/bin/env bash\n'
      printf 'echo "warning: unable to access '\''/nonexistent/.config/git/attributes'\'': Permission denied" >&2\n'
      printf 'exec %q "$@"\n' "$GIT"
    } > "$WORK/shim/git"
    chmod +x "$WORK/shim/git"
    clean=$(cd "$R" && bash "$S/history_pass.sh" alpha_token 2>/dev/null); cst=$?
    noisy=$(cd "$R" && PATH="$WORK/shim:$PATH" bash "$S/history_pass.sh" alpha_token 2>/dev/null); st=$?
    # the shim fires (control): without it this arm proves nothing
    probe=$(cd "$R" && PATH="$WORK/shim:$PATH" git --version 2>&1 >/dev/null)
    if ! has "$probe" "Permission denied"; then
      unm "H-06 the git shim did not emit its stderr warning"
    elif has "$clean" "HITS 1" && [ "$noisy" = "$clean" ] && [ "$st" -eq "$cst" ]; then
      # identical to the clean run, exit and all -- the warning changed nothing
      pass "H-06 git stderr warnings are not parsed as commit hits"
    else fail "H-06 git stderr warning leaked into parsed output" "clean_exit=$cst noisy_exit=$st
$noisy"; fi
  fi
fi

echo
echo "selftest: $PASS pass, $FAIL fail, $UNM unmeasurable"
[ "$FAIL" -gt 0 ] && exit 1
[ "$UNM" -gt 0 ] && exit 2
exit 0
