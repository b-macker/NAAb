# Gorilla Test #10 — Enterprise Supply Chain Management System

## Overview
A production-grade supply chain tracker built entirely in NAAb. Manages inventory levels, purchase orders, supplier relationships, demand forecasting, and financial reporting for a manufacturing/retail operation.

## Module Architecture

| Module | Responsibility | Key Features |
|--------|---------------|--------------|
| `models.naab` | Domain models (structs, enums, constructors) | Product, Supplier, PurchaseOrder, LineItem, 3 enums |
| `inventory.naab` | Inventory CRUD + analytics | Stock add/remove, reorder checks, turnover rates, cycle counts |
| `suppliers.naab` | Supplier scoring & risk | Composite scoring (0-100), risk evaluation, ranking, lead time analysis |
| `orders.naab` | Purchase order lifecycle | Create → approve → receive → complete, partial receipts, cancellation |
| `forecasting.naab` | Demand forecasting (Python polyglot) | Moving average, exponential smoothing, safety stock, EOQ |
| `reporting.naab` | Reports & KPIs | Inventory reports, supplier scorecards, CSV export, KPI dashboard |
| `main.naab` | Test orchestrator | 60 tests across 6 suites (10 each) |

## Expected Output
```
test_models: 10/10
test_inventory: 10/10
test_suppliers: 10/10
test_orders: 10/10
test_forecasting: 10/10
test_reporting: 10/10

TOTAL: 60/60
ALL TESTS PASSED
```

## Governance
- v3.0 enforce mode
- Languages: Python + Shell only (10 blocked)
- 12 function contracts with return_keys
- 4-tier complexity floors
- Full scanner suite (redundancy, code_quality, complexity, lang_naab)
- No secrets, no placeholders, no simulation markers (hard)

## NAAb Feature Coverage

| Feature | Usage | Count |
|---------|-------|-------|
| Multi-file imports | 6 modules imported by main | 6 |
| Structs (with `new`) | Product, Supplier, PurchaseOrder, LineItem | 4 |
| Enums | OrderStatus, TransactionType, Category | 3 |
| Python polyglot `-> JSON` | Forecasting (moving avg, exp smoothing, trend) | 3+ |
| Shell polyglot | Timestamps for orders/transactions | 2+ |
| Variable binding `[vars]` | All polyglot blocks | All |
| Closures/lambdas | filter_fn, sort comparators, reduce_fn | 10+ |
| `and`/`or`/`not` keywords | Validation, conditionals | Throughout |
| `??` null coalesce | Safe dict access patterns | 20+ |
| If-expressions | Status determination, risk levels | 10+ |
| Governance contracts | 12 functions with return_keys | 12 |
| Complexity floors | 4 tiers across all modules | All |
| Value semantics | Inventory mutation in loops | Critical |
| match expressions | Enum → string mapping | 3+ |
| try/catch | Order validation, stock operations | 5+ |
| Pipeline operator | Data transformation chains | 3+ |
| string.pad_right/pad_left | Report formatting | Tables |
| csv.stringify | CSV export | Reports |

## Running
```bash
cd ~/.naab/language/tests/gorilla/test10
../../../build/naab-lang main.naab
```
