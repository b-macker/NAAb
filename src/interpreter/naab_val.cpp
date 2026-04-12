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
// ============================================================================

namespace {
    // Fixed-size page table: avoids reallocation entirely.
    // 64K pages of 64K entries each = 4G max handles (far more than needed).
    // Each page is allocated on first use. resolveHandle is lock-free.
    static constexpr uint32_t PAGE_BITS = 16;
    static constexpr uint32_t PAGE_SIZE = 1U << PAGE_BITS;          // 65536 entries
    static constexpr uint32_t PAGE_MASK = PAGE_SIZE - 1;
    static constexpr uint32_t MAX_PAGES = 256;                      // 256 * 64K = 16M handles

    static ValueBox**           g_pages[MAX_PAGES] = {};            // fixed array of page pointers
    static std::atomic<uint32_t> g_next_handle{0};                  // monotonic counter for new handles
    static std::vector<uint32_t> g_free_handles;
    static std::mutex            g_handle_mutex;

    // Ensure the page for a given handle exists
    static void ensurePage(uint32_t page_idx) {
        if (!g_pages[page_idx]) {
            g_pages[page_idx] = new ValueBox*[PAGE_SIZE]();  // zero-initialized
        }
    }

    uint32_t allocHandle(ValueBox* box) {
        std::lock_guard<std::mutex> lock(g_handle_mutex);
        uint32_t h;
        if (!g_free_handles.empty()) {
            h = g_free_handles.back();
            g_free_handles.pop_back();
        } else {
            h = g_next_handle.fetch_add(1, std::memory_order_relaxed);
            uint32_t page_idx = h >> PAGE_BITS;
            ensurePage(page_idx);
        }
        g_pages[h >> PAGE_BITS][h & PAGE_MASK] = box;
        return h;
    }

    void freeHandle(uint32_t h) {
        std::lock_guard<std::mutex> lock(g_handle_mutex);
        g_pages[h >> PAGE_BITS][h & PAGE_MASK] = nullptr;
        g_free_handles.push_back(h);
    }

    inline ValueBox* resolveHandle(uint32_t h) {
        // Lock-free: page array is fixed-size, pages are never freed,
        // and the slot is valid as long as refcount > 0 (caller guarantees this).
        return g_pages[h >> PAGE_BITS][h & PAGE_MASK];
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
    ValueBox* box = asHeap();
    if (box && box->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        uint32_t handle = static_cast<uint32_t>(bits_ & 0xFFFFFFFFULL);
        freeHandle(handle);
        delete box;
    }
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
    return resolveHandle(handle);
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

NaabVal NaabVal::deepCopy(int depth) const {
    if (depth > 64) return *this;  // Prevent stack overflow on cyclic refs
    if (isNull() || isBool() || isInt() || isDouble() || isString()) {
        return *this;  // Scalars are value types or immutable — safe to share
    }
    if (isList()) {
        std::vector<NaabVal> new_list;
        new_list.reserve(asListConst().size());
        for (const auto& item : asListConst()) {
            new_list.push_back(item.deepCopy(depth + 1));
        }
        return makeList(std::move(new_list));
    }
    if (isDict()) {
        std::unordered_map<std::string, NaabVal> new_dict;
        for (const auto& [k, v] : asDictConst()) {
            new_dict[k] = v.deepCopy(depth + 1);
        }
        return makeDict(std::move(new_dict));
    }
    if (isStructVal()) {
        auto& sv = asStructConst();
        if (sv && sv->definition) {
            auto new_sv = std::make_shared<StructValue>();
            new_sv->definition = sv->definition;  // Share definition (immutable)
            new_sv->field_values.reserve(sv->field_values.size());
            for (const auto& fv : sv->field_values) {
                new_sv->field_values.push_back(fv.deepCopy(depth + 1));
            }
            NaabVal result;
            result = fromLegacy(std::make_shared<Value>(new_sv));
            return result;
        }
    }
    // Functions, closures, futures, etc. — share by reference (immutable or thread-local)
    return *this;
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
    // Iterate all live entries in the handle table.
    // g_next_handle is the high-water mark — no handles exist above it.
    uint32_t max_handle = g_next_handle.load(std::memory_order_relaxed);
    for (uint32_t h = 0; h < max_handle; ++h) {
        uint32_t page_idx = h >> PAGE_BITS;
        if (!g_pages[page_idx]) continue;  // Page not allocated
        ValueBox* box = g_pages[page_idx][h & PAGE_MASK];
        if (!box) continue;  // Freed handle
        // Create a NaabVal from this handle (bumps refcount temporarily)
        NaabVal val;
        val.bits_ = TAG_HEAP | static_cast<uint64_t>(h);
        box->refcount.fetch_add(1, std::memory_order_relaxed);  // retain
        callback(val);
    }
}

} // namespace interpreter
} // namespace naab
