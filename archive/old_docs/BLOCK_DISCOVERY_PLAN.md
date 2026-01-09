# NAAb Block Discovery & Search System - End-to-End Plan
## AI-First Block Assembly Platform

**Generated**: December 28, 2024
**Location**: `/storage/emulated/0/Download/.naab/naab_language`
**Current State**: Basic filesystem registry with 24,488 blocks
**Target State**: Intelligent AI-first block discovery and composition system

---

## 📊 Current State Analysis

### What Exists ✅
```cpp
// Location: include/naab/block_registry.h, src/runtime/block_registry.cpp

class BlockRegistry {
    // Basic functionality
    std::optional<BlockMetadata> getBlock(const std::string& block_id);
    std::vector<std::string> listBlocks();
    std::vector<std::string> listBlocksByLanguage(const std::string& language);

    // Current: 24,488 blocks in filesystem
    // Storage: /storage/emulated/0/Download/.naab/naab/blocks/library/
    // Format: JSON metadata + source code
};

struct BlockMetadata {
    std::string block_id;        // "BLOCK-CPP-MATH"
    std::string name;            // "Math Utilities"
    std::string language;        // "cpp"
    std::string category;        // "math"
    std::string subcategory;     // "basic"
    std::string version;         // "1.0.0"
    int token_count;             // 250
    int times_used;              // 0
    bool is_active;
};
```

### What's Missing ❌
```
❌ Semantic search ("validate email" → find relevant blocks)
❌ AI-optimized ranking (most useful blocks first)
❌ Type information (block inputs/outputs)
❌ Composition validation (can blocks chain together?)
❌ Performance metrics (speed, memory usage)
❌ Usage analytics (which blocks work well together?)
❌ Dependency tracking (block compatibility)
❌ Smart recommendations (suggest next block)
❌ Search API for AI agents
❌ Pipeline syntax validation
```

---

## 🎯 End-to-End Implementation Plan

### Phase 1: Enhanced Block Metadata (Week 1-2)

**Goal**: Enrich block metadata with AI-friendly information

#### Task 1.1: Extend BlockMetadata Structure (2 days)

**File**: `include/naab/block_loader.h`

**Add to BlockMetadata**:
```cpp
struct BlockMetadata {
    // Existing fields...

    // NEW: Type information for composition
    std::string input_types;     // "string,int" or "array<dict>"
    std::string output_type;     // "bool" or "array<string>"

    // NEW: Natural language description (AI-friendly)
    std::string description;     // "Validates email addresses using RFC 5322"
    std::string short_desc;      // "Email validator" (token-efficient)

    // NEW: Performance characteristics
    double avg_execution_ms;     // Average execution time
    int max_memory_mb;           // Memory usage
    std::string performance_tier; // "fast" | "medium" | "slow"

    // NEW: AI-optimized metadata
    std::vector<std::string> keywords;      // ["email", "validate", "RFC"]
    std::vector<std::string> use_cases;     // ["form validation", "user input"]
    std::vector<std::string> related_blocks; // ["BLOCK-PY-09234"]

    // NEW: Usage analytics
    int success_rate_percent;    // 98% - how often block succeeds
    int avg_tokens_saved;        // Estimated tokens saved vs writing from scratch
    std::string commonly_follows; // "BLOCK-JS-08001" - common next block

    // NEW: Quality metrics
    int test_coverage_percent;   // 100%
    bool security_audited;       // true
    std::string stability;       // "stable" | "beta" | "experimental"
};
```

**Migration Script** (Python):
```python
# tools/enrich_block_metadata.py
import json
import os
from pathlib import Path

def enrich_block(block_json_path):
    """Add missing metadata fields to existing blocks"""
    with open(block_json_path, 'r') as f:
        block = json.load(f)

    # Add default values for new fields
    block.setdefault('description', f"Block for {block.get('category', 'general')} operations")
    block.setdefault('short_desc', block.get('name', ''))
    block.setdefault('input_types', 'any')
    block.setdefault('output_type', 'any')
    block.setdefault('keywords', [block.get('category', ''), block.get('subcategory', '')])
    block.setdefault('use_cases', [])
    block.setdefault('related_blocks', [])
    block.setdefault('performance_tier', 'medium')
    block.setdefault('avg_execution_ms', 1.0)
    block.setdefault('max_memory_mb', 1)
    block.setdefault('success_rate_percent', 95)
    block.setdefault('avg_tokens_saved', 100)
    block.setdefault('test_coverage_percent', 80)
    block.setdefault('security_audited', False)
    block.setdefault('stability', 'stable')

    with open(block_json_path, 'w') as f:
        json.dump(block, f, indent=2)

# Enrich all blocks
blocks_dir = Path('/storage/emulated/0/Download/.naab/naab/blocks/library')
for json_file in blocks_dir.rglob('*.json'):
    enrich_block(json_file)
    print(f"Enriched: {json_file}")
```

