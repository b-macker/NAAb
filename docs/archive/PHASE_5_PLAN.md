# Phase 5: Polish & Integration

**Status**: Starting
**Estimated Time**: ~8 hours
**Focus**: Complete stdlib, optimize REPL, enhance UX

## Priorities

### 5a. JSON Library Integration ⏭️
**Time**: ~1.5 hours | **Impact**: High

Integrate nlohmann/json for real JSON support:
- Replace placeholder json.parse() and json.stringify()
- Add JSON schema validation
- Pretty printing support
- Error handling for malformed JSON

**Current**:
```naab
use BLOCK-STDLIB-JSON as json
let data = json.parse('{"key": "value"}')  # Placeholder!
```

**Target**:
```naab
use BLOCK-STDLIB-JSON as json
let data = json.parse('{"name": "NAAb", "version": 1.0}')
print(data["name"])  # "NAAb"
let str = json.stringify(data, 2)  # Pretty print with 2-space indent
```

---

### 5b. HTTP Library Integration ⏭️
**Time**: ~2 hours | **Impact**: High

Integrate libcurl for real HTTP support:
- GET, POST, PUT, DELETE requests
- Headers and query parameters
- Request/response bodies
- Timeout and retry logic
- SSL/TLS support

**Current**:
```naab
use BLOCK-STDLIB-HTTP as http
let resp = http.get("https://api.example.com")  # Placeholder!
```

**Target**:
```naab
use BLOCK-STDLIB-HTTP as http
let resp = http.get("https://api.github.com/users/octocat")
let data = json.parse(resp.body)
print("Name:", data["name"])
print("Bio:", data["bio"])
```

---

### 5c. REPL Performance Optimization ⏭️
**Time**: ~1.5 hours | **Impact**: Medium

Optimize REPL for large sessions:
- Incremental execution (only run new statements)
- Statement deduplication
- Snapshot/restore interpreter state
- Lazy parsing

**Current**: O(n) re-execution every input
**Target**: O(1) incremental execution

---

### 5d. Enhanced Error Messages ⏭️
**Time**: ~1 hour | **Impact**: Medium

Integrate error reporter with parser and type checker:
- Parser errors with source context
- Type errors with suggestions
- Runtime errors with stack traces
- "Did you mean?" suggestions for typos

**Example**:
```
error: Undefined variable 'cout'
  --> test.naab:5:5
  | 5     cout << "Hello"
            ^~~~
  help: Did you mean 'count'?
  help: Or did you mean to use 'print()'?
```

---

### 5e. REPL Readline Support ⏭️
**Time**: ~1 hour | **Impact**: Medium

Add line editing capabilities:
- Arrow key navigation
- Ctrl+A/E for line start/end
- Ctrl+R for history search
- Auto-completion hooks

**Library**: linenoise (lightweight readline alternative)

---

### 5f. Documentation Generator ⏭️
**Time**: ~1.5 hours | **Impact**: Low

Generate API docs from code:
- Extract function signatures
- Parse doc comments
- Generate markdown docs
- Block catalog with search

**Example**:
```naab
# Add two numbers
# @param a First number
# @param b Second number
# @return Sum of a and b
fn add(a, b) {
    return a + b
}
```

Generated docs:
```markdown
## add(a, b)
Add two numbers

**Parameters:**
- `a` - First number
- `b` - Second number

**Returns:** Sum of a and b
```

---

## Phase 5 Timeline

```
Week 1:
  Day 1-2: JSON library integration (5a)
  Day 3-4: HTTP library integration (5b)
  Day 5-6: REPL optimization (5c)
  Day 7: Enhanced errors (5d)

Week 2:
  Day 1-2: Readline support (5e)
  Day 3-4: Documentation generator (5f)
  Day 5: Testing and polish
  Day 6-7: Documentation
```

## Success Criteria

- [ ] JSON parse/stringify with real data
- [ ] HTTP GET/POST with real APIs
- [ ] REPL handles 1000+ statements smoothly
- [ ] Errors show source context and suggestions
- [ ] REPL supports arrow key navigation
- [ ] Auto-generated API documentation

## Dependencies

### External Libraries

1. **nlohmann/json** (header-only)
   ```bash
   git clone https://github.com/nlohmann/json.git external/json
   ```

2. **libcurl** (system library)
   ```bash
   pkg install libcurl
   ```

3. **linenoise** (single-file)
   ```bash
   wget https://raw.githubusercontent.com/antirez/linenoise/master/linenoise.h
   wget https://raw.githubusercontent.com/antirez/linenoise/master/linenoise.c
   ```

## Testing Strategy

### JSON Tests
```naab
# test_json.naab
use BLOCK-STDLIB-JSON as json

let obj = {"name": "NAAb", "version": 1, "active": true}
let str = json.stringify(obj)
let parsed = json.parse(str)
assert(parsed["name"] == "NAAb")
```

### HTTP Tests
```naab
# test_http.naab
use BLOCK-STDLIB-HTTP as http
use BLOCK-STDLIB-JSON as json

let resp = http.get("https://httpbin.org/get")
print("Status:", resp.status)
let data = json.parse(resp.body)
print("Origin:", data["origin"])
```

### REPL Performance Test
```bash
# Generate 1000 statements
for i in {1..1000}; do
  echo "let x$i = $i"
done | naab-repl
# Should complete in <1 second
```

## Deliverables

1. **Code**
   - `src/stdlib/json.cpp` (real implementation)
   - `src/stdlib/http.cpp` (real implementation)
   - `src/repl/repl_optimizer.cpp` (incremental execution)
   - `src/repl/linenoise_adapter.cpp` (readline support)

2. **Tests**
   - `examples/test_json.naab`
   - `examples/test_http.naab`
   - `examples/test_repl_perf.naab`

3. **Documentation**
   - `PHASE_5_JSON.md`
   - `PHASE_5_HTTP.md`
   - `PHASE_5_REPL_OPT.md`
   - `PHASE_5_COMPLETE.md`

## Risk Mitigation

### Risks

1. **libcurl Complexity**: HTTP is complex (redirects, auth, etc.)
   - Mitigation: Start with basic GET/POST, expand gradually

2. **REPL Optimization**: State management is tricky
   - Mitigation: Incremental approach, keep fallback to full re-execution

3. **Readline Integration**: Terminal handling varies
   - Mitigation: Use linenoise (portable, simple)

### Fallback Plans

- JSON: Keep placeholder if nlohmann/json doesn't compile
- HTTP: Basic socket implementation if libcurl unavailable
- REPL: Keep current approach if optimization causes bugs

## Metrics

Track improvement:
- JSON parse speed: >1000 objects/sec
- HTTP request latency: <500ms for typical API call
- REPL responsiveness: <100ms per statement
- Error message quality: User satisfaction survey

---

**Phase 5 starts now!**
