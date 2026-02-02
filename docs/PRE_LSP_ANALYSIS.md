# Pre-LSP/Testing Framework Analysis & Recommendations

**Date:** 2026-02-02
**Status:** Complete
**Purpose:** Identify TODOs and incomplete implementations before starting LSP Server

---

## Executive Summary

**Found:** 30+ TODOs, 6 major stubs, multiple incomplete implementations

**Recommendation:** **Option B - Quality First (3-4 weeks cleanup)**
- Implement Type Checker (CRITICAL for LSP)
- Fix Parser TODOs (quick wins)
- Replace Symbol Table stub
- Complete String Module

**Then:** Start LSP Server with solid foundation

---

## Critical Issues (Must Fix)

### 1. Type Checker - STUB 🔴
**Impact:** LSP needs this for autocomplete, diagnostics, type hints
**Files:** `src/semantic/type_checker.cpp`, `src/parser/ast_nodes.cpp`
**Status:** Largely stubbed out
**Effort:** 1-2 weeks
**Priority:** CRITICAL

### 2. Parser TODOs 🔴  
**Issues:**
- FROM token missing (using IDENTIFIER)
- Default export not implemented
- Async function syntax exists but not supported
**Effort:** 2-3 days
**Priority:** CRITICAL

### 3. Symbol Table - STUB 🟠
**Impact:** Needed for go-to-definition, rename refactoring
**File:** `src/semantic/symbol_table.cpp`
**Effort:** 3-5 days
**Priority:** IMPORTANT

### 4. String Module - STUB 🟠
**Impact:** Users expect string functions to work
**File:** `src/stdlib/string_impl_stub.cpp`
**Effort:** 2-3 days
**Priority:** IMPORTANT

---

## Medium Priority

- Debugger improvements (conditional breakpoints, better location tracking)
- C++ executor fragment detection
- Rust error handling
- Subprocess environment variable leak

---

## Low Priority (Can Defer)

- REST API execute endpoint
- REPL autocomplete improvements
- Linter placeholder metrics

---

## Three Options

### Option A: Minimal (2-3 weeks)
- Type Checker only
- Basic LSP functionality
- Come back to stubs later

### Option B: Recommended (3-4 weeks) ⭐
- Type Checker + Parser fixes
- Symbol Table + String Module
- Full-featured LSP ready
- Production quality

### Option C: Complete (5-6 weeks)
- Everything from Option B
- All stubs removed
- Debugger + linter improvements
- Fully polished

---

## Recommendation

**Do Option B: 3-4 weeks of cleanup, then LSP**

**Week 1-2:** Type Checker
**Week 3:** Parser + Symbol Table  
**Week 4:** String Module + Testing
**Then:** LSP Server (4 weeks)

**Total to LSP:** 7-8 weeks (quality first)
**Alternative:** 2-3 weeks (minimal, limited LSP features)

---

## Decision Needed

**What's your priority?**
- A) Fast to LSP (2-3 weeks minimal)
- B) Quality first (3-4 weeks recommended) ⭐
- C) Complete polish (5-6 weeks)

See `~/tmp/todo_analysis.md` for full details.