**Deliverable**: All 24,488 blocks have enhanced metadata

---

#### Task 1.2: Build SQLite Search Index (3 days)

**File**: `src/runtime/block_search_index.cpp` (NEW)

**Purpose**: Fast full-text search over block metadata

**Database Schema**:
```sql
-- /storage/emulated/0/Download/.naab/naab/blocks/block_search.db

CREATE TABLE blocks (
    block_id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    language TEXT NOT NULL,
    category TEXT,
    subcategory TEXT,
    description TEXT,
    short_desc TEXT,
    version TEXT,
    input_types TEXT,
    output_type TEXT,
    keywords TEXT,  -- JSON array
    use_cases TEXT, -- JSON array
    performance_tier TEXT,
    avg_execution_ms REAL,
    success_rate_percent INTEGER,
    avg_tokens_saved INTEGER,
    test_coverage_percent INTEGER,
    security_audited BOOLEAN,
    stability TEXT,
    times_used INTEGER DEFAULT 0,
    is_active BOOLEAN DEFAULT 1
);

-- Full-text search index
CREATE VIRTUAL TABLE blocks_fts USING fts5(
    block_id,
    name,
    description,
    keywords,
    use_cases,
    content=blocks
);

-- Indexes for common queries
CREATE INDEX idx_language ON blocks(language);
CREATE INDEX idx_category ON blocks(category);
CREATE INDEX idx_performance ON blocks(performance_tier);
CREATE INDEX idx_success_rate ON blocks(success_rate_percent DESC);
CREATE INDEX idx_usage ON blocks(times_used DESC);

-- Usage analytics table
CREATE TABLE block_usage (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    block_id TEXT NOT NULL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    context TEXT,  -- What was being built
    success BOOLEAN,
    execution_ms REAL,
    tokens_saved INTEGER,
    FOREIGN KEY(block_id) REFERENCES blocks(block_id)
);

-- Block relationships (which blocks work well together)
CREATE TABLE block_pairs (
    first_block TEXT NOT NULL,
    second_block TEXT NOT NULL,
    times_used_together INTEGER DEFAULT 1,
    avg_success_rate REAL,
    PRIMARY KEY(first_block, second_block)
);
```

**Implementation**:
```cpp
// include/naab/block_search_index.h
#pragma once

#include <string>
#include <vector>
#include <memory>
#include "naab/block_loader.h"

namespace naab {
namespace runtime {

struct SearchQuery {
    std::string text;                    // "validate email"
    std::vector<std::string> languages;  // Filter by language
    std::string performance_tier;        // "fast" | "medium" | "slow"
    int min_success_rate = 0;           // Minimum success rate %
    bool security_audited_only = false;
    int limit = 10;
};

struct SearchResult {
    BlockMetadata metadata;
    double relevance_score;  // 0.0 to 1.0
    std::string match_reason; // "Matched keywords: email, validate"
};

class BlockSearchIndex {
public:
    BlockSearchIndex(const std::string& db_path);
    ~BlockSearchIndex();

    // Build index from filesystem blocks
    void buildIndex(const std::string& blocks_library_path);

    // Search operations
    std::vector<SearchResult> search(const SearchQuery& query);
    std::vector<SearchResult> semanticSearch(const std::string& natural_language);

    // Get recommendations
    std::vector<BlockMetadata> recommendNextBlock(const std::string& current_block_id);
    std::vector<BlockMetadata> getPopularBlocks(int limit = 10);
    std::vector<BlockMetadata> getSimilarBlocks(const std::string& block_id);

    // Analytics
    void recordUsage(const std::string& block_id, bool success, double execution_ms, int tokens_saved);
    void recordBlockPair(const std::string& first, const std::string& second);

    // Maintenance
    void updateBlockMetadata(const BlockMetadata& metadata);
    void reindex();

private:
    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace runtime
} // namespace naab
```

**Example Usage**:
```cpp
// Build index once
BlockSearchIndex index("/storage/emulated/0/Download/.naab/naab/blocks/block_search.db");
index.buildIndex("/storage/emulated/0/Download/.naab/naab/blocks/library");

// Search
SearchQuery query;
query.text = "validate email address";
query.performance_tier = "fast";
query.security_audited_only = true;
query.limit = 5;

auto results = index.search(query);
for (const auto& result : results) {
    fmt::print("Block: {} (score: {:.2f})\n",
               result.metadata.block_id,
               result.relevance_score);
    fmt::print("  Reason: {}\n", result.match_reason);
}
```

