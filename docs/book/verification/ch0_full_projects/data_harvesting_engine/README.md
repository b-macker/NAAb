# NAAb-Orchestrated Cross-Platform Data Harvesting & Analytics Engine

This project demonstrates the full power of NAAb as an orchestration layer for complex, real-world data harvesting and analytics. It leverages all 8 polyglot languages (NAAb, Python, JavaScript, Bash, C++, Ruby, Go, C#) to perform a multi-stage data pipeline, interacting with external systems and producing verifiable outputs.

## Project Goal

To build a genuinely functional data intelligence pipeline, where each stage of data fetching, processing, analysis, and reporting is handled by the most suitable language, all orchestrated seamlessly by NAAb. This will rigorously test NAAb's polyglot capabilities, inter-process communication, error handling, and performance in a demanding scenario.

## Architecture & Polyglot Roles

```
+---------------------------------------------------+
|             NAAb Orchestrator (main.naab)         |
|   (Interactive CLI Menu, Pipeline Control)        |
+---------------------------------------------------+
  ^      ^      ^      ^      ^      ^      ^      ^
  |      |      |      |      |      |      |      |
  v      v      v      v      v      v      v      v
+---------+---------+---------+---------+---------+----------+----------+----------+
|  Python |   JS    |  Bash   |   C++   |  Ruby   |   Go    |    C#    | NAAb Core|
| (Scrape/ | (JSON/  | (FS/Sys)| (HPC/   | (HTML/  | (Net/   | (Data    | (Flow/   |
| Analyze)| Transform)|         | Algo)   | Text)   | Concur.)| Integrity)| Structs) |
+---------+---------+---------+---------+---------+----------+----------+----------+
    |         |         |         |         |         |          |
    v         v         v         v         v         v          v
+----------------------------------------------------------------------------------+
|      External Web (HTTP/HTML), Filesystem, Database, External APIs             |
+----------------------------------------------------------------------------------+
```

## Key Features

-   **Real Web Scraping:** Uses Python (requests/BeautifulSoup, Playwright) for web data extraction.
-   **Multi-Stage Data Pipeline:** Orchestrates fetching, cleaning, transforming, analyzing, and reporting.
-   **Comprehensive Polyglot Integration:** Every one of NAAb's 8 supported languages will be used for tasks where they excel:
    *   **Python:** Browser automation, advanced data analysis (Pandas, NumPy), database interaction.
    *   **JavaScript:** Fast JSON parsing/transformation, schema validation.
    *   **Bash:** File system management, system health checks.
    *   **C++:** High-performance data aggregation and custom algorithms.
    *   **Ruby:** Robust HTML/text parsing, advanced regex.
    *   **Go:** Concurrent network requests, efficient data writing.
    *   **C#:** Data integrity checks, complex validation logic.
-   **Interactive CLI:** User-driven menu to trigger different pipeline stages and configure tasks (simulated for simplicity).
-   **Verifiable Output:** Generates structured data files (CSV, JSON) and console reports.
-   **Robust Error Handling:** `try/catch` around all polyglot and external interactions.

## Project Structure

```
data_harvesting_engine/
├── README.md                           <-- This file
├── main.naab                           <-- NAAb Orchestrator main entry point
├── engine_config.json                  <-- Configuration file (alongside main.naab)
├── config/                             <-- Placeholder for additional config files
├── scripts/                            <-- Polyglot scripts called by NAAb
│   ├── python/                         
│   ├── js/                             
│   ├── ruby/                           
│   ├── go/                             
│   ├── csharp/                         
│   └── bash/                           
├── naab_modules/                       <-- NAAb modules for pipeline stages (currently embedded in project root for simplicity)
│   ├── app_config.naab                 <-- Loads engine_config.json
│   ├── web_scraper.naab                <-- Handles static and dynamic scraping
│   ├── data_transformer.naab           <-- Cleans and validates data
│   ├── insight_generator.naab          <-- Performs analytics and anomaly detection
│   └── report_publisher.naab           <-- Generates HTML and CSV reports
│   └── asset_manager.naab              <-- Manages files and directories
└── output/                             <-- Directory for harvested data and generated reports
```

## Setup & Running

1.  **System Dependencies:**
    *   Ensure Python, Node.js (for JS), Ruby, Go, C# runtime (e.g., Mono/dotnet), and a C++ compiler are installed.
    *   Install Python packages: `pip install requests beautifulsoup4 pandas jinja2 jsonschema textblob` (and Playwright if using dynamic scraping: `pip install playwright && playwright install`).
    *   Install Ruby gems: `gem install nokogiri` (if used).
2.  **Launch NAAb Orchestrator:**
    ```bash
    build/naab-lang run docs/book/verification/ch0_full_projects/data_harvesting_engine/main.naab
    ```
3.  **Current Scraping Behavior Note:** The configured selectors in `engine_config.json` for `https://www.vibecodingtools.tech/agents` currently lead to the scraper's fallback logic, yielding 1 item. For more extensive results, the `item_container`, `agent_name`, and `agent_description` selectors within `engine_config.json` would need to be refined based on a manual inspection of the target website's HTML structure.

## Error Handling & Issue Tracking

Any issues encountered during implementation or execution (e.g., polyglot block failures, unexpected behavior) will be documented in `ISSUES.md` with explicit `ISS-XXX` IDs for tracking and resolution. There is no time limit; the goal is a complete, fully executed, real-world example.
