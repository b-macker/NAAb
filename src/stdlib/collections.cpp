// NAAb Standard Library - Collection types
// Using Abseil containers for efficiency

#include "absl/container/flat_hash_map.h"
#include <vector>
#include <string>

namespace naab {
namespace stdlib {

// TODO: Implement List, Dict, Set using Abseil containers

template<typename T>
class NaabList {
public:
    void append(const T& item) {
        items_.push_back(item);
    }

    size_t size() const {
        return items_.size();
    }

private:
    std::vector<T> items_;
};

template<typename K, typename V>
class NaabDict {
public:
    void insert(const K& key, const V& value) {
        map_.insert({key, value});
    }

    size_t size() const {
        return map_.size();
    }

private:
    absl::flat_hash_map<K, V> map_;
};

} // namespace stdlib
} // namespace naab
