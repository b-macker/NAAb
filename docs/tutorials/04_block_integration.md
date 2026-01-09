# Tutorial 4: Block Integration

**Duration**: 25 minutes
**Prerequisites**: [Tutorial 3: Multi-File Apps](03_multi_file_apps.md)
**What you'll learn**: Search for blocks, validate composition, integrate pre-built blocks

---

## Introduction

NAAb's power comes from its block registry containing 24,483 pre-built, tested code blocks across multiple languages. Instead of writing everything from scratch, you can assemble applications from existing blocks.

---

## Step 1: Searching for Blocks

### Search by Functionality

```bash
~/naab-lang blocks search "validate email"
```

Output:
```
Search results for "validate email":

1. BLOCK-PY-09145 (python)
   Validate email address format using regex
   Input: string → Output: bool
   Score: 0.95
   Performance: fast (avg 2ms)
   Success rate: 99%

2. BLOCK-JS-03421 (javascript)
   Email validation with RFC 5322 compliance
   Input: string → Output: bool
   Score: 0.87
   Performance: medium (avg 8ms)
   Success rate: 97%
```

### Search by Language

```bash
~/naab-lang blocks search "parse json" --language python
```

### View All Available Blocks

```bash
~/naab-lang blocks list
```

Output:
```
Total blocks: 24,483

By language:
  Python:     8,234 blocks (34%)
  C++:        7,621 blocks (31%)
  JavaScript: 5,142 blocks (21%)
  Go:         2,341 blocks (10%)
  Rust:         845 blocks (3%)
  Java:         300 blocks (1%)

By category:
  Data Processing: 6,234
  Validation:      3,421
  API/HTTP:        2,876
  File I/O:        2,145
  ...
```

---

## Step 2: Using a Block

### Simple Block Integration

```naab
// Import block by ID
use BLOCK-PY-09145 as validate_email

let email1 = "alice@example.com"
let email2 = "not_an_email"

if (validate_email(email1)) {
    print(email1 + " is valid")
} else {
    print(email1 + " is invalid")
}

if (validate_email(email2)) {
    print(email2 + " is valid")
} else {
    print(email2 + " is invalid")
}
```

Output:
```
alice@example.com is valid
not_an_email is invalid
```

---

## Step 3: Composing Blocks

### Find Blocks for a Pipeline

```bash
~/naab-lang blocks search "fetch HTTP"
~/naab-lang blocks search "parse HTML"
~/naab-lang blocks search "extract links"
```

### Validate Composition

Before using blocks together, validate their types match:

```bash
~/naab-lang validate "BLOCK-PY-05231,BLOCK-JS-07842,BLOCK-CPP-03421"
```

Output:
```
Validating block composition...

✓ BLOCK-PY-05231 → BLOCK-JS-07842
  Output: string matches Input: string

✗ BLOCK-JS-07842 → BLOCK-CPP-03421
  Output: object does not match Input: array

  Suggested adapters:
    - BLOCK-PY-08765: Convert object to array
    - BLOCK-JS-02341: Flatten object to array

Validation: FAILED
Adapter needed at position 2
```

### Build Valid Pipeline

```naab
use BLOCK-PY-05231 as fetch_page
use BLOCK-JS-07842 as parse_html
use BLOCK-PY-08765 as object_to_array  // Adapter
use BLOCK-CPP-03421 as filter_links

let url = "https://example.com"

// Manual composition
let page = fetch_page(url)
let parsed = parse_html(page)
let array_data = object_to_array(parsed)
let links = filter_links(array_data)

print("Found " + length(links) + " links")
```

### Using Pipeline Syntax

```naab
let links = url
    |> fetch_page
    |> parse_html
    |> object_to_array
    |> filter_links

print("Found " + length(links) + " links")
```

---

## Step 4: Working with Block Metadata

### Check Block Information

```bash
~/naab-lang blocks info BLOCK-PY-09145
```

Output:
```
Block ID: BLOCK-PY-09145
Language: Python
Category: Validation
Description: Validate email address format using regex

Input Types: string
Output Type: bool

Performance:
  Average execution: 2.3ms
  Max memory: 1MB
  Performance tier: fast

Quality:
  Success rate: 99%
  Test coverage: 95%
  Security audited: Yes

Usage:
  Times used: 15,234
  Tokens saved (avg): 45
  Related blocks:
    - BLOCK-PY-09146 (validate URL)
    - BLOCK-PY-09147 (validate phone)
```

---

## Step 5: Building a Real Application

Let's build a web scraper using blocks:

### Find Required Blocks

```bash
~/naab-lang blocks search "HTTP GET request"
~/naab-lang blocks search "parse HTML table"
~/naab-lang blocks search "save to CSV"
```

### Application Code

```naab
use BLOCK-PY-12034 as http_get
use BLOCK-JS-08234 as parse_table
use BLOCK-CPP-03912 as save_csv

function scrape_table(url, output_file) {
    try {
        print("Fetching " + url + "...")
        let html = http_get(url)

        print("Parsing table...")
        let table_data = parse_table(html)

        print("Saving to " + output_file + "...")
        save_csv(output_file, table_data)

        print("✓ Done! Saved to " + output_file)
    } catch (error) {
        print("Error: " + error)
    }
}

// Scrape data
scrape_table("https://example.com/data.html", "output.csv")
```

---

## Step 6: Error Handling with Blocks

### Blocks Can Throw Errors

```naab
use BLOCK-PY-05231 as fetch_page

function safe_fetch(url) {
    try {
        return fetch_page(url)
    } catch (error) {
        print("Failed to fetch " + url + ": " + error)
        return ""
    } finally {
        print("Fetch attempt completed")
    }
}

let page = safe_fetch("https://example.com")
```

