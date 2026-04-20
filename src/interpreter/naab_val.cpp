// NaabVal — NaN-boxed value implementation
// See include/naab/naab_val.h for the class definition

#include "naab/naab_val.h"
#include "naab/interpreter.h"
#include "naab/vm.h"
#include <mutex>
#include <stdexcept>
#include <vector>

namespace naab {
namespace interpreter {

// ============================================================================
// Handle table: maps 32-bit handles to ValueBox pointers.
// ARM64 Android uses tagged pointers (TBI/MTE) — raw pointer bits don't fit
// in a 48-bit NaN payload without losing the tag byte. The handle table avoids
// storing raw pointers entirely; we store a small integer handle instead.
//
// Thread safety: each thread gets its own handle range via ThreadAllocator.
// Freed handles are recycled only within the same thread — no cross-thread
// reuse eliminates TOCTOU races in release(). The global page table is shared
// for reads (resolveHandle is lock-free).
// ============================================================================

namespace {
    // Fixed-size page table: avoids reallocation entirely.
    // 64K pages of 64K entries each = 4G max handles (far more than needed).
    // Each page is allocated on first use. resolveHandle is lock-free.
    static constexpr uint32_t PAGE_BITS = 16;
    static constexpr uint32_t PAGE_SIZE = 1U << PAGE_BITS;          // 65536 entries
    static constexpr uint32_t PAGE_MASK = PAGE_SIZE - 1;
    static constexpr uint32_t MAX_PAGES = 256;                      // 256 * 64K = 16M handles

    static std::atomic<ValueBox*>* g_pages[MAX_PAGES] = {};          // global page table (shared for reads)

    // Thread-local handle allocator: each thread gets its own 64K-handle range.
    // Freed handles are recycled only within the same thread.
    static constexpr uint32_t RANGE_SIZE = 65536;                    // handles per thread range
    static std::atomic<uint32_t> g_next_range{0};                   // global range counter
    static std::mutex g_page_mutex;                                  // protects ensurePage only

    struct ThreadAllocator {
        uint32_t range_start;
        uint32_t range_end;
        uint32_t next_handle;
        std::vector<uint32_t> free_handles;
    };

    static thread_local ThreadAllocator* tl_allocator = nullptr;

    static ThreadAllocator* getOrCreateAllocator() {
        if (!tl_allocator) {
            tl_allocator = new ThreadAllocator();
            uint32_t range_id = g_next_range.fetch_add(1, std::memory_order_relaxed);
            tl_allocator->range_start = range_id * RANGE_SIZE;
            tl_allocator->range_end = tl_allocator->range_start + RANGE_SIZE;
            tl_allocator->next_handle = tl_allocator->range_start;
        }
        return tl_allocator;
    }

    // Ensure the page for a given handle exists (thread-safe)
    static void ensurePage(uint32_t page_idx) {
        if (!g_pages[page_idx]) {
            std::lock_guard<std::mutex> lock(g_page_mutex);
            if (!g_pages[page_idx]) {  // double-check after acquiring lock
                g_pages[page_idx] = new std::atomic<ValueBox*>[PAGE_SIZE]();
            }
        }
    }

    uint32_t allocHandle(ValueBox* box) {
        auto* alloc = getOrCreateAllocator();
        uint32_t h;
        if (!alloc->free_handles.empty()) {
            h = alloc->free_handles.back();
            alloc->free_handles.pop_back();
        } else {
            h = alloc->next_handle++;
            if (h >= alloc->range_end) {
                // Exhausted this range, get a new one
                uint32_t range_id = g_next_range.fetch_add(1, std::memory_order_relaxed);
                alloc->range_start = range_id * RANGE_SIZE;
                alloc->range_end = alloc->range_start + RANGE_SIZE;
                alloc->next_handle = alloc->range_start + 1;
                h = alloc->range_start;
            }
            ensurePage(h >> PAGE_BITS);
        }
        g_pages[h >> PAGE_BITS][h & PAGE_MASK].store(box, std::memory_order_release);
        return h;
    }

