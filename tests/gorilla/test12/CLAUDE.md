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
let result = <<python[my_var]
x = my_var * 2
x
>>
```

### WRONG (will error):
```naab
let result = <<python x * 2 >>     // Single-line NOT allowed
```

### Variable Binding (HARD ENFORCED)
**This project enforces variable binding at HARD level.** Every polyglot block MUST
list all NAAb variables it uses. If a block uses no NAAb variables, use empty brackets:

```naab
// Block that uses NAAb variables:
let data = [1, 2, 3]
let multiplier = 10
let result = <<python[data, multiplier]
sum(d * multiplier for d in data)
>>

// Block that uses NO NAAb variables (empty brackets required):
let timestamp = <<shell[]
date '+%Y-%m-%d %H:%M:%S'
>>
```

**WRONG — will be BLOCKED by governance:**
```naab
let result = <<python        // Missing variable binding!
sum(data)
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
let data = <<python[input_data] -> JSON
import json
json.dumps({"key": "value", "count": len(input_data)})
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
**CRITICAL**: Chained `.get()` is HARD blocked by scanner! `a.get("x").get("y")` will fail.
Instead: `let x = a.get("x")` then check null before second `.get()`.

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
    For null coalesce, use the `??` operator: `x ?? default_val`
16. `dict["key"]` THROWS on missing key — use `dict.get("key")` or `dict.get("key", default)`
17. Struct instantiation requires `new`: `let p = new Point { x: 1, y: 2 }`
18. Python polyglot: Do NOT use `return` — causes `SyntaxError: 'return' outside function`
19. Value semantics: modifying a nested dict/array requires re-assigning to parent
20. The `..` range operator can collide with `".."` string literals — use intermediate variables

## Enum Best Practices

When your module defines enums, use them consistently:
```naab
// GOOD — uses the enum type:
if tx.get("type") == TransactionType.Sale { ... }
if order.get("status") == OrderStatus.Delivered { ... }

// BAD — magic values (governance may flag these):
if tx.get("type") == 1 { ... }           // What is 1?
if order.get("status") == "delivered" { ... }  // Fragile string literal
```

When passing enum values to other modules, the receiving module should import the enum:
```naab
import "./models.naab" as models
// Then use: models.TransactionType.Sale, models.OrderStatus.Pending, etc.
```

## Null Safety Pattern

When accessing nested dict values, NEVER chain `.get()` calls:
```naab
// WRONG — scanner BLOCKS this (hard):
let val = outer.get("key1").get("key2")

// RIGHT — split into steps with null check:
let inner = outer.get("key1")
if inner != null {
    let val = inner.get("key2")
}

// Or use ?? for safe fallback:
let inner = outer.get("key1") ?? {}
let val = inner.get("key2", 0)
```

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
This project has **15 function contracts**. Every listed return key must be present.

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

## Project: Fleet & Logistics Management System

A fleet management system tracking vehicles, drivers, routes, deliveries, fuel consumption, maintenance scheduling, and operational reporting.

**This is a strict governance project.** Tests verify exact computed values, edge cases are tested, and governance blocks sloppy code at hard level.

## Governance Summary (govern.json)
- Allowed languages: python, shell
- Blocked: javascript, rust, cpp, csharp, go, nim, zig, julia, ruby, php
- **Variable binding: HARD** -- every polyglot block must have `[var1, var2]` or `[]`
- Network: DISABLED
- Filesystem: READ ONLY
- Security: no_secrets (hard), no_placeholders (hard), no_simulation_markers (hard)
- **15 function contracts** with return_keys enforcement
- 4-tier complexity floor (test_/calculate_/score_/create_ tiers)
- **Scanner HARD checks:** empty_catch, dead_conditional, value_semantics_bug, missing_null_check, dict_bracket_access, python_return_in_block, top_level_let

## Module Specifications

### models.naab -- Domain Models

**Enums:**
- `VehicleStatus { Available, InTransit, Maintenance, Retired }`
- `DeliveryStatus { Pending, Assigned, InProgress, Delivered, Failed }`
- `FuelType { Diesel, Gasoline, Electric, Hybrid }`
- `VehicleType { Van, Truck, Semi, Motorcycle }`

**Exported Functions:**
- `create_vehicle(id, plate, v_type, fuel_type, capacity_kg, mileage, fuel_efficiency, status)` -> dict with all 8 keys
- `create_driver(id, name, license_class, hourly_rate, rating, total_hours, max_hours_per_week, available)` -> dict with all 8 keys
- `create_delivery(id, origin, destination, weight_kg, priority, deadline)` -> dict: {id, origin, destination, weight_kg, priority, status, created_at, deadline}
- `validate_vehicle(vehicle)` -> dict: {valid, errors}
- `validate_driver(driver)` -> dict: {valid, errors}
- `status_name(status)` -> string for VehicleStatus
- `delivery_status_name(status)` -> string for DeliveryStatus

### vehicles.naab -- Fleet Management

**Exported Functions:**
- `add_vehicle(fleet, vehicle)` -> dict: {fleet, success} (+ error key on duplicate)
- `retire_vehicle(vehicle, reason)` -> updated vehicle with status=Retired, retire_reason
- `get_available_vehicles(fleet)` -> array of Available vehicles
- `assign_vehicle(fleet, vehicle_id)` -> dict: {fleet, vehicle, success}
- `release_vehicle(fleet, vehicle_id)` -> dict: {fleet, success}
- `calculate_depreciation(vehicle, years)` -> float
  - purchase_value = capacity_kg * 50.0 (Van/Motorcycle) or * 100.0 (Truck/Semi)
  - annual = purchase_value * 0.09, total = annual * years, capped at 90% of purchase
