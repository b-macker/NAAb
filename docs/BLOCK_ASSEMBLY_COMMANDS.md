# Block Assembly System - Command Reference

Quick reference for using NAAb's 24,515+ pre-built code blocks.

## 🚀 First Time Setup (IMPORTANT!)

Before using blocks, you need to build the search index once:

```bash
cd ~/.naab/language/build
./naab-lang blocks index ~/.naab/language/blocks/library
```

This will:
- Scan all 585+ Python blocks (and others)
- Create a SQLite FTS5 search index
- Enable fast searching (<100ms)
- Only needs to be run once (or when blocks are updated)

Expected output:
```
[INFO] Building search index from: /home/user/.naab/language/blocks/library
[INFO] Indexed 585 blocks successfully
```

## 🔍 Searching for Blocks

### Basic Search
```bash
./naab-lang blocks search "validate email"
./naab-lang blocks search "parse json"
./naab-lang blocks search "http request"
./naab-lang blocks search "sort array"
./naab-lang blocks search "fibonacci"
```

### Search by Language
```bash
./naab-lang blocks search "validate email" --language python
./naab-lang blocks search "sort array" --language javascript
./naab-lang blocks search "http get" --language cpp
```

### Search by Type Signature
```bash
./naab-lang blocks search "string -> int"
./naab-lang blocks search "list -> list"
./naab-lang blocks search "dict -> string"
```

### Search Results Format
```
Search results for "validate email":

1. BLOCK-PY-09145 (python)
   Validate email address format
   Input: string → Output: bool
   Score: 0.95

2. BLOCK-JS-03421 (javascript)
   Email validation with regex
   Input: string → Output: bool
   Score: 0.87
```

## 📋 Listing Blocks

### List All Blocks
```bash
./naab-lang blocks list
```

Output:
```
Total blocks: 24,515

By language:
  Python:     8,234 blocks
  C++:        7,621 blocks
  JavaScript: 5,142 blocks
  Go:         2,341 blocks
  Rust:         845 blocks
  Java:         300 blocks
  Ruby:         172 blocks
  Swift:        100 blocks
```

### Filter by Language
```bash
./naab-lang blocks list --language python
./naab-lang blocks list --language javascript
./naab-lang blocks list --language cpp
```

### Filter by Category
```bash
./naab-lang blocks list --category validation
./naab-lang blocks list --category data-processing
./naab-lang blocks list --category algorithms
./naab-lang blocks list --category web
./naab-lang blocks list --category cryptography
```

## ℹ️ Block Information

### Get Detailed Info
```bash
./naab-lang blocks info BLOCK-PY-09145
```

Output:
```
Block ID: BLOCK-PY-09145
Language: Python
Category: validation
Description: Validate email address format using RFC 5322 regex

Signature:
  Input: string (email address)
  Output: bool (true if valid, false otherwise)

Dependencies: re (Python standard library)

Example usage:
  use BLOCK-PY-09145 as validate_email
  let is_valid = validate_email("user@example.com")

Performance: O(n) where n is email length
Tested: Yes (100% pass rate on RFC 5322 test suite)
Security: Safe (no code execution)
```

## ✅ Validating Block Composition

### Validate Block Compatibility
```bash
# Check if two blocks can be chained
./naab-lang validate "BLOCK-PY-09145,BLOCK-JS-03421"

# Validate multiple block pipeline
./naab-lang validate "BLOCK-PY-10234,BLOCK-JS-05678,BLOCK-PY-11234"
```

Success output:
```
Validation results:

✓ BLOCK-PY-09145 → BLOCK-JS-03421
  Compatible: Output type (bool) matches input type

Composition is valid!
```

Error output:
```
Validation results:

✗ BLOCK-PY-10234 → BLOCK-GO-05678
  Type mismatch: Output (dict) incompatible with input (string)
  Suggestion: Add type conversion block

Composition has issues!
```

## 💻 Using Blocks in Code

### Basic Usage
```naab
# Import a block
use BLOCK-PY-09145 as validate_email

# Use it
let email = "alice@example.com"
let is_valid = validate_email(email)

if is_valid {
    print("Valid email!")
}
```

### Multiple Blocks
```naab
# Import multiple blocks
use BLOCK-PY-09145 as validate_email
use BLOCK-PY-12345 as hash_password
use BLOCK-JS-05678 as sanitize_input

# Use them
let email = "alice@example.com"
let password = "secret123"
let username = "alice_2024"

if validate_email(email) {
    let hashed = hash_password(password)
    let clean = sanitize_input(username)
    print("User data prepared!")
}
```

### Chaining Blocks
```naab
use BLOCK-PY-10234 as parse_json
use BLOCK-JS-05678 as transform_data
use BLOCK-PY-11234 as to_csv

# Chain them
let json_data = '{"name": "Alice", "age": 30}'
let parsed = parse_json(json_data)
let transformed = transform_data(parsed)
let csv = to_csv(transformed)

print(csv)
```

## 🔎 Finding Similar Blocks

### Discover Related Blocks
```bash
./naab-lang blocks similar BLOCK-PY-09145
```

Output:
```
Similar blocks to BLOCK-PY-09145:

1. BLOCK-JS-03421 (javascript)
   Email validation with regex
   Similarity: 0.92

2. BLOCK-PY-09876 (python)
   Advanced email validation with MX check
   Similarity: 0.88

3. BLOCK-GO-05432 (go)
   Email format validator
   Similarity: 0.85
```

## 🔄 Updating Block Database

