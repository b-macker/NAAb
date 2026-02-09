// NAAb C++ Block Executor Implementation
// Compiles and executes C++ blocks via dynamic loading

#include "naab/cpp_executor.h"
#include "naab/interpreter.h"
#include "naab/resource_limits.h"
#include "naab/input_validator.h"
#include "naab/audit_logger.h"
#include "naab/sandbox.h"
#include "naab/stack_tracer.h"  // Phase 4.2.5: Cross-language stack traces
#include <fmt/core.h>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <dlfcn.h>
#include <cstdlib>

#ifdef HAVE_LIBFFI
#include <ffi.h>
#endif

namespace fs = std::filesystem;

namespace naab {
namespace runtime {

// ============================================================================
// CompiledBlock Implementation
// ============================================================================

CompiledBlock::~CompiledBlock() {
    if (handle && is_loaded) {
        dlclose(handle);
        handle = nullptr;
    }
}

// ============================================================================
// CppExecutor Implementation
// ============================================================================

CppExecutor::CppExecutor() {
    // Create cache directory in Termux home for dlopen compatibility
    // Android has namespace restrictions on external storage
    cache_dir_ = "/data/data/com.termux/files/home/.naab_cpp_cache";

    try {
        if (!fs::exists(cache_dir_)) {
            fs::create_directories(cache_dir_);
            // Created C++ compilation cache (silent)
        }
    } catch (const std::exception& e) {
        fmt::print("[ERROR] Failed to create cache directory: {}\n", e.what());
        // Fallback to current directory
        cache_dir_ = ".cpp_cache";
        fs::create_directories(cache_dir_);
    }
}

CppExecutor::~CppExecutor() {
    // Cleanup loaded blocks
    compiled_blocks_.clear();
}

std::string CppExecutor::getSourcePath(const std::string& block_id) const {
    return cache_dir_ + "/" + block_id + ".cpp";
}

std::string CppExecutor::getLibraryPath(const std::string& block_id) const {
    return cache_dir_ + "/" + block_id + ".so";
}

// Helper to wrap code fragments in main() if needed
std::string CppExecutor::wrapFragmentIfNeeded(const std::string& code) {
    // Heuristic: Check if code looks like a complete program
    bool has_main = code.find("int main") != std::string::npos ||
                   code.find("void main") != std::string::npos;

    // Check if code has extern "C" block (block library with FFI functions)
    bool has_extern_c = code.find("extern \"C\"") != std::string::npos;

    // If it has main or extern "C", assume it's complete
    if (has_main || has_extern_c) {
        return code;
    }

    // Otherwise, wrap it in a main function
    // But first, extract any #include directives to place at top level
    std::ostringstream includes;
    std::ostringstream code_without_includes;

    std::istringstream code_stream(code);
    std::string line;
    while (std::getline(code_stream, line)) {
        // Check if line contains #include directive
        size_t include_pos = line.find("#include");
        if (include_pos != std::string::npos) {
            // This is an include directive - place at top level
            includes << line << "\n";
        } else if (!line.empty() || code_stream.peek() != EOF) {
            // This is regular code - will go inside main
            code_without_includes << line << "\n";
        }
    }

    // Build final code with includes at top, code in main
    std::ostringstream wrapped;

    // Add extracted includes first (at top level)
    if (!includes.str().empty()) {
        wrapped << includes.str();
    }

    // Wrap the non-include code in main
    wrapped << "int main() {\n";
    std::string code_content = code_without_includes.str();
    if (!code_content.empty()) {
        // Add each line with proper indentation
        std::istringstream content_stream(code_content);
        std::string content_line;
        while (std::getline(content_stream, content_line)) {
            if (!content_line.empty()) {
                wrapped << "    " << content_line << "\n";
            }
        }
    }
    wrapped << "    return 0;\n";
    wrapped << "}\n";

    return wrapped.str();
}

bool CppExecutor::compileBlock(
    const std::string& block_id,
    const std::string& code,
    const std::string& entry_point,
    const std::vector<std::string>& dependencies) {

    // Compiling block (silent)

    if (!dependencies.empty()) {
        // Dependencies (silent)
        for (const auto& dep : dependencies) {
            fmt::print("{} ", dep);
        }
        fmt::print("\n");
    }

    // Check if already compiled and cached
    std::string so_path = getLibraryPath(block_id);
    if (fs::exists(so_path)) {
        // Using cached compilation (silent)
        return loadCompiledBlock(block_id);
    }

    // Write source code to file
    std::string source_path = getSourcePath(block_id);
    std::ofstream source_file(source_path);
    if (!source_file.is_open()) {
        fmt::print("[ERROR] Failed to create source file: {}\n", source_path);
        return false;
    }

    // Write the actual C++ code
    // If the code is complete and compilable, use it directly
    // If it's a fragment, we'll need wrapping (not implemented yet)
    source_file << "// Auto-generated from NAAb C++ block: " << block_id << "\n\n";

    // Inject common STL headers for inline C++ code
    // This allows users to use std::cout, std::vector, std::sort, etc. without explicit includes
    source_file << "#include <iostream>\n";
    source_file << "#include <vector>\n";
    source_file << "#include <algorithm>\n";
    source_file << "#include <string>\n";
    source_file << "#include <map>\n";
    source_file << "#include <unordered_map>\n";
    source_file << "#include <set>\n";
    source_file << "#include <unordered_set>\n";
    source_file << "#include <memory>\n";
    source_file << "#include <utility>\n";
    source_file << "#include <cmath>\n";
    source_file << "#include <cstdlib>\n";
    source_file << "\n";

    // Write the code (wrapped if it's a fragment)
    std::string final_code = wrapFragmentIfNeeded(code);
    source_file << final_code;

    source_file.close();

    // Source written (silent)

    // Compile to shared library
    bool compiled = compileToSharedLibrary(source_path, so_path, dependencies);
    if (!compiled) {
        return false;
    }

    // Load the compiled library
    return loadCompiledBlock(block_id);
}

bool CppExecutor::compileToSharedLibrary(
    const std::string& source_path,
    const std::string& so_path,
    const std::vector<std::string>& dependencies) {

    // Compiling (silent)

    // Build compilation command
    // Use clang++ on Android/Termux, g++ elsewhere
    std::string compiler = "clang++";

    // Build library flags from dependencies
    std::string lib_flags = buildLibraryFlags(dependencies);

    // Get NAAb include directory (relative to executable or known location)
    // For Termux, assume NAAb is installed in a standard location
    std::string naab_include = "/data/data/com.termux/files/home/.naab/language/include";
    std::string python_include = "/data/data/com.termux/files/usr/include/python3.12";

    std::ostringstream cmd;
    cmd << compiler << " "
        << "-std=c++17 "
        << "-fPIC "           // Position-independent code
        << "-shared "         // Build shared library
        << "-O2 "             // Optimize
        << "-I" << naab_include << " "  // Include NAAb headers
        << "-I" << python_include << " "  // Include Python headers (for pybind11)
        << "-o " << so_path << " "
        << source_path << " ";

    // Add library flags if any
    if (!lib_flags.empty()) {
        cmd << lib_flags << " ";
    }

    cmd << "2>&1";  // Capture stderr

    std::string command = cmd.str();
    // Command (silent)

    // Check sandbox permissions for command execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->canExecuteCommand(command)) {
        fmt::print("[ERROR] Sandbox violation: Command execution denied\n");
        sandbox->logViolation("executeCommand", command, "SYS_EXEC capability required");
        return false;
    }

