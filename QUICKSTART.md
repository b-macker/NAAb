# Block Discovery System - Quick Start Guide

**Location**: `/storage/emulated/0/Download/.naab/naab_language`
**Full Plan**: See `BLOCK_DISCOVERY_PLAN.md` (70+ pages)

---

## 🎯 What We're Building

Transform NAAb from **basic file registry** → **AI-first block discovery platform**

### Current State
```
24,488 blocks stored as JSON files
Basic search: getBlock(), listBlocksByLanguage()
❌ No semantic search
❌ No type checking
❌ No composition validation
❌ No AI integration
```

### Target State (12 weeks)
```
✅ Semantic search: "validate email" → finds BLOCK-PY-09145
✅ Type-safe pipelines: auto-validate block chains
✅ AI API: REST endpoints for AI agents
✅ Smart recommendations: "after csv_parser, use validator"
✅ Analytics: learns from usage patterns
✅ 94% token reduction for AI agents
```

---

## 📊 12-Week Roadmap

| Week | Phase | Deliverable |
|------|-------|-------------|
| 1-2 | Enhanced Metadata | All 24,488 blocks enriched with types, performance, keywords |
| 3-4 | Semantic Search | Natural language search with embeddings |
| 5-6 | Type System | Composition validation, type checking |
| 7-8 | Pipeline Syntax | `\|>` operator with auto-validation |
| 9-10 | AI API | REST endpoints for AI agents |
| 11 | CLI Tools | `naab-search` command |
| 12 | Analytics | Usage tracking and learning |

---

## 🚀 Phase 1: Enhanced Metadata (Week 1-2)

### Goal
Add AI-friendly metadata to all blocks

### Current Block JSON
```json
{
  "id": "BLOCK-CPP-MATH",
  "name": "Math Utilities",
  "language": "cpp",
  "category": "math",
  "code": "...",
  "token_count": 250
}
```

### Enhanced Block JSON
```json
{
  "id": "BLOCK-CPP-MATH",
  "name": "Math Utilities",
  "language": "cpp",
  "category": "math",
  "code": "...",

  "NEW_FIELDS": {
    "description": "Provides basic mathematical operations: add, subtract, multiply, divide, power, sqrt",
    "short_desc": "Math utilities",
    "input_types": "int,int",
    "output_type": "int",
    "keywords": ["math", "arithmetic", "calculation"],
    "use_cases": ["calculator", "data processing", "statistics"],
    "performance_tier": "fast",
    "avg_execution_ms": 0.1,
    "success_rate_percent": 99,
    "test_coverage_percent": 100,
    "security_audited": true
  }
}
```

### Task List

**Day 1-2**: Extend BlockMetadata C++ struct
- File: `include/naab/block_loader.h`
- Add 15+ new fields
- Update parser to read new fields

**Day 3-4**: Create enrichment script
- File: `tools/enrich_block_metadata.py`
- Auto-generate descriptions from code
- Extract keywords from category/code
- Estimate performance tier

**Day 5-7**: Run on all 24,488 blocks
```bash
cd /storage/emulated/0/Download/.naab

# Enrich all blocks
python3 tools/enrich_block_metadata.py

# Verify
grep -r "description" naab/blocks/library/ | wc -l
# Should output: 24488
```

**Day 8-10**: Build SQLite search index
- File: `src/runtime/block_search_index.cpp`
- Create database schema (blocks, FTS index, analytics tables)
- Import all block metadata
- Test full-text search

```bash
# Build search index
./build/naab-build-index

# Test search
sqlite3 naab/blocks/block_search.db \
  "SELECT block_id, name FROM blocks_fts WHERE blocks_fts MATCH 'email' LIMIT 5"
```

---

## 🔍 Phase 2: Semantic Search (Week 3-4)

### Goal
Natural language search using embeddings

### Example
```python
# Before (keyword search)
search("email")  # Returns blocks with "email" in name/description

# After (semantic search)
search("I need to validate user input from a form")
# Returns: BLOCK-PY-09145 (Email Validator)
#          BLOCK-JS-08234 (Form Input Validator)
#          BLOCK-CPP-07089 (String Validator)
# Ranked by semantic similarity
```

### Task List

**Week 3**: Install embedding model
```bash
pip install sentence-transformers

# Download model (384-dim, fast)
python3 -c "
from sentence_transformers import SentenceTransformer
model = SentenceTransformer('sentence-transformers/all-MiniLM-L6-v2')
model.save('/storage/emulated/0/Download/.naab/models/embeddings')
"
```

**Week 3-4**: Build embeddings
```bash
# Generate embeddings for all 24,488 blocks
python3 tools/build_block_embeddings.py

# Creates: block_embeddings table in SQLite
# Stores: 384-dimensional vector per block
```

**Week 4**: Add semantic search to C++
- File: `src/runtime/semantic_search.cpp`
- Call Python embedding model via pybind11
- Compute cosine similarity
- Rank results by relevance

---

## 🧩 Phase 3: Type System (Week 5-6)

### Goal
Type-safe block composition

### Example
```naab
# Before (runtime error)
use BLOCK-CPP-07001 as parser  # outputs: Array<String>
use BLOCK-PY-09001 as validator # expects: DataFrame
# 💥 CRASH at runtime!

# After (compile-time error)
data |> parser.parse |> validator.validate
# ❌ Error at compile time:
#    Type mismatch: Array<String> → DataFrame
#    Suggestion: Use BLOCK-PY-09234 (array_to_dataframe)

# Fixed
data |> parser.parse |> adapter.to_dataframe |> validator.validate
# ✅ Type-safe pipeline
```

### Task List