**Deliverable**: SQLite database with searchable block index

---

### Phase 2: Semantic Search Engine (Week 3-4)

**Goal**: AI-friendly natural language block search

#### Task 2.1: Keyword-Based Search (3 days)

**File**: `src/runtime/block_search_index.cpp`

**Algorithm**:
```cpp
std::vector<SearchResult> BlockSearchIndex::search(const SearchQuery& query) {
    // 1. Tokenize search query
    auto tokens = tokenize(query.text);  // "validate email" → ["validate", "email"]

    // 2. Build FTS query
    std::string fts_query = join(tokens, " OR ");

    // 3. Search with filters
    std::string sql = R"(
        SELECT
            b.*,
            bm_fts.rank as relevance
        FROM blocks_fts
        JOIN blocks b ON blocks_fts.block_id = b.block_id
        WHERE blocks_fts MATCH ?
    )";

    if (!query.languages.empty()) {
        sql += " AND b.language IN ('" + join(query.languages, "','") + "')";
    }
    if (!query.performance_tier.empty()) {
        sql += " AND b.performance_tier = '" + query.performance_tier + "'";
    }
    if (query.min_success_rate > 0) {
        sql += " AND b.success_rate_percent >= " + std::to_string(query.min_success_rate);
    }
    if (query.security_audited_only) {
        sql += " AND b.security_audited = 1";
    }

    sql += " ORDER BY relevance DESC, b.times_used DESC LIMIT " + std::to_string(query.limit);

    // 4. Execute and return results
    return executeSearchQuery(sql, fts_query);
}
```

**Ranking Algorithm**:
```cpp
double calculateRelevanceScore(const BlockMetadata& block, const std::vector<std::string>& query_tokens) {
    double score = 0.0;

    // Exact name match (highest weight)
    for (const auto& token : query_tokens) {
        if (contains_ignore_case(block.name, token)) score += 10.0;
        if (contains_ignore_case(block.short_desc, token)) score += 5.0;
    }

    // Keyword matches
    for (const auto& keyword : block.keywords) {
        for (const auto& token : query_tokens) {
            if (keyword == token) score += 3.0;
        }
    }

    // Category/subcategory matches
    for (const auto& token : query_tokens) {
        if (block.category == token) score += 2.0;
        if (block.subcategory == token) score += 1.0;
    }

    // Quality multipliers
    score *= (1.0 + block.success_rate_percent / 100.0);  // Higher success = better
    score *= (1.0 + block.test_coverage_percent / 100.0); // More tests = better

    // Usage popularity boost
    if (block.times_used > 100) score *= 1.2;
    if (block.times_used > 1000) score *= 1.5;

    // Stability boost
    if (block.stability == "stable") score *= 1.3;
    else if (block.stability == "beta") score *= 0.8;

    return score;
}
```

**Deliverable**: Fast keyword search with smart ranking

---

#### Task 2.2: Natural Language Search with Embeddings (4 days)

**Purpose**: Convert "validate user email input" → find BLOCK-PY-09145

**Dependencies**: Add sentence-transformers or similar (Python bridge)

**Architecture**:
```
User Query: "validate user email input"
    ↓
[Embedding Model] sentence-transformers/all-MiniLM-L6-v2
    ↓
Query Vector: [0.23, -0.45, 0.67, ...]
    ↓
[Vector Search] Compare with pre-computed block embeddings
    ↓
Top 10 Similar Blocks (cosine similarity > 0.7)
    ↓
[Re-rank] Using relevance score + usage analytics
    ↓
Final Results
```

**Implementation**:

**File**: `tools/build_block_embeddings.py` (NEW)
```python
"""
Build semantic embeddings for all blocks
Runs once, stores embeddings in SQLite
"""

from sentence_transformers import SentenceTransformer
import sqlite3
import json
from pathlib import Path
import numpy as np

def build_embeddings():
    # Load model (384-dimensional embeddings, fast)
    model = SentenceTransformer('sentence-transformers/all-MiniLM-L6-v2')

    # Connect to database
    conn = sqlite3.connect('/storage/emulated/0/Download/.naab/naab/blocks/block_search.db')
    cursor = conn.cursor()

    # Create embeddings table
    cursor.execute('''
        CREATE TABLE IF NOT EXISTS block_embeddings (
            block_id TEXT PRIMARY KEY,
            embedding BLOB NOT NULL,  -- Numpy array serialized
            embedding_text TEXT        -- What was embedded (for debugging)
        )
    ''')

    # Get all blocks
    cursor.execute('SELECT block_id, name, description, keywords FROM blocks WHERE is_active = 1')
    blocks = cursor.fetchall()

    print(f"Building embeddings for {len(blocks)} blocks...")

    for block_id, name, description, keywords_json in blocks:
        # Create text to embed
        keywords = json.loads(keywords_json) if keywords_json else []
        text_to_embed = f"{name}. {description}. Keywords: {', '.join(keywords)}"

        # Generate embedding
        embedding = model.encode(text_to_embed)

        # Store in database
        cursor.execute('''
            INSERT OR REPLACE INTO block_embeddings (block_id, embedding, embedding_text)
            VALUES (?, ?, ?)
        ''', (block_id, embedding.tobytes(), text_to_embed))

        if len(blocks) % 100 == 0:
            print(f"Processed {len(blocks)} blocks...")

    conn.commit()
    conn.close()
    print("Embeddings built successfully!")

if __name__ == '__main__':
    build_embeddings()
```

