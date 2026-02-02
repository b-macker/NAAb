# Block Assembly System - Quick Start Guide

## ✅ Block System Status

The Block Assembly System is **fully implemented and operational** with:

- **585+ Python blocks** in JSON format
- **SQLite FTS5** search index for fast lookups (<100ms)
- **Lazy BlockRegistry** for efficient loading
- **CLI commands** for searching and using blocks

**Location**: `~/.naab/language/blocks/library/`

---

## 🚀 First Time Setup (ONE-TIME ONLY)

Before you can search or use blocks, you must build the search index:

```bash
cd ~/.naab/language/build
./naab-lang blocks index ~/.naab/language/blocks/library
```

**Expected output:**
```
[INFO] Building search index from: ~/.naab/language/blocks/library
[INFO] Indexed 585 blocks successfully
```

**What this does:**
- Scans all JSON block files in `blocks/library/python/`, `blocks/library/javascript/`, etc.
- Creates SQLite database with Full-Text Search (FTS5)
- Enables fast searching and discovery
- **Only needs to be run once** (or when new blocks are added)

---

## 📚 Using Blocks - Quick Commands

### Search for Blocks

```bash
cd ~/.naab/language/build

# Search by keyword
./naab-lang blocks search "validate"
./naab-lang blocks search "parse json"
./naab-lang blocks search "http"
```

### List All Blocks

```bash
./naab-lang blocks list
```

Output example:
```
Total blocks indexed: 585

Breakdown by language:
  Python:     585 blocks
  JavaScript: 4 blocks
  C++:        5 blocks
  Ruby:       3 blocks
  Rust:       2 blocks
  Go:         2 blocks
```

### Get Block Info

```bash
./naab-lang blocks info BLOCK-PY-00001
```

---

## 💻 Using Blocks in Your Code

### Basic Usage

```naab
main {
    # Import a block by its ID
    use BLOCK-PY-09145 as validate_email

    # Use it like a function
    let email = "alice@example.com"
    let is_valid = validate_email(email)

    if is_valid {
        print("Valid email!")
    }
}
```

### Chaining Blocks

```naab
use BLOCK-PY-10234 as parse_json
use BLOCK-JS-05678 as transform
use BLOCK-PY-11234 as to_csv

main {
    let json_str = '{"name": "Alice", "age": 30}'

    # Chain blocks together
    let parsed = parse_json(json_str)
    let transformed = transform(parsed)
    let csv = to_csv(transformed)

    print(csv)
}
```

### Validate Block Compatibility

```bash
# Check if blocks can be chained
./naab-lang validate "BLOCK-PY-10234,BLOCK-JS-05678"
```

---

## 📖 Complete Documentation

For detailed tutorials and examples, see:

1. **BLOCK_ASSEMBLY_COMMANDS.md** - Complete CLI command reference
2. **TUTORIAL_BLOCK_ASSEMBLY.naab** - Interactive tutorial (run with `./naab-lang run`)
3. **README_TUTORIALS.md** - Guide to all tutorial files

---

## 🔧 Block System Architecture

### How It Works

1. **Block Files**: JSON metadata files in `~/.naab/language/blocks/library/<language>/`
   - Example: `BLOCK-PY-00001.json`
   - Contains: code, id, language, description, validation status

2. **BlockRegistry**: C++ implementation (`block_registry.h`, `block_registry.cpp`)
   - Lazy loading for fast startup
   - Filesystem-based lookup

3. **BlockSearchIndex**: SQLite FTS5 search (`block_search_index.cpp`)
   - Full-text search across block metadata
   - Performance metrics and rankings
   - <100ms search latency

4. **CLI Integration**: Built into `naab-lang` executable
   - `blocks list` - Show statistics
   - `blocks search` - Find blocks
   - `blocks index` - Build/rebuild index
   - `blocks info` - Get block details
   - `validate` - Check composition

### Block JSON Format

```json
{
  "id": "BLOCK-PY-00001",
  "language": "python",
  "code": "def my_function(): ...",
  "source_file": "/path/to/source.py",
  "source_line": 31,
  "validation_status": "validated"
}
```

---

## ✅ Next Steps

