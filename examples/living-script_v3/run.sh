#!/usr/bin/env bash
# ============================================================
# living-script_v3 — run the scenario and gate the result.
#
#   ./run.sh              keyless: drive the scenario against the routed stub
#   ./run.sh --keyed      live: requires GK1 (GK2..GK6 optional, for rotation)
#
# The keyless mode is not a degraded stand-in for the keyed one. It is the
# thing that makes a keyed run worth paying for: if the ladder cannot be walked
# against a scripted stub, it will not be walked against a live model, and no
# keyed run should be spent discovering that.
#
# Keyless mode writes summary_keyless.json. It never touches summary.json --
# a keyless run overwriting that file already destroyed one keyed record
# permanently, and the record is the only thing a keyed run leaves behind.
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR/../.."
NAAB="$REPO/build/naab-lang"

MODE=keyless
ARM=code
for _a in "$@"; do
    case "$_a" in
        --keyed) MODE=keyed ;;
        --self-test) MODE=selftest ;;
        # --prose swaps the TASK and nothing else: a different .naab plus an
        # overlay carrying only the system prompts. It is the C1d modality
        # control, so every threshold, signal toggle, budget and window must stay
        # byte-identical between the arms -- which is why the overlay is a small
        # patch applied to govern.json rather than a second config file that
        # could drift.
        --prose) ARM=prose ;;
        "") ;;
        *) echo "usage: run.sh [--keyed|--self-test] [--prose]" >&2; exit 2 ;;
    esac
done
SCRIPT_NAAB="living-script.naab"
[ "$ARM" = prose ] && SCRIPT_NAAB="living-script-prose.naab"

source "$REPO/tests/helpers/gatelib.sh"

# --- self-test: no binary, no stub, no network, no API key -----------------
# Every gate is run twice, against its own pass/ and fail/ fixture, and must
# produce exactly one PASS then exactly one FAIL. A gate with no fail/ fixture
# is itself a failure. This is the enforcement of "no gate without a proven
# failure case" -- structural rather than a thing someone remembers to do --
# and it is what makes the expensive keyed run worth paying for.
if [ "$MODE" = selftest ]; then
    FIX="$SCRIPT_DIR/gates/fixtures"
    gate_init "living-script_v3-selftest"
    source "$SCRIPT_DIR/gates/registry.sh"
    IDS=("${GATE_IDS[@]}")
    BAD=0
    echo "=== living-script_v3 gate self-test (${#IDS[@]} gates) ==="
    for _id in "${IDS[@]}"; do
        for _which in pass fail; do
            if [ ! -f "$FIX/$_id/$_which/stdout.txt" ]; then
                echo "  FAIL [$_id] no $_which/ fixture — a gate without a proven failure case"
                BAD=$((BAD + 1)); continue
            fi
            gate_reset_counters
            export G_OUT="$FIX/$_id/$_which/stdout.txt"
            export G_TELE="$FIX/$_id/$_which/telemetry.jsonl"
            GATE_QUIET=1 gate_run "$_id"
            if [ "$_which" = pass ]; then
                [ "$PASS_COUNT" -eq 1 ] || {
                    echo "  FAIL [$_id] pass/ fixture: expected 1 PASS, got p=$PASS_COUNT f=$FAIL_COUNT s=$SKIP_COUNT"
                    BAD=$((BAD + 1)); }
            else
                [ "$FAIL_COUNT" -eq 1 ] || {
                    echo "  FAIL [$_id] fail/ fixture: NOT FAILABLE — p=$PASS_COUNT f=$FAIL_COUNT s=$SKIP_COUNT"
                    BAD=$((BAD + 1)); }
            fi
        done
    done
    if [ "$BAD" -eq 0 ]; then
        echo "  all ${#IDS[@]} gates pass their own fixture and fail their negative fixture"
        exit 0
    fi
    echo "  $BAD gate/fixture pairs did not behave"
    exit 1
fi

if [ ! -x "$NAAB" ]; then
    echo "  living-script_v3: SKIPPED — build/naab-lang not found" >&2
    exit 0
fi

source "$REPO/tests/helpers/stub_platform.sh"
[ "$MODE" = keyless ] && skip_if_no_stub_support "living-script_v3"
source "$REPO/tests/helpers/stub_launch.sh"
source "$REPO/tests/helpers/trust_setup.sh"
setup_isolated_trust

if [ -d "/data/data/com.termux/files/usr/tmp" ]; then
    _SYSTMP="${TMPDIR:-/data/data/com.termux/files/usr/tmp}"
else
    _SYSTMP="${TMPDIR:-/tmp}"
fi
WDIR="${_SYSTMP}/ls-v3-$$"
RESULTS="$SCRIPT_DIR/results"
cleanup() {
    stop_stub
    teardown_isolated_trust
    [ -n "${KEEP_TMP:-}" ] && { echo "Artifacts kept: $WDIR"; return; }
    rm -rf "$WDIR"
}
trap cleanup EXIT
mkdir -p "$WDIR" "$RESULTS"

"$NAAB" --keygen "$WDIR/key.pem" >/dev/null 2>&1
"$NAAB" --trust-key "$WDIR/key.pem.pub" >/dev/null 2>&1
export NAAB_SIGNING_KEY="$WDIR/key.pem"