**File**: `src/runtime/semantic_search.cpp` (NEW)
```cpp
// include/naab/semantic_search.h
#pragma once

#include <string>
#include <vector>
#include "naab/block_loader.h"

namespace naab {
namespace runtime {

class SemanticSearch {
public:
    SemanticSearch(const std::string& db_path, const std::string& model_path);

    // Semantic search using embeddings
    std::vector<SearchResult> search(const std::string& natural_language_query, int limit = 10);

    // Find similar blocks
    std::vector<BlockMetadata> findSimilar(const std::string& block_id, int limit = 5);

private:
    // Call Python embedding model via pybind11
    std::vector<float> getQueryEmbedding(const std::string& text);

    // Cosine similarity
    double cosineSimilarity(const std::vector<float>& a, const std::vector<float>& b);

    class Impl;
    std::unique_ptr<Impl> pimpl_;
};

} // namespace runtime
} // namespace naab
```

**Example**:
```cpp
SemanticSearch search(db_path, model_path);

// Natural language query
auto results = search.search("I need to validate user email addresses from a form");

// Results ranked by semantic similarity:
// 1. BLOCK-PY-09145 "Email Validator" (score: 0.92)
// 2. BLOCK-JS-08234 "Form Input Validator" (score: 0.85)
// 3. BLOCK-CPP-07089 "String Validator" (score: 0.78)
```

**Deliverable**: Semantic search that understands natural language

---

### Phase 3: Type System & Composition Validation (Week 5-6)

**Goal**: Ensure blocks can be chained together correctly

#### Task 3.1: Define Type System (2 days)

**File**: `include/naab/type_system.h` (NEW)

**Type Definitions**:
```cpp
namespace naab {
namespace types {

enum class BaseType {
    Any,
    Int,
    Float,
    String,
    Bool,
    Array,
    Dict,
    Void
};

struct Type {
    BaseType base;
    std::vector<Type> generic_params;  // For Array<String>, Dict<String,Int>

    // Type constructors
    static Type Int() { return {BaseType::Int, {}}; }
    static Type String() { return {BaseType::String, {}}; }
    static Type Array(Type element) { return {BaseType::Array, {element}}; }
    static Type Dict(Type key, Type value) { return {BaseType::Dict, {key, value}}; }

    // Type checking
    bool isCompatibleWith(const Type& other) const;
    std::string toString() const;
};

// Parse type from string
Type parseType(const std::string& type_str);
// "int" → Type::Int()
// "array<string>" → Type::Array(Type::String())
// "dict<string,int>" → Type::Dict(Type::String(), Type::Int())

} // namespace types
} // namespace naab
```

**Update BlockMetadata**:
```cpp
struct BlockMetadata {
    // Change from strings to Type objects
    std::vector<types::Type> input_types;  // [String, Int] for validate(str, threshold)
    types::Type output_type;               // Bool for validator

    // Helper
    bool canAcceptInput(const std::vector<types::Type>& args) const;
    bool outputCompatibleWith(const types::Type& expected) const;
};
```

---

#### Task 3.2: Composition Validator (3 days)

**File**: `src/runtime/composition_validator.cpp` (NEW)

**Purpose**: Check if blocks can be chained: `block1() |> block2() |> block3()`

