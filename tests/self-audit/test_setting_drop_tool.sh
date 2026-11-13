#!/usr/bin/env bash
# ============================================================
# test_setting_drop_tool.sh -- the setting-liveness tool's own decisions
#
# tools/testrunner/setting_drop.py asks whether any test notices a govern.json
# setting removed. Its 2026-11-13 run showed three ways it read "no test
# notices" where the truth was "not measured" (docs/settings-liveness.md):
# helper binaries without the removal hook, one suite that loads the whole
# template, and no way to probe one setting alone. This suite checks the
# decisions the fix added, on synthetic inputs -- it runs no NAAb program and no
# sweep (those need a test build; tests/security/test_setting_drop_compiled_out.sh
# covers the hook itself).
#
#   ST-01  a helper built in the test build's directory is taken from there and
#          reported as carrying the hook
#   ST-01c CONTROL: a helper only in the ordinary build is taken from it and
#          reported as having NO hook
#   ST-02  a helper in the test build's directory WITHOUT the hook's name is
#          reported NO hook (a directory is not evidence)
#   ST-03  a survivor every loader of which is a broad loader is "broad-only"
#   ST-03c CONTROL: a survivor a narrow suite also loads stays "survives"
#   ST-04  a live setting stays live whoever loads it; an unloaded one untested
#   ST-05  a removal asked for and never performed is "unmeasurable"
#   ST-06  --broad moves the line: at a higher threshold the same unit is not broad
#   ST-07  re-probe verdicts: noticed -> live; performed and unnoticed ->
#          survives; performed nowhere -> unmeasurable; loaded nowhere -> untested
#   ST-08  --settings parses a comma list and an @FILE with comments, in order,
#          without duplicates
#
# Kill table (each mutant run 2026-11-13; each turns the arm red):
#   M1 pick_helpers prefers the ordinary build ..................... ST-01
#   M2 carries_hook always True ..................................... ST-01c ST-02
#   M3 classify never returns broad-only ............................ ST-03
#   M4 classify: broad-only when ANY loader is broad ................ ST-03c
#   M5 classify: unmeasurable check dropped ......................... ST-05
#   M6 broad_units ignores the threshold (always 100) ............... ST-06
#   M7 reprobe_line: a drop performed nowhere reads "survives" ...... ST-07
#   M8 parse_settings keeps duplicates .............................. ST-08
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$SCRIPT_DIR/../../tools/testrunner"

echo "=== setting_drop.py: helper selection, broad loaders, re-probe verdicts ==="

W="$(mktemp -d "${TMPDIR:-/tmp}/naab-sdtool.XXXXXX")" || exit 1
[ -n "$W" ] && [ -d "$W" ] || { echo "FATAL: no work dir" >&2; exit 1; }
trap 'rm -rf "$W"' EXIT

