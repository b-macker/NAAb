# Phase 4.6: Build System (naab-build) - Design Document

## Executive Summary

**Status:** DESIGN DOCUMENT | IMPLEMENTATION NOT STARTED
**Complexity:** MEDIUM - Multi-file compilation and dependency tracking
**Estimated Effort:** 3-4 weeks implementation
**Priority:** HIGH - Essential for multi-file projects

This document outlines the design for `naab-build`, NAAb's build system for compiling multi-file projects with incremental compilation and dependency tracking.

---

## Current Problem

**No Build System:**
- Can only run single-file programs: `naab main.naab`
- Cannot split code across multiple files
- No project structure or organization
- No incremental compilation (recompile everything every time)
- No optimization levels

**Impact:** Cannot build real applications, poor development experience for large projects.

---

## Goals

### Primary Goals

1. **Multi-File Projects** - Compile projects with multiple .naab files
2. **Module System** - Import code from other files
3. **Incremental Compilation** - Only recompile changed files
4. **Dependency Tracking** - Rebuild when dependencies change
5. **Build Configuration** - naab.build.json for project settings

### Secondary Goals

6. **Optimization Levels** - Debug (-O0) vs Release (-O2)
7. **Output Binary** - Compile to executable (future: bytecode)
8. **Build Cache** - Fast rebuilds

---

## Build Configuration (naab.build.json)

### Format

```json
{
  "name": "my-app",
  "version": "1.0.0",
  "entry": "src/main.naab",
  "output": "bin/my-app",
  "sourceDir": "src",
  "outputDir": "bin",

  "dependencies": {
    "http": "^2.0.0",
    "json": "^1.5.0"
  },

  "optimization": "release",
  "target": "executable",

  "include": [
    "src/**/*.naab"
  ],

  "exclude": [
    "src/test/**"
  ]
}
```

### Fields

- **name**: Project name
- **version**: Project version (semver)
- **entry**: Main entry point file
- **output**: Output path for compiled binary/bytecode
- **sourceDir**: Source code directory (default: `src`)
- **outputDir**: Build output directory (default: `bin`)
- **dependencies**: Package dependencies (from naab-pkg)
- **optimization**: `debug` (fast compile, debug info) or `release` (optimized)
- **target**: `executable` (native), `bytecode` (VM), `library` (future)
- **include**: Glob patterns for source files
- **exclude**: Glob patterns to exclude

---

## Architecture

### Components

```
naab-build
├── BuildConfig           # naab.build.json parser
├── DependencyGraph       # File dependency tracking
├── Compiler              # Orchestrates compilation
├── IncrementalCache      # Track what's changed
└── ModuleResolver        # Resolve imports
```

### Build Process Flow

```
1. Load naab.build.json
2. Discover all source files (src/**/*.naab)
3. Build dependency graph (who imports whom)
4. Check cache (which files changed since last build)
5. Topological sort (compile in dependency order)
6. Compile changed files + dependents
7. Link modules
8. Generate output binary/bytecode
```

---

## Module System

### Import Syntax

```naab
// Import from same project
import "./utils"                  // src/utils.naab
import "./lib/http_client"        // src/lib/http_client.naab
import "../shared/common"         // ../shared/common.naab

// Import from package
import "http"                     // naab_modules/http/
import "json"                     // naab_modules/json/

// Import from standard library
import "std/file"                 // Built-in
import "std/math"                 // Built-in
```

### Module Resolution

