# Task: Implement an Enterprise Supply Chain Management System in NAAb

You are implementing a complete supply chain management system in NAAb, a polyglot programming language. Read the CLAUDE.md file in this directory for the full NAAb language reference, stdlib documentation, and project specifications.

## What You Must Create

Create these 7 files in the current directory. Every function listed below must be implemented with real business logic — governance will block stubs, placeholders, and hardcoded results.

**Governance is STRICT in this project:**
- Variable binding on ALL polyglot blocks is **HARD enforced** — `<<python[var1, var2]` syntax required
- Chained `.get()` calls are **HARD blocked** — split into separate `let` + null check
- 17 function contracts enforce return dict keys
- All scanner hard checks will block execution

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
Returns dict with keys: id, name, sku, category, unit_cost, selling_price, min_stock, max_stock, lead_time_days, reorder_point. Cast values to proper types (int for id/stock values, float for costs, string for name/sku/category).

`export fn create_supplier(id, name, rating, lead_time_avg, lead_time_variance, fill_rate, defect_rate, active)`
Returns dict with keys: id, name, rating, lead_time_avg, lead_time_variance, fill_rate, defect_rate, active. Cast to proper types.

`export fn create_transaction(product_id, quantity, tx_type, reason)`
Returns dict with keys: product_id, quantity, type, timestamp, reason.
- `timestamp`: use a shell polyglot block with variable binding: `<<shell[] date '+%Y-%m-%d %H:%M:%S' >>`
  (No NAAb variables needed, but the `[]` binding is required by governance)

`export fn validate_product(product)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, name non-empty, sku non-empty, unit_cost >= 0, selling_price >= unit_cost, min_stock >= 0, max_stock >= min_stock, lead_time_days > 0, reorder_point >= min_stock.
Each failed check adds a descriptive string to errors array.

`export fn validate_supplier(supplier)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, name non-empty, rating between 0.0 and 5.0, lead_time_avg > 0, fill_rate between 0.0 and 1.0, defect_rate between 0.0 and 1.0.

`export fn status_name(status)` — Returns string for OrderStatus enum value via if/else chain.
`export fn transaction_type_name(tx_type)` — Returns string for TransactionType enum value.
`export fn category_name(cat)` — Returns string for Category enum value.

**Required imports:** `use string`, `use array`

---

## File 2: inventory.naab

Core inventory management. Inventory is a dict mapping product_id (as string key) to quantity (int).

**Required imports:** `use array`, `use math`, `use string`
**Required file import:** `import "./models.naab" as models`

`export fn add_stock(inventory, product_id, quantity, tx_type)`
- Add quantity to inventory[product_id] (create entry if missing, use `inventory.set()`)
- Create a transaction record via models.create_transaction
- Return dict: `{ "inventory": updated_inventory, "transaction": transaction_record }`

`export fn remove_stock(inventory, product_id, quantity, tx_type)`
- Check if sufficient stock exists (use `inventory.get(string(product_id), 0)`)
- If yes: subtract quantity, create transaction, return `{ "inventory": ..., "transaction": ..., "success": true }`
- If no: return `{ "inventory": inventory, "transaction": null, "success": false }`
- If product not in inventory at all: return `{ "inventory": inventory, "transaction": null, "success": false }`

`export fn get_stock_level(inventory, product_id)`
- Return `inventory.get(string(product_id), 0)` — the quantity, or 0 if not found

`export fn get_inventory_value(inventory, products)`
- For each product, multiply stock level by unit_cost
- Sum all values and return the total as float

`export fn check_reorder_needed(inventory, products)`
- Return array of product dicts where current stock < product's reorder_point

`export fn calculate_turnover_rate(transactions, inventory, products, days)`
- For each product: count total units sold (where transaction type == TransactionType.Sale)
- If days == 0 or current_stock == 0: turnover = 0.0
- Otherwise: turnover = (units_sold / current_stock) * (365.0 / float(days)) — annualized
- Return dict mapping product_id (string) to turnover rate (float)

`export fn adjust_stock(inventory, product_id, actual_count, reason)`
- Set inventory[product_id] = actual_count (use `inventory.set()`)
- Calculate difference from previous count
- Create an Adjustment transaction
- Return `{ "inventory": ..., "adjustment_transaction": ..., "difference": int }`

---

## File 3: suppliers.naab

Supplier scoring, ranking, and risk assessment.

**Required imports:** `use array`, `use math`, `use string`, `use json`

`export fn score_supplier(supplier, order_history)`
- Filter order_history to this supplier's orders (where `order.get("supplier_id") == supplier.get("id")`)
- **delivery_score** (0-30): Among delivered orders, count those where `delivery_days <= lead_time_avg + 2`. delivery_score = (on_time_count / total_delivered) * 30. If no delivered orders: 0.
- **quality_score** (0-25): `(1.0 - float(supplier.get("defect_rate"))) * 25.0`
- **fill_rate_score** (0-25): `float(supplier.get("fill_rate")) * 25.0`
- **price_score** (0-20): If supplier has orders, avg_cost = sum of order totals / count. If avg_cost < 100: price_score = 20. Otherwise: price_score = min(20, 2000 / avg_cost). If no orders: 10.
- total_score = delivery + quality + fill_rate + price
- Return dict: `{ "supplier_id": ..., "total_score": ..., "delivery_score": ..., "quality_score": ..., "fill_rate_score": ..., "price_score": ... }`

`export fn rank_suppliers(suppliers, order_history)`
- Score all suppliers, sort by total_score descending
- Return array of score dicts (highest first)