---

## Step 7: Performance Optimization

### Choose Blocks by Performance

Search with performance filter:

```bash
~/naab-lang blocks search "sort array" --performance fast
```

### Compare Block Performance

```naab
use BLOCK-PY-04523 as sort_python
use BLOCK-CPP-08234 as sort_cpp

let big_array = generate_random_array(10000)

// Benchmark Python block
let start1 = timestamp()
let result1 = sort_python(big_array)
let time1 = timestamp() - start1

// Benchmark C++ block
let start2 = timestamp()
let result2 = sort_cpp(big_array)
let time2 = timestamp() - start2

print("Python block: " + time1 + "ms")
print("C++ block: " + time2 + "ms")
```

---

## Step 8: Advanced Composition

### Conditional Block Selection

```naab
use BLOCK-PY-09145 as validate_email_py
use BLOCK-JS-03421 as validate_email_js

function validate_email(email, prefer_python = true) {
    if (prefer_python) {
        return validate_email_py(email)
    } else {
        return validate_email_js(email)
    }
}
```

### Dynamic Block Loading

```naab
function get_validator(type) {
    if (type == "email") {
        use BLOCK-PY-09145 as validator
    } else if (type == "url") {
        use BLOCK-PY-09146 as validator
    } else if (type == "phone") {
        use BLOCK-PY-09147 as validator
    }

    return validator
}

let email_validator = get_validator("email")
print(email_validator("test@example.com"))
```

---

## Step 9: Best Practices

### 1. Always Validate Composition

```bash
# Before writing code
~/naab-lang validate "block1,block2,block3"
```

### 2. Check Block Quality

Prefer blocks with:
- Success rate >95%
- High test coverage
- Security audited
- Good performance

### 3. Handle Errors

```naab
// Bad
let result = risky_block(input)

// Good
try {
    let result = risky_block(input)
    process(result)
} catch (error) {
    log_error(error)
    use_fallback()
}
```

### 4. Use Type Annotations

```naab
use BLOCK-PY-09145 as validate_email

function process_user(email: string): bool {
    return validate_email(email)
}
```

### 5. Monitor Block Usage

```bash
# View analytics
~/naab-lang stats

# See most used blocks
# See token savings
# See common combinations
```

---

## Step 10: Finding Related Blocks

### From Block Info

```bash
~/naab-lang blocks info BLOCK-PY-09145
```

Shows related blocks:
- BLOCK-PY-09146 (validate URL)
- BLOCK-PY-09147 (validate phone)
- BLOCK-PY-09148 (sanitize email)

### From Usage Analytics

```bash
~/naab-lang stats combinations
```

Shows blocks commonly used together:
```
Top block combinations:
1. BLOCK-PY-05231 → BLOCK-JS-07842 (used together 523 times)
2. BLOCK-CPP-08234 → BLOCK-PY-04523 (used together 412 times)
...
```

---

## Common Block Categories

### Data Validation
- Email validation
- URL validation
- Phone number validation
- Credit card validation
- Password strength

### Data Transformation
- JSON ↔ XML
- CSV ↔ JSON
- Date format conversion
- String encoding
- Data normalization

### Web Operations
- HTTP requests (GET, POST, etc.)
- HTML parsing
- XML parsing
- Web scraping
- API clients

### File Operations
- Read/write files
- Parse CSV/JSON/XML
- File compression
- File encryption
- Directory operations

### Data Processing
- Sorting algorithms
- Filtering
- Mapping
- Aggregation
- Statistics

---

## Example: Complete ETL Pipeline

```naab
use BLOCK-PY-05231 as fetch_api
use BLOCK-JS-07231 as parse_json
use BLOCK-CPP-08123 as transform_data
use BLOCK-PY-04782 as validate_schema
use BLOCK-CPP-09234 as save_database

function etl_pipeline(api_url, db_config) {
    try {
        print("Starting ETL pipeline...")

        // Extract
        print("1. Extracting data from API...")
        let raw_data = fetch_api(api_url)

        // Transform
        print("2. Parsing JSON...")
        let parsed = parse_json(raw_data)

        print("3. Transforming data...")
        let transformed = transform_data(parsed)

        // Validate
        print("4. Validating schema...")
        let valid = validate_schema(transformed)
        if (!valid) {
            throw "Schema validation failed"
        }

        // Load
        print("5. Saving to database...")
        save_database(db_config, transformed)

        print("✓ ETL pipeline complete!")
        return true
    } catch (error) {
        print("ETL pipeline failed: " + error)
        return false
    }
}

// Run pipeline
let success = etl_pipeline(
    "https://api.example.com/data",
    {"host": "localhost", "database": "analytics"}
)
```

---

## Troubleshooting

### Block Not Found

```
Error: Block BLOCK-PY-09145 not found
```

Solution: Check block ID spelling, ensure block registry is up to date

### Type Mismatch

```
Error: Type mismatch - expected string, got int
```

Solution: Use `~/naab-lang validate` to check composition, add type adapters

### Block Execution Failed

```
Error: Block execution failed with code 1
```

Solution: Check block documentation, validate inputs, add try/catch

---

## Next Steps

- **Cookbook**: [../COOKBOOK.md](../COOKBOOK.md) for more examples
- **API Reference**: [../API_REFERENCE.md](../API_REFERENCE.md)
- **Architecture**: [../ARCHITECTURE.md](../ARCHITECTURE.md)

---

**Congratulations!** You can now leverage NAAb's 24,483-block registry to build powerful applications with minimal code.
