# NAAb v0.1.0 → v1.0 Unified Roadmap
## Dual-Track Enhancement: Language + Block Integration

**Location**: `/storage/emulated/0/Download/.naab/naab_language`
**Timeline**: 6 months
**Team**: 2-3 developers
**Budget**: $67k (or 6 months development time)

---

## 🎯 The Dual Mission

NAAb is a **full programming language** designed for **multi-language block composition**.

We must enhance BOTH:
- **Track A**: NAAb Language (control flow, data structures, error handling)
- **Track B**: Block Integration (discovery, type safety, composition)

---

## 📅 6-Month Roadmap

```
Month 1          Month 2          Month 3          Month 4          Month 5          Month 6
┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐
│ PHASE 1  │    │ PHASE 1  │    │ PHASE 2  │    │ PHASE 3  │    │ PHASE 3  │    │ PHASE 4  │
│          │    │  (cont)  │    │          │    │          │    │  (cont)  │    │          │
│Language: │    │Language: │    │Language: │    │Language: │    │Language: │    │Language: │
│Operators │    │ Errors   │    │  Data    │    │ Modules  │    │ Modules  │    │Exception │
│  Loops   │    │          │    │ Methods  │    │          │    │          │    │ Handling │
│          │    │          │    │          │    │          │    │          │    │          │
│Blocks:   │    │Blocks:   │    │Blocks:   │    │Blocks:   │    │Blocks:   │    │Blocks:   │
│Metadata  │    │ Search   │    │  Types   │    │Semantic  │    │Pipeline  │    │AI API    │
│          │    │  Index   │    │Validator │    │ Search   │    │  Syntax  │    │Analytics │
└──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘    └──────────┘
   MVP Track      MVP Track      Production       Production       Production       Polish
   Foundation     Foundation        Track            Track            Track          Track
```

---

## Phase 1: Foundation (Month 1-2) - CRITICAL

### Track A: Core Language Features

**Week 1-2: Logical Operators**
```naab
# Before
if (age >= 18) {
    if (score > 80) {
        print("Eligible")
    }
}

# After
if (age >= 18 && score > 80) {
    print("Eligible")
}
```

**Implementation**:
- Add `&&`, `||`, `!` tokens to lexer
- Update parser for operator precedence
- Implement short-circuit evaluation in interpreter
- **Files**: lexer.cpp, parser.cpp, interpreter.cpp
- **Effort**: 3 days

**Week 3-4: While Loops + Break/Continue**
```naab
# While loop
let count = 0
while (count < 10) {
    count = count + 1
}

# With break
while (true) {
    if (found) break
}

# With continue
while (i < 100) {
    i++
    if (i % 2 == 0) continue
    print(i)  # Only odds
}
```

**Implementation**:
- Add `while`, `break`, `continue` tokens
- Add `WhileStmt`, `BreakStmt`, `ContinueStmt` AST nodes
- Exception-based control flow for break/continue
- **Files**: ast.h, parser.cpp, interpreter.cpp
- **Effort**: 2 days

**Week 5-8: Better Error Messages**
```bash
# Before
Error: Parse error

# After
Error at line 42, column 15 in main.naab:
  40 |     let user = users[index]
  41 |     if (user["active"]) {
  42 |         let name = user["namee"]  # Typo here
     |                         ^^^^^
  43 |         print(name)

RuntimeError: Dictionary key not found: 'namee'
  Did you mean 'name'?

Call stack:
  at process_user() (main.naab:42)
  at main (main.naab:10)
```

**Implementation**:
- ErrorReporter class with source location tracking
- Stack trace collection
- Did-you-mean suggestions (Levenshtein distance)
- Color-coded output
- **Files**: error_reporter.h/cpp, interpreter.cpp
- **Effort**: 1 week

---

### Track B: Block Discovery Foundation

**Week 1-2: Enhanced Block Metadata**

**Current metadata**:
```json
{
  "id": "BLOCK-CPP-MATH",
  "name": "Math Utilities",
  "language": "cpp",
  "code": "..."
}
```