    void freeHandle(uint32_t h) {
        g_pages[h >> PAGE_BITS][h & PAGE_MASK].store(nullptr, std::memory_order_release);
        auto* alloc = getOrCreateAllocator();
        alloc->free_handles.push_back(h);
    }

    inline ValueBox* resolveHandle(uint32_t h) {
        // Thread-safe: atomic load with acquire ordering ensures visibility
        // of writes from allocHandle/freeHandle's release stores.
        return g_pages[h >> PAGE_BITS][h & PAGE_MASK].load(std::memory_order_acquire);
    }
} // anonymous namespace

// ============================================================================
// ValueBox: heap-allocated container for complex values
// ============================================================================

struct ValueBox {
    std::atomic<int> refcount{1};
    // Store the original shared_ptr<Value> to preserve object identity.
    // This ensures toLegacy() returns the SAME shared_ptr, so mutations
    // (e.g., arr[0] = 100) affect the original object in the environment.
    std::shared_ptr<Value> shared_value;

    ValueBox() : shared_value(std::make_shared<Value>()) {}
    explicit ValueBox(std::shared_ptr<Value> v) : shared_value(std::move(v)) {}
};

// ============================================================================
// NaabVal — reference counting (called from inline destructor/copy)
// ============================================================================

// Implicit conversion constructor from shared_ptr<Value>
NaabVal::NaabVal(const std::shared_ptr<Value>& v) {
    NaabVal converted = fromLegacy(v);
    bits_ = converted.bits_;
    // Transfer ownership from converted to this (no retain needed —
    // fromLegacy created the box with refcount=1, we steal that reference)
    converted.bits_ = TAG_NULL;
}

void NaabVal::retain() {
    asHeap()->refcount.fetch_add(1, std::memory_order_relaxed);
}

void NaabVal::release() {
    uint32_t handle = static_cast<uint32_t>(bits_ & 0xFFFFFFFFULL);
    ValueBox* box = resolveHandle(handle);
    if (!box) return;  // Already freed by another thread — safe to skip
    if (box->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        freeHandle(handle);
        delete box;
    }
}

void NaabVal::enterAsyncVM() {
    // No-op: thread-local allocators eliminate cross-thread handle reuse
}

void NaabVal::exitAsyncVM() {
    // No-op: thread-local allocators eliminate cross-thread handle reuse
}

void NaabVal::flushDeferredFrees() {
    // No-op: thread-local allocators handle recycling per-thread
}

// ============================================================================
// Heap type factories
// ============================================================================

NaabVal NaabVal::makeHeap(ValueBox* box) {
    NaabVal v;
    uint32_t handle = allocHandle(box);
    v.bits_ = TAG_HEAP | static_cast<uint64_t>(handle);
    return v;
}

ValueBox* NaabVal::asHeap() const {
    uint32_t handle = static_cast<uint32_t>(bits_ & 0xFFFFFFFFULL);
    ValueBox* box = resolveHandle(handle);
    if (!box) {
        throw std::runtime_error("Dangling handle: heap object was freed");
    }
    return box;
}

NaabVal NaabVal::makeString(std::string s) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(s))));
}

NaabVal NaabVal::makeString(const char* s) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::string(s))));
}

NaabVal NaabVal::makeList(std::vector<NaabVal> v) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(v))));
}

NaabVal NaabVal::makeDict(std::unordered_map<std::string, NaabVal> v) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(v))));
}

NaabVal NaabVal::makeFunction(std::shared_ptr<FunctionValue> f) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(f))));
}

NaabVal NaabVal::makeStruct(std::shared_ptr<StructValue> s) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(s))));
}

NaabVal NaabVal::makeBlock(std::shared_ptr<BlockValue> b) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(b))));
}

NaabVal NaabVal::makeFuture(std::shared_ptr<FutureValue> f) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(f))));
}

NaabVal NaabVal::makeGenerator(std::shared_ptr<GeneratorValue> g) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(g))));
}

NaabVal NaabVal::makePythonObject(std::shared_ptr<PythonObjectValue> p) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(p))));
}

NaabVal NaabVal::makeVMClosure(std::shared_ptr<vm::VMClosure> c) {
    return makeHeap(new ValueBox(std::make_shared<Value>(std::move(c))));
}

