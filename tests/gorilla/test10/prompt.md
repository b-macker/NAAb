# Task: Implement an Enterprise Supply Chain Management System in NAAb

You are implementing a complete supply chain management system in NAAb, a polyglot programming language. Read the CLAUDE.md file in this directory for the full NAAb language reference, stdlib documentation, and project specifications.

## What You Must Create

Create these 7 files in the current directory. Every function listed below must be implemented with real business logic — governance will block stubs, placeholders, and hardcoded results.

---

## File 1: models.naab

Domain models for the supply chain system.

**Top-level declarations (structs and enums):**

```naab
enum OrderStatus { Pending, Approved, Shipped, Delivered, Cancelled }
enum TransactionType { Purchase, Sale, Adjustment, Return, WriteOff }
enum Category { RawMaterial, Component, FinishedGood, Packaging, MRO }
```

**Exported functions:**

`export fn create_product(id, name, sku, category, unit_cost, selling_price, min_stock, max_stock, lead_time_days, reorder_point)`
Returns dict with keys: id, name, sku, category, unit_cost, selling_price, min_stock, max_stock, lead_time_days, reorder_point. All values stored as-is from parameters. This is a governance contract — ALL keys must be present.

`export fn create_supplier(id, name, rating, lead_time_avg, lead_time_variance, fill_rate, defect_rate, active)`
Returns dict with keys: id, name, rating, lead_time_avg, lead_time_variance, fill_rate, defect_rate, active. Governance contract — ALL keys required.

`export fn create_transaction(product_id, quantity, tx_type, reason)`
Returns dict with keys: product_id, quantity, type, timestamp, reason.
- `timestamp`: use a shell polyglot block to get current time: `date '+%Y-%m-%d %H:%M:%S'`
- The shell block requires variable binding since govern.json requires it

`export fn validate_product(product)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, name non-empty, sku non-empty, unit_cost >= 0, selling_price >= unit_cost, min_stock >= 0, max_stock >= min_stock, lead_time_days > 0, reorder_point >= min_stock.
Each failed check adds a descriptive string to errors array.

`export fn validate_supplier(supplier)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, name non-empty, rating between 0.0 and 5.0, lead_time_avg > 0, fill_rate between 0.0 and 1.0, defect_rate between 0.0 and 1.0.

`export fn status_name(status)` — Returns string name for OrderStatus enum value using if/else chain.
`export fn transaction_type_name(tx_type)` — Returns string name for TransactionType enum value.
`export fn category_name(cat)` — Returns string name for Category enum value.

**Required imports:** `use string`, `use array`

---

## File 2: inventory.naab

Core inventory management. Inventory is a dict mapping product_id (as string key) to quantity (int).

**Required imports:** `use array`, `use math`, `use string`
**Required file import:** `import "./models.naab" as models`

`export fn add_stock(inventory, product_id, quantity, tx_type)`
- Add quantity to inventory[product_id] (create entry if missing)
- Create a transaction record via models.create_transaction
- Return dict: `{ "inventory": updated_inventory, "transaction": transaction_record }`
- IMPORTANT: Value semantics — you must re-assign the modified inventory dict

`export fn remove_stock(inventory, product_id, quantity, tx_type)`
- Check if sufficient stock exists
- If yes: subtract quantity, create transaction, return `{ "inventory": ..., "transaction": ..., "success": true }`
- If no: return `{ "inventory": inventory, "transaction": null, "success": false }`

`export fn get_stock_level(inventory, product_id)`
- Return inventory.get(string(product_id), 0) — the quantity, or 0 if product not in inventory

`export fn get_inventory_value(inventory, products)`
- For each product, multiply stock level by unit_cost
- Sum all values and return the total as float
- Must loop through products array, look up each product's stock in inventory

`export fn check_reorder_needed(inventory, products)`
- Return array of product dicts where current stock < product's reorder_point
- Must iterate through products, check each against inventory levels

`export fn calculate_turnover_rate(transactions, inventory, products, days)`
- For each product: count total units sold (TransactionType.Sale transactions)
- turnover = (units_sold / current_stock) * (365 / days) — annualized
- Return dict mapping product_id (string) to turnover rate (float)
- Handle division by zero (stock = 0 → turnover = 0)

