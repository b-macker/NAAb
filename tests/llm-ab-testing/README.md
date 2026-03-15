# LLM A/B Testing Framework — Governance Impact on Code Quality

Measures how governance rules in CLAUDE.md + govern.json affect LLM-generated NAAb code quality.

## How It Works

1. Give the **same task** to an LLM twice:
   - **Config A (No Governance)**: Minimal CLAUDE.md (syntax only) + empty govern.json
   - **Config B (With Governance)**: Full CLAUDE.md (contracts, scanner, complexity rules) + full govern.json
2. Run both outputs through the NAAb scanner and governance engine
3. Compare quality metrics side-by-side

## Quick Start

### 1. Pick a Task
```bash
cat tasks/task1-simple-calculator.txt
```

### 2. Generate Code (Config A — No Governance)
Copy `configs/no-governance/CLAUDE.md` and `configs/no-governance/govern.json` into the LLM's context, then give it the task spec. Save the output:
```bash
mkdir -p results/task1-simple-calculator/no-governance
# Save LLM output as:
# results/task1-simple-calculator/no-governance/generated.naab
```

### 3. Generate Code (Config B — With Governance)
Copy `configs/with-governance/CLAUDE.md` and `configs/with-governance/govern.json` into the LLM's context, then give it the **same** task spec. Save the output:
```bash
mkdir -p results/task1-simple-calculator/with-governance
# Save LLM output as:
# results/task1-simple-calculator/with-governance/generated.naab
```

### 4. Analyze Both Outputs
```bash
bash scripts/analyze.sh task1-simple-calculator
```

### 5. Compare Results
```bash
bash scripts/compare.sh task1-simple-calculator
```

### 6. Aggregate (After Multiple Tasks)
```bash
bash scripts/aggregate.sh
```

## Directory Structure

```
tests/llm-ab-testing/
├── configs/
│   ├── no-governance/
│   │   ├── CLAUDE.md          # Syntax-only reference
│   │   └── govern.json        # Empty {}
│   └── with-governance/
│       ├── CLAUDE.md          # Full template with governance
│       └── govern.json        # Full governance rules
├── tasks/
│   ├── task1-simple-calculator.txt
│   ├── task2-arena-combat.txt
│   └── task3-data-pipeline.txt
├── results/
│   └── <task-name>/
│       ├── no-governance/
│       │   ├── generated.naab
│       │   ├── quality-report.json
│       │   ├── governance-report.json
│       │   └── run-output.txt
│       ├── with-governance/
│       │   └── (same files)
│       └── comparison.json
├── scripts/
│   ├── analyze.sh             # Run scanner + governance on both configs
│   ├── compare.sh             # Side-by-side comparison report
│   └── aggregate.sh           # Multi-task aggregate stats
└── README.md
```

## Metrics Compared

| Metric | Source | Why It Matters |
|--------|--------|----------------|
| HARD violations | Scanner | Blocks execution — critical code quality issues |
| SOFT violations | Scanner | Reportable issues — patterns that should improve |
| Advisory notices | Scanner | Style suggestions |
| Governance violations | Governance engine | Contract, complexity, security rule breaches |
| Lines of code | wc -l | Governance may produce more thorough code |
| Test pass/fail | Runtime | Does the code actually work? |

## Expected Results

With governance rules active, LLM-generated code should have:
- Fewer scanner violations (especially HARD: no placeholders, no empty catch)
- Zero governance violations (contracts satisfied, complexity floors met)
- More lines of code (proper error handling, validation, contracts)
- Higher test pass rate (edge cases handled)

## Adding New Tasks

1. Create `tasks/taskN-description.txt` with requirements
2. Generate code with both configs
3. Save to `results/taskN-description/{no,with}-governance/generated.naab`
4. Run `bash scripts/analyze.sh taskN-description`
5. Run `bash scripts/compare.sh taskN-description`

## Testing Different LLMs

Run the same tasks with different LLMs to compare governance adherence:
- Create separate result directories: `results/task1-claude/`, `results/task1-gemini/`
- Or add an LLM suffix: `results/task1-simple-calculator-claude/`
