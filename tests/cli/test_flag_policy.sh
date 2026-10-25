#!/usr/bin/env bash
# ============================================================
# test_flag_policy.sh -- every command-line option main.cpp parses has been
# classified against govern.json's policy (Bundle 1 item 1.6)
#
# WHY THIS EXISTS
#
# govern.json is primary (CLAUDE.md): CLI flags are overrides only, and no
# behaviour should require a CLI flag without a govern.json equivalent. A new
# option can quietly break that -- a bypass flag nobody wired into the
# integrity block (CLAUDE.md A13: --sandbox-level was lockable in govern.json
# yet never consulted), or a loosening flag with no govern.json story. This
# guard forces the decision: tools/flag_policy_coverage.py extracts every
# "--name" literal main.cpp parses, and tests/cli/flag_policy.txt must carry a
# category + reason for each. A new option fails here until classified; a
# removed one fails until its line goes.
#
# It is modelled on FP-S (test_flag_position.sh, reads main.cpp, screened) and
# on test_ratchet_coverage.sh (a committed classification whose set may not
# drift). SCREENED, NOT VERIFIED: the extraction is a search over source text
# and the category is a reviewer's decision about the relationship, not a proof
# of the enforcement direction -- the governance reload/bypass suites test that.
#
#   FC-01  CONTROL: the extractor self-test passes -- it finds a planted
#          literal and excludes one that lives only in a comment, so an
#          extractor that finds nothing cannot pass this suite
#   FC-02  every option main.cpp parses is classified in flag_policy.txt
#          (a NEW option fails until someone classifies it)
#   FC-03  every option flag_policy.txt lists is still parsed by main.cpp
#          (a REMOVED option fails until its line is deleted)
#   FC-04  every bypass-guarded:<fn> line names a function still in main.cpp
#          (the named bypass-flag defense cannot silently disappear)
#   FC-05  every line is well-formed: 3 fields, a known category, a reason
#   FC-06  NEGATIVE CONTROL: a fake option planted in the piped source is
#          extracted and is absent from the file -- so FC-02 can actually fail
#   FC-07  NEGATIVE CONTROL: FC-04's grep is load-bearing -- a fabricated
#          function name is reported absent from main.cpp
#   FC-08  the tool output is ASCII
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOL="$REPO/tools/flag_policy_coverage.py"
DATA="$SCRIPT_DIR/flag_policy.txt"
MAIN_CPP="$REPO/src/cli/main.cpp"
. "$REPO/tests/helpers/encoding_controls.sh"

TMPD="$(mktemp -d)"; trap 'rm -rf "$TMPD"' EXIT

PASS=0; FAIL=0; UNM=0
ok()  { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -20 | sed 's/^/       | /'; }
unm() { UNM=$((UNM+1)); echo "  UNMEASURABLE [$1] $2"; }

echo ""
echo "=== Flag policy coverage: every CLI option is classified ==="

if ! command -v python3 >/dev/null 2>&1; then
    unm "FC-00" "python3 missing"; echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 0
fi
for f in "$TOOL" "$DATA" "$MAIN_CPP"; do
    if [ ! -f "$f" ]; then unm "FC-00" "missing $f"; echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"; exit 0; fi
done

# --- FC-01: extractor self-test (positive control) --------------------------
ST="$(python3 "$TOOL" self-test 2>&1)"; ST_RC=$?
if [ "$ST_RC" -eq 0 ] && grep -qx 'self-test failures: 0' <<<"$ST"; then
    ok "FC-01" "extractor self-test passes (planted found, comment-only excluded)"
else
    bad "FC-01" "extractor self-test failed -- the extraction below is not trustworthy" "$ST"
fi

# The classified options: column 1 of every non-comment, non-blank line. The
# parsed options: the tool's extraction of main.cpp (source on stdin so a native
# Windows python never opens an MSYS path). Both are sorted in C-locale (byte)
# order -- the tool sorts in Python codepoint order, and `comm` demands the two
# inputs share one collation, which only LC_ALL=C guarantees across locales.
CLASSIFIED="$(grep -v '^#' "$DATA" | grep -v '^[[:space:]]*$' | cut -f1 | LC_ALL=C sort -u)"
PARSED="$(python3 "$TOOL" extract < "$MAIN_CPP" | LC_ALL=C sort -u)"

# --- FC-02: no parsed option is unclassified --------------------------------
UNCLASSIFIED="$(LC_ALL=C comm -23 <(printf '%s\n' "$PARSED") <(printf '%s\n' "$CLASSIFIED") | grep -v '^$')"
if [ -z "$PARSED" ]; then
    bad "FC-02" "the extractor reported no options at all -- broken probe, not a result" "$PARSED"
elif [ -z "$UNCLASSIFIED" ]; then
    ok "FC-02" "every option main.cpp parses is classified ($(printf '%s\n' "$PARSED" | wc -l | tr -d ' ') options)"
