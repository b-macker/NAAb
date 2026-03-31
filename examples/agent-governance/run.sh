#!/bin/bash
# Multi-agent governance demo
# Shows how govern.json enforces per-agent rules and writes telemetry
cd "$(dirname "$0")"

# Setup test data
mkdir -p data config output secrets
printf "name,value\nalpha,1\nbeta,2\ngamma,3" > data/input.csv
echo "timeout=30" > config/settings.txt
echo "TOP_SECRET" > secrets/key.txt
rm -f telemetry.jsonl

echo "=== data-bot processes data (allowed) ==="
naab-lang --agent-id data-bot --governance-dashboard data_task.naab

echo ""
echo "=== ops-bot reads config (allowed) ==="
naab-lang --agent-id ops-bot --governance-dashboard ops_task.naab

echo ""
echo "=== data-bot tries to read secrets (BLOCKED) ==="
naab-lang --agent-id data-bot --governance-dashboard -e '
use file
main { print(file.read("./secrets/key.txt")) }
' 2>&1 || true

echo ""
echo "=== Telemetry report ==="
TELEMETRY_FILE=telemetry.jsonl naab-lang ../../tools/agent-governance/dashboard_cli.naab
