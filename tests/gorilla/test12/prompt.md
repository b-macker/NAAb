# Task: Implement a Fleet & Logistics Management System in NAAb

You are implementing a complete fleet and logistics management system in NAAb, a polyglot programming language. Read the CLAUDE.md file in this directory for the full NAAb language reference, stdlib documentation, and project specifications.

## What You Must Create

Create these 7 files in the current directory. Every function listed below must be implemented with real business logic — governance will block stubs, placeholders, and hardcoded results.

**Governance is STRICT in this project:**
- Variable binding on ALL polyglot blocks is **HARD enforced** — `<<python[var1, var2]` syntax required
- Chained `.get()` calls are **HARD blocked** — split into separate `let` + null check
- 15 function contracts enforce return dict keys
- All scanner hard checks will block execution

---

## File 1: models.naab

Domain models for the fleet management system.

**Top-level declarations (enums):**

```naab
enum VehicleStatus { Available, InTransit, Maintenance, Retired }
enum DeliveryStatus { Pending, Assigned, InProgress, Delivered, Failed }
enum FuelType { Diesel, Gasoline, Electric, Hybrid }
enum VehicleType { Van, Truck, Semi, Motorcycle }
```

**Exported functions:**

`export fn create_vehicle(id, plate, v_type, fuel_type, capacity_kg, mileage, fuel_efficiency, status)`
Returns dict with keys: id, plate, type, fuel_type, capacity_kg, mileage, fuel_efficiency, status.
Cast values to proper types (int for id/mileage, float for capacity_kg/fuel_efficiency, string for plate/type/fuel_type/status).

`export fn create_driver(id, name, license_class, hourly_rate, rating, total_hours, max_hours_per_week, available)`
Returns dict with keys: id, name, license_class, hourly_rate, rating, total_hours, max_hours_per_week, available.
Cast to proper types (int for id, float for hourly_rate/rating, string for name/license_class, bool for available).

`export fn create_delivery(id, origin, destination, weight_kg, priority, deadline)`
Returns dict with keys: id, origin, destination, weight_kg, priority, status, created_at, deadline.
- `status`: always starts as `"pending"`
- `created_at`: use a shell polyglot block with variable binding: `<<shell[] date '+%Y-%m-%d %H:%M:%S' >>`
  (No NAAb variables needed, but the `[]` binding is required by governance)

