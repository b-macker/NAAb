# NAAb Web Automation & Data Extraction Platform

This project is a reimplementation of a complex web automation and data extraction system, orchestrated entirely by NAAb. It leverages NAAb's polyglot capabilities to integrate with Python Playwright for headless browser control, event capturing, and data extraction.

## Goal

To provide a fully functional, production-ready application demonstrating NAAb's power in coordinating advanced web interaction workflows.

## Features

-   Headless browser automation via Python Playwright.
-   Configurable web automation flows (navigation, clicks, form filling).
-   Dynamic data extraction using CSS selectors.
-   Event capturing (DOM changes, network requests/responses, user actions).
-   Modular design using NAAb structs and modules.
-   Comprehensive reporting of collected data and events.

## Project Structure

```
NAAb_Web_Automator/
├── main.naab                                   # Main orchestration entry point
├── config/
│   └── automator_config.json                   # Defines target URLs, flows, extraction rules
├── modules/
│   ├── web_automator_core.naab                 # Core NAAb module for high-level tasks
│   ├── web_event_schemas.naab                  # NAAb structs for SiteEvents, ScrapedItem, AutomationStep, ExtractionRule
│   └── scraper_py.naab                         # NAAb functions wrapping Python Playwright blocks
├── scripts/
│   ├── python/
│   │   ├── playwright_engine.py                # Python script for Playwright interactions
│   │   └── data_analysis_lib.py                # Python script for advanced data analysis
│   └── js/
│       └── json_transformer.js                 # JS script for advanced JSON transformations
├── output/
│   ├── logs/                                   # Logs of events, errors
│   └── reports/                                # Generated HTML/CSV reports
├── templates/                                  # HTML templates for reports (e.g., Jinja2)
└── README.md                                   # This project documentation
```

## Known Issues & Workarounds

*   **Playwright Stubs:** Due to the limitations of the Termux environment, the Playwright interactions are currently stubbed within the Python polyglot blocks inside `scraper_py.naab`. To enable real browser automation, uncomment the real Playwright logic in `scraper_py.naab` and ensure Playwright is installed (`pip install playwright && playwright install`) in your Python environment.

## Setup & Running

**1. System Dependencies:**
   - Ensure Python 3.8+ is installed.
   - Install Playwright browser binaries: `pip install playwright && playwright install`
   - Install Python packages: `pip install requests beautifulsoup4 pandas numpy textblob jinja2 jsonschema`
   - Node.js (for JS polyglot blocks, if `scripts/js` are used)

**2. Launch NAAb Automator:**
   ```bash
   build/naab-lang run NAAb_Web_Automator/main.naab
   ```

## Status

**Phase 1: Project Setup and Core Data Structures - ✅ Complete**
*   Directory structure: ✅ Complete
*   `main.naab`: ✅ Complete (initial)
*   `README.md`: ✅ Complete (initial)
*   Core Event Schemas (`modules/web_event_schemas.naab`): ✅ Complete
*   Automation & Configuration Schemas (`modules/web_automator_core.naab`): ✅ Complete
*   `config/automator_config.json`: ✅ Complete
