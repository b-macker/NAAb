# Global Supply Chain Simulation & Analytics Platform

This project is a sophisticated NAAb application that simulates a global supply chain, optimizing logistics, forecasting demand, and tracking financials using a polyglot architecture. It verifies NAAb's capability to orchestrate 8+ languages and complex module import patterns.

## Project Description & Language Selection

The application follows this execution flow:

1.  **System Initialization:** The application starts by initializing the environment, verifying directory structures, and loading global configuration settings.
    *   **Language:** **NAAb** (Best for: Orchestration, Configuration, Logic Glue).
    *   **Why:** NAAb provides the module system and type safety to bind everything together.

2.  **Environment Prep:** It checks if the necessary output directories exist and cleans up old temporary files from previous runs to ensure a clean state.
    *   **Language:** **Bash** (Best for: OS Operations, File Management).
    *   **Why:** `mkdir`, `rm`, and file system checks are native and fastest in Shell.

3.  **Historical Data Loading:** The system loads historical sales data from CSV files to prepare for analysis.
    *   **Language:** **NAAb** (Best for: I/O, Struct Mapping).
    *   **Why:** Reading files and parsing basic text is efficient enough in NAAb stdlib for moderate sizes.

4.  **Demand Forecasting:** Using the historical data, the system predicts future demand for each product region using statistical linear regression.
    *   **Language:** **Python** (Best for: Data Science, ML).
    *   **Why:** Libraries like `scikit-learn` or `numpy` (or standard `statistics` module) make this trivial compared to implementing math from scratch.

5.  **Route Optimization:** Based on the predicted demand, the system calculates the optimal delivery routes from warehouses to retailers to minimize distance.
    *   **Language:** **C++** (Best for: High-Performance Algorithms).
    *   **Why:** Complex graph algorithms (like TSP variants) benefit significantly from C++'s raw speed and STL.

6.  **Simulation Execution:** The system runs a tick-based simulation of the supply chain, tracking inventory levels, production rates, and shipping statuses over time.
    *   **Language:** **Rust** (Best for: Systems Programming, Safety).
    *   **Why:** Rust's ownership model and performance are ideal for state-heavy simulations where memory safety is critical.

7.  **Logistics Network:** While the simulation runs, the system manages concurrent shipping requests and tracks package states across the network.
    *   **Language:** **Go** (Best for: Concurrency, Networking).
    *   **Why:** Go's goroutines and channels are perfect for modeling many independent moving parts (shipments) simultaneously.

8.  **Financial Reconciliation:** After the simulation, the system calculates total revenue, costs, and taxes, ensuring high-precision decimal arithmetic.
    *   **Language:** **C#** (Best for: Enterprise Logic, Financials).
    *   **Why:** C# has strong decimal support and enterprise-grade structure for handling money.

9.  **Report Formatting:** The raw financial and operational data is formatted into a pretty-printed text report.
    *   **Language:** **Ruby** (Best for: String Manipulation, DSLs).
    *   **Why:** Ruby's expressive syntax makes string interpolation and formatting very readable.

10. **Dashboard Generation:** Finally, the system aggregates all metrics into a JSON object and generates an HTML dashboard.
    *   **Language:** **JavaScript** (Best for: JSON, Web Content).
    *   **Why:** JS is the native language of JSON and HTML.

## Import Syntax Verification

This project explicitly tests NAAb's import capabilities:

1.  **`use "BLOCK-ID" as alias`**: Used to import a specialized registered block (e.g., `use "BLOCK-PY-MATH" as pymath`) to demonstrate registry integration.
2.  **`use module_path`**: Used for standard local modules (e.g., `use modules.forecasting`).
3.  **`import { Item } from "path"`**: *Experimental/Proposed Syntax*. We will attempt to use this syntax to import specific types. If the current parser version does not support it, we will fallback to standard `use` but document the attempt.

## Project Structure

```
supply_chain_sim/
├── main.naab                           # Entry point
├── config/
│   └── simulation_config.json          # Settings
├── data/
│   └── historical_sales.csv            # Input data
├── modules/
│   ├── config_loader.naab              # NAAb: Config parsing
│   ├── demand_forecast.naab            # Python wrapper
│   ├── route_optimizer.naab            # C++ wrapper
│   ├── simulation_engine.naab          # Rust wrapper
│   ├── logistics_net.naab              # Go wrapper
│   ├── finance_core.naab               # C# wrapper
│   └── reporter.naab                   # Ruby/JS wrapper
└── output/                             # Generated artifacts
```