// ============================================================================
// High-level type checks (dispatch on ValueBox's Value variant)
// ============================================================================

// Helper: get variant index from ValueBox
static size_t heapIndex(const NaabVal& v) {
    return v.asHeap()->shared_value->data.index();
}

bool NaabVal::isString()       const { return isHeap() && heapIndex(*this) == 4; }
bool NaabVal::isList()         const { return isHeap() && heapIndex(*this) == 5; }
bool NaabVal::isDict()         const { return isHeap() && heapIndex(*this) == 6; }
bool NaabVal::isBlock()        const { return isHeap() && heapIndex(*this) == 7; }
bool NaabVal::isFunction()     const { return isHeap() && heapIndex(*this) == 8; }
bool NaabVal::isPythonObject() const { return isHeap() && heapIndex(*this) == 9; }
bool NaabVal::isStructVal()    const { return isHeap() && heapIndex(*this) == 10; }
bool NaabVal::isFuture()       const { return isHeap() && heapIndex(*this) == 11; }
bool NaabVal::isGenerator()    const { return isHeap() && heapIndex(*this) == 12; }
bool NaabVal::isVMClosure()    const { return isHeap() && heapIndex(*this) == 13; }

// ============================================================================
// Heap type extraction
// ============================================================================

const std::string& NaabVal::asString() const {
    return std::get<std::string>(asHeap()->shared_value->data);
}

std::string& NaabVal::asStringMut() {
    return std::get<std::string>(asHeap()->shared_value->data);
}

std::vector<NaabVal>& NaabVal::asList() {
    return std::get<std::vector<NaabVal>>(asHeap()->shared_value->data);
}

const std::vector<NaabVal>& NaabVal::asListConst() const {
    return std::get<std::vector<NaabVal>>(asHeap()->shared_value->data);
}

std::unordered_map<std::string, NaabVal>& NaabVal::asDict() {
    return std::get<std::unordered_map<std::string, NaabVal>>(asHeap()->shared_value->data);
}

const std::unordered_map<std::string, NaabVal>& NaabVal::asDictConst() const {
    return std::get<std::unordered_map<std::string, NaabVal>>(asHeap()->shared_value->data);
}