`export fn adjust_stock(inventory, product_id, actual_count, reason)`
- Set inventory[product_id] = actual_count
- Calculate difference from previous count
- Create an Adjustment transaction
- Return `{ "inventory": ..., "adjustment_transaction": ..., "difference": int }`

---

## File 3: suppliers.naab

Supplier scoring, ranking, and risk assessment.

**Required imports:** `use array`, `use math`, `use string`

`export fn score_supplier(supplier, order_history)`
- Filter order_history to this supplier's orders only
- **delivery_score** (0-30): % of delivered orders where delivery_days <= supplier.lead_time_avg * 1.1, scaled to 30 points
- **quality_score** (0-25): (1.0 - supplier.defect_rate) * 25.0
- **fill_rate_score** (0-25): supplier.fill_rate * 25.0
- **price_score** (0-20): if supplier has orders, compare avg order cost to overall avg. Lower cost = higher score. Otherwise default to 10
- total_score = delivery_score + quality_score + fill_rate_score + price_score
- Return dict: `{ "supplier_id": ..., "total_score": ..., "delivery_score": ..., "quality_score": ..., "fill_rate_score": ..., "price_score": ... }`

`export fn rank_suppliers(suppliers, order_history)`
- Score all suppliers, sort by total_score descending
- Return array of score dicts

`export fn evaluate_supplier_risk(supplier, order_history)`
- Assess risks based on multiple factors:
  - defect_rate > 0.05 → add risk reason
  - fill_rate < 0.90 → add risk reason
  - lead_time_variance > 3.0 → add risk reason
  - fewer than 3 orders → add "insufficient history" risk
  - not active → add risk reason
- risk_level: 0 reasons = "low", 1 = "medium", 2 = "high", 3+ = "critical"
- recommendation: based on risk level ("maintain", "monitor closely", "find alternatives", "replace immediately")
- Return dict: `{ "supplier_id": ..., "risk_level": ..., "reasons": [...], "recommendation": ... }`

`export fn get_preferred_supplier(product_id, suppliers, order_history)`
- Score all active suppliers
- Return the supplier dict with the highest total_score
- If no active suppliers, return null

`export fn calculate_supplier_lead_time(supplier, order_history)`
- Filter to this supplier's delivered orders
- Calculate: avg_days, std_dev, min_days, max_days from delivery_days field
- Use Python polyglot with `-> JSON` for the statistics calculation
- Return dict: `{ "avg_days": ..., "std_dev": ..., "min_days": ..., "max_days": ..., "sample_size": ... }`
- If no delivered orders, return defaults (avg_days = supplier.lead_time_avg, std_dev = 0, etc.)

---

## File 4: orders.naab

Purchase order lifecycle management.

**Required imports:** `use array`, `use math`, `use string`, `use json`
**Required file import:** `import "./models.naab" as models`

`export fn create_purchase_order(id, supplier_id, items, priority)`
- Calculate total from items (each item has product_id, quantity, unit_price)
- Get timestamp via shell polyglot
- Return dict: `{ "id": ..., "supplier_id": ..., "items": items, "status": "pending", "total": calculated_total, "created_at": timestamp, "priority": priority }`

`export fn approve_order(order, approver_id)`
- Validate: status must be "pending" — if not, return order unchanged with error flag
- Set status to "approved", add approved_by and approved_at (shell timestamp)
- Return updated order dict

`export fn receive_order(order, received_items, inventory)`
- For each received item: update inventory (add stock)
- Track discrepancies: compare ordered quantity vs received quantity for each product
- If all items fully received → status = "delivered"
- If any item partially received → status = "shipped" (partial delivery)
- Return dict: `{ "order": updated_order, "inventory": updated_inventory, "discrepancies": [...] }`

`export fn cancel_order(order, reason)`
- Cannot cancel if status is "delivered" — return order unchanged with error
- Set status to "cancelled", add cancel_reason and cancelled_at (shell timestamp)
- Return updated order dict

`export fn calculate_order_total(order)`
- Sum (quantity * unit_price) for all items in order
- Return float total

`export fn get_order_history(orders, filters)`
- Filter orders array by any combination of:
  - filters.get("supplier_id") — match exact supplier_id
  - filters.get("status") — match exact status
  - filters.get("min_total") — order total >= min
  - filters.get("max_total") — order total <= max
- Return filtered array

---

## File 5: forecasting.naab

Demand forecasting using Python polyglot for statistical computation.

**Required imports:** `use array`, `use math`, `use json`

