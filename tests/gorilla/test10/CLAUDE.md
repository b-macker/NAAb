# NAAb Language Reference for LLMs

This is the universal NAAb language reference. Copy this into your project's CLAUDE.md
and add project-specific sections (governance rules, module specs, data paths) below.

## About NAAb
NAAb is a polyglot programming language. You embed other languages (Python, JavaScript,
Shell, Go, Nim, Rust, C++, C#, Ruby, PHP, Zig, Julia) inside `<< >>` blocks within
.naab files. A govern.json file enforces rules at execution time.
DO NOT write standalone .py/.js/.go files — all code goes in .naab files.

## Critical Syntax Rules

### File Structure
- Top level can ONLY contain: use, import, export, struct, enum, function/fn, main
- NO top-level `let` or `const` — variables MUST be inside main {} or functions
- Every executable .naab file needs a `main {}` block
- Imports: `use array`, `use json`, `use math`, etc.

### Variable Declaration
- `let x = 5` — mutable variable
- `const MAX = 100` — constant (UPPER_SNAKE_CASE)
- NO `var` keyword — NAAb does not have `var`

### Functions
- Use `fn` keyword: `fn my_function(param1, param2) { }`
- `function`, `func`, `def` also work but `fn` is preferred
- Lambda: `fn(x) { return x * 2 }` — NOT `(x) => x * 2`
- Arrow syntax `=>` is NOT supported for lambdas

### Control Flow
- `if condition { } else if condition { } else { }`
- NO `elif` — use `else if`
- `and`, `or`, `not` work as aliases for `&&`, `||`, `!` (Python-style)
- `for item in collection { }`
- `for i in 0..10 { }` — range (exclusive), `0..=10` for inclusive
- `while condition { }`
- `match value { pattern => { } default => { } }`
- `try { } catch (e) { }` — parens around `e` are REQUIRED
- `break` and `continue` work in loops
- `return value` — explicit return from functions

### If Expressions
- `let x = if condition { value_a } else { value_b }` — returns a value
- `let x = if a { 1 } else if b { 2 } else { 3 }` — else-if chains work

### Null Coalesce Operator (??)
- `let y = x ?? "default"` — returns `x` if non-null, otherwise `"default"`
- Chainable: `a ?? b ?? c` — returns first non-null value
- `false ?? "x"` returns `false` (only null triggers the fallback, NOT falsy values)
- NOTE: `||` (and `or`) returns boolean, `??` returns the actual value

### Structs and Enums (top-level only)
```naab
struct Point {
    x: int
    y: int
}

enum Color {
    Red,
    Green,
    Blue
}

// Instantiation REQUIRES `new` keyword:
let p = new Point { x: 1, y: 2 }

// Works inside function calls:
arr.push(new Point { x: 1, y: 2 })

// WRONG (will error):
// let p = Point { x: 1, y: 2 }     // Missing `new`
```

### Type Casts (builtins)
- `int(value)` — convert to integer
- `float(value)` — convert to float
- `string(value)` — convert to string
- `bool(value)` — convert to boolean
- `len(collection)` — get length
- `type(value)` — get type name as string
- `typeof(value)` — same as type()
- `range(start, end)` — generate range
- `print(value)` — print to stdout

### Imports (file-based)
```naab
import "src/module.naab" as mod     // Relative to THIS file's directory (NOT CWD)
import "./sibling.naab" as sib      // Explicit relative

// Functions in imported files MUST be exported:
//   export fn my_function() { ... }
// Non-exported functions are private to their file
```

## Polyglot Rules

### Block Syntax
Polyglot blocks MUST be multiline. The `>>` closer MUST be on its own line at column 0.
```naab
let result = <<python
x = 42
x * 2
>>
```

### WRONG (will error):
```naab
let result = <<python x * 2 >>     // Single-line NOT allowed
```

### Variable Binding
Pass NAAb variables to polyglot blocks explicitly.
If your govern.json requires variable binding, ALL variables must be listed:
```naab
let data = [1, 2, 3]
let multiplier = 10
let result = <<python[data, multiplier]
sum(d * multiplier for d in data)
>>
```

### Language-Specific Rules
- **Python**: Start at column 0 (no leading indent). Use separate lines, not semicolons.
  Last expression is the return value. `import` works.
  **Do NOT use `return`** — it causes `SyntaxError: 'return' outside function`.
  The last expression's value is automatically captured.
  WRONG: `return json.dumps(data)`
  RIGHT: `json.dumps(data)`
- **Shell**: stdout is the return value.

### JSON Sovereign Pipe
For structured return data:
```naab
let data = <<python -> JSON
import json
json.dumps({"key": "value", "count": 42})
>>
```

## Stdlib Reference (ALL functions)

### array (dot-notation works: arr.push(4))
push, pop, shift, unshift, length, contains, find, first, last,
join, reverse, sort, slice_arr, map_fn, filter_fn, reduce_fn

**GOTCHA**: map_fn, filter_fn, reduce_fn do NOT work with dot notation!
```naab
// WRONG: arr.filter_fn(fn(x) { return x > 5 })
// RIGHT:
array.filter_fn(arr, fn(x) { return x > 5 })
array.map_fn(arr, fn(x) { return x * 2 })
array.reduce_fn(arr, fn(acc, x) { return acc + x }, 0)
```

### dict (built-in, dot-notation)
- `dict.get(key)` — returns value or null (NO error on missing key)
- `dict.get(key, default)` — returns value or default
- `dict.has(key)` / `dict.contains(key)` / `dict.containsKey(key)` — boolean check
- `dict.size()` / `dict.length()` — element count
- `dict.isEmpty()` — boolean
- `dict.put(key, val)` / `dict.set(key, val)` — add/update entry
- `dict.remove(key)` / `dict.delete(key)` — remove entry
- `dict.keys()` — array of keys
- `dict.values()` — array of values
- `dict.entries()` — array of [key, value] pairs
- `dict.merge(other)` — merge another dict

**CRITICAL**: `dict["key"]` THROWS on missing key! Use `dict.get("key")` for safe access.

### string (dot-notation works: str.upper())
upper, lower, trim, split, replace, contains, starts_with, ends_with,
length, char_at, index_of, substring, reverse, repeat, pad_left, pad_right

### math
abs, floor, ceil, round, min, max, pow, sqrt, random, sin, cos, PI, E
**GOTCHA**: Use `math.PI` and `math.E` (uppercase), NOT `math.pi`/`math.e`

### json
parse, stringify

### file
read, write, exists, list_dir, delete, append

### time
now, now_millis, sleep (takes SECONDS not ms — use 0.01 for 10ms),
format_timestamp (NOT format!), parse_datetime,
year, month, day, hour, minute, second, weekday

### csv
parse, stringify

### regex
match, test, replace, split

### env
get, set_var (NOT set — the function is set_var), list

### io
write, read_line, write_error

### debug
type, inspect, keys, values, log, trace, timer, compare, diff,
snapshot, stack, env, watch
**NOTE**: debug is auto-imported (prelude). Do NOT `use debug` — it causes a file search error.

### crypto
sha256, sha512, md5, sha1, hash (dispatcher: `hash("sha256", data)`),
random_bytes, random_string, random_int,
base64_encode, base64_decode, hex_encode, hex_decode,
compare_digest, generate_token, hash_password

## Pipeline Operator
```naab
let result = data |> transform |> analyze |> format
```
`|>` passes the left value as the FIRST argument to the right function.
`data |> fn_name` becomes `fn_name(data)`.
For multi-arg stdlib functions, create a wrapper:
```naab
fn my_filter(arr) { return array.filter_fn(arr, fn(x) { return x > 5 }) }
let result = data |> my_filter
```

## Value Semantics
NAAb dictionaries and arrays use **value semantics** (copy-on-assignment).
Modifying a nested value requires re-assignment to the parent:
```naab
let stats = depts[d]
stats["count"] = int(stats["count"]) + 1
depts[d] = stats    // MANDATORY — without this, the change is lost
```

### Value Semantics in Loops (CRITICAL)
```naab
// WRONG — changes lost:
for i in 0..len(entities) {
    let e = entities[i]
    e["hp"] = e.get("hp") - 10
    // Missing: entities[i] = e
}

// RIGHT — re-assign both inner and outer:
for i in 0..len(entities) {
    let e = entities[i]
    e["hp"] = e.get("hp") - 10
    entities[i] = e        // MUST re-assign element
}
state["entities"] = entities  // MUST re-assign container
```

## Testing Pattern
Each test function returns [passed, total]. Main aggregates:
```naab
fn test_something() {
    let passed = 0
    let total = 0

    // Test 1
    total = total + 1
    if some_condition {
        passed = passed + 1
    }

    return [passed, total]
}

main {
    let results = test_something()
    print("test_something: " + string(results[0]) + "/" + string(results[1]))
}
```

## Known Gotchas (avoid these mistakes)
1. `array.push/pop/shift/unshift` MUTATE the original array
2. Float-to-string adds 6 decimals: `3.14` becomes `"3.140000"` — use `int(round(x))` for clean numbers
3. `True`/`False`/`None` are NOT NAAb keywords — use `true`/`false`/`null`
4. `elif` does NOT exist — use `else if`
5. `.len()` does NOT exist — use `.length()` or `len(x)`
6. `env.set()` does NOT exist — use `env.set_var()`
7. `catch e { }` will error — must be `catch (e) { }`
8. `use debug` causes file search error — debug is auto-imported (prelude). Don't import it.
9. Polyglot blocks inside functions + return: works, but the function must have return AFTER the block
10. Dict iteration: `for key in my_dict { }` works
11. Null comparison: use `x == null` not `x === null` (no triple equals)
12. String concatenation with `+` is permissive (auto-converts numbers)
13. Arithmetic with strings is STRICT — `"5" + 3` works (string concat), `"5" * 3` errors
14. `array.find` takes a PREDICATE function, not a value
15. `||` (and its alias `or`) ALWAYS returns boolean (true/false), NEVER the operand value.
    `null || "fallback"` returns `true` (NOT "fallback"!).
    NAAb's `||`/`or` is NOT like JavaScript's `||` or Python's `or` operator.
    For null coalesce, use the `??` operator: `x ?? default_val`
16. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)` for safe access
17. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }` — without `new` you get a parse error
18. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
19. Value semantics: modifying a nested dict/array requires re-assigning to parent (see Value Semantics section)
20. The `..` range operator can collide with `".."` string literals — use intermediate variables: `let dots = ".."; path.contains(dots)`