**Enhanced metadata**:
```json
{
  "id": "BLOCK-CPP-MATH",
  "name": "Math Utilities",
  "language": "cpp",
  "description": "Fast mathematical operations: add, subtract, multiply, divide, power, sqrt",
  "short_desc": "Math ops",
  "keywords": ["math", "arithmetic", "calculation"],
  "use_cases": ["calculator", "data processing"],
  "input_types": "int,int",
  "output_type": "int",
  "performance_tier": "fast",
  "avg_execution_ms": 0.1,
  "success_rate_percent": 99,
  "test_coverage_percent": 100,
  "security_audited": true,
  "code": "..."
}
```

**Implementation**:
- Extend BlockMetadata struct (15+ new fields)
- Python script to enrich all 24,488 blocks
- Update BlockRegistry to read new fields
- **Files**: block_loader.h, enrich_blocks.py
- **Effort**: 1 week

**Week 3-4: SQLite Search Index**

**Schema**:
```sql
CREATE TABLE blocks (
    block_id TEXT PRIMARY KEY,
    name TEXT,
    language TEXT,
    description TEXT,
    keywords TEXT,  -- JSON array
    input_types TEXT,
    output_type TEXT,
    performance_tier TEXT,
    success_rate_percent INTEGER,
    ...
);

CREATE VIRTUAL TABLE blocks_fts USING fts5(
    block_id, name, description, keywords
);
```

**Features**:
- Full-text search
- Filter by language, performance, security
- Rank by relevance, popularity, quality

**Implementation**:
- BlockSearchIndex class
- Import 24,488 blocks to SQLite
- FTS search with ranking
- **Files**: block_search_index.h/cpp
- **Effort**: 1 week

---

### Phase 1 Deliverable

**Language**:
- ✅ Logical operators (&&, ||, !)
- ✅ While loops, break, continue
- ✅ Helpful error messages with stack traces

**Blocks**:
- ✅ 24,488 blocks enriched with metadata
- ✅ SQLite search index operational
- ✅ Fast keyword search

**Demo**:
```naab
# Language improvements
function find_valid_users(users: array) {
    let valid = []
    let i = 0

    while (i < 100 && valid.length < 10) {
        let user = users[i]

        # Logical operators
        if (user["active"] && user["score"] > 80) {
            valid = valid + [user]
        }

        i++
    }

    return valid
}

# Block search
use search_blocks("email validation", performance="fast")
# Returns: BLOCK-PY-09145

# Better errors
let name = user["namee"]  # Typo
# Error at line 42: Dictionary key not found: 'namee'
#   Did you mean 'name'?
```

---

## Phase 2: Data Manipulation + Type Safety (Month 3)

### Track A: String & Array Methods

**Week 1-2: String Methods (12 functions)**
```naab
use string as str

let text = "  Hello World  "
str.length(text)           # 16
str.trim(text)             # "Hello World"
str.upper(text)            # "  HELLO WORLD  "
str.split(text, " ")       # ["", "", "Hello", "World", "", ""]
str.contains(text, "World") # true
str.replace(text, "World", "NAAb")
str.substring(text, 2, 7)   # "Hello"
str.starts_with(text, "  H") # true
str.ends_with(text, "  ")   # true
str.index_of(text, "World") # 8
str.join(["A", "B"], ",")   # "A,B"
```

**Implementation**:
- Extend stdlib/string_impl.cpp
- 12 functions using std::string
- **Effort**: 5 days

**Week 3-4: Array Methods (12 functions)**
```naab
use array as arr

let nums = [1, 2, 3, 4, 5]
arr.length(nums)           # 5
arr.push(nums, 6)          # [1,2,3,4,5,6]
arr.pop(nums)              # Returns 5, array [1,2,3,4]
arr.first(nums)            # 1
arr.last(nums)             # 5
arr.slice(nums, 1, 3)      # [2,3]
arr.reverse(nums)          # [5,4,3,2,1]
arr.sort(nums)             # [1,2,3,4,5]
arr.contains(nums, 3)      # true
arr.join(nums, ",")        # "1,2,3,4,5"
arr.map(nums, fn(x) { return x * 2 })    # [2,4,6,8,10]
arr.filter(nums, fn(x) { return x > 2 }) # [3,4,5]
```

**Implementation**:
- Extend stdlib/array_impl.cpp
- Higher-order functions (map, filter, reduce)
- **Effort**: 5 days

---

### Track B: Type System & Validation

**Week 1-2: Type System**

