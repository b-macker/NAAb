#!/usr/bin/env bash
# test_release_publisher.sh -- one job publishes a release, last, with everything
#
# A180: on a v* tag, supply-chain.yml's "Attach SBOM to release" step created
# and PUBLISHED the release seconds after the push, and release.yml added the
# notes and the four packages only when its builds passed. v2.0.2's page was
# public for 26 minutes with neither, and v2.0.0's build failed and left a
# published page with no notes and no packages. Now release.yml builds, signs
# and attests everything, and its "Create GitHub Release" job publishes once,
# after every other job. This pins that shape by reading the workflow files:
#
#   RL-01  exactly one step in .github/workflows creates or edits a release:
#          release.yml, job create-release
#   RL-02  create-release needs every other job of release.yml
#   RL-03  every file it publishes is uploaded, at the artifact root, by a job
#          of release.yml, and it downloads every artifact
#   RL-04  the published files are the 17 v2.0.2 shipped (observed through
#          GitHub's API): the four packages and their checksums, the signed
#          binary with its checksums, signature and certificate, the SBOM in
#          three forms, the provenance file
#   RL-05  only create-release may write repository contents, and every job of
#          every workflow declares its token permissions (an unset block gets
#          the repository default, which can be write)
#   RL-06  in release.yml only the signing job gets an OIDC token (keyless
#          cosign needs one; nothing else does)
#   RL-C1..C5  CONTROLS: each check above, run on a planted copy of the
#          workflows, reports the planted fault
#
# Vocabulary (screened): a step counts as publishing when it `uses:` one of
# the release actions named in RELEASE_USES, or its `run:` calls `gh release
# create|upload|edit|delete` or a `/repos/.../releases` API path. A release
# made some other way is not seen -- the list bounds what this can find.
# Not checked: that a run of the workflow does what the file says. The first
# real run is the next tag.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
WF="${RL_WORKFLOWS:-$REPO/.github/workflows}"
PASS=0; FAIL=0; UNM=0
pass() { PASS=$((PASS+1)); echo "  PASS [$1] $2"; }
fail() { FAIL=$((FAIL+1)); echo "  FAIL [$1] $2"; [ -n "${3:-}" ] && printf '%s\n' "$3" | head -12 | sed 's/^/       -> /'; }
unm()  { UNM=$((UNM+1));   echo "  UNMEASURABLE [$1] $2"; }

echo "=== one job publishes a release, last, with everything (A180) ==="
if ! python3 -c 'import yaml' 2>/dev/null; then
    unm RL-00 "python3 with PyYAML is needed to read the workflow files -- UNMEASURABLE, not a pass"
    echo "=== Results: 0 passed, 0 failed, 1 unmeasurable ==="; exit 1
fi
W=$(mktemp -d "${TMPDIR:-/tmp}/naab_rl_XXXXXX"); trap 'rm -rf "$W"' EXIT

cat > "$W/check.py" <<'PY'
# Prints one line per fact the suite asserts on. Reads files only.
import fnmatch, glob, os, re, sys
import yaml

RELEASE_USES = ("softprops/action-gh-release", "ncipollo/release-action",
                "actions/create-release", "actions/upload-release-asset",
                "svenstaro/upload-release-action")
RUN_PAT = re.compile(r"\bgh\s+release\s+(create|upload|edit|delete)\b|/repos/\S*/releases\b")

def load(path):
    with open(path, encoding="utf-8") as fh:
        return yaml.safe_load(fh)

def norm(p):
    if p is None:
        return None
    if isinstance(p, str):          # read-all / write-all
        return {"*": p}
    return dict(p)

wf = sys.argv[1]
out = []
for path in sorted(glob.glob(os.path.join(wf, "*.yml")) + glob.glob(os.path.join(wf, "*.yaml"))):
    name = os.path.basename(path)
    d = load(path) or {}
    top = norm(d.get("permissions"))
    for jn, j in (d.get("jobs") or {}).items():
        j = j or {}
        jp = norm(j["permissions"]) if "permissions" in j else top
        if jp is None:
            out.append(f"UNSET {name}:{jn}")
        else:
            if jp.get("*") == "write-all" or jp.get("contents") == "write":
                out.append(f"WRITE {name}:{jn}")
            if jp.get("*") == "write-all" or jp.get("id-token") == "write":
                out.append(f"IDTOKEN {name}:{jn}")
        for i, s in enumerate(j.get("steps") or []):
            uses = str(s.get("uses") or "")
            run = str(s.get("run") or "")
            if uses.startswith(RELEASE_USES) or RUN_PAT.search(run):
                out.append(f"PUBLISHER {name}:{jn}:{s.get('name', i)}")