```cpp
class ModuleResolver {
public:
    std::string resolve(const std::string& import_path, const std::string& from_file) {
        // 1. Standard library
        if (import_path.starts_with("std/")) {
            return resolveStdLib(import_path);
        }

        // 2. Package (naab_modules)
        if (!import_path.starts_with("./") && !import_path.starts_with("../")) {
            return resolvePackage(import_path);
        }

        // 3. Relative path
        return resolveRelative(import_path, from_file);
    }

private:
    std::string resolveStdLib(const std::string& path) {
        // Built-in modules don't have file paths
        return "<stdlib:" + path + ">";
    }

    std::string resolvePackage(const std::string& name) {
        // Check naab_modules/
        std::string pkg_dir = "naab_modules/" + name;
        if (fs::exists(pkg_dir)) {
            // Load package manifest to get main file
            auto manifest = loadManifest(pkg_dir + "/naab.json");
            return pkg_dir + "/" + manifest["main"].get<std::string>();
        }

        throw ModuleNotFoundError("Package not found: " + name);
    }

    std::string resolveRelative(const std::string& path, const std::string& from_file) {
        fs::path from_dir = fs::path(from_file).parent_path();
        fs::path resolved = from_dir / path;

        // Try with .naab extension
        if (fs::exists(resolved.string() + ".naab")) {
            return fs::canonical(resolved.string() + ".naab");
        }

        // Try as directory with index.naab
        if (fs::exists(resolved / "index.naab")) {
            return fs::canonical(resolved / "index.naab");
        }

        throw ModuleNotFoundError("Module not found: " + path);
    }
};
```

### Module Exports

**Implicit Exports (Everything is public by default):**

```naab
// utils.naab
function add(a: int, b: int) -> int {
    return a + b
}

struct Point {
    x: int
    y: int
}
```

**Importing:**

```naab
// main.naab
import "./utils"

let result = utils.add(1, 2)
let point = utils.Point { x: 10, y: 20 }
```

---

## Dependency Graph

### Building the Graph

```cpp
struct ModuleNode {
    std::string path;
    std::set<std::string> imports;      // Modules this file imports
    std::set<std::string> imported_by;  // Modules that import this file
    time_t last_modified;
};

class DependencyGraph {
public:
    void addFile(const std::string& path) {
        ModuleNode node;
        node.path = path;
        node.last_modified = fs::last_write_time(path);

        // Parse file to find imports
        Parser parser(path);
        auto program = parser.parse();
        for (const auto& import : program->getImports()) {
            std::string resolved = resolver_.resolve(import, path);
            node.imports.insert(resolved);
        }

        nodes_[path] = node;
    }

    std::vector<std::string> getCompilationOrder() {
        // Topological sort (dependency order)
        std::vector<std::string> order;
        std::set<std::string> visited;

        for (const auto& [path, node] : nodes_) {
            if (!visited.count(path)) {
                visitDFS(path, visited, order);
            }
        }

        return order;
    }

    std::set<std::string> getFilesToRecompile(const std::set<std::string>& changed_files) {
        std::set<std::string> to_recompile = changed_files;

        // Add all files that depend on changed files (transitively)
        for (const auto& changed : changed_files) {
            addDependents(changed, to_recompile);
        }

        return to_recompile;
    }

private:
    std::map<std::string, ModuleNode> nodes_;
    ModuleResolver resolver_;

    void visitDFS(const std::string& path, std::set<std::string>& visited, std::vector<std::string>& order) {
        visited.insert(path);

        // Visit dependencies first
        for (const auto& dep : nodes_[path].imports) {
            if (!visited.count(dep)) {
                visitDFS(dep, visited, order);
            }
        }

        // Then this file
        order.push_back(path);
    }

    void addDependents(const std::string& path, std::set<std::string>& result) {
        for (const auto& [other_path, node] : nodes_) {
            if (node.imports.count(path)) {
                if (!result.count(other_path)) {
                    result.insert(other_path);
                    addDependents(other_path, result);  // Recursive
                }
            }
        }
    }
};
```

---

## Incremental Compilation

### Build Cache

**Track what's been compiled:**

```cpp
struct BuildCache {
    struct Entry {
        std::string source_path;
        time_t source_modified;
        std::string output_path;
        time_t output_modified;
        std::set<std::string> dependencies;
    };

    std::map<std::string, Entry> entries;

    void load(const std::string& cache_file) {
        // Load from .naab_cache/build_cache.json
        if (fs::exists(cache_file)) {
            std::ifstream file(cache_file);
            json j;
            file >> j;

            for (const auto& [path, data] : j.items()) {
                Entry entry;
                entry.source_path = path;
                entry.source_modified = data["source_modified"];
                entry.output_path = data["output_path"];
                entry.output_modified = data["output_modified"];
                entry.dependencies = data["dependencies"].get<std::set<std::string>>();
                entries[path] = entry;
            }
        }
    }

    void save(const std::string& cache_file) {
        json j;
        for (const auto& [path, entry] : entries) {
            j[path] = {
                {"source_modified", entry.source_modified},
                {"output_path", entry.output_path},
                {"output_modified", entry.output_modified},
                {"dependencies", entry.dependencies}
            };
        }

        std::ofstream file(cache_file);
        file << j.dump(2);
    }

    bool needsRecompile(const std::string& path) {
        if (!entries.count(path)) {
            return true;  // Never compiled
        }

        auto& entry = entries[path];

        // Check if source changed
        time_t current_modified = fs::last_write_time(path);
        if (current_modified > entry.source_modified) {
            return true;
        }

        // Check if any dependency changed
        for (const auto& dep : entry.dependencies) {
            if (needsRecompile(dep)) {
                return true;
            }
        }

        return false;
    }
};
```