**Implementation**:
```cpp
// include/naab/composition_validator.h
#pragma once

#include <string>
#include <vector>
#include "naab/block_loader.h"
#include "naab/type_system.h"

namespace naab {
namespace runtime {

struct CompositionError {
    int position;           // Which block in chain
    std::string block_id;
    std::string error;
    std::vector<std::string> suggestions;  // Suggested adapter blocks
};

struct CompositionValidation {
    bool valid;
    std::vector<CompositionError> errors;
    std::vector<std::string> warnings;
};

class CompositionValidator {
public:
    // Validate a pipeline: [block1, block2, block3]
    CompositionValidation validate(const std::vector<std::string>& block_ids);

    // Suggest adapter blocks
    std::vector<BlockMetadata> suggestAdapter(const types::Type& from, const types::Type& to);

    // Check if two blocks can be chained
    bool canChain(const std::string& first_block, const std::string& second_block);

private:
    BlockSearchIndex* index_;
};

} // namespace runtime
} // namespace naab
```

**Example**:
```cpp
CompositionValidator validator;

// Check pipeline
std::vector<std::string> pipeline = {
    "BLOCK-CPP-07001",  // Outputs: Array<String>
    "BLOCK-PY-09001",   // Expects: DataFrame (INCOMPATIBLE!)
    "BLOCK-JS-08001"    // Expects: JSON
};

auto result = validator.validate(pipeline);

if (!result.valid) {
    for (const auto& error : result.errors) {
        fmt::print("Error at position {}: {}\n", error.position, error.error);
        fmt::print("  Block: {}\n", error.block_id);

        if (!error.suggestions.empty()) {
            fmt::print("  Suggestions:\n");
            for (const auto& suggestion : error.suggestions) {
                fmt::print("    - Use {} as adapter\n", suggestion);
            }
        }
    }
}

// Output:
// Error at position 1: Type mismatch
//   Block: BLOCK-PY-09001
//   Expected: DataFrame
//   Got: Array<String>
//   Suggestions:
//     - Use BLOCK-PY-09234 (array_to_dataframe) as adapter
//     - Use BLOCK-PY-09235 (csv_to_dataframe) as adapter
```

**Deliverable**: Type-safe block composition with helpful error messages

---

### Phase 4: Pipeline Syntax & Validation (Week 7-8)

**Goal**: Add `|>` operator and auto-validate pipelines

#### Task 4.1: Add Pipeline Operator to Parser (3 days)

**File**: `src/parser/parser.cpp`

**Add Token**:
```cpp
// In lexer.cpp
if (current == '|' && peek() == '>') {
    advance();
    return Token(TokenType::PIPELINE, "|>", line, column - 1);
}
```

**Add to Grammar**:
```
expression → pipeline_expr
pipeline_expr → call_expr ("|>" call_expr)*
```

**AST Node**:
```cpp
// In ast.h
struct PipelineExpr : Expr {
    std::vector<std::unique_ptr<Expr>> stages;  // Each stage is a function call

    PipelineExpr(std::vector<std::unique_ptr<Expr>> s)
        : stages(std::move(s)) {}

    void accept(ExprVisitor& visitor) override {
        visitor.visit(*this);
    }
};
```

**Parser**:
```cpp
std::unique_ptr<ast::Expr> Parser::parsePipelineExpr() {
    auto expr = parseCallExpr();

    std::vector<std::unique_ptr<ast::Expr>> stages;
    stages.push_back(std::move(expr));

    while (match(lexer::TokenType::PIPELINE)) {
        auto next_stage = parseCallExpr();
        stages.push_back(std::move(next_stage));
    }

    if (stages.size() == 1) {
        return std::move(stages[0]);
    }

    return std::make_unique<ast::PipelineExpr>(std::move(stages));
}
```

---

#### Task 4.2: Interpreter with Pipeline Validation (4 days)

**File**: `src/interpreter/interpreter.cpp`

**Execution**:
```cpp
void Interpreter::visit(ast::PipelineExpr& expr) {
    // Extract block IDs from pipeline
    std::vector<std::string> block_ids;
    for (const auto& stage : expr.stages) {
        if (auto* call = dynamic_cast<ast::CallExpr*>(stage.get())) {
            // Extract block ID from call
            block_ids.push_back(extractBlockId(call));
        }
    }

    // Validate composition BEFORE execution
    CompositionValidator validator;
    auto validation = validator.validate(block_ids);

    if (!validation.valid) {
        std::string error_msg = "Pipeline validation failed:\n";
        for (const auto& err : validation.errors) {
            error_msg += fmt::format("  Position {}: {}\n", err.position, err.error);
            if (!err.suggestions.empty()) {
                error_msg += "  Suggestions: " + join(err.suggestions, ", ") + "\n";
            }
        }
        throw std::runtime_error(error_msg);
    }

    // Execute pipeline (data flows through stages)
    auto data = evaluate(expr.stages[0].get());

    for (size_t i = 1; i < expr.stages.size(); ++i) {
        // Pass output of previous stage as input to next
        data = evaluateWithInput(expr.stages[i].get(), data);
    }

    result_ = data;
}
```

