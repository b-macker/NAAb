# Block Assembly Language - Vision vs Reality

**Date**: December 17, 2025
**Core Goal**: World's first block assembly language - write programs by assembling reusable blocks from ANY programming language

---

## The Vision

From `BLOCK_INVENTORY.md`:
> **Vision:** World's first **block assembly language** - write code by assembling reusable blocks from ANY programming language.

### What This Means

```naab
// Assemble a web scraper from pre-existing blocks
use BLOCK-PY-REQUESTS as http          // Python requests library
use BLOCK-JS-CHEERIO as parse          // JavaScript HTML parser
use BLOCK-CPP-ABSEIL-HASH as hash      // C++ hash map for caching
use BLOCK-RUST-REGEX as pattern        // Rust regex engine

main {
    // Use Python for HTTP
    let response = http.get("https://example.com")

    // Use JavaScript for parsing
    let dom = parse.load(response.text)
    let links = dom.find("a").map(e => e.href)

    // Use C++ for fast caching
    let cache = hash.create()
    cache.insert("links", links)

    // Use Rust for pattern matching
    let emails = pattern.findAll(response.text, r"\b[\w.-]+@[\w.-]+\.\w+\b")

    print("Found", links.length, "links and", emails.length, "emails")
}
```

**Key Features**:
- Mix languages in single program
- Reuse existing library code as blocks
- No reimplementation needed
- Best tool for each task

---

## Current State Assessment

### ✅ What We Have

| Component | Status | Description |
|-----------|--------|-------------|
| **Multi-language executors** | ✅ WORKING | C++, JavaScript, Python executors |
| **Block registry** | ✅ WORKING | 24,172 blocks indexed |
| **Block loading** | ✅ WORKING | Load blocks by ID from filesystem/JSON |
| **REPL commands** | ✅ WORKING | Interactive block discovery |
| **Syntax support** | ✅ WORKING | `use BLOCK-ID as name` syntax |
| **Type system** | ✅ WORKING | Basic type marshalling |

### ❌ What's Missing (The Gap)

| Component | Status | Blocker |
|-----------|--------|---------|
| **Executable blocks** | ❌ MISSING | Current blocks are raw code snippets, not callable functions |
| **Block interfaces** | ❌ MISSING | No standard function signatures/exports |
| **Dependency resolution** | ❌ MISSING | Blocks don't declare their dependencies |
| **Cross-language calls** | ⚠️ PARTIAL | Executors exist but not tested end-to-end |
| **Working examples** | ⚠️ PARTIAL | Only hand-crafted examples, not assembled from library blocks |
| **Block wrappers** | ❌ MISSING | No automatic wrapping of raw code into callable blocks |

---

## The Core Problem

### Current Block Format

From `BLOCK-CPP-00001.json`:
```json
{
  "id": "BLOCK-CPP-00001",
  "name": "create",
  "code": "{\n  auto &registry_inst = details::registry::instance();\n  ...\n}",
  "language": "c++",
  "source_file": "/storage/.../spdlog/include/spdlog/async.h",
  "source_line": 36
}
```

**Issues**:
1. **Not self-contained**: Code snippet from middle of a function
2. **Missing context**: Needs headers, namespace, surrounding code
3. **No interface**: Unclear what parameters it takes, what it returns
4. **Not callable**: Can't directly execute this code

### What We Need

```json
{
  "id": "BLOCK-CPP-LOGGER-CREATE",
  "name": "create_async_logger",
  "language": "c++",
  "interface": {
    "function": "create_async_logger",
    "parameters": [
      {"name": "logger_name", "type": "string"},
      {"name": "queue_size", "type": "int"}
    ],
    "returns": {"type": "logger_ptr"}
  },
  "code": "// Complete, compilable function\nextern \"C\" void* create_async_logger(const char* name, int queue_size) {\n  ...\n}",
  "dependencies": ["spdlog"],
  "exports": ["create_async_logger"]
}
```

---

## Gap Analysis

### What Separates Us from the Vision

1. **Block Preparation** (Not Done)
   - Extract complete functions from libraries
   - Create C-ABI wrappers for cross-language calls
   - Define clear interfaces
   - Package with dependencies

2. **Block Execution** (Partially Done)
   - ✅ Executors can run code
   - ❌ Can't call arbitrary library functions
   - ❌ No dependency injection
   - ❌ No header/import management

3. **Cross-Language Integration** (Not Tested)
   - ✅ Theory: Type marshalling exists
   - ❌ Practice: Never tested Python→C++ or C++→JavaScript calls
   - ❌ Data serialization between languages
   - ❌ Memory management across boundaries