    // Execute compilation with timeout protection
    FILE* pipe = nullptr;
    int exit_code = -1;
    std::string compiler_output;

    try {
        // Set 30-second timeout for compilation
        security::ScopedTimeout timeout(30);

        pipe = popen(command.c_str(), "r");
        if (!pipe) {
            fmt::print("[ERROR] Failed to execute compiler\n");
            security::AuditLogger::logSecurityViolation("popen() failed for C++ compilation");
            return false;
        }

        // Read compiler output
        char buffer[256];
        while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            compiler_output += buffer;
        }

        exit_code = pclose(pipe);
        pipe = nullptr;  // Mark as closed

    } catch (const security::ResourceLimitException& e) {
        if (pipe) pclose(pipe);
        fmt::print("[ERROR] Compilation timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("C++ compilation", 30);
        return false;
    } catch (const std::exception& e) {
        if (pipe) pclose(pipe);
        fmt::print("[ERROR] Compilation error: {}\n", e.what());
        return false;
    }

    if (exit_code != 0) {
        fmt::print("[ERROR] Compilation failed (exit code: {})\n", exit_code);
        fmt::print("[ERROR] Compiler output:\n{}\n", compiler_output);
        return false;
    }

    // Compiled successfully (silent)

    // Verify .so file exists
    if (!fs::exists(so_path)) {
        fmt::print("[ERROR] Compiled library not found: {}\n", so_path);
        return false;
    }

    return true;
}

bool CppExecutor::loadCompiledBlock(const std::string& block_id) {
    std::string so_path = getLibraryPath(block_id);

    if (!fs::exists(so_path)) {
        fmt::print("[ERROR] Compiled library not found: {}\n", so_path);
        return false;
    }

    // Validate library path (prevent path traversal)
    std::string canonical_path = security::InputValidator::canonicalizePath(so_path);
    if (!security::InputValidator::isSafePath(canonical_path, cache_dir_)) {
        fmt::print("[ERROR] Invalid library path: {}\n", so_path);
        security::AuditLogger::logInvalidPath(so_path, "Path traversal attempt in dlopen");
        return false;
    }

    // Check sandbox permissions for library execution
    auto* sandbox = security::ScopedSandbox::getCurrent();
    if (sandbox && !sandbox->canExecute(canonical_path)) {
        fmt::print("[ERROR] Sandbox violation: Library execution denied: {}\n", canonical_path);
        sandbox->logViolation("executeLibrary", canonical_path, "FS_EXECUTE capability required");
        return false;
    }

    // Load shared library with timeout protection
    void* handle = nullptr;
    try {
        security::ScopedTimeout timeout(5);  // 5-second timeout for dlopen

        handle = dlopen(canonical_path.c_str(), RTLD_LAZY);
        if (!handle) {
            fmt::print("[ERROR] Failed to load library: {}\n", dlerror());
            security::AuditLogger::logSecurityViolation("dlopen() failed: " + std::string(dlerror()));
            return false;
        }
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Library load timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("dlopen()", 5);
        return false;
    }

    // Loaded C++ block library (silent)
    security::AuditLogger::logBlockLoad(block_id, "");

    // Create CompiledBlock entry
    auto compiled = std::make_shared<CompiledBlock>();
    compiled->block_id = block_id;
    compiled->so_path = so_path;
    compiled->handle = handle;
    compiled->entry_point = "execute";
    compiled->is_loaded = true;

    compiled_blocks_[block_id] = compiled;

    return true;
}

bool CppExecutor::isCompiled(const std::string& block_id) const {
    // Check in-memory cache
    if (compiled_blocks_.find(block_id) != compiled_blocks_.end()) {
        return true;
    }

    // Check filesystem cache
    std::string so_path = getLibraryPath(block_id);
    return fs::exists(so_path);
}

std::shared_ptr<interpreter::Value> CppExecutor::executeBlock(
    const std::string& block_id,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Ensure block is compiled and loaded
    auto it = compiled_blocks_.find(block_id);
    if (it == compiled_blocks_.end()) {
        fmt::print("[ERROR] Block not compiled or loaded: {}\n", block_id);
        return std::make_shared<interpreter::Value>();
    }

    auto& block = it->second;

    if (!block->is_loaded || !block->handle) {
        fmt::print("[ERROR] Block loaded but handle is null: {}\n", block_id);
        return std::make_shared<interpreter::Value>();
    }

    // Get function pointer
    typedef void (*ExecuteFunc)();
    ExecuteFunc execute = (ExecuteFunc)dlsym(block->handle, block->entry_point.c_str());

    if (!execute) {
        fmt::print("[ERROR] Failed to find entry point '{}': {}\n",
                   block->entry_point, dlerror());
        return std::make_shared<interpreter::Value>();
    }

    fmt::print("[EXEC] Executing C++ block: {}\n", block_id);

    // Call the function
    execute();

    // C++ block executed (silent)

    // For now, return success indicator
    // In production, would need proper return value handling
    return std::make_shared<interpreter::Value>(true);
}

std::shared_ptr<interpreter::Value> CppExecutor::callFunction(
    const std::string& block_id,
    const std::string& function_name,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Ensure block is compiled and loaded
    auto it = compiled_blocks_.find(block_id);
    if (it == compiled_blocks_.end()) {
        throw std::runtime_error(fmt::format(
            "Block not compiled or loaded: {}", block_id));
    }

    auto& block = it->second;

    if (!block->is_loaded || !block->handle) {
        throw std::runtime_error(fmt::format(
            "Block loaded but handle is null: {}", block_id));
    }

    // Clear previous errors
    dlerror();

    // Get function pointer
    void* func_ptr = dlsym(block->handle, function_name.c_str());

    // Check for errors
    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        throw std::runtime_error(fmt::format(
            "Failed to find function '{}' in block {}: {}",
            function_name, block_id, dlsym_error));
    }