**Example NAAb Code**:
```naab
# Pipeline with auto-validation
use BLOCK-CPP-07001 as csv_parser
use BLOCK-PY-09001 as validator
use BLOCK-JS-08001 as json_formatter

function process_data(csv_file: string) {
    # Type-checked at compile time!
    return csv_file
        |> csv_parser.parse           # String → Array<Dict>
        |> validator.validate_all     # Array<Dict> → Array<Dict> (validated)
        |> json_formatter.to_json     # Array<Dict> → String (JSON)
}

# If types don't match, get helpful error:
# Error: Type mismatch at position 2
#   Expected: DataFrame
#   Got: Array<Dict>
#   Suggestion: Use BLOCK-PY-09234 (dict_array_to_dataframe)
```

**Deliverable**: Working pipeline syntax with validation

---

### Phase 5: AI Integration API (Week 9-10)

**Goal**: REST API for AI agents to discover and compose blocks

#### Task 5.1: REST API Server (4 days)

**File**: `src/api/block_api_server.cpp` (NEW)

**Tech Stack**: cpp-httplib (header-only C++ HTTP library)

**Endpoints**:
```cpp
// GET /api/search?q=validate+email&limit=10
{
  "query": "validate email",
  "results": [
    {
      "block_id": "BLOCK-PY-09145",
      "name": "Email Validator",
      "language": "python",
      "description": "Validates email addresses using RFC 5322",
      "score": 0.92,
      "input_types": ["string"],
      "output_type": "bool",
      "performance": "fast",
      "success_rate": 98,
      "tokens_saved": 150
    },
    ...
  ]
}

// POST /api/compose
{
  "task": "Process CSV file and export to JSON",
  "requirements": {
    "performance": "fast",
    "security_audited": true
  }
}

Response:
{
  "pipeline": [
    "BLOCK-CPP-07001",
    "BLOCK-PY-09001",
    "BLOCK-JS-08001"
  ],
  "code": "csv_file |> cpp_parser.parse |> py_validator.validate |> js_formatter.to_json",
  "validation": {
    "valid": true,
    "warnings": []
  },
  "estimated_performance": {
    "execution_ms": 15,
    "tokens_saved": 450
  },
  "confidence": 0.89
}

// GET /api/recommend?after=BLOCK-PY-09001
{
  "recommendations": [
    {
      "block_id": "BLOCK-JS-08001",
      "reason": "Used together 347 times with 95% success rate",
      "score": 0.85
    },
    ...
  ]
}

// POST /api/validate
{
  "pipeline": ["BLOCK-CPP-07001", "BLOCK-PY-09001"]
}

Response:
{
  "valid": true,
  "errors": [],
  "warnings": [
    "Performance: BLOCK-PY-09001 is slow for large datasets (>10K items)"
  ]
}
```

**Implementation**:
```cpp
// src/api/block_api_server.cpp
#include <httplib.h>
#include "naab/block_search_index.h"
#include "naab/semantic_search.h"
#include "naab/composition_validator.h"

class BlockAPIServer {
public:
    BlockAPIServer(int port) : port_(port) {
        index_ = std::make_unique<BlockSearchIndex>(db_path);
        semantic_ = std::make_unique<SemanticSearch>(db_path, model_path);
        validator_ = std::make_unique<CompositionValidator>();
    }

    void start() {
        httplib::Server svr;

        // Search endpoint
        svr.Get("/api/search", [this](const httplib::Request& req, httplib::Response& res) {
            handleSearch(req, res);
        });

        // Compose endpoint
        svr.Post("/api/compose", [this](const httplib::Request& req, httplib::Response& res) {
            handleCompose(req, res);
        });

        // Recommend endpoint
        svr.Get("/api/recommend", [this](const httplib::Request& req, httplib::Response& res) {
            handleRecommend(req, res);
        });

        // Validate endpoint
        svr.Post("/api/validate", [this](const httplib::Request& req, httplib::Response& res) {
            handleValidate(req, res);
        });

        fmt::print("Block API Server listening on port {}\n", port_);
        svr.listen("0.0.0.0", port_);
    }

private:
    int port_;
    std::unique_ptr<BlockSearchIndex> index_;
    std::unique_ptr<SemanticSearch> semantic_;
    std::unique_ptr<CompositionValidator> validator_;

    void handleSearch(const httplib::Request& req, httplib::Response& res);
    void handleCompose(const httplib::Request& req, httplib::Response& res);
    void handleRecommend(const httplib::Request& req, httplib::Response& res);
    void handleValidate(const httplib::Request& req, httplib::Response& res);
};

// Usage:
// BlockAPIServer server(8080);
// server.start();
```

