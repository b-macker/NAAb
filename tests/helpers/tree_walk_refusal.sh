#!/usr/bin/env bash
# tree_walk_refusal.sh -- under an enforce govern.json, `naab-lang --tree-walk`
# is refused (exit 4) before anything runs (Bundle 2; the refusal itself is
# pinned by tests/cli/test_tree_walk_enforce.sh). A suite whose tree-walk arms
# run under enforce asserts that refusal where it used to assert the
# tree-walker's own enforcement, so the arm still fails if the tree-walker
# runs the program.
#
#   tw_refused RC OUTPUT   0 when RC is 4 and OUTPUT carries the refusal (the
#                          refusal is printed to stderr: capture it)
#
#   tw_off_config FILE     rewrite a govern.json to mode off and drop its
#                          signature, for an arm about the tree-walker's engine
#                          semantics rather than enforcement. Mode off, not
#                          audit: a signed audit config is raised to enforce at
#                          load (F3), and where trusted keys are installed
#                          every config must be signed, so audit cannot run the
#                          tree-walker there. Re-sign the file if the suite
#                          signs. Returns non-zero when the file did not end up
#                          in mode off, so the caller can say the arm is
#                          UNMEASURABLE.
#
# Source it:  . "$REPO/tests/helpers/tree_walk_refusal.sh"

TW_REFUSAL_MARK="--tree-walk cannot run under this project's governance"

tw_refused() {
    [ "$1" = 4 ] && grep -qF -- "$TW_REFUSAL_MARK" <<<"$2"
}

tw_off_config() {
    local f="$1"
    sed 's/"mode"[[:space:]]*:[[:space:]]*"\(enforce\|audit\)"/"mode": "off"/' "$f" > "$f.tw.tmp" &&
        mv "$f.tw.tmp" "$f" || return 1
    rm -f "$f.sig"
    grep -q '"mode": "off"' "$f"
}
