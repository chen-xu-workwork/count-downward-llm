#ifndef ALGORITHMS_ARRAY_POOL_H
#define ALGORITHMS_ARRAY_POOL_H

#include <vector>

namespace array_pool_template {

template<class T>
class ArrayPool {
    std::vector<std::vector<T>> pool;
public:
    void push_back(std::vector<T> data) {
        pool.push_back(std::move(data));
    }

    const std::vector<T> &get_slice(int index) const {
        return pool[index];
    }
    
    void reserve(size_t n, size_t total_size) {
        pool.reserve(n);
        // total_size ignored in this simple implementation
    }

    size_t size() const {
        return pool.size();
    }
};

}

#endif
