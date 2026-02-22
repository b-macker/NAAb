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

## 17.4 Cookbook: Practical Recipes

This section provides tested, working recipes for common programming tasks in NAAb. Every recipe below uses correct NAAb syntax and has been verified against the interpreter.

### 17.4.1 String Manipulation

**Title case conversion:**

```naab
fn to_title_case(text) {
    let words = string.split(text, " ")
    let result = []
    let i = 0
    while i < array.length(words) {
        let word = words[i]
        if string.length(word) > 0 {
            let first = string.upper(string.substring(word, 0, 1))
            let rest = string.lower(string.substring(word, 1, string.length(word)))
            result.push(first + rest)
        }
        i = i + 1
    }
    return string.join(result, " ")
}

main {
    print(to_title_case("hello world from naab"))
    // Output: Hello World From Naab
}
```

**Simple template filling:**

```naab
fn fill_template(template, key, value) {
    return string.replace(template, "{" + key + "}", value)
}

main {
    let msg = "Hello {name}, welcome to {place}!"
    let msg = fill_template(msg, "name", "Alice")
    let msg = fill_template(msg, "place", "NAAb")
    print(msg)
    // Output: Hello Alice, welcome to NAAb!
}
```

### 17.4.2 Array Operations

**Filter, map, and reduce patterns:**

```naab
main {
    let numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

    // Filter: keep only even numbers
    let evens = array.filter_fn(numbers, fn(n) { return n % 2 == 0 })
    print(evens)  // [2, 4, 6, 8, 10]

    // Map: double each number
    let doubled = array.map_fn(numbers, fn(n) { return n * 2 })
    print(doubled)  // [2, 4, 6, 8, 10, 12, 14, 16, 18, 20]

    // Reduce: sum all numbers
    let total = array.reduce_fn(numbers, fn(acc, n) { return acc + n }, 0)
    print(total)  // 55
}
```

**Note:** `filter_fn`, `map_fn`, and `reduce_fn` must be called as `array.filter_fn(arr, fn)`, not with dot notation on the array (`arr.filter_fn(fn)` will not work).

### 17.4.3 Error Handling Patterns

**Retry with fallback:**

```naab
fn try_with_fallback(primary_fn, fallback_value) {
    try {
        return primary_fn()
    } catch (e) {
        print("Warning: " + e)
        return fallback_value
    }
}

fn risky_division(a, b) {
    return a / b
}

main {
    // Safe call
    let result = try_with_fallback(fn() { return risky_division(10, 2) }, 0)
    print("Result: " + result)  // 5

    // Risky call with fallback
    let result2 = try_with_fallback(fn() { return risky_division(10, 0) }, -1)
    print("Result: " + result2)  // -1
}
```

**Collecting errors without stopping:**

```naab
main {
    let errors = []
    let results = []

    let inputs = [10, 0, 5, 0, 3]
    let i = 0
    while i < array.length(inputs) {
        try {
            let val = 100 / inputs[i]
            results.push(val)
        } catch (e) {
            errors.push("Error at index " + i)
        }
        i = i + 1
    }

    print("Results: " + results)
    print("Errors: " + errors)
}
```

### 17.4.4 JSON Processing

**Parse JSON and extract data:**

```naab
main {
    let json_str = '{"name": "Alice", "scores": [90, 85, 92]}'
    let data = json.parse(json_str)

    print("Name: " + data["name"])
    print("Scores: " + data["scores"])

    // Compute average using Python
    let scores = data["scores"]
    let avg = <<python[scores]
sum(scores) / len(scores)
    >>
    print("Average: " + avg)
}
```

**Build and serialize JSON:**

```naab
main {
    let user = {
        "name": "Bob",
        "age": 25,
        "tags": ["developer", "naab-user"]
    }
    let json_output = json.stringify(user)
    print(json_output)
}
```

### 17.4.5 Polyglot Patterns

**Using Python for statistics:**

```naab
main {
    let data = [23, 45, 12, 67, 34, 89, 56, 78, 11, 99]

    let stats = <<python[data]
import statistics
{"mean": statistics.mean(data), "median": statistics.median(data), "stdev": round(statistics.stdev(data), 2)}
    >>

    print("Mean: " + stats["mean"])
    print("Median: " + stats["median"])
    print("Std Dev: " + stats["stdev"])
}
```

**Using Bash for system info:**

```naab
main {
    let hostname = <<bash
hostname
    >>
    print("Hostname: " + hostname)

    let kernel = <<bash
uname -r
    >>
    print("Kernel: " + kernel)

    let disk = <<bash
df -h / | tail -1 | awk '{print $4}'
    >>
    print("Free disk: " + disk)
}
```

**Using JavaScript for data transformation:**

```naab
main {
    let items = [3, 1, 4, 1, 5, 9, 2, 6, 5, 3]

    let unique_sorted = <<javascript[items]
[...new Set(items)].sort((a, b) => a - b).join(", ")
    >>
    print("Unique sorted: " + unique_sorted)
}
```

### 17.4.6 Environment and Configuration

```naab
main {
    // Read environment variables with defaults
    let home = env.get("HOME")
    print("Home: " + home)

    // Set environment variables for polyglot blocks
    env.set_var("APP_MODE", "production")

    let mode = <<bash
echo $APP_MODE
    >>
    print("Mode: " + mode)
}
```

## 17.5 Working with AI Assistants

When asking an AI model to generate NAAb code, keep these rules in mind:

1.  **Entry point**: Use `main { }`, not `fn main()` or `function main()`
2.  **Imports**: Use `use io`, not `import io` or `import "stdlib"`
3.  **Function keyword**: Use `fn`, not `function` (though `function` and `func` are accepted as aliases)
4.  **Dictionary keys**: Always quote them — `{"key": value}` not `{key: value}`
5.  **Loops**: Use `while` with manual index. NAAb does not support `for (x in y)` with parentheses
6.  **String operations**: Use `string.upper()`, `string.split()`, not `toUpperCase()`, `split()`
7.  **Array operations**: Use `array.length(arr)`, `arr.push(x)`. Note that `filter_fn`, `map_fn`, `reduce_fn` require the `array.filter_fn(arr, fn)` form
8.  **No `import "stdlib"`**: There is no "stdlib" module. Each module is imported individually: `use io`, `use string`, `use json`
9.  **Catch syntax**: Use `catch (e)` with parentheses, not `catch e`
10. **Lambda syntax**: Use `fn(x) { return x * 2 }`, not `(x) => x * 2`. Arrow syntax is not supported in NAAb
11. **Type annotations are optional**: NAAb infers types. Don't over-annotate
