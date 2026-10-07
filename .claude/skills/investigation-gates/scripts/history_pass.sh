#!/usr/bin/env bash
# history_pass.sh -- the mechanical half of the history pass in
# docs/investigation-method.md ("Code shows what happens now; history shows
# what was meant" and the "Before changing code" checklist).
#
# It runs the queries. It does NOT classify: deciding never built / working /
# regressed / removed on purpose / lost / decided is yours, from the commits it
# prints, read in full.
#
# Every query reports one of three outcomes:
#   HITS n        commits found (each one still has to be read)
#   NONE          the query ran on full history and matched nothing
#   UNMEASURABLE  the query could not answer -- never read this as NONE
#
# Usage:
#   history_pass.sh [-f FILE]... [-g NOUN]... [-r REGEX]... [-n MAX] [IDENTIFIER...]
#     IDENTIFIER  exact string for `git log -S` (pickaxe: commits that ADD or REMOVE it)
#     -r REGEX    `git log -G` (commits whose diff touches a line matching REGEX)
#     -f FILE     `git log --follow` on a file you will edit
#     -g NOUN     `git log -i --grep` on a subject noun (PR bodies, in a squash-merging repo)
#     -n MAX      commits to print per query (default 15; the total is always printed)
#     -p PATH     limit -S/-G/--grep to PATH (repeatable). Default is the whole repo.
#                 Measured once on NAAb (1,266 commits, this container): -S over the
#                 whole repo ~105 s; with -p src -p include ~0.5 s. A scoped NONE means
#                 NONE within those paths only -- a doc-only removal is outside it.
#
# Exit: 0 every query measurable, 2 at least one UNMEASURABLE, 64 usage error.
#
# Why shallow clones get special handling: in a shallow clone the boundary
# commit has no parent, so pickaxe attributes EVERY string in the tree to it.
# Measured on NAAb at a depth-1 clone: `git log -S'loaded_mtime_ns_'` named
# the newest commit (#296) as the one that added it. That is a broken probe
# reporting a plausible finding, not an empty result.
#
# Deliberately no `set -e` and no `pipefail`: a failing git command must be
# reported as UNMEASURABLE, and `pipefail` + an early-exiting reader inverts
# verdicts (CLAUDE.md, Gotchas). Output is ASCII only.

set -u

MAX=15
FILES=(); NOUNS=(); REGEXES=(); IDENTS=(); PATHS=()
while [ $# -gt 0 ]; do
  case "$1" in
    -f) [ $# -ge 2 ] || { echo "usage: -f needs a FILE" >&2; exit 64; }; FILES+=("$2"); shift 2 ;;
    -g) [ $# -ge 2 ] || { echo "usage: -g needs a NOUN" >&2; exit 64; }; NOUNS+=("$2"); shift 2 ;;
    -r) [ $# -ge 2 ] || { echo "usage: -r needs a REGEX" >&2; exit 64; }; REGEXES+=("$2"); shift 2 ;;
    -p) [ $# -ge 2 ] || { echo "usage: -p needs a PATH" >&2; exit 64; }; PATHS+=("$2"); shift 2 ;;
    -n) [ $# -ge 2 ] || { echo "usage: -n needs a number" >&2; exit 64; }; MAX="$2"; shift 2 ;;
    -h|--help) sed -n '2,30p' "$0"; exit 0 ;;
    --) shift; while [ $# -gt 0 ]; do IDENTS+=("$1"); shift; done ;;
    -*) echo "unknown option: $1" >&2; exit 64 ;;
    *) IDENTS+=("$1"); shift ;;
  esac
