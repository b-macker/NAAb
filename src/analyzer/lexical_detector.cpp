#include "naab/analyzer/lexical_detector.h"
#include <algorithm>
#include <regex>

namespace naab {
namespace analyzer {

LexicalDetector::LexicalDetector() {
    initializeTokens();
}

void LexicalDetector::initializeTokens() {
    // 1. Numerical computing tokens (25 patterns)
    numeric_tokens_ = {
        "matrix", "array", "vector", "tensor", "ndarray",
        "mean", "median", "std", "var", "cov", "corr",
        "dot", "cross", "norm", "det", "determinant", "eigenvalue", "eigenvector",
        "fft", "ifft", "convolve", "integrate", "derivative", "gradient",
        "linspace", "arange", "zeros", "ones", "eye", "rand",
        "svd", "qr", "cholesky", "lu",
        "cumsum", "cumprod", "diff"
    };

    // 2. String processing tokens (30 patterns)
    string_tokens_ = {
        "split", "join", "replace", "strip", "trim", "lstrip", "rstrip",
        "substr", "substring", "slice", "concat", "format", "sprintf",
        "regex", "match", "search", "findall", "sub", "gsub",
        "upper", "lower", "capitalize", "title", "swapcase",
        "startswith", "endswith", "contains", "indexOf", "lastIndexOf",
        "pad", "zfill", "center", "ljust", "rjust"
    };

    // 3. File I/O tokens (25 patterns)
    file_io_tokens_ = {
        "open", "read", "write", "close", "flush",
        "readlines", "writelines", "readline", "seek", "tell",
        "exists", "isfile", "isdir", "mkdir", "rmdir", "makedirs",
        "unlink", "remove", "rename", "move", "copy",
        "stat", "chmod", "chown",
        "glob", "walk", "listdir", "scandir"
    };

    // 4. Network/HTTP tokens (25 patterns)
    network_tokens_ = {
        "fetch", "request", "response", "http", "https",
        "get", "post", "put", "delete", "patch", "head", "options",
        "url", "uri", "endpoint", "api", "rest", "graphql",
        "socket", "connect", "send", "receive", "listen", "accept",
        "websocket", "ajax", "xhr"
    };

    // 5. Concurrency tokens (20 patterns)
    concurrency_tokens_ = {
        "async", "await", "promise", "future", "then", "catch",
        "thread", "goroutine", "coroutine", "task", "fiber",
        "lock", "mutex", "semaphore", "channel", "queue",
        "parallel", "concurrent", "spawn", "fork", "join"
    };

    // 6. Systems programming tokens (25 patterns)
    systems_tokens_ = {
        "malloc", "free", "calloc", "realloc", "alloc",
        "pointer", "address", "unsafe", "raw", "ptr",
        "syscall", "ioctl", "mmap", "munmap", "mprotect",
        "fork", "exec", "wait", "waitpid", "signal", "kill",
        "pipe", "dup", "dup2", "fcntl"
    };

    // 7. Data structures tokens (20 patterns)
    datastructure_tokens_ = {
        "list", "array", "dict", "map", "set", "frozenset",
        "stack", "queue", "deque", "heap", "priority_queue",
        "tree", "graph", "node", "edge",
        "hash", "table", "index", "key", "value"
    };

    // 8. Parsing/serialization tokens (20 patterns)
    parsing_tokens_ = {
        "json", "xml", "yaml", "toml", "csv", "ini",
        "parse", "serialize", "deserialize", "encode", "decode",
        "marshal", "unmarshal", "dump", "load", "dumps", "loads",
        "parser", "lexer", "tokenize"
    };

    // 9. Database tokens (15 patterns)
    database_tokens_ = {
        "select", "insert", "update", "delete", "drop", "create",
        "query", "execute", "fetch", "commit", "rollback",
        "transaction", "cursor", "connection", "prepare"
    };

    // 10. Shell command tokens (20 patterns)
    shell_tokens_ = {
        "find", "grep", "sed", "awk", "cut", "sort", "uniq",
        "ls", "cat", "head", "tail", "wc", "tr", "tee",
        "pipe", "redirect", "stdout", "stderr", "stdin",
        "chmod"
    };
}

int LexicalDetector::countTokens(const std::string& code, const std::vector<std::string>& tokens) const {
    int count = 0;
    std::string code_lower = code;
    std::transform(code_lower.begin(), code_lower.end(), code_lower.begin(), ::tolower);

    for (const auto& token : tokens) {
        // Build regex pattern: word boundary + token + word boundary or specific punctuation
        std::string pattern = "\\b" + token + "\\b|" + token + "\\(|\\." + token;
        std::regex re(pattern, std::regex::icase);

        auto matches_begin = std::sregex_iterator(code.begin(), code.end(), re);
        auto matches_end = std::sregex_iterator();

        count += std::distance(matches_begin, matches_end);
    }

    return count;
}

std::map<std::string, int> LexicalDetector::analyze(const std::string& code) const {
    std::map<std::string, int> results;

    results["numerical_computation"] = countTokens(code, numeric_tokens_);
    results["string_manipulation"] = countTokens(code, string_tokens_);
    results["file_operations"] = countTokens(code, file_io_tokens_);
    results["network_communication"] = countTokens(code, network_tokens_);
    results["concurrency"] = countTokens(code, concurrency_tokens_);
    results["systems_programming"] = countTokens(code, systems_tokens_);
    results["data_structures"] = countTokens(code, datastructure_tokens_);
    results["data_parsing"] = countTokens(code, parsing_tokens_);
    results["database_access"] = countTokens(code, database_tokens_);
    results["shell_commands"] = countTokens(code, shell_tokens_);

    return results;
}

std::string LexicalDetector::getDominantCategory(const std::string& code) const {
    auto results = analyze(code);

    auto max_elem = std::max_element(
        results.begin(), results.end(),
        [](const auto& a, const auto& b) {
            return a.second < b.second;
        }
    );

    return (max_elem != results.end() && max_elem->second > 0) ? max_elem->first : "";
}

std::vector<std::pair<std::string, int>> LexicalDetector::getRankedCategories(const std::string& code) const {
    auto results = analyze(code);

    std::vector<std::pair<std::string, int>> ranked;
    for (const auto& [category, count] : results) {
        if (count > 0) {
            ranked.push_back({category, count});
        }
    }

    // Sort by count descending
    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) {
            return a.second > b.second;
        }
    );

    return ranked;
}

} // namespace analyzer
} // namespace naab
