# Gorilla Test #11 — Enterprise Supply Chain Management (Strict)

## Overview
Enterprise-grade supply chain management system in NAAb. This is the **strict version** of the supply chain test — same domain as test10 but with HARD governance enforcement, exact mathematical assertions, edge case coverage, and integration testing.

## What Changed from Test #10
| Aspect | Test #10 | Test #11 |
|--------|----------|----------|
| Tests | 60 (6 suites) | **80 (8 suites)** |
| Variable binding | Soft | **HARD** |
| Null check scanner | Soft | **HARD** |
| Contracts | 12 | **17** |
| Test assertions | Loose bounds (>80) | **Exact computed values** |
| Edge cases | None | **10 dedicated tests** |
| Integration tests | None | **10 cross-module tests** |
| Partial delivery status | "shipped" | **"partially_received"** |

## Module Architecture
| Module | Responsibility | Key Polyglot |
|--------|---------------|-------------|
| models.naab | Domain structs, enums, validators | Shell (timestamps) |
| inventory.naab | Stock CRUD, valuation, turnover | — |
| suppliers.naab | Scoring (4-component), risk, ranking | Python (statistics) |
| orders.naab | PO lifecycle, state machine | Shell (timestamps) |
| forecasting.naab | 3 forecast methods, EOQ, safety stock | Python (math/stats) |
| reporting.naab | Reports, CSV, KPIs | — |
| main.naab | 80 tests across 8 suites | — |

## Test Suites (80 total)
1. **test_models** (10) — Creation, validation, enum names
2. **test_inventory** (10) — Stock operations, exact value calculations
3. **test_suppliers** (10) — Scoring with exact expected totals
4. **test_orders** (10) — PO lifecycle, state transitions
5. **test_forecasting** (10) — Exact formula outputs (MA, ES, LT, EOQ)
6. **test_reporting** (10) — Reports, KPIs with exact values
7. **test_edge_cases** (10) — Empty inputs, zero division, missing data
8. **test_integration** (10) — Cross-module lifecycle, consistency checks

## Expected Output
```
test_models: 10/10
test_inventory: 10/10
test_suppliers: 10/10
test_orders: 10/10
test_forecasting: 10/10
test_reporting: 10/10
test_edge_cases: 10/10
test_integration: 10/10

TOTAL: 80/80
ALL TESTS PASSED
```

## Governance Highlights
- 17 function contracts with return_keys
- HARD: variable binding, null checks, bracket access, empty catch, value semantics
- 4-tier complexity floors (test → forecast → score → create)
- Scanner: 139 checks enabled