**Type Definitions**:
```cpp
// type_system.h
enum class BaseType {
    Any, Int, Float, String, Bool,
    Array, Dict, Void
};

struct Type {
    BaseType base;
    std::vector<Type> generic_params;

    static Type Int();
    static Type String();
    static Type Array(Type element);
    static Type Dict(Type key, Type value);

    bool isCompatibleWith(const Type& other) const;
};
```

**Block Type Annotations**:
```json
{
  "id": "BLOCK-CPP-07001",
  "input_types": ["string"],
  "output_type": "array<dict>"
}
```

**Implementation**:
- Type system with generics
- Type parser ("array<string>" → Type object)
- Compatibility checker
- **Files**: type_system.h/cpp
- **Effort**: 1 week

**Week 3-4: Composition Validator**

**Validate pipelines**:
```naab
# This is valid (types match)
use BLOCK-CPP-07001 as parser      # String → Array<Dict>
use BLOCK-PY-09001 as validator    # Array<Dict> → Array<Dict>

data |> parser.parse |> validator.validate  # ✅

# This fails (type mismatch)
use BLOCK-RUST-05001 as hasher     # String → String

data |> parser.parse |> hasher.sha256       # ❌
# Error: hasher.sha256 expects String, got Array<Dict>
# Suggestion: Use arr.join() or find array hasher
```

**Implementation**:
- CompositionValidator class
- Check type compatibility
- Suggest adapter blocks
- **Files**: composition_validator.h/cpp
- **Effort**: 1 week

---

### Phase 2 Deliverable

**Language**:
- ✅ 12 string methods
- ✅ 12 array methods (including map/filter)
- ✅ Rich data manipulation

**Blocks**:
- ✅ Type system with generics
- ✅ Block type annotations
- ✅ Composition validation

**Demo**:
```naab
# Rich data manipulation
let emails = str.split(input, ",")
    .map(fn(e) { return str.trim(e) })
    .filter(fn(e) { return str.contains(e, "@") })

# Type-safe block composition
use BLOCK-CPP-07001 as csv_parser  # String → Array<Dict>
use BLOCK-PY-09001 as validator    # Array<Dict> → Array<Dict>
use BLOCK-JS-08001 as formatter    # Array<Dict> → String

# Validated at parse time
csv_file |> csv_parser.parse |> validator.validate |> formatter.to_json
# ✅ All types compatible
```

---

## Phase 3: Multi-File + Semantic Search (Month 4-5)

### Track A: Module System

**Week 1-2: Import/Export**

**Export**:
```naab
# validators.naab
export function validate_email(email: string) {
    use BLOCK-PY-09145 as validator
    return validator.validate(email)
}

export function validate_age(age: int) {
    return age >= 18 && age <= 120
}

export let MAX_RETRIES = 3
```

**Import**:
```naab
# main.naab
import {validate_email, validate_age} from "./validators.naab"
import * as validators from "./validators.naab"

if (validate_email(user["email"])) {
    # ...
}

if (validators.validate_age(user["age"])) {
    # ...
}
```

**Implementation**:
- Add `import`, `export` keywords
- ImportStmt, ExportStmt AST nodes
- ModuleResolver for path resolution
- Module caching
- **Files**: ast.h, parser.cpp, module_resolver.h/cpp
- **Effort**: 2 weeks

**Week 3-4: Module Search Paths**

**Search order**:
```
1. Relative: ./validators.naab
2. naab_modules/ in current directory
3. naab_modules/ in parent directories
4. Global: ~/.naab/modules/
5. System: /usr/local/naab/modules/
```

**Implementation**:
- Path resolution algorithm
- naab_modules/ support
- Circular dependency detection
- **Effort**: 1 week

---

### Track B: Semantic Search

**Week 1-2: Embedding Model**

**Build embeddings for all blocks**:
```python
# build_block_embeddings.py
from sentence_transformers import SentenceTransformer

model = SentenceTransformer('all-MiniLM-L6-v2')

for block in blocks:
    text = f"{block.name}. {block.description}. {block.keywords}"
    embedding = model.encode(text)  # 384-dim vector
    store_embedding(block.id, embedding)
```

**Implementation**:
- Install sentence-transformers
- Generate embeddings for 24,488 blocks
- Store in SQLite (block_embeddings table)
- **Effort**: 1 week