4. **Practical Assembly** (Not Demonstrated)
   - ❌ No real program assembled from library blocks
   - ❌ No complex multi-language workflow
   - ✅ Only toy examples work

---

## Path to True Block Assembly

### Phase 1: Block Preparation (High Priority)

**Goal**: Convert library code into executable blocks

**Tasks**:
1. **Create Block Extractor v2**
   - Extract complete functions (not snippets)
   - Generate C-ABI wrappers
   - Create interface definitions
   - Package dependencies

2. **Define Block Standard**
   ```
   Standard Block Format:
   - Self-contained (includes all needed code)
   - C-ABI compatible (extern "C")
   - Clear interface (function signature)
   - Explicit dependencies
   - Test harness included
   ```

3. **Prepare Reference Blocks**
   - Select 50-100 useful functions from libraries
   - Package as standard blocks
   - Test each block independently
   - Document interfaces

**Example Block (Proper Format)**:
```cpp
// BLOCK-CPP-STRING-UPPER.cpp
#include <string>
#include <algorithm>

extern "C" {
    // Convert string to uppercase
    const char* string_to_upper(const char* input) {
        static std::string result;
        result = input;
        std::transform(result.begin(), result.end(), result.begin(), ::toupper);
        return result.c_str();
    }
}
```

### Phase 2: Cross-Language Integration Testing

**Goal**: Prove C++ ↔ JavaScript ↔ Python actually works

**Test Cases**:
1. **Python → C++**
   ```naab
   use BLOCK-PY-HTTP as fetch
   use BLOCK-CPP-HASH as hash

   main {
       let data = fetch.get("https://api.github.com")
       let cache = hash.insert("api_data", data)
   }
   ```

2. **C++ → JavaScript**
   ```naab
   use BLOCK-CPP-VECTOR as vec
   use BLOCK-JS-FORMAT as fmt

   main {
       let numbers = vec.create([1, 2, 3, 4, 5])
       let sum = vec.sum(numbers)
       let message = fmt.template("Sum is {}", sum)
   }
   ```

3. **JavaScript → Python**
   ```naab
   use BLOCK-JS-PARSE as json
   use BLOCK-PY-REQUESTS as http

   main {
       let config = json.parse('{"url": "https://example.com"}')
       let response = http.get(config.url)
   }
   ```

### Phase 3: Real-World Assembly Example

**Goal**: Build a complete, useful program from blocks

**Project**: **Web Scraper** (uses all 3 languages)

```naab
// Web scraper assembled from blocks
use BLOCK-PY-REQUESTS as http          // Python: HTTP requests
use BLOCK-JS-CHEERIO as html           // JavaScript: HTML parsing
use BLOCK-CPP-REGEX as regex           // C++: Fast regex
use BLOCK-CPP-ABSEIL-HASH as cache     // C++: Hash map

main {
    // Fetch with Python
    print("Fetching page...")
    let response = http.get("https://news.ycombinator.com")

    // Parse with JavaScript
    print("Parsing HTML...")
    let dom = html.load(response.text)
    let titles = dom.find(".titleline a").map(e => e.text)

    // Extract with C++ regex
    print("Extracting links...")
    let links = regex.findAll(response.text, "https://[^\"]+")

    // Cache with C++ hash map
    print("Caching results...")
    let results = cache.create()
    cache.insert(results, "titles", titles)
    cache.insert(results, "links", links)

    // Report
    print("Found", titles.length, "titles")
    print("Found", links.length, "links")
    print("Cached in", cache.size(results), "entries")
}
```

**Expected Output**:
```
Fetching page...
[PY] HTTP GET https://news.ycombinator.com
Parsing HTML...
[JS] Loaded 1024 DOM nodes
Extracting links...
[CPP] Regex matched 87 patterns
Caching results...
[CPP] Hash map size: 2 entries
Found 30 titles
Found 87 links
Cached in 2 entries

✓ Program assembled from 4 blocks across 3 languages
```

---

## Implementation Roadmap

### Milestone 1: Executable Block Standard (Week 1)

- [ ] Define block interface standard
- [ ] Create block wrapper generator
- [ ] Convert 10 reference blocks (C++, Python, JavaScript)
- [ ] Test each block independently

### Milestone 2: Cross-Language Calls (Week 2)

- [ ] Test Python → C++ function call
- [ ] Test C++ → JavaScript function call
- [ ] Test JavaScript → Python function call
- [ ] Implement data serialization
- [ ] Handle memory management

### Milestone 3: Real Assembly Example (Week 3)

