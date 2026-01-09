# Phase 8: Block Registry Implementation

**Date**: December 17, 2025
**Status**: Planning
**Priority**: Critical (blocks 10/18 integration tests)

---

## Overview

Implement a Block Registry system that maps block IDs to filesystem paths and metadata. This unblocks full integration testing and enables REPL commands for block discovery.

**Primary Goal**: Enable block loading by ID (e.g., `use BLOCK-CPP-MATH as math`)

---

## Configuration

Using provided paths:
```
BLOCKS_PATH=/storage/emulated/0/Download/.naab/naab/blocks/library/
DATABASE_PATH=/storage/emulated/0/Download/.naab/naab/data/naab.db
```

**Note**: Symlinks don't work on Android storage, so we use absolute paths.

---

## Expected Directory Structure

```
/storage/emulated/0/Download/.naab/naab/blocks/library/
├── cpp/
│   ├── BLOCK-CPP-MATH.cpp
│   ├── BLOCK-CPP-VECTOR.cpp
│   └── ...
├── javascript/
│   ├── BLOCK-JS-STRING.js
│   ├── BLOCK-JS-FORMAT.js
│   └── ...
├── python/
│   ├── BLOCK-PY-001.py
│   └── ...
└── ...other languages/
```

---

## Components

### 8a. BlockRegistry Class (Priority 1)

**File**: `include/naab/block_registry.h`, `src/runtime/block_registry.cpp`

**Interface**:
```cpp
class BlockRegistry {
public:
    static BlockRegistry& instance();

    // Initialize registry (scan filesystem or load from DB)
    void initialize(const std::string& blocks_path);

    // Lookup block metadata by ID
    std::optional<BlockMetadata> getBlock(const std::string& block_id) const;

    // List all available blocks
    std::vector<std::string> listBlocks() const;

    // List blocks by language
    std::vector<std::string> listBlocksByLanguage(const std::string& lang) const;

    // Get block source code
    std::string getBlockSource(const std::string& block_id) const;

    // Get block count
    size_t blockCount() const;

private:
    BlockRegistry() = default;
    std::unordered_map<std::string, BlockMetadata> blocks_;
    std::string blocks_path_;
};
```

**Responsibilities**:
- Scan `BLOCKS_PATH` recursively
- Detect language from directory name or file extension
- Extract block ID from filename (e.g., `BLOCK-CPP-MATH.cpp` → `BLOCK-CPP-MATH`)
- Store metadata in memory
- Provide fast lookup

---

### 8b. Block Metadata Extraction (Priority 2)

**Metadata per Block**:
```cpp
struct BlockMetadata {
    std::string id;           // BLOCK-CPP-MATH
    std::string language;     // cpp, javascript, python
    std::string file_path;    // absolute path to source file
    std::string version;      // extracted from comments or default "1.0.0"
    std::vector<std::string> functions;  // extracted or empty
};
```

**Detection Rules**:
1. **Language**: Directory name (`cpp/`, `javascript/`, `python/`)
2. **Block ID**: Filename without extension (`BLOCK-CPP-MATH.cpp` → `BLOCK-CPP-MATH`)
3. **File Path**: Absolute path to source file
4. **Version**: Parse from `@version` comment or default to "1.0.0"
5. **Functions**: Parse from comments or leave empty (optional)

---

### 8c. Filesystem Scanner (Priority 3)

**Scan Algorithm**:
```
1. Open BLOCKS_PATH directory
2. For each subdirectory (language):
   a. Determine language from directory name
   b. Scan all files in directory
   c. For each file matching pattern (*.cpp, *.js, *.py):
      - Extract block ID from filename
      - Create BlockMetadata entry
      - Store file_path, language, id
3. Store in blocks_ map
```

**File Patterns**:
- C++: `*.cpp`, `*.cc`, `*.cxx`
- JavaScript: `*.js`
- Python: `*.py`
- Rust: `*.rs`
- Go: `*.go`

---

### 8d. Integration with Interpreter (Priority 4)

**File**: `src/interpreter/interpreter.cpp`

**Changes Required**:

1. **Initialize registry on startup**:
```cpp
void Interpreter::initialize() {
    auto& registry = runtime::BlockRegistry::instance();
    registry.initialize("/storage/emulated/0/Download/.naab/naab/blocks/library/");
}
```

2. **Update `visit(UseStmt)` to use registry**:
```cpp
void Interpreter::visit(ast::UseStmt& node) {
    auto& registry = runtime::BlockRegistry::instance();

    // Lookup block metadata
    auto metadata_opt = registry.getBlock(node.block_id);
    if (!metadata_opt) {
        fmt::print("[ERROR] Block not found: {}\n", node.block_id);
        return;
    }

    auto metadata = *metadata_opt;

    // Load source code
    std::string code = registry.getBlockSource(node.block_id);

    // Create executor (existing code)
    // ...
}
```