`export fn forecast_demand(sales_history, periods, method)`
- Uses Python polyglot with `-> JSON` pipe to compute forecasts
- **moving_average**: average of last N values (N = min(periods, len(history))), repeat for each forecast period
- **exponential_smoothing**: alpha = 2/(len(history)+1), forecast from smoothed values
- **linear_trend**: least squares regression, extrapolate
- confidence: based on data points (more = higher) and variance (lower = higher)
- Return dict: `{ "forecast": [array of floats], "method": method, "periods": periods, "confidence": float }`

`export fn calculate_reorder_point(product, supplier, service_level)`
- avg_demand = product selling rate (use product.get("reorder_point") as proxy for avg daily demand, or calculate from lead_time and min_stock)
- lead_time = supplier.get("lead_time_avg")
- safety_stock = call calculate_safety_stock with appropriate params
- reorder_point = avg_demand * lead_time + safety_stock
- Return dict: `{ "reorder_point": float, "safety_stock": float, "avg_demand": float, "lead_time": float }`

`export fn calculate_safety_stock(demand_history, lead_time_days, service_level)`
- Use Python polyglot to compute:
  - z_score from service_level (scipy.stats.norm.ppf or lookup: 0.90→1.28, 0.95→1.645, 0.99→2.326)
  - std_dev of demand_history
  - safety_stock = z_score * std_dev * sqrt(lead_time_days)
- Return float (the safety stock value)

`export fn calculate_eoq(annual_demand, order_cost, holding_cost)`
- EOQ = sqrt(2 * annual_demand * order_cost / holding_cost)
- Validate inputs > 0
- Return float

`export fn optimize_reorder_schedule(products, inventory, suppliers, budget)`
- For each product: check if stock is below reorder point
- Sort by urgency (closest to stockout first)
- For each product needing reorder: calculate order quantity (EOQ or up to max_stock)
- Select best supplier (cheapest or highest rated)
- Respect budget constraint — stop adding orders when budget exhausted
- Return dict: `{ "orders": [...], "total_cost": float, "products_covered": int }`

---

## File 6: reporting.naab

Reports, analytics, and data export.

**Required imports:** `use array`, `use math`, `use string`, `use csv`

`export fn generate_inventory_report(inventory, products)`
- Build a formatted text table with columns: SKU, Name, Stock, Min, Max, ROP, Value, Status
- Use string.pad_right() for column alignment
- Status: "LOW" if stock < reorder_point, "OVER" if stock > max_stock, "OK" otherwise
- Calculate total_value, count items_below_reorder, count items_overstocked
- Return dict: `{ "report_text": formatted_string, "total_value": float, "items_below_reorder": int, "items_overstocked": int }`

`export fn generate_supplier_scorecard(suppliers, order_history)`
- For each supplier: calculate score (use scoring logic or import suppliers module)
- Return dict mapping supplier name to their metrics

`export fn generate_purchase_summary(orders, date_range)`
- Aggregate: total_spent, order_count, avg_order_value
- Group by status (count per status)
- Group by priority (count per priority)
- Return dict: `{ "total_spent": ..., "order_count": ..., "avg_order_value": ..., "by_status": {...}, "by_priority": {...} }`

`export fn export_to_csv(data, headers)`
- Prepend headers row to data
- Use csv.stringify() to generate CSV string
- Return the CSV string

`export fn calculate_kpis(inventory, orders, transactions)`
- fill_rate: count orders with status "delivered" / total orders
- stockout_rate: products below reorder / total products (pass products as part of inventory context)
- inventory_turns: total sale quantities / avg inventory level (from transactions)
- avg_order_value: total order value / order count
- Return dict: `{ "fill_rate": float, "stockout_rate": float, "inventory_turns": float, "avg_order_value": float }`

---

## File 7: main.naab

The test orchestrator. Must import all 6 modules and run 60 tests.

**Structure:**
```naab
use array
use math
use string
use json
use csv

import "./models.naab" as models
import "./inventory.naab" as inv
import "./suppliers.naab" as sup
import "./orders.naab" as orders
import "./forecasting.naab" as forecast
import "./reporting.naab" as report
```

