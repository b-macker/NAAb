# Full NAAb Projects Verification

This directory houses comprehensive, end-to-end NAAb applications designed to showcase the language's full capabilities in real-world scenarios. Each application demonstrates the integration of multiple NAAb features, polyglot blocks, and standard library modules.

## Structure

Each full project resides in its own dedicated subfolder, named after the application.

```
ch0_full_projects/
├── README.md                   <-- This file
├── my_enterprise_app_1/        <-- Folder for Application 1
│   ├── main.naab
│   ├── module_a.naab
│   └── ...
├── another_complex_app/        <-- Folder for Application 2
│   ├── main.naab
│   └── ...
├── NAAb_Web_Automator/
│   ├── main.naab
│   └── ...
└── ...
```

## Purpose

-   **End-to-End Verification:** Ensure that all NAAb features, especially polyglot integrations and module systems, work cohesively in a larger codebase.
-   **Feature Showcase:** Provide robust, real-world examples of NAAb's power and flexibility.
-   **Integration Testing:** Validate interactions between different language executors and NAAb's core.
-   **Performance Benchmarking:** Offer larger codebases for performance analysis.

## Adding a New Project

To add a new full project:

1.  Create a new subfolder under `ch0_full_projects/` (e.g., `my_new_app/`).
2.  Place all `.naab` files and any supporting assets (e.g., Python scripts, C++ sources, config files) within that subfolder.
3.  Add a brief `README.md` to your project's subfolder explaining its purpose and how to run it.
4.  Optionally, add a test script (e.g., `run_tests.sh`) within your project's subfolder to automate its verification.

## Running Projects

Projects can be run individually by executing their `main.naab` file (or equivalent entry point) using `naab-lang`:

```bash
# Example: Running 'my_enterprise_app_1'
build/naab-lang run docs/book/verification/ch0_full_projects/my_enterprise_app_1/main.naab
```

## Status

This is a new section for comprehensive projects. Projects will be added and updated as they are developed and verified.