`export fn validate_vehicle(vehicle)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, plate non-empty, capacity_kg > 0, mileage >= 0, fuel_efficiency > 0.
Each failed check adds a descriptive string to errors array.

`export fn validate_driver(driver)`
Returns dict: `{ "valid": bool, "errors": [...] }`
Check: id > 0, name non-empty, hourly_rate > 0, rating between 0.0 and 5.0, max_hours_per_week > 0.

`export fn status_name(status)` — Returns string for VehicleStatus enum value via if/else chain.
`export fn delivery_status_name(status)` — Returns string for DeliveryStatus enum value via if/else chain.

**Required imports:** `use string`, `use array`

---

## File 2: vehicles.naab

Fleet vehicle management. Fleet is an array of vehicle dicts.

**Required imports:** `use array`, `use math`, `use string`
**Required file import:** `import "./models.naab" as models`

`export fn add_vehicle(fleet, vehicle)`
- Check for duplicate ID in fleet (loop through fleet, compare `v.get("id") == vehicle.get("id")`)
- If duplicate: return `{ "fleet": fleet, "success": false, "error": "Vehicle ID already exists" }`
- If new: push vehicle to fleet copy, return `{ "fleet": updated_fleet, "success": true }`

`export fn retire_vehicle(vehicle, reason)`
- Set status to "Retired", add retire_reason key
- Return updated vehicle dict

`export fn get_available_vehicles(fleet)`
- Return array of vehicles where `v.get("status") == "Available"`

`export fn assign_vehicle(fleet, vehicle_id)`
- Find vehicle by ID in fleet. If not found or not Available: return `{ "fleet": fleet, "vehicle": null, "success": false }`
- Set status to "InTransit", update fleet (value semantics!), return `{ "fleet": updated_fleet, "vehicle": updated_vehicle, "success": true }`

`export fn release_vehicle(fleet, vehicle_id)`
- Find vehicle by ID. If not found: return `{ "fleet": fleet, "success": false }`
- Set status to "Available", update fleet, return `{ "fleet": updated_fleet, "success": true }`

`export fn calculate_depreciation(vehicle, years)`
- Determine purchase_value based on vehicle type string:
  - "Van" or "Motorcycle": `float(vehicle.get("capacity_kg")) * 50.0`
  - "Truck" or "Semi": `float(vehicle.get("capacity_kg")) * 100.0`
  - Default: `float(vehicle.get("capacity_kg")) * 50.0`
- annual_depreciation = purchase_value * 0.09
- total_depreciation = annual_depreciation * float(years)
- Cap at 90% of purchase_value: `math.min(total_depreciation, purchase_value * 0.9)`
- Return float

`export fn get_maintenance_due(fleet, mileage_threshold)`
- Return array of vehicles where `int(v.get("mileage")) > mileage_threshold`

---

## File 3: drivers.naab

Driver management, scoring, and compliance tracking.

**Required imports:** `use array`, `use math`, `use string`, `use json`
**Required file import:** `import "./models.naab" as models`

`export fn register_driver(drivers, driver)`
- Check for duplicate ID. If duplicate: return `{ "drivers": drivers, "success": false, "error": "Driver ID already exists" }`
- If new: push driver, return `{ "drivers": updated_drivers, "success": true }`

`export fn assign_driver(drivers, driver_id)`
- Find driver by ID. If not found or not available: return `{ "drivers": drivers, "driver": null, "success": false }`
- Set available to false, update drivers array, return `{ "drivers": updated_drivers, "driver": updated_driver, "success": true }`

`export fn get_available_drivers(drivers)`
- Return array of drivers where `bool(d.get("available")) == true`

`export fn calculate_driver_score(driver, delivery_history)`
- Filter delivery_history to this driver's deliveries (where `d.get("driver_id") == driver.get("id")`)
- Count total deliveries, completed (status "delivered"), on_time (status "delivered" AND `bool(d.get("on_time", true))`)
- **delivery_score** (0-30): If total == 0: 0.0. Otherwise: (completed / total) * 30.0
- **reliability_score** (0-25): If completed == 0: 0.0. Otherwise: (on_time / completed) * 25.0
- **efficiency_score** (0-25): Use Python polyglot with `-> JSON` and variable binding `<<python[delivery_history, driver] -> JSON`
  - Filter to this driver's completed deliveries
  - If none: score = 0
  - Otherwise: compute average `fuel_used / expected_fuel` ratio from each delivery
  - deviation = abs(1.0 - avg_ratio)
  - score = max(0, 25 * (1 - deviation))
  - Return JSON: `json.dumps({"score": round(score, 2)})`
- **rating_score** (0-20): `(float(driver.get("rating")) / 5.0) * 20.0`
- total_score = delivery + reliability + efficiency + rating
- Return dict: `{ "driver_id": ..., "total_score": ..., "delivery_score": ..., "reliability_score": ..., "efficiency_score": ..., "rating_score": ... }`

`export fn update_driver_hours(driver, hours_worked)`
- Add hours_worked to driver's total_hours
- Return updated driver dict

`export fn check_hours_compliance(driver)`
- weekly_limit = int(driver.get("max_hours_per_week"))
- current_hours = int(driver.get("total_hours"))
- compliant = current_hours <= weekly_limit
- hours_remaining = if compliant then (weekly_limit - current_hours) else 0
- Return dict: `{ "compliant": bool, "hours_remaining": int, "weekly_limit": weekly_limit }`

---

## File 4: routes.naab

Route planning, cost calculation, and optimization.

**Required imports:** `use array`, `use math`, `use string`, `use json`

`export fn create_route_plan(vehicle, driver, stops, distances)`
- total_distance = sum of all values in distances array
- estimated_time = float(total_distance) / 60.0 + float(len(stops)) * 0.5
- Return dict: `{ "vehicle_id": vehicle.get("id"), "driver_id": driver.get("id"), "stops": stops, "distances": distances, "total_distance": total_distance, "estimated_hours": estimated_time, "status": "planned" }`

`export fn calculate_route_cost(route, vehicle, driver)`
- Get fuel_type string from vehicle
- fuel_price: "Diesel" = 1.50, "Gasoline" = 1.40, "Electric" = 0.30, "Hybrid" = 0.90, default = 1.40
- fuel_cost = float(route.get("total_distance")) / float(vehicle.get("fuel_efficiency")) * fuel_price
- driver_cost = float(route.get("estimated_hours")) * float(driver.get("hourly_rate"))
- total_cost = fuel_cost + driver_cost
- Return dict: `{ "fuel_cost": fuel_cost, "driver_cost": driver_cost, "total_cost": total_cost }`

`export fn optimize_route_order(stops, distances_matrix)`
- Use Python polyglot with `-> JSON` and variable binding `<<python[stops, distances_matrix] -> JSON`
- Implement nearest-neighbor TSP heuristic:
  - Start at stop index 0
  - At each step, visit the nearest unvisited stop
  - Track total distance and visited order
- Calculate savings: original_distance = sum of distances_matrix[i][i+1] for sequential order
- savings_percent = (original_distance - optimized_distance) / original_distance * 100 if original > 0 else 0
- Return JSON: `json.dumps({"optimized_stops": optimized_order, "total_distance": total_dist, "savings_percent": round(savings, 2)})`

`export fn estimate_delivery_time(distance_km, stops_count, priority)`
- base_time = float(distance_km) / 60.0 + float(stops_count) * 0.5
- If priority == "high": return base_time * 0.8
- If priority == "low": return base_time * 1.2
- Otherwise (normal): return base_time

`export fn check_route_capacity(route_deliveries, vehicle)`
- total_weight = sum of float(d.get("weight_kg")) for all deliveries in route_deliveries
- capacity = float(vehicle.get("capacity_kg"))
- utilization = if capacity > 0 then total_weight / capacity else 0.0
- fits = total_weight <= capacity
- Return dict: `{ "fits": fits, "total_weight": total_weight, "capacity": capacity, "utilization": utilization }`

`export fn get_route_history(routes, filters)`
- If filters is empty dict or has no matching keys: return all routes
- Filter by any combination of:
  - `filters.get("vehicle_id")` — match exact
  - `filters.get("driver_id")` — match exact
  - `filters.get("status")` — match exact
- Return filtered array

---

## File 5: deliveries.naab

Delivery lifecycle management.

**Required imports:** `use array`, `use string`
**Required file import:** `import "./models.naab" as models`

`export fn create_delivery_batch(deliveries, new_deliveries)`
- Append all new_deliveries to deliveries copy (loop and push each)
- Return dict: `{ "deliveries": updated_deliveries, "added_count": len(new_deliveries) }`

`export fn assign_delivery(delivery, route_id, driver_id)`
- Set status to "assigned"
- Add route_id and driver_id keys
- Add assigned_at timestamp via shell polyglot with binding `<<shell[] date '+%Y-%m-%d %H:%M:%S' >>`
- Return updated delivery dict

`export fn complete_delivery(delivery)`
- Set status to "delivered"
- Add completed_at timestamp via shell polyglot with binding
- Return updated delivery dict

`export fn fail_delivery(delivery, reason)`
- Set status to "failed"
- Add failure_reason key with reason string
- Add failed_at timestamp via shell polyglot with binding
- Return updated delivery dict

`export fn calculate_on_time_rate(deliveries)`
- Count delivered: deliveries where `d.get("status") == "delivered"`
- Count failed: deliveries where `d.get("status") == "failed"`
- total_completed = delivered + failed
- If total_completed == 0: return 0.0
- Return float(delivered) / float(total_completed)

`export fn get_deliveries_by_status(deliveries, status)`
- Return array of deliveries where `d.get("status") == status`

---

## File 6: reporting.naab

Fleet analytics, KPIs, and reports.

**Required imports:** `use array`, `use math`, `use string`, `use json`, `use csv`
**Required file import:** `import "./drivers.naab" as drv`

`export fn generate_fleet_report(fleet, routes)`
- Count vehicles by status: available, in_transit, maintenance
- Calculate avg_mileage = sum of all vehicle mileages / len(fleet). If empty fleet: 0.0
- Build formatted text table with columns: ID, Plate, Type, Status, Mileage
- Use string.pad_right() for column alignment
- Return dict: `{ "report_text": formatted_string, "total_vehicles": len(fleet), "available_count": int, "in_transit_count": int, "maintenance_count": int, "avg_mileage": float }`

`export fn calculate_fleet_kpis(fleet, routes, deliveries)`
- utilization_rate = count vehicles with status "InTransit" / len(fleet). If empty: 0.0
- delivered_count = deliveries with status "delivered"
- failed_count = deliveries with status "failed"
- delivery_success_rate = delivered / (delivered + failed). If none completed: 0.0
- avg_route_distance = sum of route total_distance / len(routes). If no routes: 0.0
- Return dict: `{ "utilization_rate": float, "delivery_success_rate": float, "avg_route_distance": float, "fleet_size": len(fleet) }`

`export fn generate_driver_scorecard(drivers, delivery_history)`
- For each driver: call drv.calculate_driver_score to get scores
- Return dict mapping driver name (string) to their score dict

`export fn calculate_fuel_efficiency(routes, fleet)`
- For each route, find matching vehicle in fleet by vehicle_id
- Calculate fuel_cost for each route: distance / vehicle.fuel_efficiency * fuel_price
  - fuel_price: "Diesel"=1.50, "Gasoline"=1.40, "Electric"=0.30, "Hybrid"=0.90
- Accumulate total_fuel_cost, total_distance
- Group by fuel_type: for each type, track {distance, cost}
- avg_cost_per_km = total_fuel_cost / total_distance. If no distance: 0.0
- For each fuel type entry: avg_cost_per_km = cost / distance. If 0: 0.0
- Return dict: `{ "total_fuel_cost": float, "total_distance": float, "avg_cost_per_km": float, "by_fuel_type": dict }`

`export fn export_delivery_csv(deliveries, headers)`
- Build data array: for each delivery, extract values matching headers order
- Prepend headers row to data
- Use csv.stringify() to generate CSV string
- Return the CSV string

`export fn forecast_maintenance_costs(fleet, months)`
- Use Python polyglot with `-> JSON` and variable binding `<<python[fleet, months] -> JSON`
- For each vehicle: monthly_cost = mileage * 0.02 (base maintenance rate)
- fleet_monthly_total = sum of all vehicle monthly costs
- forecast = array of fleet_monthly_total repeated for each month
- avg_monthly = fleet_monthly_total
- total = fleet_monthly_total * months
- Return JSON: `json.dumps({"forecast": forecast, "avg_monthly": avg_monthly, "total": total})`

---

## File 7: main.naab — Test Orchestrator (60 tests)

**Structure:**
```naab
use array
use math
use string
use json
use csv

