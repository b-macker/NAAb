#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>

std::string hashCode(const std::string& code) {
    std::hash<std::string> hasher;
    size_t hash1 = hasher(code);

    size_t hash2 = code.length();
    if (!code.empty()) {
        hash2 ^= (size_t)code[0] << 16;
        hash2 ^= (size_t)code[code.length() / 2] << 8;
        hash2 ^= (size_t)code[code.length() - 1];
    }

    size_t final_hash = hash1 ^ (hash2 << 1);

    std::ostringstream oss;
    oss << std::hex << std::setfill('0') << std::setw(16) << final_hash;
    return oss.str();
}

int main() {
    std::string code1 = "#include <iostream>\n#include <string>\n#include <vector>\n#include <map>\nint main() {\n    auto result = (5 + 5);\n    std::cout << result;\n    return 0;\n}\n";
    std::string code2 = "#include <iostream>\n#include <string>\n#include <vector>\n#include <map>\nint main() {\n    auto result = (10 * 2);\n    std::cout << result;\n    return 0;\n}\n";

    std::cout << "Code 1 length: " << code1.length() << ", hash: " << hashCode(code1) << std::endl;
    std::cout << "Code 2 length: " << code2.length() << ", hash: " << hashCode(code2) << std::endl;
    std::cout << "Hashes equal: " << (hashCode(code1) == hashCode(code2) ? "YES - COLLISION!" : "NO") << std::endl;
}