    // Phase 4.2.5: Push stack frame for cross-language tracing
    error::ScopedStackFrame stack_frame("cpp", function_name, "<cpp>", 0);

    // Wrap function call in try/catch to capture C++ exceptions
    try {

    // Check if we have a registered signature for this function
    auto sig_it = block->function_signatures.find(function_name);

#ifdef HAVE_LIBFFI
    if (sig_it != block->function_signatures.end()) {
        // Use libffi for dynamic calling with registered signature
        auto result = callWithFFI(func_ptr, sig_it->second, args);
        return result;
    }
#endif

    // Fallback: hardcoded function signatures (legacy support)
    // int f(int, int)
    // double f(double, double)
    // int f()
    //
    // NOTE: This is limited! Register function signatures for better support.

    if (args.empty()) {
        // No arguments - call as int f()
        typedef int (*FuncNoArgs)();
        FuncNoArgs func = reinterpret_cast<FuncNoArgs>(func_ptr);
        int result = func();
        return marshaller_.fromInt(result);
    } else if (args.size() == 1) {
        auto arg = marshaller_.toCpp(args[0]);

        // Function name hints for double functions (until proper signature registration)
        bool prefer_double = (function_name.find("sqrt") != std::string::npos ||
                             function_name.find("log") != std::string::npos ||
                             function_name.find("exp") != std::string::npos ||
                             function_name.find("sin") != std::string::npos ||
                             function_name.find("cos") != std::string::npos ||
                             function_name.find("tan") != std::string::npos ||
                             function_name.find("ceil") != std::string::npos ||
                             function_name.find("floor") != std::string::npos);

        // Try int f(int) if INT and function doesn't prefer double
        if (!prefer_double && arg.type == CppType::INT) {
            typedef int (*FuncInt)(int);
            FuncInt func = reinterpret_cast<FuncInt>(func_ptr);
            int result = func(static_cast<int>(arg.i));
            return marshaller_.fromInt(result);
        }
        // Try double f(double) with automatic int→double promotion
        else if (arg.type == CppType::INT || arg.type == CppType::DOUBLE) {
            double d = (arg.type == CppType::INT) ? static_cast<double>(arg.i) : arg.d;
            typedef double (*FuncDbl)(double);
            FuncDbl func = reinterpret_cast<FuncDbl>(func_ptr);
            double result = func(d);
            return marshaller_.fromDouble(result);
        } else {
            throw std::runtime_error(fmt::format(
                "Unsupported argument type for {}: {}",
                function_name,
                marshaller_.typeName(arg.type)));
        }
    } else if (args.size() == 2) {
        auto arg1 = marshaller_.toCpp(args[0]);
        auto arg2 = marshaller_.toCpp(args[1]);

        // Function name hints for double functions (until proper signature registration)
        // Common math functions that work with floating point
        bool prefer_double = (function_name == "power" ||
                             function_name == "pow" ||
                             function_name.find("sqrt") != std::string::npos ||
                             function_name.find("log") != std::string::npos ||
                             function_name.find("exp") != std::string::npos ||
                             function_name.find("sin") != std::string::npos ||
                             function_name.find("cos") != std::string::npos ||
                             function_name.find("tan") != std::string::npos);

        // Try int f(int, int) if both are INT and function doesn't prefer double
        if (!prefer_double && arg1.type == CppType::INT && arg2.type == CppType::INT) {
            typedef int (*FuncIntInt)(int, int);
            FuncIntInt func = reinterpret_cast<FuncIntInt>(func_ptr);
            int result = func(static_cast<int>(arg1.i), static_cast<int>(arg2.i));
            return marshaller_.fromInt(result);
        }
        // Try double f(double, double) with automatic int→double promotion
        else if ((arg1.type == CppType::INT || arg1.type == CppType::DOUBLE) &&
                 (arg2.type == CppType::INT || arg2.type == CppType::DOUBLE)) {
            // Convert both to double
            double d1 = (arg1.type == CppType::INT) ? static_cast<double>(arg1.i) : arg1.d;
            double d2 = (arg2.type == CppType::INT) ? static_cast<double>(arg2.i) : arg2.d;

            typedef double (*FuncDblDbl)(double, double);
            FuncDblDbl func = reinterpret_cast<FuncDblDbl>(func_ptr);
            double result = func(d1, d2);
            return marshaller_.fromDouble(result);
        } else {
            throw std::runtime_error(fmt::format(
                "Unsupported argument types for {}: {} and {}",
                function_name,
                marshaller_.typeName(arg1.type),
                marshaller_.typeName(arg2.type)));
        }
    } else {
        throw std::runtime_error(fmt::format(
            "Unsupported number of arguments for {}: {}",
            function_name, args.size()));
    }

    // Phase 4.2.5: End of try block
    } catch (const std::exception& ex) {
        // Phase 4.2.5: C++ exception caught - add to stack trace
        error::StackFrame cpp_frame("cpp", ex.what(), "<cpp>", 0);
        error::StackTracer::pushFrame(cpp_frame);

        fmt::print("[TRACE] C++ frame: {} (<cpp>:0)\n", ex.what());

        // Re-throw with enriched stack trace
        throw std::runtime_error(fmt::format(
            "C++ function '{}' threw exception: {}\n{}",
            function_name, ex.what(), error::StackTracer::formatTrace()));
    } catch (...) {
        // Unknown exception type
        error::StackFrame cpp_frame("cpp", "unknown exception", "<cpp>", 0);
        error::StackTracer::pushFrame(cpp_frame);

        throw std::runtime_error(fmt::format(
            "C++ function '{}' threw unknown exception\n{}",
            function_name, error::StackTracer::formatTrace()));
    }
}

