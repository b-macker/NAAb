# Monitoring System (Full Project Example)

This project is a comprehensive enterprise DevOps monitoring and alerting system written in NAAb. It showcases the full power of NAAb's polyglot capabilities, module system, type system, error handling, and standard library integrations.

## Features Demonstrated

-   **Polyglot Execution:** Integrates Python, JavaScript, C++, and Bash for various tasks (CPU monitoring, JSON processing, performance metrics, system commands).
-   **Module System:** Utilizes NAAb's Rust-style module system to organize code into logical units (e.g., `http_monitor`, `log_analyzer`, `alert_manager`, `metrics_engine`).
-   **Type System:** Leverages Enums, Structs, Generics (`Result<T>`), and Null Safety (`string?`) for robust and type-safe data handling.
-   **Error Handling:** Implements `try/catch` blocks for graceful error management.
-   **Standard Library:** Makes extensive use of `time`, `array`, `json`, `string`, `regex`, and `crypto` modules.
-   **Data Flow:** Seamlessly passes complex data structures between NAAb and foreign language blocks.
-   **Inline Code Caching:** Demonstrates performance benefits of cached inline C++ execution.
-   **Comprehensive Workflow:** Simulates an end-to-end monitoring cycle, including configuration loading, metrics collection, analysis, alerting, and reporting.

## Project Structure

```
monitoring_system/
├── README.md               <-- This file
├── MAIN.naab               <-- Main entry point for the monitoring system
├── http_monitor.naab       <-- Module for HTTP endpoint health checks
├── log_analyzer.naab       <-- Module for log parsing and analysis (exports Enum)
├── alert_manager.naab      <-- Module for managing and sending alerts
├── metrics_engine.naab     <-- Module for high-performance metrics processing
└── config.json             <-- (Optional) Example configuration file
```

## How to Run

To execute the monitoring system:

```bash
build/naab-lang run docs/book/verification/ch0_full_projects/monitoring_system/MAIN.naab
```

## Expected Output

The script will simulate monitoring various servers, collect metrics using different polyglot blocks, analyze data, potentially trigger alerts, generate a dashboard summary, and perform trend analysis. Expect detailed print statements showing the execution flow and results.
