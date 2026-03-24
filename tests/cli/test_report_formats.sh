#!/bin/bash
# Integration tests for governance report formats (SARIF, JUnit, JSON, CSV, HTML)
# Validates that report generation produces well-formed, complete output

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NAAB_BIN="${SCRIPT_DIR}/../../build/naab-lang"
PASS=0
FAIL=0
TEST_DIR="${HOME}/.naab_report_test_$$"
mkdir -p "$TEST_DIR"

cleanup() { rm -rf "$TEST_DIR"; }
trap cleanup EXIT

check() {
    local desc="$1"
    local condition="$2"
    if eval "$condition"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== NAAb Report Format Tests ==="
echo ""

# --- Generate reports from a file with governance violations ---
VIOLATION_FILE="${SCRIPT_DIR}/../../tests/governance_v3/test_v3_secret_detection.naab"
SARIF_OUT="$TEST_DIR/test.sarif"
JUNIT_OUT="$TEST_DIR/test.xml"
JSON_OUT="$TEST_DIR/test.json"

echo "--- Generating reports from violation file ---"
"$NAAB_BIN" run "$VIOLATION_FILE" \
    --governance-sarif "$SARIF_OUT" \
    --governance-junit "$JUNIT_OUT" \
    --governance-report "$JSON_OUT" \
    > "$TEST_DIR/stdout.txt" 2>&1 || true

# ============================================================
# SARIF Tests
# ============================================================
echo ""
echo "--- SARIF Report Tests ---"

check "SARIF file created" "[ -s '$SARIF_OUT' ]"

check "SARIF is valid JSON" \
    "python3 -c \"import json; json.load(open('$SARIF_OUT'))\" 2>/dev/null"

check "SARIF version is 2.1.0" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
assert d['version'] == '2.1.0', f'got {d[\"version\"]}'
\" 2>/dev/null"

check "SARIF has schema field" \
    "python3 -c 'import json; d=json.load(open(\"$SARIF_OUT\")); assert any(\"schema\" in k for k in d.keys()), \"missing schema\"' 2>/dev/null"

check "SARIF has tool driver name" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
name = d['runs'][0]['tool']['driver']['name']
assert 'NAAb' in name or 'naab' in name, f'got {name}'
\" 2>/dev/null"

check "SARIF has tool driver version" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
v = d['runs'][0]['tool']['driver']['version']
assert v, 'empty version'
\" 2>/dev/null"

check "SARIF has rules array" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
rules = d['runs'][0]['tool']['driver']['rules']
assert isinstance(rules, list) and len(rules) > 0, f'rules count: {len(rules)}'
\" 2>/dev/null"

check "SARIF has results with ruleId" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
results = [r for r in d['runs'][0]['results']]
assert len(results) > 0, 'no results'
assert all('ruleId' in r for r in results), 'missing ruleId'
\" 2>/dev/null"

check "SARIF results have level (error/warning)" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
results = d['runs'][0]['results']
for r in results:
    assert r.get('level') in ('error', 'warning', 'note'), f'bad level: {r.get(\"level\")}'
\" 2>/dev/null"

check "SARIF results have message.text" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
results = d['runs'][0]['results']
for r in results:
    assert r.get('message', {}).get('text'), 'empty message'
\" 2>/dev/null"

check "SARIF has invocations" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
inv = d['runs'][0]['invocations']
assert isinstance(inv, list) and len(inv) > 0
assert 'executionSuccessful' in inv[0]
\" 2>/dev/null"

check "SARIF rules have ruleIndex references" \
    "python3 -c \"
import json
d = json.load(open('$SARIF_OUT'))
rules = d['runs'][0]['tool']['driver']['rules']
results = d['runs'][0]['results']
rule_ids = [r['id'] for r in rules]
for r in results:
    assert r['ruleId'] in rule_ids, f'{r[\"ruleId\"]} not in rules'
\" 2>/dev/null"

# ============================================================
# JUnit Tests
# ============================================================
echo ""
echo "--- JUnit Report Tests ---"

check "JUnit file created" "[ -s '$JUNIT_OUT' ]"

check "JUnit is well-formed XML" \
    "python3 -c \"
import xml.etree.ElementTree as ET
ET.parse('$JUNIT_OUT')
\" 2>/dev/null"

check "JUnit has testsuite root" \
    "python3 -c \"
import xml.etree.ElementTree as ET
tree = ET.parse('$JUNIT_OUT')
root = tree.getroot()
assert root.tag == 'testsuite', f'got {root.tag}'
\" 2>/dev/null"

check "JUnit testsuite has tests attribute" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
assert root.get('tests') is not None, 'missing tests attr'
assert int(root.get('tests')) > 0, 'tests=0'
\" 2>/dev/null"

check "JUnit testsuite has failures attribute" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
assert root.get('failures') is not None, 'missing failures attr'
\" 2>/dev/null"

check "JUnit testcases have name attribute" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
tcs = root.findall('testcase')
assert len(tcs) > 0, 'no testcases'
for tc in tcs:
    assert tc.get('name'), f'missing name: {tc.attrib}'
\" 2>/dev/null"

check "JUnit testcases have classname attribute" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
tcs = root.findall('testcase')
for tc in tcs:
    cn = tc.get('classname')
    assert cn and '.' in cn, f'bad classname: {cn}'
\" 2>/dev/null"

check "JUnit failures have message and type" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
failures = root.findall('.//failure')
assert len(failures) > 0, 'no failures found'
for f in failures:
    assert f.get('message'), 'missing failure message'
    assert f.get('type'), 'missing failure type'
\" 2>/dev/null"

check "JUnit failure message contains actual error text (not just rule_name)" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
failures = root.findall('.//failure')
for f in failures:
    msg = f.get('message', '')
    # Should contain meaningful text, not just a rule identifier
    assert len(msg) > 20, f'message too short: {msg}'
\" 2>/dev/null"

check "JUnit failure body has content" \
    "python3 -c \"
import xml.etree.ElementTree as ET
root = ET.parse('$JUNIT_OUT').getroot()
failures = root.findall('.//failure')
for f in failures:
    assert f.text and len(f.text) > 5, f'empty failure body'
\" 2>/dev/null"

# ============================================================
# JSON Report Tests
# ============================================================
echo ""
echo "--- JSON Report Tests ---"

check "JSON file created" "[ -s '$JSON_OUT' ]"

check "JSON is valid" \
    "python3 -c \"import json; json.load(open('$JSON_OUT'))\" 2>/dev/null"

check "JSON has results array" \
    "python3 -c \"
import json
d = json.load(open('$JSON_OUT'))
assert 'results' in d, 'missing results key'
assert isinstance(d['results'], list), 'results not a list'
\" 2>/dev/null"

check "JSON results have category field" \
    "python3 -c \"
import json
d = json.load(open('$JSON_OUT'))
for r in d['results']:
    assert 'category' in r, f'missing category in {r.get(\"rule\")}'
\" 2>/dev/null"

check "JSON results have severity field" \
    "python3 -c \"
import json
d = json.load(open('$JSON_OUT'))
failed = [r for r in d['results'] if not r.get('passed')]
for r in failed:
    assert 'severity' in r, f'missing severity in {r.get(\"rule\")}'
\" 2>/dev/null"

check "JSON has summary stats" \
    "python3 -c \"
import json
d = json.load(open('$JSON_OUT'))
assert 'summary' in d, 'missing summary'
s = d['summary']
assert 'total' in s, 'missing total'
assert 'passed' in s, 'missing passed count'
\" 2>/dev/null"

# ============================================================
# Clean file tests (no violations)
# ============================================================
echo ""
echo "--- Clean File Tests ---"

# Create a minimal clean .naab file with its own govern.json
CLEAN_DIR="$TEST_DIR/clean_project"
mkdir -p "$CLEAN_DIR"
cat > "$CLEAN_DIR/clean.naab" << 'NAAB'
main {
    let x = 42
    print(x)
}
NAAB
cat > "$CLEAN_DIR/govern.json" << 'GOV'
{
    "version": "3.0",
    "languages": { "allowed": ["python", "shell"] },
    "capabilities": { "network": false, "filesystem": false, "shell": true },
    "output": { "format": "text" }
}
GOV

CLEAN_SARIF="$TEST_DIR/clean.sarif"
CLEAN_JUNIT="$TEST_DIR/clean.xml"
"$NAAB_BIN" run "$CLEAN_DIR/clean.naab" \
    --governance-sarif "$CLEAN_SARIF" \
    --governance-junit "$CLEAN_JUNIT" \
    > /dev/null 2>&1 || true

check "Clean file SARIF has empty results" \
    "python3 -c \"
import json, os
if not os.path.exists('$CLEAN_SARIF') or os.path.getsize('$CLEAN_SARIF') == 0:
    # No SARIF = no violations = OK
    pass
else:
    d = json.load(open('$CLEAN_SARIF'))
    results = d.get('runs', [{}])[0].get('results', [])
    assert len(results) == 0, f'expected 0 results, got {len(results)}'
\" 2>/dev/null"

check "Clean file JUnit has 0 failures" \
    "python3 -c \"
import xml.etree.ElementTree as ET, os
if not os.path.exists('$CLEAN_JUNIT') or os.path.getsize('$CLEAN_JUNIT') == 0:
    pass  # No JUnit = OK
else:
    root = ET.parse('$CLEAN_JUNIT').getroot()
    failures = int(root.get('failures', 0))
    assert failures == 0, f'expected 0 failures, got {failures}'
\" 2>/dev/null"

# ============================================================
# Summary
# ============================================================
echo ""
TOTAL=$((PASS + FAIL))
echo "=== Report Format Test Summary: $PASS/$TOTAL passed ==="
echo ""

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    exit 0
fi
