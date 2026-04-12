#pragma once
// NaabVal: NaN-boxed value representation (8 bytes)
// Eliminates heap allocation for int, double, bool, null values.
// Complex types (string, array, dict, function, etc.) use heap-allocated ValueBox.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace naab {
namespace interpreter {

// Forward declarations — full definitions in interpreter.h
class Value;
struct BlockValue;
struct FunctionValue;
struct PythonObjectValue;
struct StructValue;
struct FutureValue;
struct GeneratorValue;

} // namespace interpreter

namespace vm {
struct VMClosure;
class VM;
} // namespace vm

namespace interpreter {

// Heap-allocated box for complex values.
// Fully defined in naab_val.cpp. Other code accesses via NaabVal typed methods.
struct ValueBox;

class NaabVal {
    friend class vm::VM;  // VM accesses bits_ directly for arithmetic fast paths
    uint64_t bits_;

    // NaN-boxing tag constants.
    // IEEE 754 quiet NaN: exponent=0x7FF, bit51=1. We use bits[50:48] as type tag.
    static constexpr uint64_t QNAN     = 0x7FF8000000000000ULL;
    static constexpr uint64_t TAG_NULL = 0x7FF9000000000000ULL; // bits[50:48] = 001
    static constexpr uint64_t TAG_BOOL = 0x7FFA000000000000ULL; // bits[50:48] = 010
    static constexpr uint64_t TAG_INT  = 0x7FFB000000000000ULL; // bits[50:48] = 011
    static constexpr uint64_t TAG_HEAP = 0x7FFC000000000000ULL; // bits[50:48] = 100
    static constexpr uint64_t TAG_MASK = 0xFFFF000000000000ULL;
    static constexpr uint64_t PTR_MASK = 0x0000FFFFFFFFFFFFULL;

    // Out-of-line ref management (defined in naab_val.cpp)
    void retain();
    void release();

public:
    // Default: null
    NaabVal() : bits_(TAG_NULL) {}

    // Implicit conversion from shared_ptr<Value> (migration bridge)
    // Allows existing code like `result_ = make_shared<Value>(42)` to work
    NaabVal(const std::shared_ptr<Value>& v);  // defined in naab_val.cpp

    // Destructor: decref heap values
    ~NaabVal() {
        if ((bits_ & TAG_MASK) == TAG_HEAP) release();
    }

    // Copy: incref heap values
    NaabVal(const NaabVal& other) : bits_(other.bits_) {
        if ((bits_ & TAG_MASK) == TAG_HEAP) retain();
    }

    // Move: steal bits, null-out source (no refcount change)
    NaabVal(NaabVal&& other) noexcept : bits_(other.bits_) {
        other.bits_ = TAG_NULL;
    }

    // Copy assign
    NaabVal& operator=(const NaabVal& other) {
        if (this != &other) {
            if ((bits_ & TAG_MASK) == TAG_HEAP) release();
            bits_ = other.bits_;
            if ((bits_ & TAG_MASK) == TAG_HEAP) retain();
        }
        return *this;
    }

    // Move assign
    NaabVal& operator=(NaabVal&& other) noexcept {
        if (this != &other) {
            if ((bits_ & TAG_MASK) == TAG_HEAP) release();
            bits_ = other.bits_;
            other.bits_ = TAG_NULL;
        }
        return *this;
    }

    // ========================================================================
    // Factory methods — small types are inline (zero heap allocation)
    // ========================================================================

    static NaabVal makeNull() {
        NaabVal v;
        v.bits_ = TAG_NULL;
        return v;
    }

    static NaabVal makeBool(bool b) {
        NaabVal v;
        v.bits_ = TAG_BOOL | (b ? 1ULL : 0ULL);
        return v;
    }

    static NaabVal makeInt(int i) {
        NaabVal v;
        uint32_t u;
        std::memcpy(&u, &i, sizeof(u));
        v.bits_ = TAG_INT | static_cast<uint64_t>(u);
        return v;
    }

    static NaabVal makeDouble(double d) {
        NaabVal v;
        std::memcpy(&v.bits_, &d, 8);
        // Canonicalize NaN values that would collide with our tag space
        uint64_t tag = v.bits_ & TAG_MASK;
        if (tag == TAG_NULL || tag == TAG_BOOL || tag == TAG_INT || tag == TAG_HEAP) {
            v.bits_ = QNAN; // Canonical quiet NaN
        }
        return v;
    }

    // Heap type factories (out-of-line, defined in naab_val.cpp)
    static NaabVal makeHeap(ValueBox* box);
    static NaabVal makeString(std::string s);
    static NaabVal makeString(const char* s);
    static NaabVal makeList(std::vector<NaabVal> v);
    static NaabVal makeDict(std::unordered_map<std::string, NaabVal> v);
    static NaabVal makeFunction(std::shared_ptr<FunctionValue> f);
    static NaabVal makeStruct(std::shared_ptr<StructValue> s);
    static NaabVal makeBlock(std::shared_ptr<BlockValue> b);
    static NaabVal makeFuture(std::shared_ptr<FutureValue> f);
    static NaabVal makeGenerator(std::shared_ptr<GeneratorValue> g);
    static NaabVal makePythonObject(std::shared_ptr<PythonObjectValue> p);
    static NaabVal makeVMClosure(std::shared_ptr<vm::VMClosure> c);