**Deliverable**: REST API for AI agents

---

#### Task 5.2: AI Agent Integration (3 days)

**Python Client Library**:
```python
# naab_client.py
import requests

class NaabClient:
    def __init__(self, base_url="http://localhost:8080"):
        self.base_url = base_url

    def search(self, query, limit=10, **filters):
        """Search for blocks"""
        params = {"q": query, "limit": limit, **filters}
        response = requests.get(f"{self.base_url}/api/search", params=params)
        return response.json()

    def compose(self, task, requirements=None):
        """Auto-compose a pipeline for a task"""
        data = {"task": task, "requirements": requirements or {}}
        response = requests.post(f"{self.base_url}/api/compose", json=data)
        return response.json()

    def validate(self, pipeline):
        """Validate a block pipeline"""
        response = requests.post(f"{self.base_url}/api/validate", json={"pipeline": pipeline})
        return response.json()

# Usage in AI agent:
client = NaabClient()

# Agent wants to validate emails
results = client.search("validate email address", performance="fast", security_audited=True)
best_block = results["results"][0]
print(f"Use: {best_block['block_id']}")

# Or let API compose automatically
composition = client.compose("Process CSV and export to JSON")
print(f"Pipeline: {composition['code']}")
print(f"Confidence: {composition['confidence']}")
```

**Deliverable**: Python client library for AI integration

---

### Phase 6: CLI Tools (Week 11)

**Goal**: Command-line tools for developers

#### Task 6.1: naab-search CLI (2 days)

**File**: `src/cli/naab_search.cpp` (NEW)

**Usage**:
```bash
# Search for blocks
$ naab-search "validate email"
Found 5 blocks:

[1] BLOCK-PY-09145 - Email Validator
    Language: python
    Performance: fast (0.5ms avg)
    Success rate: 98%
    Usage: 4,523 apps
    Description: Validates email addresses using RFC 5322

[2] BLOCK-JS-08234 - Form Input Validator
    Language: javascript
    Performance: medium (2ms avg)
    Success rate: 95%
    Usage: 1,234 apps

# Search with filters
$ naab-search "hash password" --language=rust --performance=fast --security-audited
Found 2 blocks:

[1] BLOCK-RUST-05089 - BCrypt Password Hasher
    Performance: fast (10ms avg)
    Security: ✓ Audited
    Success rate: 99%

# Get block details
$ naab-search --block BLOCK-PY-09145 --verbose
Block ID: BLOCK-PY-09145
Name: Email Validator
Language: Python
Category: validation / input
Version: 2.1.0

Description:
  Validates email addresses according to RFC 5322 specification.
  Supports international domain names (IDN).

Input: string (email address)
Output: bool (valid/invalid)

Performance:
  Average: 0.5ms
  P99: 2ms
  Memory: <1MB

Quality:
  Test coverage: 100%
  Security: ✓ Audited
  Stability: stable
  Success rate: 98%

Usage:
  import {validate_email} from "BLOCK-PY-09145"

  let is_valid = validate_email("user@example.com")  # true

Related blocks:
  - BLOCK-PY-09146: Email normalizer
  - BLOCK-PY-09147: Email domain validator

# Validate pipeline
$ naab-search --validate "BLOCK-CPP-07001,BLOCK-PY-09001,BLOCK-JS-08001"
✓ Pipeline is valid
  Stage 1: BLOCK-CPP-07001 → Array<Dict>
  Stage 2: BLOCK-PY-09001 → Array<Dict> (validated)
  Stage 3: BLOCK-JS-08001 → String (JSON)

Estimated performance: 15ms for 1K items
Tokens saved: ~450 tokens vs writing from scratch
```

**Deliverable**: naab-search CLI tool

---

### Phase 7: Usage Analytics & Learning (Week 12)

**Goal**: System learns from usage patterns

#### Task 7.1: Analytics Collection (3 days)

**Automatic Tracking**:
```cpp
// In interpreter, after executing block
void Interpreter::executeBlock(const std::string& block_id, const std::vector<Value>& args) {
    auto start_time = std::chrono::high_resolution_clock::now();

    try {
        auto result = execute_block_internal(block_id, args);

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        // Record successful usage
        analytics_.recordUsage(block_id, true, duration_ms, estimate_tokens_saved(block_id));

        return result;
    } catch (const std::exception& e) {
        // Record failed usage
        analytics_.recordUsage(block_id, false, 0, 0);
        throw;
    }
}

// Track block pairs (which blocks are used together)
void Interpreter::visit(ast::PipelineExpr& expr) {
    for (size_t i = 0; i < expr.stages.size() - 1; ++i) {
        std::string first = extractBlockId(expr.stages[i].get());
        std::string second = extractBlockId(expr.stages[i+1].get());

        analytics_.recordBlockPair(first, second);
    }

    // ... execute pipeline
}
```