rel_path = os.path.join(wf, "release.yml")
rel = load(rel_path) if os.path.exists(rel_path) else {}
jobs = rel.get("jobs") or {}
cr = jobs.get("create-release")
if cr is None:
    out.append("NO-CREATE-RELEASE")
else:
    needs = cr.get("needs") or []
    needs = [needs] if isinstance(needs, str) else list(needs)
    for jn in jobs:
        if jn != "create-release" and jn not in needs:
            out.append(f"NEEDS-MISSING {jn}")
    uploaded = []
    for jn, j in jobs.items():
        for s in (j or {}).get("steps") or []:
            if str(s.get("uses") or "").startswith("actions/upload-artifact"):
                paths = [p.strip() for p in str((s.get("with") or {}).get("path", "")).split("\n") if p.strip()]
                if len({os.path.dirname(p) for p in paths}) > 1:
                    out.append(f"UPLOAD-NESTED {jn}")      # would not land at the download root
                uploaded += [os.path.basename(p) for p in paths]
    steps = cr.get("steps") or []
    pub = [s for s in steps if str(s.get("uses") or "").startswith("softprops/action-gh-release")]
    dl = [s for s in steps if str(s.get("uses") or "").startswith("actions/download-artifact")]
    if not any(str((s.get("with") or {}).get("merge-multiple", "")).lower() == "true"
               and not (s.get("with") or {}).get("name") and not (s.get("with") or {}).get("pattern")
               for s in dl):
        out.append("DOWNLOAD-PARTIAL")
    names = []
    for s in pub:
        names += [x.strip() for x in str((s.get("with") or {}).get("files", "")).split("\n") if x.strip()]
    for n in names:
        if not any(fnmatch.fnmatch(n, u) for u in uploaded):
            out.append(f"UNBUILT {n}")
    out.append("ASSETS " + " ".join(sorted(names)))
print("\n".join(out))
PY

check() { python3 "$W/check.py" "$1" 2>&1; }
OUT=$(check "$WF"); RC=$?
if [ "$RC" -ne 0 ]; then
    fail RL-00 "the checker did not run" "$OUT"
    echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="; exit 1
fi

# --- the subject ---------------------------------------------------------
PUB=$(grep '^PUBLISHER ' <<<"$OUT" || true)
if [ "$(grep -c . <<<"$PUB")" -eq 1 ] && grep -q '^PUBLISHER release.yml:create-release:' <<<"$PUB"; then
    pass RL-01 "one step publishes: $(sed 's/^PUBLISHER //' <<<"$PUB")"
else
    fail RL-01 "release-publishing steps are not exactly release.yml's create-release" "${PUB:-(none found)}"
fi

BAD=$(grep -E '^(NEEDS-MISSING|NO-CREATE-RELEASE)' <<<"$OUT" || true)
if [ -z "$BAD" ]; then pass RL-02 "create-release needs every other job of release.yml"
else fail RL-02 "create-release can run before a job it should wait for" "$BAD"; fi

BAD=$(grep -E '^(UNBUILT|UPLOAD-NESTED|DOWNLOAD-PARTIAL)' <<<"$OUT" || true)
if [ -z "$BAD" ]; then pass RL-03 "every published file is uploaded at an artifact root, and every artifact is downloaded"
else fail RL-03 "a published file is not built, or would not be found" "$BAD"; fi

EXPECTED="naab-lang naab-lang.pem naab-lang.sha256 naab-lang.sha512 naab-lang.sig naab-linux-aarch64.tar.gz naab-linux-aarch64.tar.gz.sha256 naab-linux-x86_64.tar.gz naab-linux-x86_64.tar.gz.sha256 naab-macos-arm64.tar.gz naab-macos-arm64.tar.gz.sha256 naab-sbom.cdx.json naab-sbom.spdx.json naab-sbom.txt naab-windows-x86_64.zip naab-windows-x86_64.zip.sha256 provenance.json"
GOT=$(sed -n 's/^ASSETS //p' <<<"$OUT")
if [ "$GOT" = "$EXPECTED" ]; then pass RL-04 "the 17 files v2.0.2 shipped are published"
else fail RL-04 "the published files differ from v2.0.2's" "$(diff <(tr ' ' '\n' <<<"$EXPECTED") <(tr ' ' '\n' <<<"$GOT"))"; fi