    // ========================================================================
    // Type checks — inline bit-mask operations
    // ========================================================================

    bool isNull() const { return (bits_ & TAG_MASK) == TAG_NULL; }
    bool isBool() const { return (bits_ & TAG_MASK) == TAG_BOOL; }
    bool isInt()  const { return (bits_ & TAG_MASK) == TAG_INT; }
    bool isHeap() const { return (bits_ & TAG_MASK) == TAG_HEAP; }

    bool isDouble() const {
        uint64_t tag = bits_ & TAG_MASK;
        return tag != TAG_NULL && tag != TAG_BOOL && tag != TAG_INT && tag != TAG_HEAP;
    }

    // High-level type checks (out-of-line, dispatch on ValueBox kind)
    bool isString() const;
    bool isList() const;
    bool isDict() const;
    bool isFunction() const;
    bool isStructVal() const;
    bool isBlock() const;
    bool isFuture() const;
    bool isGenerator() const;
    bool isPythonObject() const;
    bool isVMClosure() const;

    bool isNumeric() const { return isInt() || isDouble() || isBool(); }

    // ========================================================================
    // Extraction — inline for small types
    // ========================================================================

    double asDouble() const {
        double d;
        std::memcpy(&d, &bits_, 8);
        return d;
    }

    bool asBool() const {
        return (bits_ & 1ULL) != 0;
    }

    int asInt() const {
        uint32_t u = static_cast<uint32_t>(bits_);
        int i;
        std::memcpy(&i, &u, sizeof(i));
        return i;
    }

    ValueBox* asHeap() const;  // out-of-line (uses handle table on ARM64)

    // Heap type extraction (out-of-line, defined in naab_val.cpp)
    const std::string& asString() const;
    std::string& asStringMut();
    std::vector<NaabVal>& asList();
    const std::vector<NaabVal>& asListConst() const;
    std::unordered_map<std::string, NaabVal>& asDict();
    const std::unordered_map<std::string, NaabVal>& asDictConst() const;
    std::shared_ptr<FunctionValue>& asFunction();
    const std::shared_ptr<FunctionValue>& asFunctionConst() const;
    std::shared_ptr<StructValue>& asStruct();
    const std::shared_ptr<StructValue>& asStructConst() const;
    std::shared_ptr<BlockValue>& asBlock();
    const std::shared_ptr<BlockValue>& asBlockConst() const;
    std::shared_ptr<FutureValue>& asFuture();
    const std::shared_ptr<FutureValue>& asFutureConst() const;
    std::shared_ptr<GeneratorValue>& asGenerator();
    const std::shared_ptr<GeneratorValue>& asGeneratorConst() const;
    std::shared_ptr<PythonObjectValue>& asPythonObject();
    const std::shared_ptr<PythonObjectValue>& asPythonObjectConst() const;
    std::shared_ptr<vm::VMClosure>& asVMClosure();
    const std::shared_ptr<vm::VMClosure>& asVMClosureConst() const;

    // V-CONC-006: Deep copy for thread isolation — recursively duplicates containers
    NaabVal deepCopy(int depth = 0) const;

    // ========================================================================
    // Conversion methods (match Value API)
    // ========================================================================

    std::string toString() const;
    bool toBool() const;
    int toInt() const;
    double toFloat() const;

    // Get type name as string (for error messages)
    std::string getTypeName() const;

    // ========================================================================
    // Legacy conversion bridge
    // ========================================================================

    static NaabVal fromLegacy(const std::shared_ptr<Value>& v);
    std::shared_ptr<Value> toLegacy() const;

    // GC support: traverse reachable values (only for heap types)
    void traverse(std::function<void(const NaabVal&)> visitor) const;

    // GC support: iterate all live heap values in the handle table
    static void forEachHeapValue(std::function<void(NaabVal)> callback);

    // GC support: get refcount of heap object (0 for inline types)
    int getHeapRefCount() const;

    // ========================================================================
    // Operators
    // ========================================================================

    bool operator==(const NaabVal& other) const { return bits_ == other.bits_; }
    bool operator!=(const NaabVal& other) const { return bits_ != other.bits_; }

    // Truthiness check (NOT null check — all NaabVals are valid values)
    explicit operator bool() const { return !isNull(); }

    // Raw bits access (for debugging/GC)
    uint64_t rawBits() const { return bits_; }
};

static_assert(sizeof(NaabVal) == 8, "NaabVal must be exactly 8 bytes");

// Convenience conversion helpers
inline NaabVal toNaabVal(const std::shared_ptr<Value>& v) { return NaabVal::fromLegacy(v); }
inline std::shared_ptr<Value> toSharedValue(NaabVal v) { return v.toLegacy(); }

} // namespace interpreter
} // namespace naab