1. **Run the index command** (if you haven't already):
   ```bash
   cd ~/.naab/language/build
   ./naab-lang blocks index ~/.naab/language/blocks/library
   ```

2. **Test searching**:
   ```bash
   ./naab-lang blocks search "validate"
   ./naab-lang blocks list
   ```

3. **Try the tutorial**:
   ```bash
   ./naab-lang run ../TUTORIAL_BLOCK_ASSEMBLY.naab
   ```

4. **Write your first program** using blocks!

---

## ✅ Runtime Behavior & Technical Notes

### Inline Polyglot Code Behavior

**Python Inline Blocks:**
- **Status**: ✅ FIXED (2026-01-20)
- **Behavior**: Automatically handles both expressions and multi-line statements
- **How it works**: Tries `eval()` first for expressions, falls back to `exec()` for multi-line code
- **Example - Single expression**: `let result = <<python sum([1,2,3]) >>`
- **Example - Multi-line statements**:
  ```naab
  <<python
  import statistics
  data = [1, 2, 3, 4, 5]
  print(f"Mean: {statistics.mean(data)}")
  >>
  ```
- **Note**: Multi-line blocks with `exec()` return `void`, expression blocks return values

**JavaScript Inline Blocks:**
- **Status**: ✅ FIXED (2026-01-20)
- **Behavior**: Each block runs in isolated scope (IIFE wrapper)
- **How it works**: Code is automatically wrapped in `(function() { ... })()`
- **Example - No more redeclaration errors**:
  ```naab
  <<javascript
  const data = [1, 2, 3];  // Block 1
  >>
  <<javascript
  const data = [4, 5, 6];  // Block 2 - NO ERROR!
  >>
  ```
- **Note**: Variables are scoped locally; global objects like `console` are still accessible

**C++ Inline Blocks:**
- **Status**: ✅ FIXED (2026-01-20)
- **Behavior**: Auto-generated wrapper includes all common STL headers
- **How it works**: Automatically injects 12 STL headers (iostream, vector, algorithm, string, etc.)
- **Example - Now works without manual headers**:
  ```naab
  <<cpp
  std::cout << "Hello!" << std::endl;  // ✓ Works automatically!
  std::vector<int> v = {1, 2, 3};
  std::sort(v.begin(), v.end());
  >>
  ```
- **Headers included**: iostream, vector, algorithm, string, map, unordered_map, set, unordered_set, memory, utility, cmath, cstdlib

**Bash/Shell Inline Blocks:**
- **Status**: ✅ Works perfectly
- **No issues**: Most reliable for inline system operations

**Ruby, Go, Rust, C# Inline Blocks:**
- **Status**: ✅ Generally work well
- **Minor**: Subprocess execution adds ~50-100ms overhead vs native

### Performance Characteristics

**Native C++ Stdlib vs Polyglot:**
- **Native** (array.sort, string.trim, math.sqrt): 10-100x faster
- **Polyglot** (<<python>>, <<javascript>>): Slower but ecosystem access
- **Recommendation**: Use native stdlib when possible, polyglot when you need specific libraries

**Block Loading:**
- **First Use**: Blocks compile/load on first execution (~50-200ms for C++, ~10ms for Python)
- **Cached**: Subsequent calls are much faster (C++ blocks are cached as .so files)
- **Lazy Registry**: Startup is fast (~10ms) because blocks load on-demand

---

## 🐛 Troubleshooting

### "Total blocks indexed: 0"

**Cause:** Search index not built yet.

**Solution:** Run the index command:
```bash
./naab-lang blocks index ~/.naab/language/blocks/library
```

### "No blocks found matching 'query'"

**Possible causes:**
1. Index not built (run index command)
2. No blocks match your search term (try broader search)
3. Block metadata doesn't contain search term

**Solutions:**
- List all blocks: `./naab-lang blocks list`
- Try different search terms
- Check that blocks exist: `ls ~/.naab/language/blocks/library/python/ | wc -l`

### Block System Files

- Block library: `~/.naab/language/blocks/library/`
- Search index database: `~/.naab/language/blocks/blocks.db` (created after indexing)
- Block registry code: `~/.naab/language/src/runtime/block_registry.cpp`
- Search index code: `~/.naab/language/src/runtime/block_search_index.cpp`

---

**Ready to use blocks! 🎉**