void CppExecutor::clearCache() {
    fmt::print("[INFO] Clearing C++ compilation cache...\n");

    // Close all loaded libraries
    compiled_blocks_.clear();

    // Remove cached files
    try {
        for (const auto& entry : fs::directory_iterator(cache_dir_)) {
            if (entry.path().extension() == ".cpp" ||
                entry.path().extension() == ".so") {
                fs::remove(entry.path());
            }
        }
        fmt::print("[INFO] Cache cleared successfully\n");
    } catch (const std::exception& e) {
        fmt::print("[WARN] Failed to clear cache: {}\n", e.what());
    }
}

void CppExecutor::registerFunctionSignature(
    const std::string& block_id,
    const std::string& function_name,
    const FunctionSignature& signature) {

    auto it = compiled_blocks_.find(block_id);
    if (it != compiled_blocks_.end()) {
        it->second->function_signatures[function_name] = signature;
        fmt::print("[INFO] Registered signature for {}::{}: {} ({})\n",
            block_id, function_name, signature.return_type, signature.param_types.size());
    } else {
        fmt::print("[WARN] Cannot register signature for unknown block: {}\n", block_id);
    }
}

std::string CppExecutor::buildLibraryFlags(const std::vector<std::string>& dependencies) const {
    // Map dependency names to linker flags and include paths
    // These are the common libraries found in the 23,906 C++ blocks
    static const std::unordered_map<std::string, std::string> library_map = {
        // Core formatting and logging (most common)
        {"spdlog", "-lspdlog -lfmt"},
        {"fmt", "-lfmt"},

        // Abseil (Google's C++ library)
        {"abseil", "-labsl_strings -labsl_time -labsl_base -labsl_synchronization -labsl_hash -labsl_flat_hash_map"},
        {"absl", "-labsl_strings -labsl_time -labsl_base -labsl_synchronization -labsl_hash -labsl_flat_hash_map"},

        // Threading and concurrency
        {"pthread", "-lpthread"},
        {"threads", "-lpthread"},

        // Math libraries
        {"math", "-lm"},
        {"m", "-lm"},

        // Dynamic loading
        {"dl", "-ldl"},
        {"dload", "-ldl"},

        // JSON processing
        {"json", ""},  // Header-only

        // SQLite database
        {"sqlite3", "-lsqlite3"},

        // OpenSSL crypto
        {"openssl", "-lssl -lcrypto"},
        {"crypto", "-lcrypto"},

        // QuickJS (JavaScript engine)
        {"quickjs", "-lquickjs"},

        // Compression libraries
        {"zlib", "-lz"},
        {"bzip2", "-lbz2"},

        // Networking
        {"curl", "-lcurl"},

        // Boost (if available)
        {"boost_filesystem", "-lboost_filesystem -lboost_system"},
        {"boost_system", "-lboost_system"},

        // LLVM and Clang (major addition for 4,567 blocks)
        {"llvm", "-lLLVM -lLLVMSupport -lLLVMCore -lLLVMIRReader"},
        {"clang", "-lclang -lclangAST -lclangBasic -lclangDriver -lclangFrontend -lclangSerialization"},

        // OpenMP (parallel processing)
        {"openmp", "-fopenmp"},
        {"omp", "-fopenmp"},

        // Additional libraries detected by BlockEnricher
        {"gtest", "-lgtest -lgtest_main -lpthread"},
        {"gmock", "-lgmock -lgtest -lpthread"},
        {"benchmark", "-lbenchmark -lpthread"},
        {"protobuf", "-lprotobuf"},
        {"grpc", "-lgrpc++ -lgrpc -lprotobuf"},
        {"pybind11", ""},  // Header-only
        {"eigen", ""},  // Header-only
        {"opencv", "-lopencv_core -lopencv_imgproc -lopencv_highgui"},
    };

    std::ostringstream flags;

    for (const auto& dep : dependencies) {
        auto it = library_map.find(dep);
        if (it != library_map.end()) {
            if (!it->second.empty()) {
                flags << it->second << " ";
            }
            fmt::print("[LIBS] Mapped '{}' -> '{}'\n", dep, it->second);
        } else {
            // Unknown dependency - try direct linker flag
            fmt::print("[WARN] Unknown dependency '{}', trying -l{}\n", dep, dep);
            flags << "-l" << dep << " ";
        }
    }

    return flags.str();
}