**Week 5**: Define type system
- File: `include/naab/type_system.h`
- Types: Int, Float, String, Bool, Array<T>, Dict<K,V>
- Type parser: "array<string>" → Type object
- Compatibility checker

**Week 6**: Composition validator
- File: `src/runtime/composition_validator.cpp`
- Check if blocks can chain
- Suggest adapter blocks
- Helpful error messages

---

## ⚡ Phase 4: Pipeline Syntax (Week 7-8)

### Goal
Add `|>` operator to NAAb language

### Example
```naab
# Before (verbose)
let data = load_csv("data.csv")
let validated = validator.validate(data)
let json = formatter.to_json(validated)
return json

# After (pipeline)
return "data.csv" |> load_csv |> validator.validate |> formatter.to_json
# Auto-validated at compile time!
```

### Task List

**Week 7**: Add to lexer/parser
- Tokenize `|>`
- Parse pipeline expressions
- Create AST node

**Week 8**: Interpreter execution
- Execute pipeline stages
- Pass data through stages
- Validate types before execution

---

## 🤖 Phase 5: AI API (Week 9-10)

### Goal
REST API for AI agents

### Endpoints
```bash
# Search
curl "http://localhost:8080/api/search?q=validate+email"

# Auto-compose
curl -X POST http://localhost:8080/api/compose \
  -d '{"task": "Process CSV and export to JSON"}'

# Validate pipeline
curl -X POST http://localhost:8080/api/validate \
  -d '{"pipeline": ["BLOCK-CPP-07001", "BLOCK-PY-09001"]}'
```

### Python Client
```python
from naab_client import NaabClient

client = NaabClient()

# Search
results = client.search("validate email", performance="fast")
print(results["results"][0]["block_id"])

# Auto-compose
pipeline = client.compose("Process CSV to JSON")
print(pipeline["code"])
# Output: csv |> parse |> validate |> to_json
```

---

## 🛠️ Development Workflow

### Daily Development Cycle

**Morning** (2 hours):
1. Pick task from current phase
2. Read relevant files
3. Implement feature
4. Write unit tests

**Afternoon** (2 hours):
5. Test on real blocks
6. Fix bugs
7. Update documentation
8. Commit changes

**Evening** (1 hour):
9. Review progress
10. Plan next day
11. Update todo list

### Testing Strategy

**Unit Tests**:
```bash
# Test type system
./build/test_type_system

# Test composition validator
./build/test_composition_validator

# Test search
./build/test_block_search
```

**Integration Tests**:
```bash
# End-to-end search
./build/naab-search "validate email" --test

# End-to-end pipeline
echo 'data |> parse |> validate' | ./build/naab-lang --test

# API test
curl http://localhost:8080/api/health
```

---

## 📈 Success Metrics

### Week 2
- ✅ All 24,488 blocks enriched
- ✅ SQLite search index built
- ✅ FTS search working

### Week 4
- ✅ Semantic search operational
- ✅ Search accuracy >90%
- ✅ Search latency <100ms

### Week 6
- ✅ Type system complete
- ✅ Composition validation working
- ✅ Helpful error messages

### Week 8
- ✅ Pipeline syntax in NAAb
- ✅ Auto-validation working
- ✅ Zero runtime type errors

### Week 10
- ✅ REST API operational
- ✅ Python client working
- ✅ AI agents can discover blocks

### Week 12
- ✅ CLI tools ready
- ✅ Analytics tracking usage
- ✅ Full system operational

---

## 🚦 Getting Started

### Today (Day 1)

**Step 1**: Set up development environment
```bash
cd /storage/emulated/0/Download/.naab/naab_language

# Ensure dependencies installed
pkg install cmake g++ sqlite python

# Ensure build directory exists
mkdir -p build
cd build
cmake ..
make -j4
```

**Step 2**: Explore current block structure
```bash
# Check block count
find ../naab/blocks/library -name "*.json" | wc -l
# Should output: 24488

# Sample block
cat ../naab/blocks/library/cpp/BLOCK-CPP-MATH.json
```

**Step 3**: Create enrichment script
```bash
# Create tools directory
mkdir -p ../tools

# Start enrichment script
nano ../tools/enrich_block_metadata.py
```

### Week 1 Goals
- ✅ Extend BlockMetadata struct (Day 1-2)
- ✅ Create enrichment script (Day 3-4)
- ✅ Enrich 1,000 test blocks (Day 5-7)

### Week 2 Goals
- ✅ Enrich remaining 23,488 blocks
- ✅ Build SQLite search index
- ✅ Test FTS search

---

## 📞 Support & Resources

**Full Plan**: `BLOCK_DISCOVERY_PLAN.md` (70+ pages)
**Vision Document**: See `/storage/emulated/0/Download/test_hij/BLOCK_SYSTEM_VISION.md`

**Key Files**:
- `include/naab/block_loader.h` - BlockMetadata structure
- `include/naab/block_registry.h` - Current registry
- `src/runtime/block_registry.cpp` - Current implementation

**Database**:
- Location: `/storage/emulated/0/Download/.naab/naab/blocks/block_search.db`
- Schema: See Phase 1, Task 1.2

**Blocks Location**:
- `/storage/emulated/0/Download/.naab/naab/blocks/library/`
- 24,488 blocks across 15 languages

---

## 🎯 Focus Areas

**Week 1-2**: Metadata is foundation for everything
**Week 3-4**: Semantic search = game changer for AI
**Week 5-6**: Type safety = bulletproof composition
**Week 7-8**: Pipeline syntax = developer experience
**Week 9-10**: API = AI agent integration
**Week 11-12**: Polish and analytics

---

**Start with Phase 1, Task 1.1: Extend BlockMetadata structure!**

*Generated: December 28, 2024*