- `get_maintenance_due(fleet, mileage_threshold)` -> array of vehicles where mileage > threshold

### drivers.naab -- Driver Management

**Exported Functions:**
- `register_driver(drivers, driver)` -> dict: {drivers, success} (+ error on duplicate)
- `assign_driver(drivers, driver_id)` -> dict: {drivers, driver, success}
- `get_available_drivers(drivers)` -> array of available drivers
- `calculate_driver_score(driver, delivery_history)` -> dict: {driver_id, total_score, delivery_score, reliability_score, efficiency_score, rating_score}
  - delivery_score (0-30): completed/total * 30 (0 if no deliveries)
  - reliability_score (0-25): on_time/completed * 25 (0 if none completed)
  - efficiency_score (0-25): Python polyglot with -> JSON
  - rating_score (0-20): (rating / 5.0) * 20
- `update_driver_hours(driver, hours_worked)` -> updated driver
- `check_hours_compliance(driver)` -> dict: {compliant, hours_remaining, weekly_limit}

### routes.naab -- Route Planning

**Exported Functions:**
- `create_route_plan(vehicle, driver, stops, distances)` -> dict with route details
  - estimated_time = total_distance / 60.0 + len(stops) * 0.5
- `calculate_route_cost(route, vehicle, driver)` -> dict: {fuel_cost, driver_cost, total_cost}
  - fuel_cost = distance / efficiency * fuel_price (diesel=1.50, gasoline=1.40, electric=0.30, hybrid=0.90)
  - driver_cost = estimated_hours * hourly_rate
- `optimize_route_order(stops, distances_matrix)` -> dict: {optimized_stops, total_distance, savings_percent}
  - Python polyglot: nearest-neighbor TSP heuristic
- `estimate_delivery_time(distance_km, stops_count, priority)` -> float hours
  - base = distance/60 + stops*0.5; high: base*0.8, normal: base, low: base*1.2
- `check_route_capacity(route_deliveries, vehicle)` -> dict: {fits, total_weight, capacity, utilization}
- `get_route_history(routes, filters)` -> filtered array

### deliveries.naab -- Delivery Lifecycle

**Exported Functions:**
- `create_delivery_batch(deliveries, new_deliveries)` -> dict: {deliveries, added_count}
- `assign_delivery(delivery, route_id, driver_id)` -> updated delivery (status=Assigned)
- `complete_delivery(delivery)` -> updated delivery (status=Delivered, completed_at)
- `fail_delivery(delivery, reason)` -> updated delivery (status=Failed, failure_reason)
- `calculate_on_time_rate(deliveries)` -> float (Delivered / (Delivered+Failed); 0.0 if none)
- `get_deliveries_by_status(deliveries, status)` -> filtered array

### reporting.naab -- Analytics & Reports

**Exported Functions:**
- `generate_fleet_report(fleet, routes)` -> dict: {report_text, total_vehicles, available_count, in_transit_count, maintenance_count, avg_mileage}
- `calculate_fleet_kpis(fleet, routes, deliveries)` -> dict: {utilization_rate, delivery_success_rate, avg_route_distance, fleet_size}
  - NOTE: 3 parameters (fleet, routes, deliveries)
- `generate_driver_scorecard(drivers, delivery_history)` -> dict: driver_name -> score_dict
- `calculate_fuel_efficiency(routes, fleet)` -> dict: {total_fuel_cost, total_distance, avg_cost_per_km, by_fuel_type}
- `export_delivery_csv(deliveries, headers)` -> CSV string
- `forecast_maintenance_costs(fleet, months)` -> dict: {forecast, avg_monthly, total}
  - Python polyglot: per vehicle monthly_cost = mileage * 0.02

### main.naab -- Test Orchestrator (60 tests, 6 suites)

Imports all 6 modules. Contains 6 test functions (10 tests each):
- `test_models()` -- creation, validation, enum names
- `test_vehicles()` -- fleet ops, depreciation, maintenance
- `test_drivers()` -- registration, scoring, hours compliance
- `test_routes()` -- route planning, cost calc, optimization
- `test_deliveries()` -- lifecycle, on-time rate, filtering
- `test_reporting()` -- KPIs, scorecards, fuel efficiency, CSV, forecasts

**Tests verify exact computed values, not just types or ranges.**

## What NOT to Do (project-specific)
- Do NOT write standalone .py, .js, .go files
- Do NOT hardcode results, use placeholders, or stub functions
- Do NOT leave TODO/FIXME/STUB comments -- governance BLOCKS these patterns
- Do NOT use hedging comments: "simplified", "basic", "for now", "in a real system"
- Do NOT write empty/trivial functions (pass-only, return True, return [])
- Do NOT swallow errors silently (empty catch blocks -- HARD blocked)
- Do NOT pad functions with dummy loops to pass complexity checks
- Do NOT use `return` inside Python polyglot blocks -- SyntaxError
- Do NOT use `dict["key"]` -- HARD blocked. Use `dict.get("key")`
- Do NOT chain `.get()` calls -- HARD blocked. Split into separate let + null check
- Do NOT forget variable binding on polyglot blocks -- HARD blocked
- Do NOT forget value semantics re-assignment when mutating dicts/arrays in loops
- Do NOT use magic numbers/strings when enums are available
- The governance engine detects 200+ stub/evasion patterns and will BLOCK execution