### test_models() — 10 tests
1. create_product returns all required keys
2. create_product values are correct
3. create_supplier returns all required keys
4. create_supplier with edge values (rating=0, fill_rate=1.0)
5. create_transaction has timestamp (non-empty)
6. validate_product passes for valid product
7. validate_product fails for invalid (negative cost, min > max)
8. validate_supplier fails for invalid (rating > 5, fill_rate > 1)
9. status_name returns correct strings for all statuses
10. category_name returns correct strings for all categories

### test_inventory() — 10 tests
1. add_stock to empty inventory creates entry
2. add_stock to existing inventory adds quantity
3. remove_stock succeeds with sufficient stock
4. remove_stock fails with insufficient stock (success=false)
5. get_stock_level returns 0 for missing product
6. get_stock_level returns correct count after add
7. get_inventory_value computes total correctly
8. check_reorder_needed identifies low-stock products
9. calculate_turnover_rate computes annualized rate
10. adjust_stock sets correct count and reports difference

### test_suppliers() — 10 tests
1. score_supplier returns all required keys
2. score_supplier: perfect supplier scores high (>80)
3. score_supplier: poor supplier scores low (<40)
4. rank_suppliers returns sorted order (best first)
5. evaluate_supplier_risk: good supplier = "low" risk
6. evaluate_supplier_risk: bad supplier = "high" or "critical"
7. evaluate_supplier_risk returns reasons array
8. get_preferred_supplier returns highest-scored active
9. get_preferred_supplier skips inactive suppliers
10. calculate_supplier_lead_time computes stats from history

### test_orders() — 10 tests
1. create_purchase_order calculates correct total
2. create_purchase_order has status "pending"
3. approve_order changes status to "approved"
4. approve_order rejects non-pending order
5. receive_order updates inventory correctly
6. receive_order tracks discrepancies for partial delivery
7. cancel_order sets status to "cancelled"
8. cancel_order rejects delivered order
9. calculate_order_total matches manual calculation
10. get_order_history filters by supplier_id and status

### test_forecasting() — 10 tests
1. forecast_demand with moving_average returns correct periods
2. forecast_demand moving_average: steady data → flat forecast
3. forecast_demand with exponential_smoothing returns values
4. forecast_demand with linear_trend: increasing data → increasing forecast
5. calculate_reorder_point returns all required keys
6. calculate_reorder_point: higher service level → higher reorder point
7. calculate_safety_stock returns positive value
8. calculate_safety_stock: more variance → higher safety stock
9. calculate_eoq returns correct value (verify: EOQ for D=1000, S=50, H=2 → ~223.6)
10. optimize_reorder_schedule respects budget constraint

### test_reporting() — 10 tests
1. generate_inventory_report returns all required keys
2. generate_inventory_report report_text contains column headers
3. generate_inventory_report identifies low-stock items
4. generate_supplier_scorecard returns per-supplier data
5. generate_purchase_summary aggregates totals correctly
6. generate_purchase_summary groups by status
7. export_to_csv includes headers in output
8. export_to_csv content is comma-separated
9. calculate_kpis returns all required keys
10. calculate_kpis fill_rate between 0 and 1

### main block
```naab
main {
    let total_passed = 0
    let total_tests = 0

    // Run each suite, print results, aggregate
    let r1 = test_models()
    total_passed = total_passed + r1[0]
    total_tests = total_tests + r1[1]
    print("test_models: " + string(r1[0]) + "/" + string(r1[1]))

    // ... repeat for all 6 suites ...

    print("")
    print("TOTAL: " + string(total_passed) + "/" + string(total_tests))
    if total_passed == total_tests {
        print("ALL TESTS PASSED")
    } else {
        print("FAILURES: " + string(total_tests - total_passed))
    }
}
```

## Important Reminders

1. **Read CLAUDE.md carefully** — it has the complete NAAb syntax reference
2. **govern.json is strict** — violations will block execution
3. **All functions must have real logic** — governance detects stubs, hardcoded returns, and simulation markers
4. **Value semantics** — re-assign dicts/arrays to parents after mutation (see CLAUDE.md)
5. **Python polyglot** — start at column 0, NO `return`, use `-> JSON` for structured data
6. **Shell polyglot** — stdout is the return value
7. **Variable binding** — list all NAAb variables used in polyglot blocks: `<<python[var1, var2]`
8. **`dict.get()` not `dict["key"]`** — bracket access throws on missing keys
9. **Struct instantiation needs `new`** — `let p = new Point { x: 1, y: 2 }`
10. **`>>` must be on its own line** — polyglot blocks are always multiline

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