- [ ] Select real-world use case
- [ ] Identify needed blocks
- [ ] Prepare/wrap required blocks
- [ ] Assemble complete program
- [ ] Demonstrate end-to-end

### Milestone 4: Block Library Expansion (Week 4)

- [ ] Convert 100 useful blocks
- [ ] Document block catalog
- [ ] Create block search tool
- [ ] Build dependency resolver
- [ ] Performance optimization

---

## Current vs Target Architecture

### Current Architecture
```
User writes NAAb code
  ↓
Parser creates AST
  ↓
Interpreter loads blocks (JSON metadata)
  ↓
Executor runs code snippets (limited)
  ↓
Basic output
```

**Problem**: Blocks are not truly reusable or composable

### Target Architecture
```
User writes NAAb code
  ↓
Parser creates AST
  ↓
Block resolver finds and loads blocks
  ↓
Dependency resolver ensures all deps available
  ↓
Multi-language executor coordinates calls
  ↓
Type marshaller converts data between languages
  ↓
Functions execute in native runtime
  ↓
Results marshalled back to NAAb
  ↓
Rich output with cross-language composition
```

**Benefit**: True block assembly across languages

---

## What We Can Do Now vs What We Need

### Can Do Now ✅
- Load blocks by ID
- List available blocks
- Parse NAAb syntax
- Execute simple C++/JavaScript/Python code
- Basic type conversion

### Can't Do Yet ❌
- Call arbitrary library functions from blocks
- Assemble complex programs from existing blocks
- Resolve block dependencies automatically
- Handle cross-language data structures
- Package and distribute block libraries
- Generate block wrappers automatically

---

## Proof of Concept Needed

### Minimum Viable Block Assembly

**Goal**: Demonstrate ONE real example of assembling from library blocks

**Simplest Example**: String processing pipeline

```naab
// Process text using 3 languages
use BLOCK-CPP-STRING-UPPER as upper    // C++: Fast string ops
use BLOCK-JS-STRING-SPLIT as split     // JS: String utilities
use BLOCK-PY-STRING-CLEAN as clean     // Python: Unicode handling

main {
    let text = "  Hello, 世界! Welcome to NAAb.  "

    // Python: Clean unicode
    let cleaned = clean.normalize(text)

    // JavaScript: Split into words
    let words = split.byWhitespace(cleaned)

    // C++: Uppercase each word
    let result = words.map(w => upper.toUpper(w))

    print(result.join(" "))
}
```

**Output**:
```
HELLO, 世界! WELCOME TO NAAB.
```

**What This Proves**:
1. ✅ Blocks from 3 different languages
2. ✅ Data flows between languages
3. ✅ Real library code reuse
4. ✅ Practical composition

---

## Next Steps

### Immediate Actions

1. **Create 3 Executable Blocks** (one per language)
   - BLOCK-CPP-STRING-UPPER (C++ string uppercase)
   - BLOCK-JS-STRING-SPLIT (JavaScript string split)
   - BLOCK-PY-STRING-CLEAN (Python unicode normalization)

2. **Test Cross-Language Flow**
   - Verify data can flow: Python → JavaScript → C++
   - Test type conversions
   - Handle errors gracefully

3. **Demonstrate Assembly**
   - Write example program using all 3 blocks
   - Execute end-to-end
   - Document the process

4. **Document the Standard**
   - Block interface specification
   - Wrapper generation guide
   - Best practices for block creation

### Long-term Vision

- **Block Marketplace**: Central repository of blocks
- **Auto-discovery**: Find blocks by capability
- **Dependency Management**: Automatic resolution
- **Performance**: JIT compilation, caching
- **IDE Integration**: Auto-complete for blocks
- **Testing**: Automated block testing
- **Versioning**: Semantic versioning for blocks

---

## Conclusion

### Where We Are
- ✅ Infrastructure built (executors, registry, parser)
- ✅ 24,172 blocks indexed
- ⚠️ Blocks not yet executable/composable

### Where We Need to Be
- 🎯 Blocks are executable functions with clear interfaces
- 🎯 Cross-language calls work reliably
- 🎯 Real programs assembled from library blocks
- 🎯 Developer can write programs without reimplementing

### The Gap
**We have the foundation, but need to prepare blocks for true assembly**

**Biggest Blocker**: Current blocks are code snippets, not executable functions

**Solution**: Create block preparation pipeline to wrap library code

**Metric of Success**: Assemble a real, useful program from pre-existing blocks across multiple languages

---

**Status**: 🔨 **FOUNDATION COMPLETE, ASSEMBLY LAYER NEEDED**

**Next Phase**: Build the block preparation and cross-language integration layer to achieve the true vision of block assembly programming.