## Complexity Scoring (for governance)

If govern.json has `complexity_floor` enabled, functions must reach a minimum
complexity score. Here's what contributes:

| Pattern | Score | Example |
|---------|-------|---------|
| Real loop (for/while) | +5 each | `for item in data { ... }` |
| Nested loops | +15 | `for row in grid { for col in row { } }` |
| try/catch | +5 | `try { risky() } catch (e) { handle(e) }` |
| Array operations | +5 | `array.filter_fn(arr, predicate)` |
| Pipeline operator | +5 | `data \|> transform \|> format` |
| Function definition | +3 each | `fn helper(x) { ... }` |
| External function call | +1 each | `math.sqrt(x)` |
| Recursion | +10 | Function calls itself |

Functions named `get_*`, `set_*`, `is_*`, `has_*`, `to_*`, `make_*`, `apply_*`,
`move_*`, `check_*`, `find_*`, `create_*`, `update_*`, `remove_*`, `delete_*`,
`add_*`, `reset_*`, `init_*`, `validate_*`, `convert_*` have LOW threshold (score >= 3).

Functions named `forecast_*`, `optimize_*`, `calculate_*`, `process_*`, `analyze_*`
need substantial logic (score >= 20, must have loops or conditionals).

Functions shorter than `min_lines_for_check` (6) lines skip the floor entirely.