done
if [ ${#FILES[@]} -eq 0 ] && [ ${#NOUNS[@]} -eq 0 ] && [ ${#REGEXES[@]} -eq 0 ] && [ ${#IDENTS[@]} -eq 0 ]; then
  echo "usage: history_pass.sh [-f FILE]... [-g NOUN]... [-r REGEX]... [-n MAX] [IDENTIFIER...]" >&2
  exit 64
fi
case "$MAX" in ''|*[!0-9]*) echo "-n must be a number" >&2; exit 64 ;; esac

UNMEASURABLE_COUNT=0
unmeasurable() { UNMEASURABLE_COUNT=$((UNMEASURABLE_COUNT + 1)); echo "  UNMEASURABLE: $*"; }

# Print at most MAX lines of $1, each cut to 220 chars (CLAUDE.md lines run to 5,500).
print_capped() {
  local n=0 line
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    n=$((n + 1))
    [ "$n" -le "$MAX" ] && printf '    %s\n' "${line:0:220}"
  done <<< "$1"
  [ "$n" -gt "$MAX" ] && echo "    ... $((n - MAX)) more (raise -n to see them)"
}

count_lines() { local c=0 line; while IFS= read -r line; do [ -n "$line" ] && c=$((c + 1)); done <<< "$1"; echo "$c"; }

# ---- usability: is the instrument live at all? -------------------------------
# stdout only -- a git stderr warning merged in here (2>&1) became part of the
# path and the later `cd "$top"` died on it (see log_query's note). The error
# text for the not-a-repo message is read from stderr separately.
top_errf=$(mktemp 2>/dev/null) || top_errf=""
if [ -n "$top_errf" ]; then
  top=$(git rev-parse --show-toplevel 2>"$top_errf"); st=$?
  top_err=$(cat "$top_errf" 2>/dev/null); rm -f "$top_errf"
else
  top=$(git rev-parse --show-toplevel 2>/dev/null); st=$?; top_err=""
fi
if [ $st -ne 0 ]; then
  echo "UNMEASURABLE: not inside a git repository (${top_err:0:200})"
  exit 2
fi
cd "$top" || { echo "UNMEASURABLE: cannot cd to $top"; exit 2; }

head_sha=$(git rev-parse --short HEAD 2>/dev/null)
branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)
total=$(git rev-list --count HEAD 2>/dev/null)
shallow=$(git rev-parse --is-shallow-repository 2>/dev/null)
BOUNDARY=()
if [ "$shallow" = "true" ]; then
  shallow_file=$(git rev-parse --git-path shallow)
  while IFS= read -r b; do [ -n "$b" ] && BOUNDARY+=("$(git rev-parse --short "$b" 2>/dev/null)"); done < "$shallow_file"
fi

echo "== history pass @ $head_sha ($branch), $total commits reachable"
if [ "$shallow" = "true" ]; then
  echo "!! SHALLOW CLONE: history is truncated at ${BOUNDARY[*]}."
  echo "!! Every NONE below becomes UNMEASURABLE, and any hit ON a boundary commit is an"
  echo "!! artifact (the boundary 'adds' the whole tree), not an origin. To measure:"
  remote_branch=$(git rev-parse --abbrev-ref '@{upstream}' 2>/dev/null || git symbolic-ref --short refs/remotes/origin/HEAD 2>/dev/null || echo "origin/<branch>")
  echo "!!   git fetch --depth=1000 origin ${remote_branch#origin/}    (bounded; works where --unshallow is blocked)"
  echo "!!   git fetch --unshallow origin"
fi
echo