else
    bad "FC-02" "option(s) main.cpp parses with no line in flag_policy.txt -- decide how each relates to govern.json and classify it" "$UNCLASSIFIED"
fi

# --- FC-03: no classified option is stale -----------------------------------
STALE="$(LC_ALL=C comm -13 <(printf '%s\n' "$PARSED") <(printf '%s\n' "$CLASSIFIED") | grep -v '^$')"
if [ -z "$STALE" ]; then
    ok "FC-03" "every option in flag_policy.txt is still parsed by main.cpp"
else
    bad "FC-03" "flag_policy.txt lists option(s) main.cpp no longer parses -- remove them" "$STALE"
fi

# --- FC-04: bypass-guarded lines name a live function -----------------------
MAIN_SRC="$(cat "$MAIN_CPP")"
guard_bad=""
while IFS=$'\t' read -r opt cat reason; do
    case "$cat" in
        bypass-guarded:*)
            fn="${cat#bypass-guarded:}"
            if [ -z "$fn" ]; then
                guard_bad+="$opt: category has no function after 'bypass-guarded:'"$'\n'
            elif ! grep -qF -- "$fn" <<<"$MAIN_SRC"; then
                guard_bad+="$opt: named defense '$fn' is not in main.cpp"$'\n'
            fi
            ;;
    esac
done < <(grep -v '^#' "$DATA" | grep -v '^[[:space:]]*$')
if [ -z "$guard_bad" ]; then
    ok "FC-04" "every bypass-guarded option names a function present in main.cpp"
else
    bad "FC-04" "a bypass-guarded line names a function main.cpp no longer has" "$guard_bad"
fi

# --- FC-05: well-formedness -------------------------------------------------
KNOWN_RE='^(bypass-guarded:[A-Za-z_][A-Za-z0-9_]*|run-policy|governance-evidence|governance-admin|neutral)$'
form_bad=""
while IFS= read -r line; do
    opt="$(cut -f1 <<<"$line")"; cat="$(cut -f2 <<<"$line")"; reason="$(cut -f3 <<<"$line")"
    nfields="$(awk -F'\t' '{print NF}' <<<"$line")"
    if [ "$nfields" != 3 ]; then
        form_bad+="$opt: expected 3 tab-separated fields, got $nfields"$'\n'; continue
    fi
    if ! grep -qE "$KNOWN_RE" <<<"$cat"; then
        form_bad+="$opt: unknown category '$cat'"$'\n'
    fi
    if [ -z "${reason// /}" ]; then
        form_bad+="$opt: empty reason"$'\n'
    fi
done < <(grep -v '^#' "$DATA" | grep -v '^[[:space:]]*$')
if [ -z "$form_bad" ]; then
    ok "FC-05" "every line has 3 fields, a known category, and a reason"
else
    bad "FC-05" "malformed line(s) in flag_policy.txt" "$form_bad"
fi

# --- FC-06: NEGATIVE CONTROL -- a planted option is detectable ---------------
# Insert a fresh "--zzz-flag-policy-probe" parse site into the piped source and
# confirm the extractor surfaces it and that it is NOT already classified, so
# FC-02's comparison provably CAN fire on a new option.
PLANT='--zzz-flag-policy-probe'
PLANTED_SRC="$(printf '%s\n    } else if (arg == "%s") { probe = true;\n' "$MAIN_SRC" "$PLANT")"
PROBE_OUT="$(printf '%s' "$PLANTED_SRC" | python3 "$TOOL" extract)"
if grep -qx -- "$PLANT" <<<"$PROBE_OUT" && ! grep -qx -- "$PLANT" <<<"$CLASSIFIED"; then
    ok "FC-06" "control: a planted option is extracted and would fail FC-02"
else
    bad "FC-06" "control failed: the planted option was not surfaced as a new, unclassified option" "$PROBE_OUT"
fi

# --- FC-07: NEGATIVE CONTROL -- FC-04's grep distinguishes ------------------
FAKE_FN='checkBlocked_this_name_does_not_exist_zzz'
if grep -qF -- "$FAKE_FN" <<<"$MAIN_SRC"; then
    bad "FC-07" "control failed: a fabricated function name was found in main.cpp" ""
else
    ok "FC-07" "control: a fabricated function name is reported absent (FC-04's grep is load-bearing)"
fi

# --- FC-08: ASCII -----------------------------------------------------------
printf '%s\n%s\n' "$PARSED" "$ST" > "$TMPD/tool_out.txt"
if enc_has_non_ascii "$TMPD/tool_out.txt"; then
    bad "FC-08" "tool output is not ASCII" ""
else
    ok "FC-08" "tool output is ASCII"
fi

echo ""
echo "  Results: $PASS passed, $FAIL failed, $UNM unmeasurable"
[ "$FAIL" -eq 0 ]
