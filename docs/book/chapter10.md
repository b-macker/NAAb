# Chapter 10: The Block Registry

One of NAAb's core strengths is its ecosystem of reusable code blocks. The Block Assembly System allows you to discover, inspect, and integrate pre-built functionality from a vast library of over 24,000 blocks across 8 programming languages. This chapter explains how to navigate the Block Registry and use blocks in your programs.

## 10.1 Understanding Block Assembly

A "block" in NAAb is a self-contained unit of code — often written in Python, C++, JavaScript, or another language — that performs a specific task. Each block has a unique ID (e.g., `BLOCK-PY-09145`), metadata describing its inputs, outputs, and dependencies, and the source code itself.

The Block Registry is a SQLite database that indexes these blocks for fast discovery. Instead of writing polyglot snippets from scratch for every task, you can search the registry for an existing block and `use` it directly.

## 10.2 First-Time Setup

Before searching for blocks, you must build the search index once:

```bash
naab-lang blocks index ~/.naab/language/blocks/library
```

This scans all block JSON files and creates a SQLite Full-Text Search (FTS5) index. You only need to run this once, or again when new blocks are added to the library.

## 10.3 CLI Commands

### 10.3.1 Searching for Blocks

```bash
# Search by keyword
naab-lang blocks search "validate email"

# Search by type signature
naab-lang blocks search "string -> int"
```

Search results show the Block ID, language, description, and relevance score:

```
Search results for "validate email":

1. BLOCK-PY-09145 (python)
   Validate email address format
   Input: string -> Output: bool
   Score: 0.95

2. BLOCK-JS-03421 (javascript)
   Email validation with regex
   Input: string -> Output: bool
   Score: 0.87
```

### 10.3.2 Listing Blocks

```bash
# Show overview with language distribution
naab-lang blocks list

# Overall usage statistics
naab-lang stats
```

### 10.3.3 Inspecting a Block

Get detailed metadata for a specific block:

```bash
naab-lang blocks info BLOCK-PY-09145
```

This displays the block's description, input/output types, language, dependencies, and usage example.

### 10.3.4 Validating Block Composition

Before chaining blocks in a pipeline, validate their type compatibility:

```bash
naab-lang validate "BLOCK-PY-10234,BLOCK-JS-05678"
```

## 10.4 Using Blocks in Code

Blocks are imported using the `use ... as ...` syntax and called like regular functions:

```naab
use BLOCK-PY-09145 as validate_email

main {
    let email = "user@example.com"
    let is_valid = validate_email(email)

    if is_valid {
        print("Valid email!")
    } else {
        print("Invalid email!")
    }
}
```

### 10.4.1 Multiple Blocks

```naab
use BLOCK-PY-09145 as validate_email
use BLOCK-PY-12345 as hash_password
use BLOCK-JS-05678 as sanitize_input

main {
    let email = "alice@example.com"

    if validate_email(email) {
        let hashed = hash_password("secret123")
        let clean = sanitize_input("alice_2024")
        print("User data prepared!")
    }
}
```

### 10.4.2 Block Pipelines

Blocks can be chained using the pipeline operator for clean data flow:

```naab
use BLOCK-PY-10234 as parse_json
use BLOCK-JS-05678 as transform_data
use BLOCK-PY-11234 as to_csv

main {
    let json_data = '{"name": "Alice", "age": 30}'

    let csv = json_data
        |> parse_json
        |> transform_data
        |> to_csv

    print(csv)
}
```

## 10.5 Block System Architecture

### 10.5.1 Block JSON Format

Each block is stored as a JSON file in `blocks/library/<language>/`:

```json
{
  "id": "BLOCK-PY-00001",
  "language": "python",
  "code": "def my_function(): ...",
  "source_file": "/path/to/source.py",
  "validation_status": "validated",
  "input_type": "string",
  "output_type": "string"
}
```

### 10.5.2 Internal Components

| Component | Purpose |
|-----------|---------|
| **BlockRegistry** | Lazy-loading registry with filesystem-based lookup |
| **BlockSearchIndex** | SQLite FTS5 index for full-text search (<100ms) |
| **BlockLoader** | Loads block metadata, assigns the appropriate executor |
| **CLI Integration** | `blocks list`, `search`, `info`, `index` commands |

### 10.5.3 Block Loading Flow

When you write `use BLOCK-PY-09145 as validate_email`:

1. The parser creates a `UseStmt` AST node
2. The interpreter calls `BlockLoader::loadBlock("BLOCK-PY-09145")`
3. The loader queries the SQLite database for the block metadata and code
4. A `BlockValue` is created with the block's code and assigned executor (Python)
5. The block is stored in the interpreter's environment under the alias `validate_email`
6. When called, the Python executor runs the block's code and marshals the result back to NAAb

### 10.5.4 Performance

*   **First use**: Blocks are compiled/loaded on first execution (~10ms for Python, ~50-200ms for C++)
*   **Cached**: C++ blocks are cached as `.so` shared libraries for subsequent calls
*   **Lazy registry**: Startup is fast (~10ms) because blocks are loaded on-demand
*   **Search**: FTS5 index provides sub-100ms search across 24,000+ blocks

## 10.6 Block Categories

Blocks are organized into categories for easier discovery:

| Category | Examples |
|----------|---------|
| validation | Email, URL, phone format checking |
| data-processing | JSON, CSV, XML parsing and transformation |
| algorithms | Sorting, searching, graph algorithms |
| web | HTTP clients, web scraping |
| file | File read/write, path manipulation |
| cryptography | Hashing, encryption, password handling |
| math | Statistics, linear algebra |
| string | Regex, formatting, encoding |

## 10.7 Blocks vs Inline Polyglot

When should you use a block from the registry versus writing an inline polyglot block?

| | Registry Blocks | Inline Polyglot |
|---|---|---|
| **Best for** | Common, reusable tasks | One-off, project-specific logic |
| **Testing** | Pre-tested and validated | You write and test yourself |
| **Discovery** | Search by keyword/type | Write from scratch |
| **Maintenance** | Updated centrally | Maintained in your code |
| **Performance** | Cached after first use | Re-executed each time |

In practice, use registry blocks for standard tasks (validation, parsing, common algorithms) and inline polyglot blocks when you need custom logic or access to specific libraries not covered by existing blocks.