---

## Build System Implementation

### Compiler Class

```cpp
class BuildSystem {
public:
    BuildSystem(const BuildConfig& config);

    void build();
    void clean();
    void rebuild();

private:
    BuildConfig config_;
    DependencyGraph dep_graph_;
    BuildCache cache_;
    ModuleResolver resolver_;

    void discoverSourceFiles();
    void buildDependencyGraph();
    std::set<std::string> getChangedFiles();
    void compileFiles(const std::vector<std::string>& files);
    void linkModules();
};

void BuildSystem::build() {
    std::cout << "Building " << config_.name << "...\n";

    // 1. Discover all source files
    discoverSourceFiles();

    // 2. Build dependency graph
    buildDependencyGraph();

    // 3. Check what changed
    auto changed = getChangedFiles();
    auto to_compile = dep_graph_.getFilesToRecompile(changed);

    std::cout << to_compile.size() << " files to compile\n";

    // 4. Get compilation order
    auto order = dep_graph_.getCompilationOrder();

    // 5. Compile in order (only changed files)
    std::vector<std::string> compile_list;
    for (const auto& file : order) {
        if (to_compile.count(file)) {
            compile_list.push_back(file);
        }
    }

    compileFiles(compile_list);

    // 6. Link (if creating executable)
    if (config_.target == "executable") {
        linkModules();
    }

    // 7. Update cache
    cache_.save(".naab_cache/build_cache.json");

    std::cout << "Build complete: " << config_.output << "\n";
}

void BuildSystem::compileFiles(const std::vector<std::string>& files) {
    for (const auto& file : files) {
        std::cout << "Compiling " << file << "...\n";

        // Parse
        Parser parser(file);
        auto program = parser.parse();

        // Type check (if type checker exists)
        // TypeChecker checker;
        // checker.check(program.get());

        // Generate output (AST pickle, bytecode, or interpret)
        std::string output_path = getOutputPath(file);

        // For now: Just validate (interpreter-based language)
        // Future: Generate bytecode

        // Update cache
        BuildCache::Entry entry;
        entry.source_path = file;
        entry.source_modified = fs::last_write_time(file);
        entry.output_path = output_path;
        entry.output_modified = time(nullptr);
        entry.dependencies = dep_graph_.getNode(file).imports;
        cache_.entries[file] = entry;
    }
}
```

---

## CLI Interface

### Commands

```bash
# Build project
naab-build

# Clean build artifacts
naab-build clean

# Rebuild from scratch
naab-build rebuild

# Build with specific config
naab-build --config custom.build.json

# Build in release mode
naab-build --release

# Build in debug mode
naab-build --debug

# Verbose output
naab-build --verbose

# Run after build
naab-build --run
```

### Example Output

```
$ naab-build
Building my-app...
Discovering source files... 15 files found
Building dependency graph...
3 files changed since last build
Compiling src/utils.naab...
Compiling src/http_client.naab...
Compiling src/main.naab...
Linking modules...
Build complete: bin/my-app

$ naab-build --run
Building my-app...
0 files changed since last build
Build is up to date
Running bin/my-app...
[program output]
```

---

## Optimization Levels

### Debug Mode (-O0)

**Characteristics:**
- Fast compilation
- No optimizations
- Include debug symbols
- Preserve source locations
- Enable assertions

**Use Case:** Development, debugging

### Release Mode (-O2)