Do NOT pad functions with `for i in 0..1 { }` or `for i in 0..2 { }` loops to pass
complexity checks. Instead: add real logic — input validation, edge case handling,
error recovery with try/catch.

## Contract Patterns

When govern.json defines function contracts, your return value MUST match.
Common patterns:

**return_type: dict + return_keys:**
```naab
// Contract: return_keys: ["id", "type", "hp", "x", "y"]
return {
    "id": computed_id,
    "type": entity_type,
    "hp": calculated_hp,
    "x": position_x,
    "y": position_y
}
```

---

## Project: Enterprise Supply Chain Management System

An enterprise-grade supply chain tracker managing inventory, purchase orders,
supplier relationships, demand forecasting, and financial reporting.

## Governance (govern.json)
- Allowed languages: python, shell
- Blocked languages: javascript, rust, cpp, csharp, go, nim, zig, julia, ruby, php
- Variable binding: REQUIRED (soft enforcement)
- Network: DISABLED
- Filesystem: READ ONLY
- Security: no_secrets (hard), no_placeholders (hard), no_simulation_markers (hard)
- 12 function contracts with return_keys enforcement
- 4-tier complexity floor:
  - `test_` functions: min_score 0
  - `forecast_`, `optimize_`, `calculate_reorder/safety/eoq/turnover`: min_score 20, loops required
  - `score_`, `evaluate_`, `generate_`, `rank_`, `receive_order`, `get_order_history`: min_score 15, loops required
  - `create_`, `get_`, `add_`, `validate_`, `check_`, etc.: min_score 3

## Module Specifications

### models.naab — Domain Models

