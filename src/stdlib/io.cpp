// NAAb Standard Library - I/O operations
// read_file(), write_file(), etc.

#include <fstream>
#include <string>
#include <sstream>

namespace naab {
namespace stdlib {

// read_file(path)
std::string read_file(const std::string& path) {
    std::ifstream file(path);
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
