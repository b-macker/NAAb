#!/usr/bin/env bash
# test_blocks_search.sh -- `naab-lang blocks search` finds blocks (A170).
#
# The block search index declared its full-text table as an external-content
# table over `blocks`, naming keywords/use_cases columns `blocks` does not
# have, so every text query failed inside SQLite and search() returned an
# empty list: `blocks search metadata` printed "No blocks found" for a block
# whose keywords include "metadata" (observed on aff33269 and d32f8603).
# `blocks list --language X` sent "*" as the query, which is not FTS5 syntax,
# so it listed nothing either. Now the table stores its own text, an index
# made the old way is rebuilt when opened (unit test
# AnOldFormatIndexIsRebuiltOnOpen), a query that cannot run is reported
# (exit 1) instead of read as "none", and `blocks similar` sends the
# description's words, each quoted, rather than raw text whose punctuation is
# FTS5 syntax.
#
#   BS-00  CONTROL: `blocks index` indexes the two fixture blocks.
#   BS-01  a text search finds the block whose text has the word.
#   BS-02  CONTROL: a word in neither block finds nothing (exit 0).
#   BS-03  a query FTS5 cannot parse is reported, exit 1 (it said "none").
#   BS-04  `blocks list --language javascript` lists both blocks.
#   BS-05  re-indexing does not list a block twice.
#   BS-06  `blocks similar` runs and finds the other block (a shared word).
#
# Kill table (mutants of src/runtime/block_search_index.cpp and
# src/cli/main.cpp, run by hand):
#   B1 the external-content definition back       -> BS-01, BS-04
#   B2 a failed step returns what it has, silently -> BS-03
#   B3 "*" sent as an FTS query again              -> BS-04
#   B4 no delete before the re-index insert        -> BS-05
#   B5 `similar` sends the raw description         -> BS-06
# Not killed here: a failed PREPARE returning empty again. No query this suite
# can send fails at prepare (an FTS5 syntax error surfaces at the step); the
# throw stays as the reporting side of the same rule.

set -uo pipefail
PASS=0; FAIL=0
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
NAAB="${NAAB:-$REPO/build/naab-lang}"
W="$(mktemp -d "${TMPDIR:-/tmp}/naab_bsearch.XXXXXX")"
trap 'rm -rf "$W"' EXIT
ok()  { echo "  PASS $1: $2"; PASS=$((PASS+1)); }
bad() { echo "  FAIL $1: $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -6 | sed 's/^/      /'; FAIL=$((FAIL+1)); }
[ -x "$NAAB" ] || { echo "UNMEASURABLE: $NAAB not built"; exit 1; }

mkdir -p "$W/home" "$W/blocks"
cp -r "$REPO/tests/fixtures/block-samples/." "$W/blocks/"
nb() { OUT=$(cd "$W" && HOME="$W/home" "$NAAB" blocks "$@" 2>&1); RC=$?; }

nb index "$W/blocks"
if [ "$RC" -eq 0 ] && grep -qE '(^|[^0-9])2 blocks|Indexed 2|indexed: 2' <<<"$OUT"; then ok BS-00 "CONTROL: blocks index indexed the two fixture blocks"
else bad BS-00 "blocks index did not index 2 blocks (rc=$RC) -- every arm UNMEASURABLE" "$OUT"; echo "blocks search: $PASS passed, $FAIL failed"; exit 1; fi

nb search metadata
if [ "$RC" -eq 0 ] && grep -q 'TEST-ENHANCED-META' <<<"$OUT" && ! grep -q 'TEST-BASIC-BLOCK' <<<"$OUT"; then
    ok BS-01 "a text search finds the block whose text has the word"
else bad BS-01 "blocks search metadata (rc=$RC)" "$OUT"; fi

nb search zzqqnotawordanywhere
if [ "$RC" -eq 0 ] && grep -q 'No blocks found' <<<"$OUT"; then ok BS-02 "CONTROL: a word in neither block finds nothing"
else bad BS-02 "expected 'No blocks found' (rc=$RC)" "$OUT"; fi

nb search '"unbalanced'
if [ "$RC" -ne 0 ] && grep -qi 'search failed\|error' <<<"$OUT" && ! grep -q 'No blocks found' <<<"$OUT"; then
    ok BS-03 "a query that cannot run is reported, not read as none"
else bad BS-03 "expected an error, exit 1 (rc=$RC)" "$OUT"; fi

nb list --language javascript
if [ "$RC" -eq 0 ] && grep -q 'TEST-ENHANCED-META' <<<"$OUT" && grep -q 'TEST-BASIC-BLOCK' <<<"$OUT"; then
    ok BS-04 "blocks list --language lists every block in the language"
else bad BS-04 "blocks list --language javascript (rc=$RC)" "$OUT"; fi

nb index "$W/blocks"
nb search metadata
n=$(grep -o 'TEST-ENHANCED-META' <<<"$OUT" | wc -l)
if [ "$RC" -eq 0 ] && [ "$n" -eq 1 ]; then ok BS-05 "re-indexing does not list a block twice"
else bad BS-05 "TEST-ENHANCED-META listed $n times after a re-index (rc=$RC)" "$OUT"; fi

nb similar TEST-BASIC-BLOCK
if [ "$RC" -eq 0 ] && grep -q 'TEST-ENHANCED-META' <<<"$OUT"; then ok BS-06 "blocks similar runs and finds the block sharing a word"
else bad BS-06 "blocks similar TEST-BASIC-BLOCK (rc=$RC)" "$OUT"; fi

echo
echo "blocks search: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
