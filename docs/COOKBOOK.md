# NAAb Cookbook

**Version**: 0.1.0
**Last Updated**: December 30, 2024

Practical recipes and examples for common NAAb programming tasks.

---

## Table of Contents

1. [String Manipulation](#string-manipulation)
2. [Array Operations](#array-operations)
3. [File I/O](#file-io)
4. [JSON Processing](#json-processing)
5. [HTTP Requests](#http-requests)
6. [Data Validation](#data-validation)
7. [Error Handling Patterns](#error-handling-patterns)
8. [Functional Programming](#functional-programming)
9. [Working with Dates](#working-with-dates)
10. [CSV Processing](#csv-processing)
11. [Environment Variables](#environment-variables)
12. [Regular Expressions](#regular-expressions)
13. [Performance Optimization](#performance-optimization)
14. [Testing Patterns](#testing-patterns)

---

## String Manipulation

### Recipe 1.1: String Formatting

```naab
import "stdlib" as std

function format_user(name, age, city) {
    return name + " (" + age + ") from " + city
}

let user = format_user("Alice", 30, "New York")
print(user)  // "Alice (30) from New York"
```

### Recipe 1.2: String Validation

```naab
import "stdlib" as std

function is_valid_email(email) {
    // Check if contains @ and .
    if (!std.contains(email, "@")) {
        return false
    }

    let at_pos = std.indexOf(email, "@")
    let dot_pos = std.lastIndexOf(email, ".")

    return dot_pos > at_pos && dot_pos < std.length(email) - 1
}

print(is_valid_email("alice@example.com"))  // true
print(is_valid_email("invalid.email"))      // false
```

### Recipe 1.3: String Transformation

```naab
import "stdlib" as std

function to_title_case(text) {
    let words = std.split(text, " ")
    let result = []

    for (word in words) {
        if (std.length(word) > 0) {
            let first = std.substring(word, 0, 1)
            let rest = std.substring(word, 1)
            let titled = std.toUpperCase(first) + std.toLowerCase(rest)
            result = std.push(result, titled)
        }
    }

    return std.join(result, " ")
}

print(to_title_case("hello world from naab"))  // "Hello World From Naab"
```

### Recipe 1.4: Template Strings

```naab
import "stdlib" as std

function fill_template(template, values) {
    let result = template

    for (key in std.keys(values)) {
        let placeholder = "{" + key + "}"
        result = std.replace(result, placeholder, values[key])
    }

    return result
}

let template = "Hello {name}, you have {count} new messages"
let data = {"name": "Alice", "count": "5"}
print(fill_template(template, data))
// "Hello Alice, you have 5 new messages"
```

---

## Array Operations

### Recipe 2.1: Array Filtering

```naab
import "stdlib" as std

let numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

// Filter even numbers
let evens = std.filter_fn(numbers, function(n) {
    return n % 2 == 0
})

print(evens)  // [2, 4, 6, 8, 10]
```

### Recipe 2.2: Array Mapping

```naab
import "stdlib" as std

let prices = [10, 20, 30, 40, 50]

// Apply 20% discount
let discounted = std.map_fn(prices, function(price) {
    return price * 0.8
})

print(discounted)  // [8, 16, 24, 32, 40]
```

### Recipe 2.3: Array Reduction

```naab
import "stdlib" as std

let numbers = [1, 2, 3, 4, 5]

// Sum all numbers
let sum = std.reduce_fn(numbers, function(acc, n) {
    return acc + n
}, 0)

print(sum)  // 15
```

### Recipe 2.4: Array Grouping

```naab
import "stdlib" as std

function group_by(array, key_fn) {
    let groups = {}

    for (item in array) {
        let key = key_fn(item)

        if (!std.has(groups, key)) {
            groups[key] = []
        }

        groups[key] = std.push(groups[key], item)
    }

    return groups
}

let people = [
    {"name": "Alice", "age": 25},
    {"name": "Bob", "age": 30},
    {"name": "Charlie", "age": 25}
]

let by_age = group_by(people, function(p) {
    return p["age"]
})

// {"25": [{"name": "Alice", ...}, {"name": "Charlie", ...}],
//  "30": [{"name": "Bob", ...}]}
```

### Recipe 2.5: Array Deduplication

```naab
import "stdlib" as std

function unique(array) {
    let seen = {}
    let result = []

    for (item in array) {
        if (!std.has(seen, item)) {
            seen[item] = true
            result = std.push(result, item)
        }
    }

    return result
}

let numbers = [1, 2, 2, 3, 3, 3, 4, 5, 5]
print(unique(numbers))  // [1, 2, 3, 4, 5]
```

---

## File I/O

### Recipe 3.1: Read and Write Files

```naab
import "stdlib" as std

// Write to file
let data = "Hello, NAAb!\nThis is a test file."
std.write_file("output.txt", data)

// Read from file
let content = std.read_file("output.txt")
print(content)
```

### Recipe 3.2: Append to File

```naab
import "stdlib" as std

function append_log(filename, message) {
    let timestamp = std.now()
    let entry = timestamp + ": " + message + "\n"

    let existing = ""
    if (std.exists(filename)) {
        existing = std.read_file(filename)
    }

    std.write_file(filename, existing + entry)
}

append_log("app.log", "Application started")
append_log("app.log", "User logged in")
```

### Recipe 3.3: Line-by-Line Processing

```naab
import "stdlib" as std

function process_file(filename) {
    let content = std.read_file(filename)
    let lines = std.split(content, "\n")
    let count = 0

    for (line in lines) {
        if (std.length(std.trim(line)) > 0) {
            count = count + 1
            print("Line " + count + ": " + line)
        }
    }

    return count
}

let line_count = process_file("data.txt")
print("Total lines: " + line_count)
```

### Recipe 3.4: Batch File Operations

```naab
import "stdlib" as std

function backup_files(files, backup_dir) {
    let success = []
    let failed = []

    for (file in files) {
        try {
            if (std.exists(file)) {
                let content = std.read_file(file)
                let backup_path = backup_dir + "/" + file + ".bak"
                std.write_file(backup_path, content)
                success = std.push(success, file)
            } else {
                failed = std.push(failed, file + " (not found)")
            }
        } catch (error) {
            failed = std.push(failed, file + " (" + error + ")")
        }
    }

    return {"success": success, "failed": failed}
}

let files = ["config.txt", "data.json", "settings.ini"]
let result = backup_files(files, "backups")
print("Backed up: " + std.length(result["success"]))
print("Failed: " + std.length(result["failed"]))
```

---

## JSON Processing

### Recipe 4.1: Parse and Generate JSON

```naab
import "stdlib" as std

// Parse JSON
let json_str = "{\"name\": \"Alice\", \"age\": 30, \"city\": \"NYC\"}"
let data = std.parse(json_str)

print(data["name"])  // "Alice"
print(data["age"])   // 30

// Generate JSON
let person = {
    "name": "Bob",
    "age": 25,
    "hobbies": ["reading", "coding"]
}

let json_output = std.stringify(person)
print(json_output)
```

### Recipe 4.2: JSON Configuration File

```naab
import "stdlib" as std

function load_config(filename) {
    if (!std.exists(filename)) {
        // Return default config
        return {
            "host": "localhost",
            "port": 8080,
            "debug": false
        }
    }

    let content = std.read_file(filename)
    return std.parse(content)
}

function save_config(filename, config) {
    let json = std.stringify(config)
    std.write_file(filename, json)
}

// Usage
let config = load_config("config.json")
config["debug"] = true
config["port"] = 9000
save_config("config.json", config)
```

### Recipe 4.3: JSON API Response Handling

```naab
import "stdlib" as std

function fetch_users() {
    // Simulated API response
    let response = "[{\"id\": 1, \"name\": \"Alice\"}, {\"id\": 2, \"name\": \"Bob\"}]"
    return std.parse(response)
}

function extract_names(users) {
    return std.map_fn(users, function(user) {
        return user["name"]
    })
}

let users = fetch_users()
let names = extract_names(users)
print(names)  // ["Alice", "Bob"]
```

### Recipe 4.4: Nested JSON Navigation

```naab
import "stdlib" as std

function get_nested(obj, path) {
    let keys = std.split(path, ".")
    let current = obj

    for (key in keys) {
        if (std.has(current, key)) {
            current = current[key]
        } else {
            return undefined
        }
    }

    return current
}

let data = {
    "user": {
        "profile": {
            "name": "Alice",
            "email": "alice@example.com"
        }
    }
}

print(get_nested(data, "user.profile.name"))  // "Alice"
print(get_nested(data, "user.profile.phone"))  // undefined
```

---

## HTTP Requests

### Recipe 5.1: Simple GET Request

```naab
import "stdlib" as std

function fetch_page(url) {
    try {
        let response = std.http_get(url)
        return {"success": true, "data": response}
    } catch (error) {
        return {"success": false, "error": error}
    }
}

let result = fetch_page("https://api.example.com/data")
if (result["success"]) {
    print("Data: " + result["data"])
} else {
    print("Error: " + result["error"])
}
```

### Recipe 5.2: API Client with Error Handling

```naab
import "stdlib" as std

function api_request(endpoint, params) {
    let url = "https://api.example.com" + endpoint

    // Add query parameters
    if (std.length(std.keys(params)) > 0) {
        url = url + "?"
        let first = true
        for (key in std.keys(params)) {
            if (!first) {
                url = url + "&"
            }
            url = url + key + "=" + params[key]
            first = false
        }
    }

    try {
        let response = std.http_get(url)
        return std.parse(response)
    } catch (error) {
        throw "API request failed: " + error
    }
}

// Usage
let data = api_request("/users", {"page": "1", "limit": "10"})
print(data)
```

### Recipe 5.3: Retry Logic

```naab
import "stdlib" as std

function fetch_with_retry(url, max_retries) {
    let attempt = 0

    while (attempt < max_retries) {
        try {
            return std.http_get(url)
        } catch (error) {
            attempt = attempt + 1
            if (attempt >= max_retries) {
                throw "Failed after " + max_retries + " attempts: " + error
            }
            print("Retry " + attempt + "/" + max_retries)
            std.sleep(1000)  // Wait 1 second
        }
    }
}

let data = fetch_with_retry("https://api.example.com/data", 3)
```

---

## Data Validation

### Recipe 6.1: Email Validation

```naab
import "stdlib" as std

function validate_email(email) {
    if (std.length(email) == 0) {
        return {"valid": false, "error": "Email cannot be empty"}
    }

    if (!std.contains(email, "@")) {
        return {"valid": false, "error": "Email must contain @"}
    }

    if (std.startsWith(email, "@") || std.endsWith(email, "@")) {
        return {"valid": false, "error": "Invalid @ position"}
    }

    return {"valid": true}
}

let result = validate_email("alice@example.com")
print(result["valid"])  // true
```

### Recipe 6.2: Form Validation

```naab
import "stdlib" as std

function validate_registration(data) {
    let errors = []

    // Username validation
    if (!std.has(data, "username") || std.length(data["username"]) < 3) {
        errors = std.push(errors, "Username must be at least 3 characters")
    }

    // Password validation
    if (!std.has(data, "password") || std.length(data["password"]) < 8) {
        errors = std.push(errors, "Password must be at least 8 characters")
    }

    // Email validation
    if (!std.has(data, "email") || !std.contains(data["email"], "@")) {
        errors = std.push(errors, "Valid email required")
    }

    return {
        "valid": std.length(errors) == 0,
        "errors": errors
    }
}

let user_data = {
    "username": "alice",
    "password": "secret123",
    "email": "alice@example.com"
}

let result = validate_registration(user_data)
if (result["valid"]) {
    print("Registration valid!")
} else {
    print("Errors: " + std.join(result["errors"], ", "))
}
```

### Recipe 6.3: Schema Validation

```naab
import "stdlib" as std

function validate_schema(data, schema) {
    let errors = []

    for (field in std.keys(schema)) {
        let rules = schema[field]

        // Required check
        if (std.has(rules, "required") && rules["required"]) {
            if (!std.has(data, field)) {
                errors = std.push(errors, field + " is required")
                continue
            }
        }

        if (std.has(data, field)) {
            let value = data[field]

            // Type check
            if (std.has(rules, "type")) {
                // Type checking logic here
            }

            // Min length check
            if (std.has(rules, "min_length")) {
                if (std.length(value) < rules["min_length"]) {
                    errors = std.push(errors, field + " too short")
                }
            }

            // Max length check
            if (std.has(rules, "max_length")) {
                if (std.length(value) > rules["max_length"]) {
                    errors = std.push(errors, field + " too long")
                }
            }
        }
    }

    return {
        "valid": std.length(errors) == 0,
        "errors": errors
    }
}

let schema = {
    "username": {"required": true, "min_length": 3, "max_length": 20},
    "password": {"required": true, "min_length": 8},
    "bio": {"required": false, "max_length": 500}
}

let data = {"username": "alice", "password": "secret123"}
let result = validate_schema(data, schema)
```

---

## Error Handling Patterns

### Recipe 7.1: Result Type Pattern

```naab
function divide(a, b) {
    if (b == 0) {
        return {"ok": false, "error": "Division by zero"}
    }
    return {"ok": true, "value": a / b}
}

let result = divide(10, 2)
if (result["ok"]) {
    print("Result: " + result["value"])
} else {
    print("Error: " + result["error"])
}
```

### Recipe 7.2: Try-Catch with Cleanup

```naab
import "stdlib" as std

function process_file(filename) {
    let file_handle = undefined

    try {
        file_handle = std.read_file(filename)
        let lines = std.split(file_handle, "\n")
        return std.length(lines)
    } catch (error) {
        print("Error processing file: " + error)
        return -1
    } finally {
        // Cleanup logic
        print("Cleanup complete")
    }
}

let count = process_file("data.txt")
```

### Recipe 7.3: Custom Error Types

```naab
function create_error(type, message, details) {
    return {
        "type": type,
        "message": message,
        "details": details,
        "timestamp": std.now()
    }
}

function validate_age(age) {
    if (age < 0) {
        throw create_error("ValidationError", "Age cannot be negative", {"age": age})
    }
    if (age > 150) {
        throw create_error("ValidationError", "Age too large", {"age": age})
    }
    return true
}

try {
    validate_age(-5)
} catch (error) {
    print("Error type: " + error["type"])
    print("Message: " + error["message"])
    print("Details: " + std.stringify(error["details"]))
}
```

---

## Functional Programming

### Recipe 8.1: Function Composition

```naab
import "stdlib" as std

function compose(f, g) {
    return function(x) {
        return f(g(x))
    }
}

function double(x) {
    return x * 2
}

function add_ten(x) {
    return x + 10
}

let double_then_add_ten = compose(add_ten, double)
print(double_then_add_ten(5))  // 20 (5*2=10, 10+10=20)
```

### Recipe 8.2: Partial Application

```naab
function partial(fn, ...fixed_args) {
    return function(...remaining_args) {
        let all_args = std.concat(fixed_args, remaining_args)
        return fn(...all_args)
    }
}

function greet(greeting, name) {
    return greeting + ", " + name + "!"
}

let say_hello = partial(greet, "Hello")
print(say_hello("Alice"))  // "Hello, Alice!"
print(say_hello("Bob"))    // "Hello, Bob!"
```

### Recipe 8.3: Pipeline Pattern

```naab
import "stdlib" as std

let numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]

let result = numbers
    |> std.filter_fn(function(n) { return n % 2 == 0 })
    |> std.map_fn(function(n) { return n * n })
    |> std.reduce_fn(function(acc, n) { return acc + n }, 0)

print(result)  // Sum of squares of even numbers: 220
```

---

## Working with Dates

### Recipe 9.1: Timestamp Operations

```naab
import "stdlib" as std

function get_current_timestamp() {
    return std.now()
}

function format_timestamp(ts) {
    // Convert to readable format (simplified)
    return "Timestamp: " + ts
}

let now = get_current_timestamp()
print(format_timestamp(now))
```

### Recipe 9.2: Time Elapsed

```naab
import "stdlib" as std

function measure_execution(fn) {
    let start = std.now()
    fn()
    let end = std.now()
    let elapsed = end - start
    return elapsed
}

function slow_operation() {
    let sum = 0
    let i = 0
    while (i < 1000000) {
        sum = sum + i
        i = i + 1
    }
    return sum
}

let time = measure_execution(slow_operation)
print("Execution time: " + time + "ms")
```

---

## CSV Processing

### Recipe 10.1: Parse CSV

```naab
import "stdlib" as std

function parse_csv(content) {
    let lines = std.split(content, "\n")
    let rows = []

    for (line in lines) {
        if (std.length(std.trim(line)) > 0) {
            let cells = std.split(line, ",")
            rows = std.push(rows, cells)
        }
    }

    return rows
}

let csv = "name,age,city\nAlice,30,NYC\nBob,25,LA"
let data = parse_csv(csv)

// Print each row
for (row in data) {
    print(std.join(row, " | "))
}
```

### Recipe 10.2: CSV to JSON

```naab
import "stdlib" as std

function csv_to_json(csv_content) {
    let lines = std.split(csv_content, "\n")
    let headers = std.split(lines[0], ",")
    let records = []

    let i = 1
    while (i < std.length(lines)) {
        let line = lines[i]
        if (std.length(std.trim(line)) > 0) {
            let cells = std.split(line, ",")
            let record = {}

            let j = 0
            while (j < std.length(headers)) {
                record[headers[j]] = cells[j]
                j = j + 1
            }

            records = std.push(records, record)
        }
        i = i + 1
    }

    return records
}

let csv = "name,age,city\nAlice,30,NYC\nBob,25,LA"
let json = csv_to_json(csv)
print(std.stringify(json))
```

---

## Environment Variables

### Recipe 11.1: Get Environment Variable

```naab
import "stdlib" as std

function get_env(key, default_value) {
    let value = std.getenv(key)
    if (value == undefined || std.length(value) == 0) {
        return default_value
    }
    return value
}

let home = get_env("HOME", "/default/home")
let user = get_env("USER", "anonymous")
print("User: " + user)
print("Home: " + home)
```

### Recipe 11.2: Configuration from Environment

```naab
import "stdlib" as std

function load_config_from_env() {
    return {
        "database_url": get_env("DATABASE_URL", "sqlite://local.db"),
        "api_key": get_env("API_KEY", ""),
        "debug": get_env("DEBUG", "false") == "true",
        "port": get_env("PORT", "8080")
    }
}

let config = load_config_from_env()
print("Config: " + std.stringify(config))
```

---

## Regular Expressions

### Recipe 12.1: Pattern Matching

```naab
import "stdlib" as std

function extract_emails(text) {
    // Simplified email extraction
    let words = std.split(text, " ")
    let emails = []

    for (word in words) {
        if (std.contains(word, "@") && std.contains(word, ".")) {
            emails = std.push(emails, word)
        }
    }

    return emails
}

let text = "Contact alice@example.com or bob@test.org for help"
let emails = extract_emails(text)
print(emails)  // ["alice@example.com", "bob@test.org"]
```

---

## Performance Optimization

### Recipe 13.1: Memoization

```naab
function memoize(fn) {
    let cache = {}

    return function(x) {
        if (std.has(cache, x)) {
            return cache[x]
        }

        let result = fn(x)
        cache[x] = result
        return result
    }
}

function fibonacci(n) {
    if (n <= 1) {
        return n
    }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

let fast_fib = memoize(fibonacci)
print(fast_fib(10))  // Much faster than plain fibonacci
```

### Recipe 13.2: Lazy Evaluation

```naab
import "stdlib" as std

function lazy_range(start, end) {
    return {
        "start": start,
        "end": end,
        "to_array": function() {
            let result = []
            let i = start
            while (i < end) {
                result = std.push(result, i)
                i = i + 1
            }
            return result
        }
    }
}

let range = lazy_range(0, 1000)
// Range not computed yet

let array = range["to_array"]()
// Now computed
```

---

## Testing Patterns

### Recipe 14.1: Unit Test Framework

```naab
import "stdlib" as std

function assert_equals(actual, expected, message) {
    if (actual != expected) {
        throw "FAIL: " + message + " (expected " + expected + ", got " + actual + ")"
    }
    print("PASS: " + message)
}

function test_string_length() {
    assert_equals(std.length("hello"), 5, "string length")
}

function test_array_push() {
    let arr = [1, 2, 3]
    arr = std.push(arr, 4)
    assert_equals(std.length(arr), 4, "array push")
}

// Run tests
test_string_length()
test_array_push()
print("All tests passed!")
```

### Recipe 14.2: Integration Testing

```naab
import "stdlib" as std

function test_suite(name, tests) {
    print("\n=== " + name + " ===")
    let passed = 0
    let failed = 0

    for (test in tests) {
        try {
            test()
            passed = passed + 1
        } catch (error) {
            print("FAILED: " + error)
            failed = failed + 1
        }
    }

    print("\nResults: " + passed + " passed, " + failed + " failed")
    return failed == 0
}

// Usage
let all_passed = test_suite("String Tests", [
    function() { assert_equals(std.length("test"), 4, "length") },
    function() { assert_equals(std.toUpperCase("abc"), "ABC", "uppercase") }
])

if (all_passed) {
    print("\n✓ All test suites passed!")
}
```

---

## Advanced Recipes

### Recipe 15: State Machine

```naab
function create_state_machine(initial, transitions) {
    let current = initial

    return {
        "state": function() { return current },
        "transition": function(event) {
            let key = current + ":" + event
            if (std.has(transitions, key)) {
                current = transitions[key]
                return true
            }
            return false
        }
    }
}

// Usage: Traffic light
let traffic_light = create_state_machine("red", {
    "red:timer": "green",
    "green:timer": "yellow",
    "yellow:timer": "red"
})

print(traffic_light["state"]())  // "red"
traffic_light["transition"]("timer")
print(traffic_light["state"]())  // "green"
```

### Recipe 16: Event Emitter

```naab
function create_event_emitter() {
    let listeners = {}

    return {
        "on": function(event, callback) {
            if (!std.has(listeners, event)) {
                listeners[event] = []
            }
            listeners[event] = std.push(listeners[event], callback)
        },
        "emit": function(event, data) {
            if (std.has(listeners, event)) {
                for (callback in listeners[event]) {
                    callback(data)
                }
            }
        }
    }
}

// Usage
let emitter = create_event_emitter()

emitter["on"]("message", function(data) {
    print("Received: " + data)
})

emitter["emit"]("message", "Hello!")
```

---

## More Examples

See the [tutorials](docs/tutorials/) directory for complete applications:

- [Getting Started](docs/tutorials/01_getting_started.md) - Basic syntax and REPL
- [First Application](docs/tutorials/02_first_application.md) - Todo app with file persistence
- [Multi-File Apps](docs/tutorials/03_multi_file_apps.md) - Module system and imports
- [Block Integration](docs/tutorials/04_block_integration.md) - Using the 24,483-block registry

---

## Contributing Recipes

Have a useful recipe? Submit it via pull request:

1. Fork the repository
2. Add your recipe to this file
3. Include clear description and example code
4. Submit PR with "Add recipe:" prefix

---

**Last Updated**: December 30, 2024
**NAAb Version**: 0.1.0