**Week 3-4: Semantic Search API**

**Usage**:
```cpp
SemanticSearch search(db_path);

// Natural language query
auto results = search.search("I need to validate user email addresses");

// Returns (ranked by semantic similarity):
// 1. BLOCK-PY-09145 "Email Validator" (score: 0.92)
// 2. BLOCK-JS-08234 "Form Validator" (score: 0.85)
// 3. BLOCK-CPP-07089 "String Validator" (score: 0.78)
```

**Implementation**:
- SemanticSearch class
- Call Python embedding model via pybind11
- Cosine similarity ranking
- **Files**: semantic_search.h/cpp
- **Effort**: 1 week

**Week 5: Pipeline Syntax**

**Add `|>` operator**:
```naab
# Pipeline syntax
data |> parser.parse |> validator.validate |> formatter.to_json

# Equivalent to:
formatter.to_json(validator.validate(parser.parse(data)))
```

**Implementation**:
- Add `|>` token
- PipelineExpr AST node
- Execute stages sequentially
- Auto-validate types
- **Files**: lexer.cpp, parser.cpp, interpreter.cpp
- **Effort**: 3 days

---

### Phase 3 Deliverable

**Language**:
- ✅ Import/export system
- ✅ Multi-file applications
- ✅ Module search paths

**Blocks**:
- ✅ Semantic search with embeddings
- ✅ Natural language block discovery
- ✅ Pipeline syntax with validation

**Demo**:
```naab
# Multi-file application
# File: lib/validators.naab
export function validate_user(user: dict) {
    # Semantic search finds best block
    use find_block("email validation", performance="fast")
    return email_validator.validate(user["email"])
}

# File: main.naab
import {validate_user} from "./lib/validators.naab"

function process_users(csv_file: string) {
    # Type-safe pipeline
    return csv_file
        |> cpp_parser.parse
        |> filter(fn(u) { return validate_user(u) })
        |> js_formatter.to_json
}
```

---

## Phase 4: Production Polish (Month 6)

### Track A: Exception Handling

**Week 1-2: Try/Catch/Finally**

```naab
function load_data(file: string) {
    let data = null

    try {
        data = io.read_file(file)
        data = json.parse(data)

        use BLOCK-PY-09234 as validator
        data = validator.validate(data)

    } catch (error) {
        print("Error: " + error.message)
        print("Stack: " + error.stack)
        data = get_default_data()

    } finally {
        print("Load complete")
    }

    return data
}

# Throw errors
function validate(data: any) {
    if (data == null) {
        throw "Data cannot be null"
    }
}
```

**Implementation**:
- TryStmt, CatchStmt, FinallyStmt, ThrowStmt
- Error objects with stack traces
- Exception propagation
- **Files**: ast.h, parser.cpp, interpreter.cpp
- **Effort**: 2 weeks

---

### Track B: AI Integration

**Week 1-2: REST API**

**Endpoints**:
```bash
# Search
GET /api/search?q=validate+email&performance=fast

# Auto-compose
POST /api/compose
{
  "task": "Process CSV and export JSON",
  "requirements": {"performance": "fast", "security": true}
}

# Validate
POST /api/validate
{
  "pipeline": ["BLOCK-CPP-07001", "BLOCK-PY-09001"]
}
```

**Implementation**:
- BlockAPIServer using cpp-httplib
- REST endpoints
- JSON request/response
- **Files**: block_api_server.cpp
- **Effort**: 1 week

**Week 3: CLI Tools**

```bash
# Search blocks
$ naab-search "validate email"
Found 5 blocks:
[1] BLOCK-PY-09145 - Email Validator (score: 0.92)
    Performance: fast (0.5ms)
    Success rate: 98%

# Validate pipeline
$ naab-validate "BLOCK-CPP-07001,BLOCK-PY-09001"
✅ Pipeline valid
  Stage 1: String → Array<Dict>
  Stage 2: Array<Dict> → Array<Dict>
```

**Implementation**:
- naab-search command
- naab-validate command
- **Files**: cli/naab_search.cpp
- **Effort**: 3 days

**Week 4: Analytics**