WRITE=$(grep '^WRITE ' <<<"$OUT" || true); UNSET=$(grep '^UNSET ' <<<"$OUT" || true)
if [ "$WRITE" = "WRITE release.yml:create-release" ] && [ -z "$UNSET" ]; then
    pass RL-05 "only create-release may write contents, and every job declares its permissions"
else
    fail RL-05 "another job may write contents, or a job's permissions are unset" "${WRITE}${WRITE:+$'\n'}${UNSET}"
fi

IDT=$(grep '^IDTOKEN release.yml:' <<<"$OUT" || true)
if [ "$IDT" = "IDTOKEN release.yml:sign" ]; then pass RL-06 "in release.yml only the signing job gets an OIDC token"
else fail RL-06 "release.yml's OIDC token is not limited to the signing job" "${IDT:-(no job has one)}"; fi

# --- controls: each check reports a planted fault ---------------------------
plant() {   # plant NAME PYTHON-SNIPPET -> a copy of the workflows with the fault
    local d="$W/$1"; rm -rf "$d"; cp -r "$WF" "$d"
    python3 - "$d" "$2" <<'PY' || return 1
import sys, yaml
d, snippet = sys.argv[1], sys.argv[2]
def load(n):
    with open(f"{d}/{n}", encoding="utf-8") as fh: return yaml.safe_load(fh)
def save(n, y):
    with open(f"{d}/{n}", "w", encoding="utf-8") as fh: yaml.safe_dump(y, fh, sort_keys=False)
exec(snippet)
PY
    echo "$d"
}
control() {  # control ID LINE-REGEX DESCRIPTION NAME SNIPPET
    local d out
    if ! d=$(plant "$4" "$5"); then fail "$1" "could not plant: $3"; return; fi
    out=$(check "$d")
    if grep -qE "$2" <<<"$out"; then pass "$1" "CONTROL: $3 is reported"
    else fail "$1" "CONTROL: $3 is not reported" "$out"; fi
}
control RL-C1 '^PUBLISHER supply-chain.yml:' "a release action in supply-chain.yml" c1 '
y = load("supply-chain.yml"); j = next(iter(y["jobs"].values()))
j["steps"].append({"name": "planted", "uses": "softprops/action-gh-release@v2", "with": {"files": "x"}})
save("supply-chain.yml", y)'
control RL-C2 '^PUBLISHER ci.yml:' "a gh release upload in a run step of ci.yml" c2 '
y = load("ci.yml"); j = next(iter(y["jobs"].values()))
j["steps"].append({"name": "planted", "run": "gh release upload v9.9.9 build/x"})
save("ci.yml", y)'
control RL-C3 '^NEEDS-MISSING sign$' "create-release not waiting for the signing job" c3 '
y = load("release.yml"); y["jobs"]["create-release"]["needs"] = [n for n in y["jobs"]["create-release"]["needs"] if n != "sign"]
save("release.yml", y)'
control RL-C4 '^UNBUILT planted.txt$' "a published file nothing uploads" c4 '
y = load("release.yml")
for s in y["jobs"]["create-release"]["steps"]:
    if str(s.get("uses","")).startswith("softprops/action-gh-release"): s["with"]["files"] += "planted.txt\n"
save("release.yml", y)'
control RL-C5 '^(UNSET windows.yml:|WRITE ci.yml:)' "a workflow with no permissions block, or a job that may write" c5 '
y = load("windows.yml"); y.pop("permissions", None); save("windows.yml", y)
y = load("ci.yml"); y["permissions"] = {"contents": "write"}; save("ci.yml", y)'

echo ""
echo "=== Results: $PASS passed, $FAIL failed, $UNM unmeasurable ==="
[ "$FAIL" -eq 0 ] && [ "$UNM" -eq 0 ]
