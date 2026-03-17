# Gorilla Test #9 — ML Pipeline (Maximum Complexity)

## Summary
- **Domain:** Machine learning pipeline (data → preprocess → train → evaluate)
- **Result:** 50/50 tests passed, 0 governance violations
- **Files:** 5 .naab files (4 modules + 1 main), 1 govern.json
- **Total lines:** ~900 across all files
- **Scanner:** 197 issues across 5 files (1 hard false-positive, 186 soft, 10 advisory)
- **Regression:** 208/208 (184 pass + 24 expected failures)

## Feature Coverage (First gorilla test using these)
| Feature | Count | Status |
|---------|-------|--------|
| Multi-file imports | 4 modules | PASS |
| Structs (with `new`) | 5 structs | PASS |
| Enums | 3 enums (ModelType, Normalization, SplitStrategy) | PASS |
| Python polyglot `-> JSON` | 4 blocks (correlation, hash, AUC, serialize) | PASS |
| Shell polyglot | 1 block (timestamp) | PASS |
| Variable binding `[vars]` | All polyglot blocks | PASS |
| Closures/lambdas | 10+ (reduce_fn, filter_fn, pipeline, CV) | PASS |
| `and`/`or`/`not` keywords | Throughout (validate, detect, split) | PASS |
| `??` null coalesce | 15+ uses | PASS |
| If-expressions | Integration test label assignment | PASS |
| Governance contracts | 8 functions with return_keys | PASS |
| Complexity floors | 4 tiers (test_, train/compute, normalize/detect, create/get) | PASS |
| Value semantics | Nested dict/array mutation in loops | PASS |

## Test Breakdown
| Suite | Tests | Result |
|-------|-------|--------|
| test_models | 10 | 10/10 |
| test_preprocessing | 10 | 10/10 |
| test_pipeline | 10 | 10/10 |
| test_evaluation | 10 | 10/10 |
| test_integration | 10 | 10/10 |
| **TOTAL** | **50** | **50/50** |

## Scanner Results Per File
| File | Issues | Hard | Soft | Advisory |
|------|--------|------|------|----------|
| models.naab | 21 | 0 | 19 | 2 |
| preprocessing.naab | 41 | 0 | 39 | 2 |
| pipeline.naab | 38 | 0 | 36 | 2 |
| evaluation.naab | 17 | 0 | 15 | 2 |
| main.naab | 80 | 1* | 77 | 2 |
| **TOTAL** | **197** | **1** | **186** | **10** |

*1 hard = false positive: `value_semantics_bug` on `means[0]` read access in test code

## Bugs Found During Development
1. **Match arms can't contain `return` or `let`** — match is an expression, not a statement block. Refactored to if/else chains.
2. **`string.pad_right()` doesn't exist** — not in stdlib. Created local helper function.
3. **Outlier detection threshold sensitivity** — z-score with outlier in sample affects std, making detection threshold-sensitive. Adjusted test from 2.0 to 1.5.

## Governance Config
- v3.0 enforce mode
- Languages: python + shell allowed, 10 blocked
- 8 function contracts with return_keys enforcement
- 4-tier complexity floor (test_ = 0, train/compute = 20, normalize/detect = 15, create/get = 3)
- Full scanner with redundancy, code_quality, complexity, and lang_naab checks
- Variable binding required (soft)