`export fn evaluate_supplier_risk(supplier, order_history)`
- Count risk factors:
  - `defect_rate > 0.05` → reason: "High defect rate"
  - `fill_rate < 0.90` → reason: "Low fill rate"
  - `lead_time_variance > 3.0` → reason: "Unreliable lead times"
  - Fewer than 3 supplier orders in history → reason: "Insufficient order history"
  - `active == false` → reason: "Supplier inactive"
- risk_level: 0 reasons = "low", 1 = "medium", 2 = "high", 3+ = "critical"
- recommendation: "low"→"Maintain relationship", "medium"→"Monitor closely", "high"→"Seek alternatives", "critical"→"Replace immediately"
- Return dict: `{ "supplier_id": ..., "risk_level": ..., "reasons": [...], "recommendation": ... }`

`export fn get_preferred_supplier(product_id, suppliers, order_history)`
- Filter to active suppliers only (`supplier.get("active") == true`)
- Score each active supplier
- Return the supplier dict (not score dict) with highest total_score
- If no active suppliers, return null

`export fn calculate_supplier_lead_time(supplier, order_history)`
- Filter to this supplier's delivered orders
- Use Python polyglot with `-> JSON` and variable binding to compute statistics
- Return dict: `{ "avg_days": float, "std_dev": float, "min_days": float, "max_days": float, "sample_size": int }`
- If no delivered orders: return `{ "avg_days": supplier.lead_time_avg, "std_dev": 0.0, "min_days": 0.0, "max_days": 0.0, "sample_size": 0 }`

---

## File 4: orders.naab

Purchase order lifecycle management.

**Required imports:** `use array`, `use math`, `use string`
**Required file import:** `import "./models.naab" as models`, `import "./inventory.naab" as inv`

`export fn create_purchase_order(id, supplier_id, items, priority)`
- Calculate total from items: sum of (quantity * unit_price) for each item
- Get timestamp via shell polyglot with variable binding
- Return dict: `{ "id": ..., "supplier_id": ..., "items": items, "status": "pending", "total": calculated_total, "created_at": timestamp, "priority": priority }`

`export fn approve_order(order, approver_id)`
- Validate: `order.get("status") == "pending"` — if not, return dict with original order fields plus `"error": "Can only approve pending orders"`
- Set status to "approved", add approved_by and approved_at (shell timestamp with binding)
- Return updated order dict

`export fn receive_order(order, received_items, inventory)`
- For each received item: call `inv.add_stock(inventory, product_id, quantity_received, models.TransactionType.Purchase)`
- Update inventory after each add (value semantics!)
- Track discrepancies: for each item, compare ordered quantity vs received
- If ALL items fully received → status = "delivered"
- If any item partially received → status = "partially_received"
- Return dict: `{ "order": updated_order, "inventory": updated_inventory, "discrepancies": [...] }`

`export fn cancel_order(order, reason)`
- Cannot cancel if status is "delivered" or "partially_received" — return dict with `"error": "Cannot cancel delivered/received orders"`
- Set status to "cancelled", add cancel_reason and cancelled_at (shell timestamp with binding)
- Return updated order dict

`export fn calculate_order_total(order)`
- Sum (quantity * unit_price) for all items in order
- Return float total

`export fn get_order_history(orders, filters)`
- If filters is empty dict or has no matching keys: return all orders
- Filter by any combination of:
  - `filters.get("supplier_id")` — match exact
  - `filters.get("status")` — match exact
  - `filters.get("min_total")` — order total >= min
  - `filters.get("max_total")` — order total <= max
- Return filtered array

---

## File 5: forecasting.naab

Demand forecasting using Python polyglot for statistical computation.

**Required imports:** `use array`, `use math`, `use json`

`export fn forecast_demand(sales_history, periods, method)`
- If sales_history is empty: return `{ "forecast": [0.0 repeated periods times], "method": method, "periods": periods, "confidence": 0.0 }`
- Use Python polyglot with `-> JSON` pipe AND variable binding `<<python[sales_history, periods, method] -> JSON`
- **moving_average**: average ALL values in history, repeat that average for each forecast period
- **exponential_smoothing**: alpha = 2/(len(history)+1), iterate through history updating smoothed value, repeat final smoothed value for forecast
- **linear_trend**: least squares regression (slope + intercept), extrapolate for each forecast period
- confidence: `min(1.0, len(history) / 20.0) * (1.0 / (1.0 + sqrt(variance) / (mean + 1)))`
- Return dict: `{ "forecast": [array of floats], "method": method, "periods": periods, "confidence": float }`

**EXACT FORMULAS (tests will verify these):**
- moving_average([10, 12, 14], 1): avg = 12.0 → forecast = [12.0]
- exponential_smoothing([10, 12, 14], 1): alpha = 2/4 = 0.5, smoothed: 10 → 0.5*12+0.5*10=11 → 0.5*14+0.5*11=12.5 → forecast = [12.5]
- linear_trend([10, 12, 14], 1): x_avg=1, y_avg=12, slope=2.0, intercept=10.0, x=3 → 16.0

`export fn calculate_reorder_point(product, supplier, service_level)`
- avg_daily_demand = float(product.get("min_stock")) / float(product.get("lead_time_days"))
- If avg_daily_demand == 0: avg_daily_demand = 1.0
- lead_time = float(supplier.get("lead_time_avg"))
- Create synthetic demand history: [avg_daily_demand * 0.9, avg_daily_demand * 1.1, avg_daily_demand]
- safety_stock = call calculate_safety_stock(demand_history, lead_time, service_level)
- reorder_point = (avg_daily_demand * lead_time) + safety_stock
- Return dict: `{ "reorder_point": float, "safety_stock": float, "avg_demand": float, "lead_time": float }`