import "./models.naab" as models
import "./vehicles.naab" as veh
import "./drivers.naab" as drv
import "./routes.naab" as routes
import "./deliveries.naab" as del
import "./reporting.naab" as report
```

### test_models() — 10 tests

1. **create_vehicle returns all 8 required keys**
   ```
   let v = models.create_vehicle(1, "ABC-123", "Van", "Diesel", 1000.0, 50000, 8.0, "Available")
   // Verify: v.has("id") and v.has("plate") and v.has("type") and v.has("fuel_type") and v.has("capacity_kg") and v.has("mileage") and v.has("fuel_efficiency") and v.has("status")
   ```

2. **create_vehicle values are correctly typed**
   ```
   // int(v.get("id")) == 1 and string(v.get("plate")) == "ABC-123" and float(v.get("capacity_kg")) == 1000.0
   ```

3. **create_driver returns all 8 required keys**
   ```
   let d = models.create_driver(1, "John Smith", "Class A", 25.0, 4.5, 40, 45, true)
   // Verify all 8 keys present
   ```

4. **create_driver boundary values preserved**
   ```
   let d0 = models.create_driver(2, "Zero", "Class B", 15.0, 0.0, 0, 40, false)
   // float(d0.get("rating")) == 0.0 and bool(d0.get("available")) == false
   ```

5. **create_delivery has non-empty timestamp and status "pending"**
   ```
   let del = models.create_delivery(1, "Warehouse A", "Store B", 500.0, "high", "2026-04-01")
   // string(del.get("created_at")).length() > 0 and del.get("status") == "pending"
   ```

6. **validate_vehicle passes for valid vehicle**
   ```
   let vv = models.validate_vehicle(v)
   // bool(vv.get("valid")) == true and len(vv.get("errors")) == 0
   ```

7. **validate_vehicle catches multiple errors**
   ```
   let bad_v = models.create_vehicle(-1, "", "Van", "Diesel", -100.0, -500, 0.0, "Available")
   let vb = models.validate_vehicle(bad_v)
   // bool(vb.get("valid")) == false and len(vb.get("errors")) >= 4
   // At least: id <= 0, plate empty, capacity <= 0, fuel_efficiency <= 0
   ```

8. **validate_driver catches out-of-range values**
   ```
   let bad_d = models.create_driver(0, "", "X", -10.0, 6.0, 0, 0, true)
   let vd = models.validate_driver(bad_d)
   // bool(vd.get("valid")) == false and len(vd.get("errors")) >= 4
   ```

9. **status_name returns correct strings for all 4 statuses**
   ```
   // models.status_name(models.VehicleStatus.Available) == "Available"
   // models.status_name(models.VehicleStatus.InTransit) == "InTransit"
   // models.status_name(models.VehicleStatus.Maintenance) == "Maintenance"
   // models.status_name(models.VehicleStatus.Retired) == "Retired"
   ```

10. **delivery_status_name works for all 5 statuses**
    ```
    // models.delivery_status_name(models.DeliveryStatus.Pending) == "Pending"
    // models.delivery_status_name(models.DeliveryStatus.Delivered) == "Delivered"
    // models.delivery_status_name(models.DeliveryStatus.Failed) == "Failed"
    ```

### test_vehicles() — 10 tests

1. **add_vehicle to empty fleet succeeds**
   ```
   let fleet = []
   let v = models.create_vehicle(1, "ABC-123", "Van", "Diesel", 1000.0, 50000, 8.0, "Available")
   let r = veh.add_vehicle(fleet, v)
   fleet = r.get("fleet")
   // bool(r.get("success")) == true and len(fleet) == 1
   ```

2. **add_vehicle duplicate ID fails with error**
   ```
   let r2 = veh.add_vehicle(fleet, v)
   // bool(r2.get("success")) == false and r2.has("error")
   ```

3. **get_available_vehicles filters correctly**
   ```
   let v2 = models.create_vehicle(2, "DEF-456", "Truck", "Gasoline", 5000.0, 30000, 6.0, "Available")
   let v3 = models.create_vehicle(3, "GHI-789", "Van", "Electric", 800.0, 10000, 12.0, "Maintenance")
   let r_add = veh.add_vehicle(fleet, v2)
   fleet = r_add.get("fleet")
   let r_add2 = veh.add_vehicle(fleet, v3)
   fleet = r_add2.get("fleet")
   let available = veh.get_available_vehicles(fleet)
   // len(available) == 2 (v1 and v2 are Available, v3 is Maintenance)
   ```

4. **assign_vehicle sets status InTransit**
   ```
   let r_assign = veh.assign_vehicle(fleet, 1)
   fleet = r_assign.get("fleet")
   let assigned_v = r_assign.get("vehicle")
   // bool(r_assign.get("success")) == true and assigned_v.get("status") == "InTransit"
   ```

5. **assign_vehicle on non-Available fails**
   ```
   let r_assign2 = veh.assign_vehicle(fleet, 3)
   // bool(r_assign2.get("success")) == false
   ```

6. **release_vehicle sets status Available**
   ```
   let r_release = veh.release_vehicle(fleet, 1)
   fleet = r_release.get("fleet")
   // bool(r_release.get("success")) == true
   ```

7. **calculate_depreciation: Van, capacity 1000, 5yr**
   ```
   // purchase = 1000 * 50.0 = 50000, annual = 50000 * 0.09 = 4500, total = 4500 * 5 = 22500
   let dep = veh.calculate_depreciation(v, 5)
   // int(round(dep)) == 22500
   ```

8. **calculate_depreciation capped at 90%: 15 years**
   ```
   // 4500 * 15 = 67500, but cap at 50000 * 0.9 = 45000
   let dep15 = veh.calculate_depreciation(v, 15)
   // int(round(dep15)) == 45000
   ```

9. **get_maintenance_due: threshold 50000**
   ```
   // v1 has mileage 50000, v2 has 30000, v3 has 10000
   let due = veh.get_maintenance_due(fleet, 40000)
   // len(due) == 1 and int(due[0].get("id")) == 1
   ```

10. **retire_vehicle sets status and reason**
    ```
    let retired = veh.retire_vehicle(v3, "End of life")
    // retired.get("status") == "Retired" and retired.get("retire_reason") == "End of life"
    ```

### test_drivers() — 10 tests

All tests use these exact inputs:
```
let d1 = models.create_driver(1, "Alice", "Class A", 25.0, 4.5, 40, 45, true)
let d2 = models.create_driver(2, "Bob", "Class B", 20.0, 3.0, 48, 45, true)
```

1. **register_driver succeeds**
   ```
   let drivers = []
   let r1 = drv.register_driver(drivers, d1)
   drivers = r1.get("drivers")
   // bool(r1.get("success")) == true and len(drivers) == 1
   ```

2. **register_driver duplicate fails**
   ```
   let r_dup = drv.register_driver(drivers, d1)
   // bool(r_dup.get("success")) == false and r_dup.has("error")
   ```

3. **get_available_drivers filters correctly**
   ```
   let r2 = drv.register_driver(drivers, d2)
   drivers = r2.get("drivers")
   let avail = drv.get_available_drivers(drivers)
   // len(avail) == 2
   ```

4. **assign_driver sets available=false**
   ```
   let r_assign = drv.assign_driver(drivers, 1)
   drivers = r_assign.get("drivers")
   let assigned = r_assign.get("driver")
   // bool(r_assign.get("success")) == true and bool(assigned.get("available")) == false
   ```

5. **calculate_driver_score returns all 6 keys**
   ```
   let history = [
       { "driver_id": 1, "status": "delivered", "on_time": true, "fuel_used": 10.0, "expected_fuel": 10.0 },
       { "driver_id": 1, "status": "delivered", "on_time": true, "fuel_used": 11.0, "expected_fuel": 10.0 },
       { "driver_id": 1, "status": "delivered", "on_time": false, "fuel_used": 9.0, "expected_fuel": 10.0 },
       { "driver_id": 1, "status": "failed", "on_time": false, "fuel_used": 0.0, "expected_fuel": 10.0 }
   ]
   let sc = drv.calculate_driver_score(d1, history)
   // sc.has("driver_id") and sc.has("total_score") and sc.has("delivery_score") and sc.has("reliability_score") and sc.has("efficiency_score") and sc.has("rating_score")
   ```

6. **calculate_driver_score exact computation**
   ```
   // 4 total deliveries, 3 completed, 2 on-time
   // delivery: (3/4) * 30 = 22.5
   // reliability: (2/3) * 25 = 16.67 → int(round(float(sc.get("reliability_score")) * 100)) == 1667
   // rating: (4.5/5.0) * 20 = 18.0
   // Test delivery_score: int(round(float(sc.get("delivery_score")) * 10)) == 225
   ```

7. **calculate_driver_score: no deliveries → zeros except rating**
   ```
   let sc_empty = drv.calculate_driver_score(d1, [])
   // float(sc_empty.get("delivery_score")) == 0.0 and float(sc_empty.get("reliability_score")) == 0.0
   // float(sc_empty.get("rating_score")) == 18.0
   ```

8. **update_driver_hours adds correctly**
   ```
   let updated = drv.update_driver_hours(d1, 8)
   // int(updated.get("total_hours")) == 48
   ```

9. **check_hours_compliance: 40/45 → compliant**
   ```
   let comp = drv.check_hours_compliance(d1)
   // bool(comp.get("compliant")) == true and int(comp.get("hours_remaining")) == 5
   ```

10. **check_hours_compliance: 48/45 → not compliant**
    ```
    let comp2 = drv.check_hours_compliance(d2)
    // bool(comp2.get("compliant")) == false and int(comp2.get("hours_remaining")) == 0
    ```

### test_routes() — 10 tests

1. **create_route_plan calculates total_distance and estimated_time**
   ```
   let v = models.create_vehicle(1, "ABC-123", "Van", "Diesel", 1000.0, 50000, 8.0, "Available")
   let d = models.create_driver(1, "Alice", "Class A", 25.0, 4.5, 40, 45, true)
   let plan = routes.create_route_plan(v, d, ["A", "B", "C"], [40, 50, 30])
   // int(plan.get("total_distance")) == 120
   ```

2. **create_route_plan: 3 stops, 120km → time = 120/60 + 3*0.5 = 3.5 hours**
   ```
   // float(plan.get("estimated_hours")) == 3.5
   ```

3. **calculate_route_cost exact: diesel, 100km, efficiency 8.0**
   ```
   let route = { "total_distance": 100, "estimated_hours": 2.5 }
   let cost = routes.calculate_route_cost(route, v, d)
   // fuel: 100/8.0 * 1.50 = 18.75
   // driver: 2.5 * 25.0 = 62.50
   // total: 81.25
   // int(round(float(cost.get("fuel_cost")) * 100)) == 1875
   // int(round(float(cost.get("total_cost")) * 100)) == 8125
   ```

4. **calculate_route_cost: electric vehicle → fuel_price 0.30**
   ```
   let ev = models.create_vehicle(2, "EV-001", "Van", "Electric", 800.0, 5000, 10.0, "Available")
   let ev_cost = routes.calculate_route_cost(route, ev, d)
   // fuel: 100/10.0 * 0.30 = 3.0
   // int(round(float(ev_cost.get("fuel_cost")) * 100)) == 300
   ```

5. **optimize_route_order returns optimized stops with savings**
   ```
   let stops = ["Depot", "Site A", "Site B", "Site C"]
   let matrix = [
       [0, 10, 50, 30],
       [10, 0, 20, 40],
       [50, 20, 0, 15],
       [30, 40, 15, 0]
   ]
   let opt = routes.optimize_route_order(stops, matrix)
   // opt.has("optimized_stops") and opt.has("total_distance") and opt.has("savings_percent")
   // len(opt.get("optimized_stops")) == 4
   ```

6. **estimate_delivery_time: 60km, 2 stops, high priority**
   ```
   // base = 60/60 + 2*0.5 = 2.0, high = 2.0 * 0.8 = 1.6
   let t_high = routes.estimate_delivery_time(60, 2, "high")
   // int(round(float(t_high) * 10)) == 16
   ```

7. **estimate_delivery_time: same but low priority**
   ```
   // base = 2.0, low = 2.0 * 1.2 = 2.4
   let t_low = routes.estimate_delivery_time(60, 2, "low")
   // int(round(float(t_low) * 10)) == 24
   ```

8. **check_route_capacity: 800kg in 1000kg vehicle → fits**
   ```
   let dels = [
       { "weight_kg": 300.0 },
       { "weight_kg": 200.0 },
       { "weight_kg": 300.0 }
   ]
   let cap = routes.check_route_capacity(dels, v)
   // bool(cap.get("fits")) == true and float(cap.get("utilization")) == 0.8
   ```

9. **check_route_capacity: 1200kg in 1000kg → doesn't fit**
   ```
   let dels2 = [
       { "weight_kg": 500.0 },
       { "weight_kg": 400.0 },
       { "weight_kg": 300.0 }
   ]
   let cap2 = routes.check_route_capacity(dels2, v)
   // bool(cap2.get("fits")) == false and float(cap2.get("total_weight")) == 1200.0
   ```

10. **get_route_history filters by vehicle_id**
    ```
    let all_routes = [
        { "vehicle_id": 1, "driver_id": 1, "status": "completed", "total_distance": 100 },
        { "vehicle_id": 2, "driver_id": 1, "status": "planned", "total_distance": 200 },
        { "vehicle_id": 1, "driver_id": 2, "status": "completed", "total_distance": 150 }
    ]
    let filtered = routes.get_route_history(all_routes, { "vehicle_id": 1 })
    // len(filtered) == 2
    let all_unfiltered = routes.get_route_history(all_routes, {})
    // len(all_unfiltered) == 3
    ```

### test_deliveries() — 10 tests

1. **create_delivery_batch adds all deliveries**
   ```
   let batch = []
   let new_dels = [
       models.create_delivery(1, "Warehouse", "Store A", 200.0, "high", "2026-04-01"),
       models.create_delivery(2, "Warehouse", "Store B", 350.0, "normal", "2026-04-02")
   ]
   let r = del.create_delivery_batch(batch, new_dels)
   batch = r.get("deliveries")
   // int(r.get("added_count")) == 2 and len(batch) == 2
   ```

2. **assign_delivery sets status to assigned**
   ```
   let d1 = batch[0]
   d1 = del.assign_delivery(d1, 101, 1)
   // d1.get("status") == "assigned" and d1.has("assigned_at")
   ```

3. **complete_delivery sets status to delivered**
   ```
   d1 = del.complete_delivery(d1)
   // d1.get("status") == "delivered" and d1.has("completed_at")
   ```

4. **fail_delivery sets status and reason**
   ```
   let d2 = batch[1]
   d2 = del.assign_delivery(d2, 102, 2)
   d2 = del.fail_delivery(d2, "Address not found")
   // d2.get("status") == "failed" and d2.get("failure_reason") == "Address not found"
   ```

5. **calculate_on_time_rate: 1 delivered, 1 failed → 0.5**
   ```
   let rate_dels = [d1, d2]
   let rate = del.calculate_on_time_rate(rate_dels)
   // float(rate) == 0.5
   ```

6. **calculate_on_time_rate: 0 completed → 0.0**
   ```
   let pending_only = [models.create_delivery(3, "A", "B", 100.0, "normal", "2026-05-01")]
   // del.calculate_on_time_rate(pending_only) == 0.0
   ```

7. **get_deliveries_by_status: filter pending**
   ```
   let mixed = [d1, d2, pending_only[0]]
   let pending = del.get_deliveries_by_status(mixed, "pending")
   // len(pending) == 1
   ```

8. **Multiple lifecycle: create→assign→complete**
   ```
   let d3 = models.create_delivery(4, "Hub", "Client", 150.0, "high", "2026-04-05")
   // d3.get("status") == "pending"
   d3 = del.assign_delivery(d3, 200, 5)
   // d3.get("status") == "assigned"
   d3 = del.complete_delivery(d3)
   // d3.get("status") == "delivered"
   ```

9. **create_delivery has correct priority preserved**
   ```
   let d_hp = models.create_delivery(5, "X", "Y", 50.0, "high", "2026-06-01")
   // d_hp.get("priority") == "high"
   let d_lp = models.create_delivery(6, "X", "Y", 50.0, "low", "2026-06-01")
   // d_lp.get("priority") == "low"
   ```

10. **fail_delivery preserves original data**
    ```
    let d_fail = models.create_delivery(7, "Origin", "Dest", 999.0, "normal", "2026-07-01")
    d_fail = del.assign_delivery(d_fail, 300, 3)
    d_fail = del.fail_delivery(d_fail, "Damaged")
    // d_fail.get("origin") == "Origin" and d_fail.get("destination") == "Dest" and float(d_fail.get("weight_kg")) == 999.0
    ```

### test_reporting() — 10 tests

1. **generate_fleet_report returns all 6 keys**
   ```
   let fleet = [
       models.create_vehicle(1, "V1", "Van", "Diesel", 1000.0, 50000, 8.0, "Available"),
       models.create_vehicle(2, "V2", "Truck", "Gasoline", 5000.0, 80000, 6.0, "InTransit"),
       models.create_vehicle(3, "V3", "Van", "Electric", 800.0, 20000, 12.0, "Maintenance")
   ]
   let rep = report.generate_fleet_report(fleet, [])
   // rep.has("report_text") and rep.has("total_vehicles") and rep.has("available_count") and rep.has("in_transit_count") and rep.has("maintenance_count") and rep.has("avg_mileage")
   ```

2. **generate_fleet_report correct counts**
   ```
   // int(rep.get("total_vehicles")) == 3
   // int(rep.get("available_count")) == 1
   // int(rep.get("in_transit_count")) == 1
   // int(rep.get("maintenance_count")) == 1
   ```

3. **report_text contains column header**
   ```
   // string(rep.get("report_text")).contains("Plate")
   ```

4. **calculate_fleet_kpis exact values**
   ```
   let kpi_deliveries = [
       { "status": "delivered" },
       { "status": "delivered" },
       { "status": "delivered" },
       { "status": "delivered" },
       { "status": "failed" }
   ]
   let kpi_routes = [
       { "total_distance": 100 },
       { "total_distance": 200 }
   ]
   let kpis = report.calculate_fleet_kpis(fleet, kpi_routes, kpi_deliveries)
   // 1/3 in transit → utilization ≈ 0.333
   // int(round(float(kpis.get("utilization_rate")) * 1000)) == 333
   // 4/(4+1) = 0.8 delivery success
   // float(kpis.get("delivery_success_rate")) == 0.8
   // avg route distance = (100+200)/2 = 150.0
   // float(kpis.get("avg_route_distance")) == 150.0
   ```

5. **calculate_fleet_kpis empty inputs → all 0.0**
   ```
   let kpis_empty = report.calculate_fleet_kpis([], [], [])
   // float(kpis_empty.get("utilization_rate")) == 0.0
   // float(kpis_empty.get("delivery_success_rate")) == 0.0
   // float(kpis_empty.get("avg_route_distance")) == 0.0
   ```

6. **generate_driver_scorecard returns per-driver data**
   ```
   let drivers = [models.create_driver(1, "Alice", "A", 25.0, 4.0, 40, 45, true)]
   let sc = report.generate_driver_scorecard(drivers, [])
   // sc.has("Alice")
   ```

7. **calculate_fuel_efficiency: 2 routes, different fuel types**
   ```
   let eff_fleet = [
       models.create_vehicle(1, "V1", "Van", "Diesel", 1000.0, 50000, 8.0, "Available"),
       models.create_vehicle(2, "V2", "Van", "Electric", 800.0, 20000, 10.0, "Available")
   ]
   let eff_routes = [
       { "vehicle_id": 1, "total_distance": 100 },
       { "vehicle_id": 2, "total_distance": 80 }
   ]
   let eff = report.calculate_fuel_efficiency(eff_routes, eff_fleet)
   // Diesel: 100/8.0 * 1.50 = 18.75
   // Electric: 80/10.0 * 0.30 = 2.40
   // total_fuel_cost = 21.15, total_distance = 180
   // int(round(float(eff.get("total_fuel_cost")) * 100)) == 2115
   let by_ft = eff.get("by_fuel_type") ?? {}
   let diesel_data = by_ft.get("Diesel") ?? {}
   // int(round(float(diesel_data.get("cost")) * 100)) == 1875
   ```

8. **export_delivery_csv contains headers and data**
   ```
   let csv_data = [["D1", "Store A", "delivered"], ["D2", "Store B", "pending"]]
   let csv_out = report.export_delivery_csv(csv_data, ["ID", "Destination", "Status"])
   // csv_out.contains("ID") and csv_out.contains("D1") and csv_out.contains("Store A")
   ```

9. **forecast_maintenance_costs: 3 vehicles, 6 months**
   ```
   // v1: 50000*0.02=1000, v2: 80000*0.02=1600, v3: 20000*0.02=400
   // monthly total = 3000
   let fc = report.forecast_maintenance_costs(fleet, 6)
   // len(fc.get("forecast")) == 6
   // int(round(float(fc.get("avg_monthly")))) == 3000
   // int(round(float(fc.get("total")))) == 18000
   ```

10. **forecast_maintenance_costs: empty fleet → all zeros**
    ```
    let fc_empty = report.forecast_maintenance_costs([], 3)
    // float(fc_empty.get("avg_monthly")) == 0.0 and float(fc_empty.get("total")) == 0.0
    // len(fc_empty.get("forecast")) == 3
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

    let r2 = test_vehicles()
    total_passed = total_passed + r2[0]
    total_tests = total_tests + r2[1]
    print("test_vehicles: " + string(r2[0]) + "/" + string(r2[1]))

    let r3 = test_drivers()
    total_passed = total_passed + r3[0]
    total_tests = total_tests + r3[1]
    print("test_drivers: " + string(r3[0]) + "/" + string(r3[1]))

    let r4 = test_routes()
    total_passed = total_passed + r4[0]
    total_tests = total_tests + r4[1]
    print("test_routes: " + string(r4[0]) + "/" + string(r4[1]))

    let r5 = test_deliveries()
    total_passed = total_passed + r5[0]
    total_tests = total_tests + r5[1]
    print("test_deliveries: " + string(r5[0]) + "/" + string(r5[1]))

    let r6 = test_reporting()
    total_passed = total_passed + r6[0]
    total_tests = total_tests + r6[1]
    print("test_reporting: " + string(r6[0]) + "/" + string(r6[1]))

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
5. **Use enums, not magic values** — use `models.VehicleStatus.Available` not string `"Available"` where comparing against enum values
6. **Value semantics** — re-assign dicts/arrays to parents after mutation
7. **Python polyglot** — start at column 0, NO `return`, use `-> JSON` for structured data
8. **`dict.get()` not `dict["key"]`** — bracket access throws on missing keys
9. **All functions must have real logic** — governance detects stubs, hardcoded returns, simulation markers
10. **60 tests total** — 6 suites x 10 tests each. EVERY test must pass.
11. **Exact values matter** — tests check precise computed results, not just types or ranges
12. **Handle empty inputs gracefully** — edge case tests verify behavior with empty arrays, 0 values

## Expected Output
```
test_models: 10/10
test_vehicles: 10/10
test_drivers: 10/10
test_routes: 10/10
test_deliveries: 10/10
test_reporting: 10/10

TOTAL: 60/60
ALL TESTS PASSED
```