cp "$SCRIPT_DIR/src/$SCRIPT_NAAB" "$WDIR/"
cp "$SCRIPT_DIR/src/govern.json" "$WDIR/govern.json"
if [ "$ARM" = prose ]; then
    # Merge system prompts only. Any key the overlay does not name keeps the
    # code arm's value by construction, so the two arms cannot differ in
    # thresholds however either file is later edited.
    python3 - "$WDIR/govern.json" "$SCRIPT_DIR/src/prose_overlay.json" <<'PYOV'
import json, sys
cfg_path, ov_path = sys.argv[1], sys.argv[2]
cfg = json.load(open(cfg_path))
ov = json.load(open(ov_path))
changed = []
for name, fields in ov.get("agents", {}).items():
    if name not in cfg["agents"]:
        raise SystemExit("prose overlay names unknown agent %r" % name)
    for k, v in fields.items():
        cfg["agents"][name][k] = v
        changed.append("%s.%s" % (name, k))
cfg.setdefault("meta", {})["arm"] = "prose"
json.dump(cfg, open(cfg_path, "w"), indent=2)
print("  prose overlay applied to: %s" % ", ".join(changed))
PYOV
    [ $? -eq 0 ] || { echo "prose overlay failed" >&2; exit 1; }
fi

if [ "$MODE" = keyless ]; then
    # The engine still requires the configured key env to exist, even though
    # the stub never validates it. GK1 is the first entry of the rotation the
    # committed config uses, matching living-script v1/v2 so a keyed run needs
    # no new environment setup.
    export GK1="stub-key-not-a-real-credential"
    # V3_FIXTURE overrides the generated response stream. Used by
    # probe_modality.sh to run this scenario twice with everything identical
    # except the responses themselves -- the only way to isolate the two signals
    # behind C1d, since semantic_stability and entity_consistency read the
    # response stream and nothing else.
    if [ -n "${V3_FIXTURE:-}" ]; then
        cp "$V3_FIXTURE" "$WDIR/fixture.json" || exit 1
    else
        python3 "$SCRIPT_DIR/gen_fixture.py" "$WDIR/fixture.json" "$ARM" || exit 1
    fi
    start_stub "$WDIR/fixture.json" "$WDIR" || { echo "stub failed to start" >&2; exit 1; }
    # api_base is injected rather than committed: the port is chosen at launch,
    # and a committed loopback URL would be a live-run footgun.
    python3 - "$WDIR/govern.json" "$STUB_PORT" <<'PY'
import json, sys
p, port = sys.argv[1], sys.argv[2]
d = json.load(open(p))
for a in d["agents"].values():
    a["api_base"] = "http://127.0.0.1:%s" % port
json.dump(d, open(p, "w"), indent=2)
PY
else
    # Inter-call spacing is injected for keyed runs only. v1 and v2 both carry
    # delay_between_calls_ms 1000 against the same provider; without it a live
    # run invites rate-limit errors, which CDD would then have to be told to
    # treat as infrastructure rather than drift. It is NOT committed to the
    # config because it would add ~40s to every keyless CI run for no benefit --
    # the stub has no rate limit.
    python3 - "$WDIR/govern.json" <<'PYX'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
for a in d["agents"].values():
    a["rate_limit"] = {"requests_per_minute": 0, "delay_between_calls_ms": 1000}
json.dump(d, open(p, "w"), indent=2)
PYX
    if [ -z "${GK1:-}" ]; then
        echo "  --keyed requires GK1 (and optionally GK2..GK6 for rotation)" >&2
        echo "  These are the same key envs living-script v1 and v2 use." >&2
        exit 2
    fi
fi

(cd "$WDIR" && NAAB_SIGNING_KEY="$NAAB_SIGNING_KEY" "$NAAB" --sign-governance >/dev/null 2>&1)

echo "=== living-script_v3 ($MODE, $ARM arm) ==="
# The shell `timeout` is a backstop only. The ENGINE's own script timeout is
# limits.timeout.global in govern.json, which defaults to 30s -- and that is
# what killed the first keyed run at 11 of ~39 calls, not this wrapper. Keep
# the wrapper above the engine limit so whichever fires is the configured one.
(cd "$WDIR" && timeout 960s "$NAAB" "$SCRIPT_NAAB") > "$WDIR/stdout.txt" 2> "$WDIR/stderr.txt"
RC=$?
stop_stub
[ "$RC" -eq 3 ] && GOV_KILL="engine exited 3 (governance block)"

export G_OUT="$WDIR/stdout.txt"
export G_TELE="$WDIR/telemetry.jsonl"
[ -f "$G_TELE" ] || G_TELE="$WDIR/telemetry.jsonl"

TS=$(date +%Y%m%d_%H%M%S)
cp "$WDIR/stdout.txt"  "$RESULTS/stdout_${MODE}_${TS}.txt"  2>/dev/null
cp "$WDIR/stderr.txt"  "$RESULTS/stderr_${MODE}_${TS}.txt"  2>/dev/null
cp "$G_TELE"           "$RESULTS/telemetry_${MODE}_${TS}.jsonl" 2>/dev/null

gate_init "living-script_v3"
source "$SCRIPT_DIR/gates/registry.sh"
for _g in "${GATE_IDS[@]}"; do gate_run "$_g"; done

gate_print_summary
gate_summary_json "$RESULTS/summary.json" "$MODE"
echo "  artifacts: $RESULTS/*_${MODE}_${TS}.*"
gate_exit