### Update to Latest Blocks
```bash
./naab-lang blocks update
```

Output:
```
Updating block database...
Downloaded 245 new blocks
Updated 18 existing blocks
Removed 3 deprecated blocks

Total blocks: 24,515
Database version: 2.1.0
Last updated: 2026-01-20
```

## 📦 Creating Your Own Blocks

### Create Block from Code
```bash
# Create a new block
./naab-lang blocks create my_function.py \
    --name "My Custom Function" \
    --description "Calculates something useful" \
    --input string \
    --output int \
    --category custom
```

### Test Your Block
```bash
./naab-lang blocks test BLOCK-CUSTOM-00001
```

### Submit to Registry (Public)
```bash
./naab-lang blocks submit BLOCK-CUSTOM-00001 \
    --author "Your Name" \
    --license MIT \
    --public
```

## 🏷️ Block Categories

Common block categories in the registry:

| Category | Description | Example Blocks |
|----------|-------------|----------------|
| **validation** | Input validation, format checking | Email, URL, phone validation |
| **data-processing** | Data transformation, parsing | JSON, CSV, XML parsing |
| **algorithms** | Sorting, searching, etc. | Quick sort, binary search |
| **web** | HTTP, HTML, API clients | HTTP GET/POST, web scraping |
| **file** | File operations | Read, write, copy, move |
| **cryptography** | Hashing, encryption | SHA-256, AES, password hashing |
| **datetime** | Date/time operations | Parsing, formatting, timezone |
| **math** | Mathematical functions | Statistics, linear algebra |
| **string** | String manipulation | Regex, formatting, encoding |
| **database** | Database operations | Query builders, ORMs |

## 📊 Block Statistics

### Get Database Stats
```bash
./naab-lang blocks stats
```

Output:
```
Block Database Statistics:

Total blocks: 24,515
Total languages: 8
Total categories: 25
Total authors: 1,234

Most popular language: Python (8,234 blocks)
Most popular category: data-processing (3,456 blocks)
Average block rating: 4.7/5.0
Total downloads: 1,245,678
```

## 🔧 Advanced Commands

### Export Blocks
```bash
# Export specific blocks
./naab-lang blocks export BLOCK-PY-09145 > my_block.json

# Export by category
./naab-lang blocks export --category validation > validation_blocks.json
```

### Import Blocks
```bash
./naab-lang blocks import my_blocks.json
```

### Backup Database
```bash
./naab-lang blocks backup > blocks_backup_2026_01_20.db
```

### Restore Database
```bash
./naab-lang blocks restore blocks_backup_2026_01_20.db
```

## 🐛 Troubleshooting

### Block Not Found
```bash
# Update database first
./naab-lang blocks update

# Then search again
./naab-lang blocks search "your query"
```

### Type Mismatch Errors
```bash
# Check block signatures
./naab-lang blocks info BLOCK-ID

# Validate composition
./naab-lang validate "BLOCK-1,BLOCK-2"
```

### Block Execution Fails
```bash
# Check block dependencies
./naab-lang blocks info BLOCK-ID

# Report issue
./naab-lang blocks report BLOCK-ID \
    --issue "Execution fails with error: ..." \
    --context "Input was: ..."
```

## 📚 Quick Reference Table

| Task | Command |
|------|---------|
| Search blocks | `./naab-lang blocks search "query"` |
| List all blocks | `./naab-lang blocks list` |
| Get block info | `./naab-lang blocks info BLOCK-ID` |
| Validate chain | `./naab-lang validate "BLOCK-1,BLOCK-2"` |
| Find similar | `./naab-lang blocks similar BLOCK-ID` |
| Update database | `./naab-lang blocks update` |
| Create block | `./naab-lang blocks create file.py` |
| Test block | `./naab-lang blocks test BLOCK-ID` |
| Get stats | `./naab-lang blocks stats` |

## 🎯 Usage Examples

### Example 1: Email Validation Pipeline
```bash
# Search for blocks
./naab-lang blocks search "validate email"
./naab-lang blocks search "sanitize input"

# Validate they work together
./naab-lang validate "BLOCK-JS-05678,BLOCK-PY-09145"

# Use in code
```
```naab
use BLOCK-JS-05678 as sanitize
use BLOCK-PY-09145 as validate_email

let email = sanitize("  Alice@Example.COM  ")
if validate_email(email) {
    print("Valid!")
}
```

### Example 2: Data Processing
```bash
# Find data processing blocks
./naab-lang blocks list --category data-processing

# Get details
./naab-lang blocks info BLOCK-PY-10234
```
```naab
use BLOCK-PY-10234 as parse_csv
use BLOCK-JS-05432 as transform
use BLOCK-PY-11234 as to_json

let csv = "name,age\nAlice,30\nBob,25"
let data = parse_csv(csv)
let transformed = transform(data)
let json = to_json(transformed)
```

## 💡 Pro Tips

1. **Search first** - Most common tasks already have blocks
2. **Validate chains** - Always validate block composition before using
3. **Check metadata** - Read performance and security info
4. **Update regularly** - New blocks added frequently
5. **Contribute back** - Share your useful blocks with the community
6. **Use similar** - Find alternative implementations
7. **Read docs** - Block info includes usage examples

## 🌐 Block Registry Location

Database file: `~/.naab/language/build/blocks.db`
Size: ~50MB
Blocks: 24,515+
Last updated: Check with `./naab-lang blocks stats`

---

**Happy block hunting! 🚀**