std::shared_ptr<FunctionValue>& NaabVal::asFunction() {
    return std::get<std::shared_ptr<FunctionValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<FunctionValue>& NaabVal::asFunctionConst() const {
    return std::get<std::shared_ptr<FunctionValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<StructValue>& NaabVal::asStruct() {
    return std::get<std::shared_ptr<StructValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<StructValue>& NaabVal::asStructConst() const {
    return std::get<std::shared_ptr<StructValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<BlockValue>& NaabVal::asBlock() {
    return std::get<std::shared_ptr<BlockValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<BlockValue>& NaabVal::asBlockConst() const {
    return std::get<std::shared_ptr<BlockValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<FutureValue>& NaabVal::asFuture() {
    return std::get<std::shared_ptr<FutureValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<FutureValue>& NaabVal::asFutureConst() const {
    return std::get<std::shared_ptr<FutureValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<GeneratorValue>& NaabVal::asGenerator() {
    return std::get<std::shared_ptr<GeneratorValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<GeneratorValue>& NaabVal::asGeneratorConst() const {
    return std::get<std::shared_ptr<GeneratorValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<PythonObjectValue>& NaabVal::asPythonObject() {
    return std::get<std::shared_ptr<PythonObjectValue>>(asHeap()->shared_value->data);
}

const std::shared_ptr<PythonObjectValue>& NaabVal::asPythonObjectConst() const {
    return std::get<std::shared_ptr<PythonObjectValue>>(asHeap()->shared_value->data);
}

std::shared_ptr<vm::VMClosure>& NaabVal::asVMClosure() {
    return std::get<std::shared_ptr<vm::VMClosure>>(asHeap()->shared_value->data);
}

const std::shared_ptr<vm::VMClosure>& NaabVal::asVMClosureConst() const {
    return std::get<std::shared_ptr<vm::VMClosure>>(asHeap()->shared_value->data);
}

// ============================================================================
// V-CONC-006: Deep copy for async thread isolation
// ============================================================================

NaabVal NaabVal::deepCopy(int depth, std::unordered_set<const void*>* visited) const {
    if (depth > 64) {
        throw std::runtime_error(
            "Async error: nested structure exceeds maximum depth (64)\n\n"
            "  The value being copied for async execution is nested more than 64 levels deep.\n\n"
            "  Help:\n"
            "  - Flatten the structure before passing it to an async block\n"
            "  - Extract only the data you need at a shallow depth\n"
        );
    }
    if (isNull() || isBool() || isInt() || isDouble()) {
        return *this;  // Inline scalars — no heap allocation, safe to share
    }
    if (isString()) {
        // Strings are immutable but heap-allocated via handle table.
        // Must create a new handle to avoid cross-thread refcount races.
        return makeString(asString());
    }

    // V-CONC-006: Cycle detection via visited pointer set
    std::unordered_set<const void*> local_visited;
    if (!visited) visited = &local_visited;
    const void* ptr = static_cast<const void*>(asHeap());
    if (ptr && !visited->insert(ptr).second) {
        throw std::runtime_error(
            "Async error: circular reference detected during value copy\n\n"
            "  A container references itself, which cannot be safely copied for async execution.\n"
            "  Async tasks require independent copies of all captured data.\n\n"
            "  Help:\n"
            "  - Break the cycle before passing data to an async block\n"
            "  - Copy only the fields you need into a flat structure\n\n"
            "  Example:\n"
            "    ✗ Wrong: let a = []; a.push(a); async { use a }\n"
            "    ✓ Right: let a = [1, 2, 3]; async { use a }\n"
        );
    }

    NaabVal copy_result;
    if (isList()) {
        std::vector<NaabVal> new_list;
        new_list.reserve(asListConst().size());
        for (const auto& item : asListConst()) {
            new_list.push_back(item.deepCopy(depth + 1, visited));
        }
        copy_result = makeList(std::move(new_list));
    } else if (isDict()) {
        std::unordered_map<std::string, NaabVal> new_dict;
        for (const auto& [k, v] : asDictConst()) {
            new_dict[k] = v.deepCopy(depth + 1, visited);
        }
        copy_result = makeDict(std::move(new_dict));
    } else if (isStructVal()) {
        auto& sv = asStructConst();
        if (sv && sv->definition) {
            auto new_sv = std::make_shared<StructValue>();
            new_sv->definition = sv->definition;  // Share definition (immutable)
            new_sv->field_values.reserve(sv->field_values.size());
            for (const auto& fv : sv->field_values) {
                new_sv->field_values.push_back(fv.deepCopy(depth + 1, visited));
            }
            copy_result = fromLegacy(std::make_shared<Value>(new_sv));
        } else {
            copy_result = *this;
        }
    } else {
        // Functions, closures, futures, etc. — create a new handle wrapping the
        // same underlying Value. Gives the copy independent refcounting so async
        // cleanup won't affect the original's handle.
        auto* box = asHeap();
        if (box && box->shared_value) {
            copy_result = fromLegacy(box->shared_value);
        } else {
            copy_result = *this;
        }
    }

    if (ptr) visited->erase(ptr);  // Allow same object in different branches
    return copy_result;
}

// ============================================================================
// Conversion methods
// ============================================================================

std::string NaabVal::toString() const {
    if (isNull()) return "null";
    if (isBool()) return asBool() ? "true" : "false";
    if (isInt()) return std::to_string(asInt());
    if (isDouble()) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%.15g", asDouble());
        return std::string(buf);
    }
    // Heap: delegate to Value::toString()
    if (isHeap()) return asHeap()->shared_value->toString();
    return "unknown";
}

bool NaabVal::toBool() const {
    if (isNull()) return false;
    if (isBool()) return asBool();
    if (isInt()) return asInt() != 0;
    if (isDouble()) return asDouble() != 0.0;
    if (isHeap()) return asHeap()->shared_value->toBool();
    return false;
}

int NaabVal::toInt() const {
    if (isInt()) return asInt();
    if (isDouble()) return static_cast<int>(asDouble());
    if (isBool()) return asBool() ? 1 : 0;
    if (isHeap()) return asHeap()->shared_value->toInt();
    return 0;
}

double NaabVal::toFloat() const {
    if (isDouble()) return asDouble();
    if (isInt()) return static_cast<double>(asInt());
    if (isBool()) return asBool() ? 1.0 : 0.0;
    if (isHeap()) return asHeap()->shared_value->toFloat();
    return 0.0;
}

std::string NaabVal::getTypeName() const {
    if (isNull()) return "null";
    if (isBool()) return "bool";
    if (isInt()) return "int";
    if (isDouble()) return "float";
    if (!isHeap()) return "unknown";

    // Dispatch based on variant index
    switch (asHeap()->shared_value->data.index()) {
        case 0: return "null";    // monostate
        case 1: return "int";
        case 2: return "float";
        case 3: return "bool";
        case 4: return "string";
        case 5: return "array";
        case 6: return "dict";
        case 7: return "block";
        case 8: return "function";
        case 9: return "python_object";
        case 10: return "struct";
        case 11: return "future";
        case 12: return "generator";
        default: return "unknown";
    }
}

// ============================================================================
// Legacy conversion bridge
// ============================================================================

NaabVal NaabVal::fromLegacy(const std::shared_ptr<Value>& v) {
    if (!v) return makeNull();

    // Dispatch based on variant index for optimal conversion
    switch (v->data.index()) {
        case 0: // monostate (null)
            return makeNull();
        case 1: // int
            return makeInt(std::get<int>(v->data));
        case 2: // double
            return makeDouble(std::get<double>(v->data));
        case 3: // bool
            return makeBool(std::get<bool>(v->data));
        default: {
            // Complex types: store the ORIGINAL shared_ptr (preserves identity).
            // This ensures toLegacy() returns the same pointer, so mutations
            // to arrays/dicts/structs affect the original object.
            // Note: list/dict containers already store NaabVal elements (Phase E),
            // so no recursive conversion needed — just wrap the Value.
            return makeHeap(new ValueBox(v));
        }
    }
}

std::shared_ptr<Value> NaabVal::toLegacy() const {
    if (isNull()) return std::make_shared<Value>();
    if (isBool()) return std::make_shared<Value>(asBool());
    if (isInt()) return std::make_shared<Value>(asInt());
    if (isDouble()) return std::make_shared<Value>(asDouble());
    if (isHeap()) {
        // Return the ORIGINAL shared_ptr — preserves object identity
        return asHeap()->shared_value;
    }
    return std::make_shared<Value>();
}

// ============================================================================
// GC support
// ============================================================================

void NaabVal::traverse(std::function<void(const NaabVal&)> visitor) const {
    if (isHeap()) {
        asHeap()->shared_value->traverse(visitor);
    }
}

int NaabVal::getHeapRefCount() const {
    if (!isHeap()) return 0;
    return asHeap()->refcount.load(std::memory_order_relaxed);
}

void NaabVal::forEachHeapValue(std::function<void(NaabVal)> callback) {
    // Iterate all live entries across all thread-local ranges.
    // g_next_range * RANGE_SIZE is the high-water mark.
    uint32_t max_range = g_next_range.load(std::memory_order_relaxed);
    uint32_t max_handle = max_range * RANGE_SIZE;
    for (uint32_t h = 0; h < max_handle; ++h) {
        uint32_t page_idx = h >> PAGE_BITS;
        if (!g_pages[page_idx]) continue;  // Page not allocated
        ValueBox* box = g_pages[page_idx][h & PAGE_MASK].load(std::memory_order_acquire);
        if (!box) continue;  // Freed handle
        // Create a NaabVal from this handle (bumps refcount temporarily)
        NaabVal val;
        val.bits_ = TAG_HEAP | static_cast<uint64_t>(h);
        box->refcount.fetch_add(1, std::memory_order_relaxed);  // retain
        callback(val);
        // val's destructor calls release(), callback's copy (by value) also
        // calls release() — net effect: the two releases balance the one
        // manual retain + one copy-constructor retain.
    }
}

} // namespace interpreter
} // namespace naab