SCOPE_ARGS=(); SCOPE_LABEL=""
if [ ${#PATHS[@]} -gt 0 ]; then SCOPE_ARGS=(-- "${PATHS[@]}"); SCOPE_LABEL=" -- ${PATHS[*]}  [SCOPED: NONE means none in these paths]"; fi

is_boundary() { local c="$1" b; for b in "${BOUNDARY[@]:-}"; do [ -n "$b" ] && [ "$c" = "$b" ] && return 0; done; return 1; }

# Run one log query. $1 = label, rest = git log args.
# Sets LAST_REAL_HITS to the number of non-boundary commits found.
#
# stdout and stderr are kept SEPARATE on purpose. git writes warnings to
# stderr -- e.g. "unable to access '$HOME/.config/git/attributes': Permission
# denied" when run as a user who cannot read the ambient config, as a non-root
# CI job does. Merged with 2>&1 those warning lines were parsed as commit
# rows: each became a bogus HITS, which hid the shallow-clone UNMEASURABLE and
# inflated the broken-probe control's count past zero (selftest H-03 and M-H
# failed only under that condition). Only stdout is parsed; stderr is reported
# as the reason when, and only when, git exits non-zero.
LAST_REAL_HITS=0
log_query() {
  local label="$1"; shift
  local out st err real="" art="" line sha
  local errf; errf=$(mktemp 2>/dev/null) || errf=""
  if [ -n "$errf" ]; then
    out=$(git log --format='%h %ad %s' --date=short "$@" 2>"$errf"); st=$?
    err=$(cat "$errf" 2>/dev/null); rm -f "$errf"
  else
    out=$(git log --format='%h %ad %s' --date=short "$@" 2>/dev/null); st=$?; err=""
  fi
  echo "-- $label"
  if [ $st -ne 0 ]; then
    LAST_REAL_HITS=0
    unmeasurable "git log failed: ${err:0:200}"
    return
  fi
  while IFS= read -r line; do
    [ -z "$line" ] && continue
    sha=${line%% *}
    if is_boundary "$sha"; then art+="$line"$'\n'; else real+="$line"$'\n'; fi
  done <<< "$out"
  LAST_REAL_HITS=$(count_lines "$real")
  if [ "$LAST_REAL_HITS" -gt 0 ]; then
    echo "  HITS $LAST_REAL_HITS"
    print_capped "$real"
  fi
  if [ -n "$art" ]; then
    echo "  (ignored: shallow-boundary artifact, not an origin)"
    print_capped "$art"
  fi
  if [ "$LAST_REAL_HITS" -eq 0 ]; then
    if [ "$shallow" = "true" ]; then
      unmeasurable "no hits, but history is truncated -- the origin may lie past the boundary"
    else
      echo "  NONE"
    fi
  fi
}

# ---- 1. pickaxe on each identifier, with a built-in positive control ---------
for id in "${IDENTS[@]:-}"; do
  [ -z "$id" ] && continue
  log_query "git log -S'$id'$SCOPE_LABEL  (commits adding or removing it)" -S"$id" "${SCOPE_ARGS[@]}"
  hits=$LAST_REAL_HITS
  # Scope the control exactly as the query, or a string living outside -p paths
  # would read as a broken probe.
  in_tree=$(git grep -l -F -e "$id" HEAD -- "${PATHS[@]:-.}" 2>/dev/null)
  ntree=$(count_lines "$in_tree")
  # Control: a string present in HEAD was added by SOME commit. With full
  # history and zero adding commits, the instrument -- not the code -- is wrong.
  if [ "$ntree" -gt 0 ] && [ "$hits" -eq 0 ] && [ "$shallow" != "true" ]; then
    unmeasurable "present in $ntree tracked file(s) at HEAD but no commit adds it -- the probe is broken (check quoting/escaping)"
  fi
  if [ "$ntree" -gt 0 ]; then
    echo "  present at HEAD in $ntree file(s):"
    print_capped "$in_tree"
  else
    echo "  absent at HEAD (exact string; check aliases/namesakes before calling it gone)"
  fi
  echo
done

# ---- 2. regex history --------------------------------------------------------
for rx in "${REGEXES[@]:-}"; do
  [ -z "$rx" ] && continue
  log_query "git log -G'$rx'$SCOPE_LABEL  (diffs touching matching lines)" -G"$rx" "${SCOPE_ARGS[@]}"
  echo
done

# ---- 3. file lineage ---------------------------------------------------------
for f in "${FILES[@]:-}"; do
  [ -z "$f" ] && continue
  if [ ! -e "$f" ]; then
    echo "-- git log --follow -- $f"
    echo "  not in the working tree: if it was deleted, search its name with -g or as an IDENTIFIER"
    echo
    continue
  fi
  log_query "git log --follow -- $f" --follow -- "$f"
  echo "  next: git blame -L <start>,<end> -- $f   on the lines you will edit, then read each commit in full (git show <sha>)"
  echo
done

# ---- 4. commit-message search on subject nouns -------------------------------
for noun in "${NOUNS[@]:-}"; do
  [ -z "$noun" ] && continue
  log_query "git log -i --grep='$noun'$SCOPE_LABEL" -i --grep="$noun" "${SCOPE_ARGS[@]}"
  echo
done

# ---- 5. decision docs (current tree -- valid in a shallow clone) -------------
DOCS=()
for p in CLAUDE.md docs/open-investigations.md docs/governance-campaign-findings.md \
         docs/security-decisions.md docs/engine-divergences.md docs/investigation-method.md; do
  [ -f "$p" ] && DOCS+=("$p")
done
for p in docs/plan-*.md docs/findings/*.md; do [ -f "$p" ] && DOCS+=("$p"); done
TERMS=()
for t in "${IDENTS[@]:-}" "${NOUNS[@]:-}"; do [ -n "$t" ] && TERMS+=("$t"); done
if [ ${#TERMS[@]} -gt 0 ]; then
  echo "== decision docs (${#DOCS[@]} files: CLAUDE.md, open-investigations, campaign findings, security-decisions, plan-*, findings/)"
  if [ ${#DOCS[@]} -eq 0 ]; then
    unmeasurable "none of the decision docs exist here -- wrong repository or directory?"
  fi
  for t in "${TERMS[@]}"; do
    [ ${#DOCS[@]} -eq 0 ] && break
    out=$(grep -H -n -i -F -e "$t" -- "${DOCS[@]}" 2>/dev/null)   # -H: name the file even when only one doc exists
    n=$(count_lines "$out")
    echo "-- '$t'"
    if [ "$n" -gt 0 ]; then echo "  HITS $n"; print_capped "$out"; else echo "  NONE (exact, case-insensitive; vocabulary bounds what this finds)"; fi
  done
  echo
fi

# ---- 6. what is left for you --------------------------------------------------
cat <<'EOF'
== not done by this script (the checklist still needs them)
  - read each commit above IN FULL (git show <sha>); a subject line is not intent
  - GitHub PR review threads for those commits (they never reach git)
  - classify: never built / working / regressed / removed on purpose / lost / decided,
    quoting the commit or doc line that puts it there
  - copy anything the reference docs lack into the doc a future reader will search
EOF

if [ "$UNMEASURABLE_COUNT" -gt 0 ]; then
  echo
  echo "!! $UNMEASURABLE_COUNT UNMEASURABLE result(s). Do not report them as 'no history'."
  exit 2
fi
exit 0