**Characteristics:**
- Slower compilation
- Apply optimizations:
  - Constant folding
  - Dead code elimination
  - Inline small functions
- Strip debug symbols
- Disable assertions

**Use Case:** Production deployment

**Implementation:**

```cpp
struct OptimizationConfig {
    bool constant_folding = false;
    bool dead_code_elimination = false;
    bool inline_functions = false;
    bool strip_debug_info = false;
    bool enable_assertions = true;
};

OptimizationConfig getOptimizationConfig(const std::string& level) {
    OptimizationConfig config;

    if (level == "debug") {
        // Defaults (no optimization)
    } else if (level == "release") {
        config.constant_folding = true;
        config.dead_code_elimination = true;
        config.inline_functions = true;
        config.strip_debug_info = true;
        config.enable_assertions = false;
    }

    return config;
}
```

---

## Integration with Package Manager

### Using Dependencies

**naab.build.json includes dependencies:**

```json
{
  "dependencies": {
    "http": "^2.0.0",
    "json": "^1.5.0"
  }
}
```

**Build process:**

```cpp
void BuildSystem::resolveDependencies() {
    // Check if naab_modules exists
    if (!fs::exists("naab_modules")) {
        std::cout << "Installing dependencies...\n";
        system("naab-pkg install");
    }

    // Add naab_modules to module search path
    resolver_.addSearchPath("naab_modules");
}
```

---

## File Watching (Future)

### Auto-rebuild on change

```bash
naab-build watch
```

**Implementation:**

```cpp
void BuildSystem::watch() {
    std::cout << "Watching for changes...\n";

    std::map<std::string, time_t> file_times;
    for (const auto& file : getAllSourceFiles()) {
        file_times[file] = fs::last_write_time(file);
    }

    while (true) {
        sleep(1);  // Poll every second

        bool changed = false;
        for (const auto& file : getAllSourceFiles()) {
            time_t current_time = fs::last_write_time(file);
            if (current_time > file_times[file]) {
                changed = true;
                file_times[file] = current_time;
            }
        }

        if (changed) {
            std::cout << "Change detected, rebuilding...\n";
            build();
        }
    }
}
```

---

## Implementation Plan

### Week 1: Core Build System (5 days)

- [ ] Implement BuildConfig (naab.build.json parsing)
- [ ] Implement ModuleResolver
- [ ] Implement DependencyGraph
- [ ] Test: Multi-file compilation

### Week 2: Incremental Compilation (5 days)

- [ ] Implement BuildCache
- [ ] Track file modifications
- [ ] Recompile only changed files
- [ ] Test: Fast rebuilds

### Week 3: CLI & Integration (5 days)

- [ ] Implement CLI (build, clean, rebuild)
- [ ] Integration with naab-pkg
- [ ] Output formatting
- [ ] Test: End-to-end builds

### Week 4: Optimization & Polish (5 days)

- [ ] Optimization levels (debug/release)
- [ ] Error handling and recovery
- [ ] Documentation
- [ ] Test: Real projects

**Total: 4 weeks**

---

## Testing Strategy

### Test Cases

**Multi-file Project:**
```
project/
├── naab.build.json
├── src/
│   ├── main.naab         (imports ./utils, ./lib/http)
│   ├── utils.naab        (exports functions)
│   └── lib/
│       └── http.naab     (imports "json")
└── naab_modules/
    └── json/
```

**Test:**
1. Build project
2. Verify compilation order (json → http → utils → main)
3. Modify utils.naab
4. Rebuild
5. Verify only utils.naab and main.naab recompiled

---

## Success Metrics

### Phase 4.6 Complete When:

- [x] naab-build CLI working
- [x] Multi-file projects compile
- [x] Module system (import/export) working
- [x] Incremental compilation working
- [x] Dependency tracking correct
- [x] Integration with naab-pkg
- [x] Optimization levels supported
- [x] Documentation complete

---

## Conclusion

**Phase 4.6 Status: DESIGN COMPLETE**

A build system will:
- Enable multi-file projects
- Fast incremental compilation
- Professional project structure

**Implementation Effort:** 4 weeks

**Priority:** HIGH (essential for real projects)

**Dependencies:** Module resolver, package manager

Once implemented, NAAb will have build capabilities for organizing large codebases.