**Structs:**
- `Product { id: int, name: string, sku: string, category: string, unit_cost: float, selling_price: float, min_stock: int, max_stock: int, lead_time_days: int, reorder_point: int }`
- `Supplier { id: int, name: string, rating: float, lead_time_avg: float, lead_time_variance: float, fill_rate: float, defect_rate: float, active: bool }`

**Enums:**
- `OrderStatus { Pending, Approved, Shipped, Delivered, Cancelled }`
- `TransactionType { Purchase, Sale, Adjustment, Return, WriteOff }`
- `Category { RawMaterial, Component, FinishedGood, Packaging, MRO }`

**Exported Functions:**
- `create_product(id, name, sku, category, unit_cost, selling_price, min_stock, max_stock, lead_time_days, reorder_point)` → dict with all 10 keys
- `create_supplier(id, name, rating, lead_time_avg, lead_time_variance, fill_rate, defect_rate, active)` → dict with all 8 keys
- `create_transaction(product_id, quantity, tx_type, reason)` → dict with keys: product_id, quantity, type, timestamp, reason (timestamp via shell polyglot)
- `validate_product(product)` → dict with keys: valid (bool), errors (array of strings)
- `validate_supplier(supplier)` → dict with keys: valid (bool), errors (array of strings)
- `status_name(status)` → string name for OrderStatus enum
- `transaction_type_name(tx_type)` → string name for TransactionType enum
- `category_name(cat)` → string name for Category enum

### inventory.naab — Inventory Management

**Exported Functions:**
- `add_stock(inventory, product_id, quantity, tx_type)` → dict: {inventory, transaction}
- `remove_stock(inventory, product_id, quantity, tx_type)` → dict: {inventory, transaction, success}
- `get_stock_level(inventory, product_id)` → int (0 if not found)
- `get_inventory_value(inventory, products)` → float (sum of quantity * unit_cost for all products)
- `check_reorder_needed(inventory, products)` → array of product dicts where stock < reorder_point
- `calculate_turnover_rate(transactions, inventory, products, days)` → dict mapping product_id to turnover rate
- `adjust_stock(inventory, product_id, actual_count, reason)` → dict: {inventory, adjustment_transaction, difference}

inventory is a dict mapping product_id (string) → quantity (int).
transactions is an array of transaction dicts.

### suppliers.naab — Supplier Scoring & Risk

**Exported Functions:**
- `score_supplier(supplier, order_history)` → dict: {supplier_id, total_score, delivery_score, quality_score, fill_rate_score, price_score}
  - delivery_score: % of orders delivered within lead_time_avg + tolerance (30 pts max)
  - quality_score: (1 - defect_rate) * 25 (25 pts max)
  - fill_rate_score: fill_rate * 25 (25 pts max)
  - price_score: based on relative competitiveness across order history (20 pts max)
  - total_score: sum of all four (0-100)
- `rank_suppliers(suppliers, order_history)` → array of score dicts sorted by total_score descending
- `evaluate_supplier_risk(supplier, order_history)` → dict: {supplier_id, risk_level, reasons, recommendation}
  - risk_level: "low" / "medium" / "high" / "critical"
  - Based on: defect_rate > 5% (+risk), fill_rate < 90% (+risk), lead_time_variance > 3 (+risk), low order count (+risk)
- `get_preferred_supplier(product_id, suppliers, order_history)` → supplier dict (highest scored active supplier)
- `calculate_supplier_lead_time(supplier, order_history)` → dict: {avg_days, std_dev, min_days, max_days, sample_size}

order_history is an array of order dicts, each with: supplier_id, items, status, delivery_days, total

### orders.naab — Purchase Order Lifecycle

**Exported Functions:**
- `create_purchase_order(id, supplier_id, items, priority)` → dict: {id, supplier_id, items, status, total, created_at, priority}
  - items: array of {product_id, quantity, unit_price}
  - status: starts as "pending"
  - total: sum of (quantity * unit_price) for all items
  - created_at: shell timestamp
  - priority: "low" / "normal" / "high" / "critical"
- `approve_order(order, approver_id)` → dict: updated order with status "approved", approved_by, approved_at
  - Validates: must be "pending" to approve
- `receive_order(order, received_items, inventory)` → dict: {order, inventory, discrepancies}
  - received_items: array of {product_id, quantity_received}
  - Updates inventory for each received item
  - Tracks discrepancies (ordered vs received)
  - Sets status to "delivered" if all items received, keeps "shipped" if partial
- `cancel_order(order, reason)` → dict: updated order with status "cancelled", cancel_reason
  - Cannot cancel if status is "delivered"