`export fn calculate_safety_stock(demand_history, lead_time_days, service_level)`
- If demand_history is empty: return 0.0
- Use Python polyglot with variable binding `<<python[demand_history, lead_time_days, service_level]`
- z_score lookup: {0.90: 1.282, 0.95: 1.645, 0.99: 2.326}, default 1.645
- std_dev = sqrt(sum((x - mean)^2) / n)
- safety_stock = z_score * std_dev * sqrt(lead_time_days)
- Return float

`export fn calculate_eoq(annual_demand, order_cost, holding_cost)`
- If any input <= 0: return 0.0
- EOQ = sqrt(2 * annual_demand * order_cost / holding_cost)
- Return float

`export fn optimize_reorder_schedule(products, inventory, suppliers, budget)`
- For each product: check if stock < reorder_point
- Calculate urgency = (reorder_point - current_stock) / reorder_point
- Sort candidates by urgency descending (highest urgency first)
- For each candidate: order quantity = max_stock - current_stock, cost = quantity * unit_cost
- Respect budget: skip products whose order cost exceeds remaining budget
- Return dict: `{ "orders": [...], "total_cost": float, "products_covered": int }`

---

## File 6: reporting.naab

Reports, analytics, and data export.

**Required imports:** `use array`, `use math`, `use string`, `use csv`
**Required file import:** `import "./suppliers.naab" as sup` (for scorecard)

`export fn generate_inventory_report(inventory, products)`
- If products is empty: return `{ "report_text": "No products", "total_value": 0.0, "items_below_reorder": 0, "items_overstocked": 0 }`
- Build formatted text table with columns: SKU, Name, Stock, Min, Max, ROP, Value, Status
- Use string.pad_right() for column alignment
- Status: "LOW" if stock < reorder_point, "OVER" if stock > max_stock, "OK" otherwise
- Calculate total_value, count items_below_reorder, count items_overstocked
- Return dict: `{ "report_text": formatted_string, "total_value": float, "items_below_reorder": int, "items_overstocked": int }`

`export fn generate_supplier_scorecard(suppliers, order_history)`
- For each supplier: call sup.score_supplier to get scores
- Return dict mapping supplier name (string) to their score dict

`export fn generate_purchase_summary(orders, date_range)`
- If orders is empty: return `{ "total_spent": 0.0, "order_count": 0, "avg_order_value": 0.0, "by_status": {}, "by_priority": {} }`
- Aggregate: total_spent (sum of all order totals), order_count, avg_order_value = total / count
- Group by status (count per status string)
- Group by priority (count per priority string)
- Return dict: `{ "total_spent": ..., "order_count": ..., "avg_order_value": ..., "by_status": {...}, "by_priority": {...} }`

`export fn export_to_csv(data, headers)`
- Prepend headers row to data
- Use csv.stringify() to generate CSV string
- Return the CSV string

`export fn calculate_kpis(inventory, orders, transactions, products)`
- If orders is empty: fill_rate = 0.0, avg_order_value = 0.0
- fill_rate: count orders with status "delivered" / len(orders)
- stockout_rate: count products where stock < reorder_point / len(products). If no products: 0.0
- inventory_turns: sum sale quantities from transactions / total inventory value. If value is 0: 0.0
- avg_order_value: sum order totals / len(orders)
- Return dict: `{ "fill_rate": float, "stockout_rate": float, "inventory_turns": float, "avg_order_value": float }`

NOTE: `calculate_kpis` takes 4 parameters (inventory, orders, transactions, products). The 4th is needed for stockout_rate.

---

