# Chapter 17: Case Studies

To truly understand NAAb's capabilities, it's helpful to see how it can be applied to real-world problems. This chapter presents conceptual case studies, illustrating how NAAb's polyglot and orchestration features address common software engineering challenges.

## 17.1 Case Study A: A Data Processing Monolith

**Problem:** A growing startup needs to process large volumes of incoming data (CSV files, API feeds) from various sources, perform complex transformations, generate reports, and store results in a database. Different stages of this pipeline would benefit from different languages: Python for data science, SQL for database interaction, and optimized C++ for specific algorithms.

**NAAb Solution:**

NAAb can serve as the central orchestrator for this data processing monolith, coordinating tasks across multiple languages.

1.  **Ingestion (Python Block)**: Use a `<<python>>` block to read data from various sources (CSV, HTTP API). Python's rich data libraries (e.g., Pandas) are ideal for initial parsing and cleaning.

    ```naab
    // main.naab
    use BLOCK-PY-CSV_READER as csv_reader
    use BLOCK-PY-API_FETCHER as api_fetcher

    main {
        let csv_data = csv_reader("input.csv")
        let api_data = api_fetcher("https://api.example.com/data")
        // ...
    }
    ```

2.  **Transformation (JavaScript Block)**: For specific data mapping, validation, or light-weight transformations, a `<<javascript>>` block can be used. JavaScript's JSON capabilities are well-suited for this.

    ```naab
    // ...
    let combined_data = combine(csv_data, api_data) // NAAb native combine function
    let transformed_data = <<javascript[combined_data]
    // JavaScript code to filter, map, and reshape data
    // e.g., combined_data.filter(item => item.valid).map(...)
    >>
    // ...
    ```

3.  **Complex Algorithms (C++ Block)**: If a specific transformation or analytical step requires high performance (e.g., a custom clustering algorithm or a highly optimized filter), a `<<cpp>>` block can be leveraged.

    ```naab
    // ...
    let result_cpp = <<cpp[transformed_data]
    // C++ code for a specific, performance-critical algorithm
    // Requires manual parsing of transformed_data (if complex JSON string)
    // or passing simple types.
    >>
    // ...
    ```

4.  **Database Loading (Bash/Python/SQL Blocks)**: Depending on the database type and complexity, a `<<bash>>` block could execute `psql` or `mysql` commands, or a `<<python>>` block could use an ORM (like SQLAlchemy), or a future `<<sql>>` block could embed raw SQL.

    ```naab
    // ...
    <<bash[result_cpp]
    echo "$result_cpp" | psql -c "COPY data_table FROM STDIN CSV;"
    >>
    // ...
    ```

**Benefits:** NAAb provides a unified control plane, allowing developers to pick the best tool for each stage without fragmenting the project across multiple separate services or complex inter-process communication mechanisms.

## 17.2 Case Study B: A System Automation Utility

**Problem:** An DevOps team needs to build a versatile automation script that interacts with local system resources (files, processes), remote servers (SSH, HTTP), and cloud APIs. The script needs to be robust, maintainable, and easily extendable by team members with diverse language skills.

**NAAb Solution:**

NAAb excels in scenarios requiring interaction with the operating system and external services.

1.  **Local System Operations (Bash Blocks)**: Manage local files, execute system commands, check process status.

    ```naab
    // ...
    <<bash
    if [ ! -d "/var/log/myapp" ]; then
        mkdir /var/log/myapp
    fi
    >>
    // ...
    ```

2.  **Remote Interaction (Python Blocks)**: Use Python's `requests` library for HTTP API calls or `paramiko` for SSH automation.

    ```naab
    // ...
    let remote_host = "server.example.com"
    let api_key = env.get("API_SECRET") // From NAAb's env module

    let remote_status = <<python[remote_host, api_key]
    import requests
    response = requests.get(f"https://{remote_host}/status", headers={"Authorization": api_key})
    // ... process response
    >>
    // ...
    ```

3.  **Reporting and Notification (NAAb Native/Stdlib)**: Aggregate results and send notifications using NAAb's native string processing or future email/messaging stdlib modules.

**Benefits:** A single NAAb script replaces a patchwork of Bash, Python, and other scripts, making the automation logic clearer, more type-safe (with NAAb's core), and easier to maintain.

## 17.3 Case Study C: A Polyglot Web Service

**Problem:** Develop a high-performance web API where some endpoints require the speed of C++/Rust (e.g., image processing, complex computations), while others benefit from the rapid development of Python (e.g., database interaction, business logic), and frontend data preparation is best in JavaScript.

**NAAb Solution:**

NAAb can act as a lightweight API gateway or a service mesh component, routing requests and executing specialized logic in different languages.

1.  **API Endpoint Dispatch (NAAb Core)**: NAAb can parse incoming requests, validate them, and dispatch to the appropriate language handler.

    ```naab
    // Hypothetical HTTP server in NAAb (or using a future HTTP server module)
    fn handle_request(request: Request) -> Response {
        if request.path == "/process_image" {
            return process_image_cpp(request.body)
        } else if request.path == "/user_data" {
            return get_user_data_py(request.user_id)
        }
        return error_response("Not Found")
    }
    ```

2.  **Image Processing Endpoint (C++ or Rust Block)**: For a high-throughput endpoint like image processing, C++ or Rust blocks provide the necessary speed and memory control.

    ```naab
    fn process_image_cpp(image_data: string) -> string {
        let processed_image = <<cpp[image_data]
        // C++ code for image manipulation using OpenCV or similar
        >>
        return processed_image
    }
    ```

3.  **User Data Endpoint (Python Block)**: For database interaction and business logic, Python's mature libraries for ORMs (SQLAlchemy, Django ORM) and data validation are ideal.

    ```naab
    fn get_user_data_py(user_id: int) -> dict<string, any> {
        let user_info = <<python[user_id]
        // Python code to query database, apply business rules
        >>
        return user_info
    }
    ```

4.  **Frontend Data Preparation (JavaScript Block)**: Before sending data to a client, a JavaScript block can reshape, filter, or aggregate the data into a format optimized for the frontend.

    ```naab
    fn prepare_frontend_data(raw_api_response: dict<string, any>) -> dict<string, any> {
        let frontend_data = <<javascript[raw_api_response]
        // JavaScript code to transform raw_api_response for frontend
        >>
        return frontend_data
    }
    ```

**Benefits:** This architecture allows developers to build a truly heterogeneous microservice, using the best language for each micro-task, all managed and coordinated by NAAb, leading to optimal performance and development efficiency across the stack.