#ifdef HAVE_LIBFFI

ffi_type* CppExecutor::mapTypeToFFI(const std::string& type_name) {
    // Map C++ type names to libffi types
    if (type_name == "void") return &ffi_type_void;
    if (type_name == "int") return &ffi_type_sint32;
    if (type_name == "long") return &ffi_type_slong;
    if (type_name == "short") return &ffi_type_sshort;
    if (type_name == "char") return &ffi_type_schar;
    if (type_name == "unsigned int" || type_name == "uint") return &ffi_type_uint32;
    if (type_name == "unsigned long" || type_name == "ulong") return &ffi_type_ulong;
    if (type_name == "float") return &ffi_type_float;
    if (type_name == "double") return &ffi_type_double;
    if (type_name == "bool") return &ffi_type_uint8;

    // Pointers (char*, void*, std::string*, etc.)
    if (type_name.find('*') != std::string::npos || type_name == "string") {
        return &ffi_type_pointer;
    }

    // Default to pointer for unknown types
    fmt::print("[WARN] Unknown type '{}', defaulting to pointer\n", type_name);
    return &ffi_type_pointer;
}

std::shared_ptr<interpreter::Value> CppExecutor::callWithFFI(
    void* func_ptr,
    const FunctionSignature& signature,
    const std::vector<std::shared_ptr<interpreter::Value>>& args) {

    // Validate argument count
    if (args.size() != signature.param_types.size()) {
        throw std::runtime_error(fmt::format(
            "Argument count mismatch: expected {}, got {}",
            signature.param_types.size(), args.size()));
    }

    // Map return type
    ffi_type* return_type = mapTypeToFFI(signature.return_type);

    // Map parameter types
    std::vector<ffi_type*> param_types;
    for (const auto& param_type : signature.param_types) {
        param_types.push_back(mapTypeToFFI(param_type));
    }

    // Prepare ffi_cif
    ffi_cif cif;
    ffi_status status = ffi_prep_cif(
        &cif,
        FFI_DEFAULT_ABI,
        param_types.size(),
        return_type,
        param_types.data()
    );

    if (status != FFI_OK) {
        throw std::runtime_error(fmt::format(
            "ffi_prep_cif failed with status {}", static_cast<int>(status)));
    }

    // Convert NAAb values to C values
    std::vector<void*> arg_values;
    std::vector<int> int_args;
    std::vector<double> double_args;
    std::vector<void*> ptr_args;

    for (size_t i = 0; i < args.size(); ++i) {
        auto cpp_val = marshaller_.toCpp(args[i]);
        const std::string& param_type = signature.param_types[i];

        if (param_type == "int" || param_type == "long" || param_type == "short") {
            int_args.push_back(static_cast<int>(cpp_val.i));
            arg_values.push_back(&int_args.back());
        } else if (param_type == "double" || param_type == "float") {
            double_args.push_back(cpp_val.d);
            arg_values.push_back(&double_args.back());
        } else {
            // Pointer type - treat as void*
            ptr_args.push_back(reinterpret_cast<void*>(cpp_val.i));
            arg_values.push_back(&ptr_args.back());
        }
    }

    // Prepare return value storage
    union {
        int i;
        long l;
        double d;
        void* ptr;
    } result;

    // Make the FFI call with timeout protection
    try {
        security::ScopedTimeout timeout(10);  // 10-second timeout for function execution
        ffi_call(&cif, FFI_FN(func_ptr), &result, arg_values.data());
    } catch (const security::ResourceLimitException& e) {
        fmt::print("[ERROR] Function execution timeout: {}\n", e.what());
        security::AuditLogger::logTimeout("FFI function call", 10);
        throw std::runtime_error("Function execution timed out");
    }

    // Convert result back to NAAb Value
    if (signature.return_type == "void") {
        return std::make_shared<interpreter::Value>();
    } else if (signature.return_type == "int" || signature.return_type == "long" ||
               signature.return_type == "short" || signature.return_type == "char") {
        return marshaller_.fromInt(result.i);
    } else if (signature.return_type == "double" || signature.return_type == "float") {
        return marshaller_.fromDouble(result.d);
    } else if (signature.return_type == "bool") {
        return marshaller_.fromBool(static_cast<bool>(result.i));
    } else {
        // Pointer type - for now return as int
        fmt::print("[WARN] Returning pointer type {} as int\n", signature.return_type);
        return marshaller_.fromInt(reinterpret_cast<intptr_t>(result.ptr));
    }
}

#endif // HAVE_LIBFFI

} // namespace runtime
} // namespace naab
