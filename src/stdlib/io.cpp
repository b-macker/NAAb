// NAAb Standard Library - I/O operations
// read_file(), write_file(), etc.

#include "naab/limits.h"  // Week 1, Task 1.2: Input size caps
#include <fstream>
#include <string>
#include <sstream>

namespace naab {
namespace stdlib {

// read_file(path)
std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::ate);  // Open at end to get size
    if (!file) {
        throw std::runtime_error("Cannot open file: " + path);
    }

    // Week 1, Task 1.2: Check file size before reading
    size_t file_size = file.tellg();
    limits::checkFileSize(file_size, path);

    // Read file content
    file.seekg(0, std::ios::beg);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// write_file(path, data)
void write_file(const std::string& path, const std::string& data) {
    std::ofstream file(path);
    file << data;
}

// TODO: Implement remaining I/O functions

} // namespace stdlib
} // namespace naab