# The tool is fed BYTES and directories the suite made; nothing here depends on
# a build. Python does the asserting and prints one PASS/FAIL line per arm.
OUT=$(ST_WORK="$W" ST_TOOLS="$TOOLS" python3 -B - <<'PY' 2>&1
import os, sys
sys.path.insert(0, os.environ["ST_TOOLS"])
W = os.environ["ST_WORK"]
try:
    import setting_drop as sd
except Exception as e:                      # a broken import must not read as a pass
    print("  FAIL [ST-00] the tool does not import: %r" % (e,))
    sys.exit(0)

def arm(name, cond, what, detail=""):
    print("  %s [%s] %s%s" % ("PASS" if cond else "FAIL", name, what,
                              ("\n       -> " + detail) if (detail and not cond) else ""))

# ---- helpers -----------------------------------------------------------------
mut, build = os.path.join(W, "mut"), os.path.join(W, "build")
os.makedirs(mut); os.makedirs(build)
def put(d, name, hook):
    with open(os.path.join(d, name), "wb") as f:
        f.write(b"\x7fELF...." + (sd.HOOK_MARK if hook else b"plain") + b"....")
put(mut, "naab-gov", True);  put(build, "naab-gov", False)     # both: test build wins
put(build, "naab-lsp", False)                                    # ordinary only
put(mut, "naab-verify-audit", False)                             # in mut dir, no hook
h = sd.pick_helpers(mut, build, ("naab-gov", "naab-lsp", "naab-verify-audit", "absent"))
d = sd.describe_helpers(h)
arm("ST-01", d.get("naab-gov") == ["test build", True] and h["naab-gov"][0].startswith(mut),
    "a helper built in the test build's directory is taken from there, with the hook", repr(d.get("naab-gov")))
arm("ST-01c", d.get("naab-lsp") == ["ordinary build", False] and "absent" not in d,
    "CONTROL: a helper only in the ordinary build comes from it, NO hook; one built nowhere is left out", repr(d))
arm("ST-02", d.get("naab-verify-audit") == ["test build", False],
    "a helper in the test build's directory without the hook's name is reported NO hook", repr(d.get("naab-verify-audit")))

# ---- classification ------------------------------------------------------------
tmpl = {"k%d" % i for i in range(150)} | {"a.live", "a.narrow", "a.broad", "a.none", "a.undone", "a.rationale"}
big = sorted("k%d" % i for i in range(120)) + ["a.broad", "a.narrow", "a.live", "a.undone"]
m = {"units": {
    "tests/big.sh":    {"stable": True,  "loaded": big},
    "tests/narrow.sh": {"stable": True,  "loaded": ["a.narrow", "a.live"]},
    "tests/flaky.sh":  {"stable": False, "loaded": ["a.none"]},     # unstable: not a loader
}}
p = {"live": {"a.live": "tests/narrow.sh"}, "asked_not_performed": ["a.undone"],
     "dropped_values": {"a.broad": ["1"], "a.narrow": ["1"]}}
rows, broad = sd.classify(m, p, tmpl, 100)
st = {r["setting"]: r["status"] for r in rows}
arm("ST-03", st.get("a.broad") == "broad-only" and broad == {"tests/big.sh"},
    "a survivor only a broad loader loads is broad-only", "%r broad=%r" % (st.get("a.broad"), broad))
arm("ST-03c", st.get("a.narrow") == "survives",
    "CONTROL: a survivor a narrow suite also loads stays survives", repr(st.get("a.narrow")))
arm("ST-04", st.get("a.live") == "live" and st.get("a.none") == "untested" and st.get("a.rationale") == "documentation",
    "live stays live, a setting only an unstable unit loads is untested, a rationale is documentation",
    "%r %r %r" % (st.get("a.live"), st.get("a.none"), st.get("a.rationale")))
arm("ST-05", st.get("a.undone") == "unmeasurable",
    "a removal asked for and never performed is unmeasurable", repr(st.get("a.undone")))
rows2, broad2 = sd.classify(m, p, tmpl, 500)
st2 = {r["setting"]: r["status"] for r in rows2}
arm("ST-06", broad2 == set() and st2.get("a.broad") == "survives",
    "--broad moves the line: at 500 the same unit is not broad", "%r %r" % (broad2, st2.get("a.broad")))

# ---- re-probe verdicts -----------------------------------------------------------
def line(n, u, x, nowhere=False):
    return sd.reprobe_line("s", {"noticed_by": n, "not_noticed_by": u, "not_performed_in": x}, nowhere).split()[0]
got = (line(["a"], ["b"], []), line([], ["b"], ["c"]), line([], [], ["c"]), line([], [], [], True))
arm("ST-07", got == ("live", "survives", "unmeasurable", "untested"),
    "re-probe verdicts: live / survives / unmeasurable / untested", repr(got))

# ---- --settings -------------------------------------------------------------------
f = os.path.join(W, "keys.txt")
with open(f, "w") as fh:
    fh.write("# a comment\nx.y\n\nz.w  # trailing\nx.y\n")
got = (sd.parse_settings("b.c, a.b,b.c,"), sd.parse_settings("@" + f))
arm("ST-08", got == (["b.c", "a.b"], ["x.y", "z.w"]),
    "--settings reads a comma list and an @FILE, in order, without duplicates", repr(got))
PY
)
printf '%s\n' "$OUT"

PASS=$(grep -c '^  PASS \[' <<<"$OUT")
FAIL=$(grep -c '^  FAIL \[' <<<"$OUT")
EXPECTED=10
if [ "$PASS" -ne "$EXPECTED" ] && [ "$FAIL" -eq 0 ]; then
    # Fewer arms than written means the driver died part-way: not a pass.
    echo "  FAIL [ST-99] $PASS of $EXPECTED arms reported (the driver stopped early)"
    FAIL=1
fi
echo ""
echo "Results: $PASS passed, $FAIL failed, 0 skipped"
[ "$FAIL" -eq 0 ]