**Analytics Dashboard**:
```bash
$ naab-stats

NAAb Block Usage Statistics
============================

Top 10 Most Used Blocks:
  1. BLOCK-PY-09145 (Email Validator) - 4,523 uses, 98% success
  2. BLOCK-JS-08001 (JSON Formatter) - 3,891 uses, 99% success
  3. BLOCK-CPP-07001 (CSV Parser) - 2,456 uses, 95% success
  ...

Top Block Combinations:
  1. BLOCK-CPP-07001 → BLOCK-PY-09001 (347 times, 95% success)
  2. BLOCK-PY-09001 → BLOCK-JS-08001 (289 times, 97% success)
  ...

Performance Summary:
  Total blocks executed: 12,345
  Avg execution time: 5.2ms
  Total tokens saved: ~1.2M tokens
  Cost savings: ~$35 (vs writing from scratch)

Quality Metrics:
  Overall success rate: 96.5%
  Blocks with >95% success: 89%
  Security-audited blocks used: 76%
```

**Deliverable**: Analytics system that learns usage patterns

---

## 📋 Implementation Summary

### Milestones

**Week 2**: Enhanced metadata for all 24,488 blocks
**Week 4**: SQLite search index operational
**Week 6**: Semantic search working with embeddings
**Week 8**: Type system + composition validation complete
**Week 10**: Pipeline syntax working in NAAb language
**Week 12**: REST API + CLI tools + analytics ready

### Files Created/Modified

**New Files** (15+):
```
include/naab/block_search_index.h
include/naab/semantic_search.h
include/naab/type_system.h
include/naab/composition_validator.h
src/runtime/block_search_index.cpp
src/runtime/semantic_search.cpp
src/runtime/composition_validator.cpp
src/api/block_api_server.cpp
src/cli/naab_search.cpp
tools/enrich_block_metadata.py
tools/build_block_embeddings.py
naab_client.py
```

**Modified Files** (5):
```
include/naab/block_loader.h (extend BlockMetadata)
include/naab/ast.h (add PipelineExpr)
src/parser/parser.cpp (add pipeline parsing)
src/interpreter/interpreter.cpp (add pipeline execution)
src/lexer/lexer.cpp (add |> token)
```

### Testing Strategy

**Unit Tests**:
- Type system (50+ tests)
- Composition validator (40+ tests)
- Search ranking (30+ tests)
- Pipeline parsing (25+ tests)

**Integration Tests**:
- End-to-end search
- Pipeline execution
- API endpoints
- CLI commands

**Performance Tests**:
- Search latency (<100ms for 24K blocks)
- Embedding lookup (<50ms)
- Pipeline validation (<10ms)
- API response time (<200ms)

---

## 🚀 Quick Start Commands

### Build Search Index
```bash
cd /storage/emulated/0/Download/.naab/naab_language

# Build search index from blocks
python3 tools/enrich_block_metadata.py
python3 tools/build_block_embeddings.py

# Test search
./build/naab-search "validate email"
```

### Start API Server
```bash
# Start REST API on port 8080
./build/naab-api-server --port 8080

# Test from Python
python3 -c "
from naab_client import NaabClient
client = NaabClient()
results = client.search('validate email')
print(results['results'][0]['block_id'])
"
```

### Use in NAAb Code
```naab
# Enable pipeline syntax
use BLOCK-CPP-07001 as csv_parser
use BLOCK-PY-09001 as validator
use BLOCK-JS-08001 as formatter

# Type-safe pipeline
let data = "data.csv" |> csv_parser.parse |> validator.validate |> formatter.to_json
```

---

## 🎯 Success Metrics

**Search Quality**:
- 95% of searches find relevant block in top 3 results
- Semantic search accuracy >90%

**Performance**:
- Search latency <100ms
- Pipeline validation <10ms
- API response <200ms

**AI Adoption**:
- 10+ AI agents using API
- 80% token reduction in AI-generated code
- 95% fewer bugs (using tested blocks)

**Developer Experience**:
- Find right block in <30 seconds
- Compose pipeline in <5 minutes
- Zero type errors at runtime (caught at parse time)

---

**Ready to implement! Start with Phase 1 (Weeks 1-2): Enhanced Block Metadata**

---

*Generated: December 28, 2024*
*Location: /storage/emulated/0/Download/.naab/naab_language/BLOCK_DISCOVERY_PLAN.md*