- `calculate_order_total(order)` → float (sum of line item totals)
- `get_order_history(orders, filters)` → filtered array of orders
  - filters: dict with optional keys: supplier_id, status, min_total, max_total

### forecasting.naab — Demand Forecasting (Python Polyglot)

**Exported Functions:**
- `forecast_demand(sales_history, periods, method)` → dict: {forecast, method, periods, confidence}
  - sales_history: array of floats (historical demand per period)
  - periods: int (how many future periods to forecast)
  - method: "moving_average" / "exponential_smoothing" / "linear_trend"
  - forecast: array of predicted values
  - confidence: float 0-1 (higher = more data points, lower variance)
  - Uses Python polyglot with `-> JSON` for computation
- `calculate_reorder_point(product, supplier, service_level)` → dict: {reorder_point, safety_stock, avg_demand, lead_time}
  - reorder_point = avg_demand * lead_time + safety_stock
  - service_level: float 0-1 (e.g., 0.95 for 95% service)
- `calculate_safety_stock(demand_history, lead_time_days, service_level)` → float
  - safety_stock = z_score * std_dev(demand) * sqrt(lead_time)
  - z_score from service_level: 0.90→1.28, 0.95→1.65, 0.99→2.33 (use Python polyglot)
- `calculate_eoq(annual_demand, order_cost, holding_cost)` → float
  - EOQ = sqrt(2 * annual_demand * order_cost / holding_cost)
- `optimize_reorder_schedule(products, inventory, suppliers, budget)` → dict: {orders, total_cost, products_covered}
  - Prioritize products closest to stockout
  - Respect budget constraint
  - Select best supplier for each product

### reporting.naab — Reports & Analytics

**Exported Functions:**
- `generate_inventory_report(inventory, products)` → dict: {report_text, total_value, items_below_reorder, items_overstocked}
  - report_text: formatted table with columns: SKU, Name, Stock, Min, Max, Reorder, Value, Status
  - Status: "LOW" if below reorder, "OVER" if above max, "OK" otherwise
  - Uses string.pad_right for column alignment
- `generate_supplier_scorecard(suppliers, order_history)` → dict: per-supplier performance summary
- `generate_purchase_summary(orders, date_range)` → dict: {total_spent, order_count, avg_order_value, by_status, by_priority}
- `export_to_csv(data, headers)` → string (CSV formatted)
  - data: array of arrays (rows)
  - headers: array of column names
  - Uses csv.stringify
- `calculate_kpis(inventory, orders, transactions)` → dict: {fill_rate, stockout_rate, inventory_turns, avg_order_value}
  - fill_rate: % of orders fully delivered
  - stockout_rate: % of products below reorder point
  - inventory_turns: total sales / avg inventory value
  - avg_order_value: total order value / order count

### main.naab — Test Orchestrator (60 tests)

Imports all 6 modules. Contains 6 test functions (10 tests each):
- `test_models()` — struct creation, validation, enum names, edge cases
- `test_inventory()` — add/remove stock, value calc, reorder check, turnover, adjustments
- `test_suppliers()` — scoring, ranking, risk evaluation, lead time, preferred supplier
- `test_orders()` — PO lifecycle (create → approve → receive), cancellation, filtering, totals
- `test_forecasting()` — moving avg, exp smoothing, linear trend, reorder point, safety stock, EOQ
- `test_reporting()` — inventory report, scorecard, CSV export, KPIs

Each test verifies actual computed values, not just types.

## What NOT to Do (project-specific)
- Do NOT write standalone .py, .js, .go files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments — governance BLOCKS these patterns
- Do NOT use comments that admit code is incomplete: "simplified version", "for demonstration",
  "would normally do X", "in a real system", "basic implementation", "mock data", "placeholder"
- Do NOT write empty/trivial functions (pass-only, return True, return [])
- Do NOT swallow errors silently (empty catch blocks, except: pass)
- Do NOT pad functions with `for i in 0..1 { }` or `for i in 0..2 { }` loops to pass
  complexity checks. Instead: add real logic — input validation, edge case handling,
  error recovery with try/catch. The governance engine rewards: loops over real data,
  conditionals, try/catch, function calls, array operations (map_fn, filter_fn, reduce_fn).
- Do NOT add hedging comments like "for now", "simplified", "basic implementation",
  "in a real system". If the code works, it IS the real implementation. Delete the
  qualifying comment — governance blocks these patterns even when the code is correct.
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution
- Do NOT use `return` inside Python polyglot blocks — it causes SyntaxError
- Do NOT use `dict["key"]` — always use `dict.get("key")` for safe access
- Do NOT forget value semantics re-assignment when mutating dicts/arrays in loops