**Track usage**:
```cpp
// Auto-track in interpreter
analytics.recordUsage(block_id, success, duration_ms, tokens_saved);
analytics.recordBlockPair(first_block, second_block);
```

**Dashboard**:
```bash
$ naab-stats
Top 10 Blocks:
  1. BLOCK-PY-09145 (4,523 uses, 98% success)
  2. BLOCK-JS-08001 (3,891 uses, 99% success)

Top Combinations:
  1. BLOCK-CPP-07001 → BLOCK-PY-09001 (347 times)

Tokens saved: ~1.2M tokens (~$35 cost savings)
```

**Implementation**:
- Analytics tracking
- Usage database
- Stats dashboard
- **Effort**: 4 days

---

### Phase 4 Deliverable

**Language**:
- ✅ Try/catch/finally
- ✅ Throw statements
- ✅ Robust error handling

**Blocks**:
- ✅ REST API for AI agents
- ✅ CLI tools (naab-search)
- ✅ Usage analytics

**Demo**:
```naab
# Complete production application
import {Database} from "./lib/database.naab"
import {Logger} from "./lib/logger.naab"

function etl_pipeline(csv_file: string) {
    let logger = Logger("etl")

    try {
        # Multi-language pipeline
        let data = csv_file
            |> cpp_parser.parse        # C++ for speed
            |> py_validator.validate   # Python for ML
            |> js_formatter.to_json    # JS for web

        # Store in database
        let db = Database.connect("postgresql://localhost/data")
        db.insert("processed", data)

        logger.info("Processed " + data.length + " records")

    } catch (error) {
        logger.error("Pipeline failed: " + error.message)
        throw error
    }
}

# AI agent can auto-generate this via API:
POST /api/compose {"task": "ETL pipeline CSV to PostgreSQL"}
```

---

## 📊 Complete Feature Matrix

### After 6 Months

| Feature | Status | Impact |
|---------|--------|--------|
| **Language Features** |||
| Logical operators (&&, \|\|, !) | ✅ | HIGH |
| While loops | ✅ | HIGH |
| Break/continue | ✅ | MODERATE |
| Better error messages | ✅ | HIGH |
| String methods (12) | ✅ | VERY HIGH |
| Array methods (12) | ✅ | VERY HIGH |
| Import/export | ✅ | CRITICAL |
| Module system | ✅ | CRITICAL |
| Exception handling | ✅ | HIGH |
| Subscript assignment | ⏸️ | MODERATE |
| Ternary operator | ⏸️ | LOW |
| **Block Integration** |||
| Enhanced metadata | ✅ | CRITICAL |
| SQLite search index | ✅ | CRITICAL |
| Keyword search | ✅ | HIGH |
| Semantic search | ✅ | VERY HIGH |
| Type system | ✅ | CRITICAL |
| Composition validation | ✅ | CRITICAL |
| Pipeline syntax | ✅ | HIGH |
| REST API | ✅ | HIGH |
| CLI tools | ✅ | MODERATE |
| Usage analytics | ✅ | MODERATE |

---

## 🎯 Success Criteria

### Month 3 (MVP)
- ✅ Write complete applications in NAAb
- ✅ Logical operators, loops, string/array methods
- ✅ Search 24,488 blocks in <30 seconds
- ✅ Type system prevents 80% of errors

### Month 5 (Production)
- ✅ Multi-file applications
- ✅ Semantic search 90% accuracy
- ✅ Pipeline syntax with auto-validation
- ✅ 10+ real applications built

### Month 6 (Complete)
- ✅ Exception handling for production apps
- ✅ REST API for AI agents
- ✅ 80% token reduction for AI
- ✅ CLI tools for developers

---

## 🚀 Start Implementation

**Today**: Read COMPLETE_VISION.md
**Week 1**: Implement logical operators
**Week 2**: Implement while loops
**Week 3**: Enrich block metadata
**Week 4**: Build SQLite search index

**Location**: `/storage/emulated/0/Download/.naab/naab_language`

**All plans ready**:
- `COMPLETE_VISION.md` - Full vision
- `BLOCK_DISCOVERY_PLAN.md` - Block system details
- `UNIFIED_ROADMAP.md` - This roadmap
- `QUICKSTART.md` - Getting started

---

*Ready to build NAAb v1.0 - a complete multi-language block assembly programming language!*
