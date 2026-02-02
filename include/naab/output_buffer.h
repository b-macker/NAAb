#ifndef NAAB_OUTPUT_BUFFER_H
#define NAAB_OUTPUT_BUFFER_H

#include <string>
#include <sstream>
#include <mutex>
#include <fmt/core.h>

namespace naab {
namespace runtime {

/**
 * @brief A thread-safe buffer for capturing stdout/stderr from embedded interpreters.
 */
class OutputBuffer {
public:
    /**
     * @brief Appends a string to the buffer.
     * @param str The string to append.
     */
    void append(const std::string& str) {
        std::lock_guard<std::mutex> lock(mutex_);
        buffer_ << str;
    }

    /**
     * @brief Retrieves the current content of the buffer and clears it.
     * @return The content of the buffer.
     */
    std::string getAndClear() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::string content = buffer_.str();
        buffer_.str(""); // Clear the buffer
        buffer_.clear();  // Clear error flags
        return content;
    }

    /**
     * @brief Checks if the buffer is empty.
     * @return True if the buffer is empty, false otherwise.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.str().empty();
    }

private:
    std::stringstream buffer_;
    mutable std::mutex mutex_; // mutable for const empty() method
};

} // namespace runtime
} // namespace naab

#endif // NAAB_OUTPUT_BUFFER_H