## File 7: main.naab — Test Orchestrator (80 tests)

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
import "./forecasting.naab" as forecaster
import "./reporting.naab" as report
```

### test_models() — 10 tests

1. **create_product returns all 10 required keys**
   ```
   let p = models.create_product(1, "Widget", "WDG-001", "FinishedGood", 10.0, 25.0, 5, 50, 3, 10)
   // Verify: p.has("id") and p.has("name") and ... all 10 keys
   ```

2. **create_product values are correctly typed**
   ```
   // int(p.get("id")) == 1 and string(p.get("sku")) == "WDG-001" and float(p.get("unit_cost")) == 10.0
   ```

3. **create_supplier returns all 8 required keys**
   ```
   let s = models.create_supplier(1, "AcmeCorp", 4.5, 5.0, 1.0, 0.95, 0.02, true)
   // Verify all 8 keys present
   ```

4. **create_supplier boundary values preserved**
   ```
   let s0 = models.create_supplier(2, "Zero", 0.0, 1.0, 0.0, 0.0, 0.0, false)
   // float(s0.get("rating")) == 0.0 and bool(s0.get("active")) == false
   ```

5. **create_transaction has non-empty timestamp**
   ```
   let tx = models.create_transaction(1, 10, models.TransactionType.Purchase, "Restock")
   // string(tx.get("timestamp")).length() > 0
   ```

6. **validate_product passes for valid product**
   ```
   let vp = models.validate_product(p)
   // bool(vp.get("valid")) == true and len(vp.get("errors")) == 0
   ```

7. **validate_product catches multiple errors**
   ```
   let bad = models.create_product(-1, "", "", "X", -5.0, -10.0, -1, -5, 0, -2)
   let vb = models.validate_product(bad)
   // bool(vb.get("valid")) == false and len(vb.get("errors")) >= 5
   // At least: id <= 0, name empty, sku empty, unit_cost < 0, lead_time_days <= 0
   ```

8. **validate_supplier catches out-of-range values**
   ```
   let bad_s = models.create_supplier(0, "", 6.0, -1.0, 0.0, 1.5, -0.1, true)
   let vs = models.validate_supplier(bad_s)
   // bool(vs.get("valid")) == false and len(vs.get("errors")) >= 4
   ```

9. **status_name returns correct strings for all 5 statuses**
   ```
   // models.status_name(models.OrderStatus.Pending) == "Pending"
   // models.status_name(models.OrderStatus.Delivered) == "Delivered"
   // models.status_name(models.OrderStatus.Cancelled) == "Cancelled"
   ```

10. **category_name and transaction_type_name work**
    ```
    // models.category_name(models.Category.RawMaterial) == "RawMaterial"
    // models.transaction_type_name(models.TransactionType.Sale) == "Sale"
    ```

### test_inventory() — 10 tests

1. **add_stock to empty inventory creates entry with correct quantity**
   ```
   let inv_data = {}
   let r1 = inv.add_stock(inv_data, 1, 20, models.TransactionType.Purchase)
   inv_data = r1.get("inventory")
   // int(inv_data.get("1")) == 20
   ```

2. **add_stock to existing inventory adds correctly**
   ```
   let r2 = inv.add_stock(inv_data, 1, 15, models.TransactionType.Purchase)
   inv_data = r2.get("inventory")
   // int(inv_data.get("1")) == 35
   ```

3. **remove_stock succeeds and returns correct remaining**
   ```
   let r3 = inv.remove_stock(inv_data, 1, 10, models.TransactionType.Sale)
   inv_data = r3.get("inventory")
   // bool(r3.get("success")) == true and int(inv_data.get("1")) == 25
   ```

4. **remove_stock fails with insufficient stock**
   ```
   let r4 = inv.remove_stock(inv_data, 1, 100, models.TransactionType.Sale)
   // bool(r4.get("success")) == false and int(inv_data.get("1")) == 25
   ```

5. **get_stock_level returns 0 for missing product**
   ```
   // inv.get_stock_level(inv_data, 999) == 0
   ```

6. **get_inventory_value computes exactly: 25 units * 10.0 = 250.0**
   ```
   let products = [models.create_product(1, "P1", "S1", "Raw", 10.0, 20.0, 5, 50, 3, 10)]
   // inv.get_inventory_value(inv_data, products) == 250.0
   ```

7. **check_reorder_needed identifies products below reorder point**
   ```
   // inv_data has product 1 at 25, reorder_point is 10 → not needed
   // Add product 2 at 0 stock, reorder_point 15 → needed
   let products2 = [
       models.create_product(1, "P1", "S1", "Raw", 10.0, 20.0, 5, 50, 3, 10),
       models.create_product(2, "P2", "S2", "Raw", 5.0, 10.0, 10, 100, 5, 15)
   ]
   let needed = inv.check_reorder_needed(inv_data, products2)
   // len(needed) == 1 and int(needed[0].get("id")) == 2
   ```

8. **calculate_turnover_rate: 5 units sold, 25 in stock, 30 days → (5/25)*(365/30) = 2.433...**
   ```
   let txs = [models.create_transaction(1, 5, models.TransactionType.Sale, "Sold")]
   let rates = inv.calculate_turnover_rate(txs, inv_data, products, 30)
   // Verify: rate for product "1" is approximately 2.43 (int(round(float(rates.get("1")) * 100)) == 243)
   ```

9. **adjust_stock sets correct count and reports difference**
   ```
   let r_adj = inv.adjust_stock(inv_data, 1, 30, "Physical count")
   inv_data = r_adj.get("inventory")
   // int(inv_data.get("1")) == 30 and int(r_adj.get("difference")) == 5
   ```

10. **adjust_stock difference is negative when count decreased**
    ```
    let r_adj2 = inv.adjust_stock(inv_data, 1, 20, "Shrinkage")
    inv_data = r_adj2.get("inventory")
    // int(r_adj2.get("difference")) == -10
    ```

### test_suppliers() — 10 tests

All tests use these exact inputs:
```
let good_s = models.create_supplier(1, "TopCorp", 4.8, 5.0, 0.5, 0.98, 0.01, true)
let bad_s = models.create_supplier(2, "BadCorp", 1.5, 10.0, 5.0, 0.60, 0.15, true)
let history = [
    { "supplier_id": 1, "status": "delivered", "delivery_days": 5, "total": 100.0 },
    { "supplier_id": 1, "status": "delivered", "delivery_days": 6, "total": 200.0 },
    { "supplier_id": 1, "status": "delivered", "delivery_days": 5, "total": 150.0 }
]
```

1. **score_supplier returns all 6 required keys**
   ```
   let sc = sup.score_supplier(good_s, history)
   // sc.has("total_score") and sc.has("delivery_score") and sc.has("quality_score") and sc.has("fill_rate_score") and sc.has("price_score")
   ```

2. **score_supplier exact computation for good supplier**
   ```
   // delivery: all 3 orders have days <= 5.0 + 2 = 7.0 → 3/3 * 30 = 30.0
   // quality: (1 - 0.01) * 25 = 24.75
   // fill_rate: 0.98 * 25 = 24.5
   // price: avg_cost = (100+200+150)/3 = 150.0 → min(20, 2000/150) = 13.33
   // total = 30 + 24.75 + 24.5 + 13.33 = 92.58
   // Test: int(round(float(sc.get("total_score")))) == 93
   ```

3. **score_supplier: poor supplier with no orders scores low**
   ```
   let sc_bad = sup.score_supplier(bad_s, history)
   // bad_s has supplier_id 2, no orders in history match
   // delivery: 0 (no orders), quality: (1-0.15)*25=21.25, fill_rate: 0.60*25=15.0, price: 10 (default)
   // total = 0 + 21.25 + 15.0 + 10.0 = 46.25
   // Test: int(round(float(sc_bad.get("total_score")))) == 46
   ```

4. **rank_suppliers: good_s ranked before bad_s**
   ```
   let ranked = sup.rank_suppliers([bad_s, good_s], history)
   // int(ranked[0].get("supplier_id")) == 1 (good_s first despite input order)
   ```

5. **evaluate_supplier_risk: good supplier = "low"**
   ```
   let risk_good = sup.evaluate_supplier_risk(good_s, history)
   // risk_good.get("risk_level") == "low" and len(risk_good.get("reasons")) == 0
   ```

6. **evaluate_supplier_risk: bad supplier = "critical" (3+ reasons)**
   ```
   let risk_bad = sup.evaluate_supplier_risk(bad_s, history)
   // bad_s: defect_rate 0.15 > 0.05 ✓, fill_rate 0.60 < 0.90 ✓, lead_time_variance 5.0 > 3.0 ✓, 0 orders < 3 ✓
   // 4 reasons → "critical"
   // risk_bad.get("risk_level") == "critical" and len(risk_bad.get("reasons")) >= 3
   ```

7. **evaluate_supplier_risk recommendation matches level**
   ```
   // risk_bad.get("recommendation") == "Replace immediately"
   // risk_good.get("recommendation") == "Maintain relationship"
   ```

8. **get_preferred_supplier returns good_s (highest active scorer)**
   ```
   let pref = sup.get_preferred_supplier(1, [bad_s, good_s], history)
   // int(pref.get("id")) == 1
   ```

9. **get_preferred_supplier skips inactive suppliers**
   ```
   let inactive = models.create_supplier(3, "Dead", 5.0, 1.0, 0.0, 1.0, 0.0, false)
   let pref2 = sup.get_preferred_supplier(1, [inactive, bad_s], history)
   // pref2 != null and int(pref2.get("id")) == 2 (bad_s is active, inactive is skipped)
   ```

10. **calculate_supplier_lead_time: exact stats from 3 deliveries**
    ```
    let lt = sup.calculate_supplier_lead_time(good_s, history)
    // delivery_days: [5, 6, 5] → avg=5.333, std_dev≈0.471, min=5, max=6, sample=3
    // int(round(float(lt.get("avg_days")) * 10)) == 53  (5.333 * 10 rounded)
    // int(lt.get("sample_size")) == 3
    ```

### test_orders() — 10 tests

1. **create_purchase_order calculates total: 2 items → 10*5 + 20*3 = 110.0**
   ```
   let items = [
       { "product_id": 1, "quantity": 10, "unit_price": 5.0 },
       { "product_id": 2, "quantity": 20, "unit_price": 3.0 }
   ]
   let po = orders.create_purchase_order(1, 101, items, "high")
   // float(po.get("total")) == 110.0
   ```

2. **create_purchase_order status is "pending"**
   ```
   // po.get("status") == "pending"
   ```

3. **approve_order transitions to "approved"**
   ```
   po = orders.approve_order(po, "ADMIN-1")
   // po.get("status") == "approved" and po.has("approved_by")
   ```

4. **approve_order rejects non-pending (already approved)**
   ```
   let dup = orders.approve_order(po, "ADMIN-2")
   // dup.has("error")
   ```

5. **receive_order full delivery: inventory updated, status "delivered"**
   ```
   let inventory = { "1": 5, "2": 10 }
   let received = [
       { "product_id": 1, "quantity_received": 10 },
       { "product_id": 2, "quantity_received": 20 }
   ]
   let res = orders.receive_order(po, received, inventory)
   inventory = res.get("inventory")
   // int(inventory.get("1")) == 15 and int(inventory.get("2")) == 30
   let recv_order = res.get("order")
   // recv_order.get("status") == "delivered"
   ```

6. **receive_order partial delivery: discrepancies reported**
   ```
   let po2 = orders.create_purchase_order(2, 101, items, "normal")
   po2 = orders.approve_order(po2, "ADMIN-1")
   let partial = [{ "product_id": 1, "quantity_received": 5 }, { "product_id": 2, "quantity_received": 20 }]
   let res2 = orders.receive_order(po2, partial, inventory)
   // len(res2.get("discrepancies")) > 0 (product 1: ordered 10, received 5)
   let recv2 = res2.get("order")
   // recv2.get("status") == "partially_received"
   ```

7. **cancel_order on pending succeeds**
   ```
   let po3 = orders.create_purchase_order(3, 102, items, "low")
   po3 = orders.cancel_order(po3, "Budget cut")
   // po3.get("status") == "cancelled" and po3.has("cancel_reason")
   ```

8. **cancel_order on delivered fails with error**
   ```
   // recv_order has status "delivered"
   let cant_cancel = orders.cancel_order(recv_order, "Too late")
   // cant_cancel.has("error")
   ```

9. **calculate_order_total matches: 10*5 + 20*3 = 110.0**
   ```
   // orders.calculate_order_total(po) == 110.0
   ```

10. **get_order_history filters by status**
    ```
    let all_orders = [po, recv2, po3]
    let delivered = orders.get_order_history(all_orders, { "status": "delivered" })
    // len(delivered) == 1
    let all_unfiltered = orders.get_order_history(all_orders, {})
    // len(all_unfiltered) == 3
    ```

### test_forecasting() — 10 tests

1. **moving_average([10, 12, 14], 2): avg=12.0 → [12.0, 12.0]**
   ```
   let f1 = forecaster.forecast_demand([10.0, 12.0, 14.0], 2, "moving_average")
   // len(f1.get("forecast")) == 2
   // int(round(float(f1.get("forecast")[0]))) == 12
   ```

2. **exponential_smoothing([10, 12, 14], 1): alpha=0.5, result=12.5**
   ```
   let f2 = forecaster.forecast_demand([10.0, 12.0, 14.0], 1, "exponential_smoothing")
   // int(round(float(f2.get("forecast")[0]) * 10)) == 125  (12.5 * 10)
   ```

3. **linear_trend([10, 12, 14], 1): slope=2, next=16.0**
   ```
   let f3 = forecaster.forecast_demand([10.0, 12.0, 14.0], 1, "linear_trend")
   // int(round(float(f3.get("forecast")[0]))) == 16
   ```

4. **confidence is between 0 and 1**
   ```
   // float(f3.get("confidence")) >= 0.0 and float(f3.get("confidence")) <= 1.0
   ```

5. **calculate_reorder_point returns all 4 keys**
   ```
   let prod = models.create_product(1, "P", "S", "Raw", 10.0, 20.0, 10, 50, 5, 15)
   let supp = models.create_supplier(1, "S", 4.0, 5.0, 1.0, 0.95, 0.02, true)
   let rop = forecaster.calculate_reorder_point(prod, supp, 0.95)
   // rop.has("reorder_point") and rop.has("safety_stock") and rop.has("avg_demand") and rop.has("lead_time")
   ```

6. **higher service level → higher reorder point**
   ```
   let rop99 = forecaster.calculate_reorder_point(prod, supp, 0.99)
   // float(rop99.get("reorder_point")) > float(rop.get("reorder_point"))
   ```

7. **calculate_safety_stock positive for normal data**
   ```
   let ss = forecaster.calculate_safety_stock([10.0, 12.0, 14.0], 5, 0.95)
   // ss > 0.0
   ```

8. **calculate_safety_stock: higher variance → higher stock**
   ```
   let ss_v = forecaster.calculate_safety_stock([5.0, 25.0], 5, 0.95)
   // ss_v > ss (variance of [5,25] >> variance of [10,12,14])
   ```

9. **calculate_eoq(1000, 50, 2) = sqrt(50000) ≈ 223.607 → int(round) == 224**
   ```
   let eoq = forecaster.calculate_eoq(1000.0, 50.0, 2.0)
   // int(round(eoq)) == 224
   ```

10. **optimize_reorder_schedule: budget 500, product needs 40 units @ 10.0 = 400 → fits**
    ```
    let prods = [models.create_product(1, "P", "S", "Raw", 10.0, 20.0, 5, 50, 3, 20)]
    let inv_sched = { "1": 5 }  // 5 < reorder_point 20
    let sched = forecaster.optimize_reorder_schedule(prods, inv_sched, [], 500.0)
    // int(sched.get("products_covered")) == 1
    // float(sched.get("total_cost")) <= 500.0
    ```

### test_reporting() — 10 tests

1. **generate_inventory_report returns all 4 keys**
   ```
   let prods = [
       models.create_product(1, "Widget", "WDG", "Raw", 10.0, 20.0, 5, 50, 3, 20),
       models.create_product(2, "Gadget", "GDG", "Raw", 25.0, 50.0, 10, 100, 5, 30)
   ]
   let inv_data = { "1": 15, "2": 120 }
   let rep = report.generate_inventory_report(inv_data, prods)
   // rep.has("report_text") and rep.has("total_value") and rep.has("items_below_reorder") and rep.has("items_overstocked")
   ```

2. **report_text contains column headers**
   ```
   // string(rep.get("report_text")).contains("SKU")
   ```

3. **total_value exact: 15*10 + 120*25 = 150 + 3000 = 3150.0**
   ```
   // float(rep.get("total_value")) == 3150.0
   ```

4. **items_below_reorder: product 1 (15 < 20) = 1 item low**
   ```
   // int(rep.get("items_below_reorder")) == 1
   ```

5. **items_overstocked: product 2 (120 > 100) = 1 item over**
   ```
   // int(rep.get("items_overstocked")) == 1
   ```

6. **generate_supplier_scorecard returns per-supplier data**
   ```
   let supps = [models.create_supplier(1, "Acme", 4.0, 5.0, 1.0, 0.90, 0.05, true)]
   let sc = report.generate_supplier_scorecard(supps, [])
   // sc.has("Acme")
   ```

7. **generate_purchase_summary exact aggregation**
   ```
   let ord_list = [
       { "total": 100.0, "status": "delivered", "priority": "high" },
       { "total": 200.0, "status": "pending", "priority": "normal" }
   ]
   let summ = report.generate_purchase_summary(ord_list, {})
   // float(summ.get("total_spent")) == 300.0
   // int(summ.get("order_count")) == 2
   // float(summ.get("avg_order_value")) == 150.0
   ```

8. **purchase_summary groups by status correctly**
   ```
   let by_status = summ.get("by_status")
   // int(by_status.get("delivered")) == 1 and int(by_status.get("pending")) == 1
   ```

9. **export_to_csv includes headers and data**
   ```
   let csv_out = report.export_to_csv([["A1", "B1"], ["A2", "B2"]], ["ColA", "ColB"])
   // csv_out.contains("ColA") and csv_out.contains("A1")
   ```

10. **calculate_kpis exact: 1 delivered / 2 orders → fill_rate = 0.5**
    ```
    let kpis = report.calculate_kpis(inv_data, ord_list, [], prods)
    // float(kpis.get("fill_rate")) == 0.5
    // float(kpis.get("avg_order_value")) == 150.0
    ```

### test_edge_cases() — 10 tests

1. **forecast_demand with empty history returns zeros**
   ```
   let f_empty = forecaster.forecast_demand([], 3, "moving_average")
   // len(f_empty.get("forecast")) == 3
   // float(f_empty.get("forecast")[0]) == 0.0 and float(f_empty.get("confidence")) == 0.0
   ```

2. **calculate_safety_stock with empty history returns 0.0**
   ```
   // forecaster.calculate_safety_stock([], 5, 0.95) == 0.0
   ```

3. **remove_stock on product not in inventory returns success=false**
   ```
   let empty_inv = {}
   let r = inv.remove_stock(empty_inv, 999, 10, models.TransactionType.Sale)
   // bool(r.get("success")) == false
   ```

4. **calculate_turnover_rate with 0 days returns 0 for all products**
   ```
   let inv_t = { "1": 10 }
   let prods_t = [models.create_product(1, "P", "S", "Raw", 10.0, 20.0, 5, 50, 3, 10)]
   let txs_t = [models.create_transaction(1, 5, models.TransactionType.Sale, "Sale")]
   let rates = inv.calculate_turnover_rate(txs_t, inv_t, prods_t, 0)
   // float(rates.get("1")) == 0.0
   ```

5. **score_supplier with empty history: delivery=0, price=10 default**
   ```
   let s_empty = models.create_supplier(1, "New", 3.0, 5.0, 1.0, 0.90, 0.03, true)
   let sc = sup.score_supplier(s_empty, [])
   // float(sc.get("delivery_score")) == 0.0
   // quality: (1-0.03)*25=24.25, fill_rate: 0.90*25=22.5, price: 10
   // total = 0 + 24.25 + 22.5 + 10 = 56.75
   // int(round(float(sc.get("total_score")))) == 57
   ```

6. **get_order_history with empty filters returns all**
   ```
   let all = [{ "id": 1, "status": "pending", "total": 50.0 }, { "id": 2, "status": "delivered", "total": 100.0 }]
   let result = orders.get_order_history(all, {})
   // len(result) == 2
   ```

7. **generate_inventory_report with empty products**
   ```
   let empty_rep = report.generate_inventory_report({}, [])
   // int(empty_rep.get("items_below_reorder")) == 0 and float(empty_rep.get("total_value")) == 0.0
   ```

8. **calculate_kpis with empty orders**
   ```
   let kpis_empty = report.calculate_kpis({}, [], [], [])
   // float(kpis_empty.get("fill_rate")) == 0.0 and float(kpis_empty.get("avg_order_value")) == 0.0
   ```

9. **validate_product catches all-zero/negative values**
   ```
   let zero_p = models.create_product(0, "", "", "X", 0.0, 0.0, 0, 0, 0, 0)
   let vz = models.validate_product(zero_p)
   // bool(vz.get("valid")) == false and len(vz.get("errors")) >= 3
   ```

10. **cancel_order on already cancelled returns error**
    ```
    let cancelled_po = { "id": 99, "status": "cancelled", "total": 0.0 }
    let re_cancel = orders.cancel_order(cancelled_po, "Again")
    // re_cancel.has("error")
    ```

### test_integration() — 10 tests

1. **Full PO lifecycle: stock matches after create→stock→PO→approve→receive**
   ```
   let p1 = models.create_product(1, "IntP1", "IP1", "Raw", 10.0, 20.0, 5, 50, 3, 15)
   let integration_inv = {}
   let add_r = inv.add_stock(integration_inv, 1, 10, models.TransactionType.Purchase)
   integration_inv = add_r.get("inventory")
   // Start: 10 units
   let po_items = [{ "product_id": 1, "quantity": 20, "unit_price": 10.0 }]
   let po = orders.create_purchase_order(1, 1, po_items, "high")
   po = orders.approve_order(po, "MGR")
   let recv = orders.receive_order(po, [{ "product_id": 1, "quantity_received": 20 }], integration_inv)
   integration_inv = recv.get("inventory")
   // Final: 10 + 20 = 30
   // int(integration_inv.get("1")) == 30
   ```

2. **Supplier rank matches score ordering**
   ```
   let s1 = models.create_supplier(1, "Best", 4.5, 3.0, 0.5, 0.99, 0.01, true)
   let s2 = models.create_supplier(2, "Worst", 2.0, 8.0, 4.0, 0.70, 0.10, true)
   let oh = [{ "supplier_id": 1, "status": "delivered", "delivery_days": 3, "total": 100.0 }]
   let ranked = sup.rank_suppliers([s2, s1], oh)
   let sc1 = sup.score_supplier(s1, oh)
   // float(ranked[0].get("total_score")) == float(sc1.get("total_score"))
   ```

3. **Forecast + reorder point + schedule: products_covered > 0**
   ```
   let fp = models.create_product(1, "FP", "FP1", "Raw", 10.0, 20.0, 10, 50, 5, 20)
   let fs = models.create_supplier(1, "FS", 4.0, 5.0, 1.0, 0.95, 0.02, true)
   let rop = forecaster.calculate_reorder_point(fp, fs, 0.95)
   // rop.has("reorder_point")
   let f_inv = { "1": 5 }  // well below reorder point
   let schedule = forecaster.optimize_reorder_schedule([fp], f_inv, [fs], 1000.0)
   // int(schedule.get("products_covered")) >= 1
   ```

4. **Multiple add/remove: net sum matches final stock**
   ```
   let net_inv = {}
   let a1 = inv.add_stock(net_inv, 1, 100, models.TransactionType.Purchase)
   net_inv = a1.get("inventory")
   let r1 = inv.remove_stock(net_inv, 1, 30, models.TransactionType.Sale)
   net_inv = r1.get("inventory")
   let a2 = inv.add_stock(net_inv, 1, 50, models.TransactionType.Purchase)
   net_inv = a2.get("inventory")
   let r2 = inv.remove_stock(net_inv, 1, 20, models.TransactionType.Sale)
   net_inv = r2.get("inventory")
   // 100 - 30 + 50 - 20 = 100
   // int(net_inv.get("1")) == 100
   ```

5. **Cancel order does NOT change inventory**
   ```
   let cancel_inv = { "1": 50 }
   let cancel_po = orders.create_purchase_order(10, 1, [{ "product_id": 1, "quantity": 100, "unit_price": 5.0 }], "normal")
   cancel_po = orders.cancel_order(cancel_po, "Changed mind")
   // int(cancel_inv.get("1")) == 50  (unchanged)
   ```

6. **Budget constraint: total_cost never exceeds budget**
   ```
   let budget_prods = [
       models.create_product(1, "Exp", "E1", "Raw", 100.0, 200.0, 5, 50, 3, 20),
       models.create_product(2, "Cheap", "C1", "Raw", 5.0, 10.0, 5, 50, 3, 20)
   ]
   let budget_inv = { "1": 0, "2": 0 }
   let b_sched = forecaster.optimize_reorder_schedule(budget_prods, budget_inv, [], 300.0)
   // float(b_sched.get("total_cost")) <= 300.0
   ```

7. **CSV export contains all product SKUs from report data**
   ```
   let csv_data = [["WDG", "15", "OK"], ["GDG", "120", "OVER"]]
   let csv = report.export_to_csv(csv_data, ["SKU", "Stock", "Status"])
   // csv.contains("WDG") and csv.contains("GDG") and csv.contains("SKU")
   ```

8. **KPI fill_rate exact: 2 delivered out of 4 orders = 0.5**
   ```
   let kpi_orders = [
       { "total": 100.0, "status": "delivered", "priority": "high" },
       { "total": 200.0, "status": "delivered", "priority": "normal" },
       { "total": 150.0, "status": "pending", "priority": "high" },
       { "total": 50.0, "status": "cancelled", "priority": "low" }
   ]
   let kpi_prods = [models.create_product(1, "K", "K1", "Raw", 10.0, 20.0, 5, 50, 3, 10)]
   let kpi_inv = { "1": 20 }
   let kpis = report.calculate_kpis(kpi_inv, kpi_orders, [], kpi_prods)
   // float(kpis.get("fill_rate")) == 0.5
   ```

9. **Supplier risk escalation: bad data → high/critical risk**
   ```
   let risky = models.create_supplier(5, "Risky", 1.0, 15.0, 6.0, 0.50, 0.20, true)
   let risk = sup.evaluate_supplier_risk(risky, [])
   // risk.get("risk_level") == "critical"
   // len(risk.get("reasons")) >= 4
   ```

10. **Report total_value matches manual inventory value calculation**
    ```
    let val_prods = [
        models.create_product(1, "A", "A1", "Raw", 10.0, 20.0, 5, 50, 3, 10),
        models.create_product(2, "B", "B1", "Raw", 25.0, 50.0, 10, 100, 5, 30)
    ]
    let val_inv = { "1": 10, "2": 40 }
    let val_rep = report.generate_inventory_report(val_inv, val_prods)
    let manual_value = inv.get_inventory_value(val_inv, val_prods)
    // float(val_rep.get("total_value")) == float(manual_value)
    // manual: 10*10 + 40*25 = 100 + 1000 = 1100.0
    ```

### main block

```naab
main {
    let total_passed = 0
    let total_tests = 0

    let r1 = test_models()
    total_passed = total_passed + r1[0]
    total_tests = total_tests + r1[1]
    print("test_models: " + string(r1[0]) + "/" + string(r1[1]))

    let r2 = test_inventory()
    total_passed = total_passed + r2[0]
    total_tests = total_tests + r2[1]
    print("test_inventory: " + string(r2[0]) + "/" + string(r2[1]))

    let r3 = test_suppliers()
    total_passed = total_passed + r3[0]
    total_tests = total_tests + r3[1]
    print("test_suppliers: " + string(r3[0]) + "/" + string(r3[1]))

    let r4 = test_orders()
    total_passed = total_passed + r4[0]
    total_tests = total_tests + r4[1]
    print("test_orders: " + string(r4[0]) + "/" + string(r4[1]))

    let r5 = test_forecasting()
    total_passed = total_passed + r5[0]
    total_tests = total_tests + r5[1]
    print("test_forecasting: " + string(r5[0]) + "/" + string(r5[1]))

    let r6 = test_reporting()
    total_passed = total_passed + r6[0]
    total_tests = total_tests + r6[1]
    print("test_reporting: " + string(r6[0]) + "/" + string(r6[1]))

    let r7 = test_edge_cases()
    total_passed = total_passed + r7[0]
    total_tests = total_tests + r7[1]
    print("test_edge_cases: " + string(r7[0]) + "/" + string(r7[1]))

    let r8 = test_integration()
    total_passed = total_passed + r8[0]
    total_tests = total_tests + r8[1]
    print("test_integration: " + string(r8[0]) + "/" + string(r8[1]))

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
2. **govern.json is STRICT** — hard violations BLOCK execution entirely
3. **Variable binding is HARD** — every `<<python` and `<<shell` block must list variables: `<<python[var1, var2]` or `<<shell[]` (empty list if none needed)
4. **Chained `.get()` is HARD blocked** — `a.get("x").get("y")` will fail scanner. Instead: `let x = a.get("x")` then `if x != null { x.get("y") }`
5. **Use enums, not magic values** — use `models.TransactionType.Sale` not integer `1`, use `models.OrderStatus.Pending` not string `"pending"` where comparing against enum values
6. **Value semantics** — re-assign dicts/arrays to parents after mutation
7. **Python polyglot** — start at column 0, NO `return`, use `-> JSON` for structured data
8. **`dict.get()` not `dict["key"]`** — bracket access throws on missing keys
9. **All functions must have real logic** — governance detects stubs, hardcoded returns, simulation markers
10. **80 tests total** — 8 suites × 10 tests each. EVERY test must pass.
11. **Exact values matter** — tests check precise computed results, not just types or ranges
12. **Handle empty inputs gracefully** — edge case tests verify behavior with empty arrays, 0 values, missing products

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