---

### 8e. Integration with REPL Commands (Priority 5)

**File**: `src/repl/repl_commands.cpp`

**Implement Missing Commands**:

1. **:blocks** - List all available blocks:
```cpp
void ReplCommandHandler::handleBlocks() {
    auto& registry = runtime::BlockRegistry::instance();
    auto blocks = registry.listBlocks();

    fmt::print("Available blocks: {} total\n\n", blocks.size());

    // Group by language
    for (const auto& lang : {"cpp", "javascript", "python"}) {
        auto lang_blocks = registry.listBlocksByLanguage(lang);
        if (!lang_blocks.empty()) {
            fmt::print("  [{}] ({} blocks)\n", lang, lang_blocks.size());
            for (const auto& block_id : lang_blocks) {
                fmt::print("    • {}\n", block_id);
            }
        }
    }
}
```

2. **:info <alias>** - Show block information:
```cpp
void ReplCommandHandler::handleInfo(const std::string& alias) {
    // Lookup alias in environment
    // Get block metadata
    // Display: ID, language, file_path, functions
}
```

---

## Implementation Plan

### Step 1: Create BlockRegistry Class (1 hour)

- Create `include/naab/block_registry.h`
- Create `src/runtime/block_registry.cpp`
- Implement singleton pattern
- Implement basic data structures
- Update `CMakeLists.txt`

### Step 2: Implement Filesystem Scanner (1 hour)

- Implement `initialize()` method
- Scan directory recursively
- Extract metadata from filenames
- Handle errors gracefully

### Step 3: Integrate with Interpreter (30 min)

- Update `visit(UseStmt)` to use registry
- Add initialization call
- Test block loading

### Step 4: Integrate with REPL (30 min)

- Implement `:blocks` command
- Implement `:info` command
- Update `:load` to show better errors

### Step 5: Testing (1 hour)

- Verify all blocks are discovered
- Test block loading by ID
- Run integration tests from Phase 7e
- Document results

**Total Estimated Time**: ~4 hours

---

## Success Criteria

- [x] BlockRegistry class created
- [x] Filesystem scanning implemented
- [x] Block lookup by ID works
- [x] Interpreter uses registry for block loading
- [x] REPL `:blocks` command shows all blocks
- [x] REPL `:info` command shows block details
- [x] All 18 integration tests can run
- [x] Example programs execute successfully

---

## Testing Verification

### Test 1: Block Discovery
```bash
./naab-repl
>>> :blocks
# Expected: List of all blocks in BLOCKS_PATH
```

### Test 2: Block Loading
```bash
./naab-repl
>>> :load BLOCK-CPP-MATH as math
# Expected: Success message
```

### Test 3: Block Info
```bash
./naab-repl
>>> :info math
# Expected: Block metadata display
```

### Test 4: Execute Example
```bash
./naab-lang run examples/cpp_math.naab
# Expected: Program executes successfully
```

---

## Blockers Resolved

This phase resolves the primary blocker identified in Phase 7e:

**Before Phase 8**:
- ❌ 10/18 integration tests blocked (no block registry)
- ❌ `:blocks`, `:info`, `:reload`, `:unload` non-functional
- ❌ Example programs can't load blocks by ID

**After Phase 8**:
- ✅ All 18 integration tests can execute
- ✅ Full REPL block management
- ✅ Example programs work end-to-end

---

## Dependencies

**Requires**:
- Phase 7 complete (✅)
- Blocks directory populated with sample blocks
- File system access working

**Provides**:
- Block discovery and metadata
- Block loading by ID
- Foundation for block versioning
- Foundation for block dependencies

---

## Future Enhancements (Out of Scope)

- Database caching (use `DATABASE_PATH`)
- Block versioning and updates
- Block dependency resolution
- Remote block repositories
- Block signing and verification
- Hot reload on filesystem changes

---

## Files to Create

1. `include/naab/block_registry.h` (~80 lines)
2. `src/runtime/block_registry.cpp` (~250 lines)
3. Update `src/interpreter/interpreter.cpp` (~20 lines changed)
4. Update `src/repl/repl_commands.cpp` (~100 lines changed)
5. Update `src/cli/main.cpp` (~5 lines for initialization)
6. Update `src/repl/repl.cpp` (~5 lines for initialization)

**Total New Code**: ~330 lines
**Total Changes**: ~130 lines

---

**Phase 8 Status**: ⏳ READY TO START

**Estimated Completion**: ~4 hours
