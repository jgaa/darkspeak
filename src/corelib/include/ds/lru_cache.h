#ifndef cache_H
#define cache_H

#include <list>
#include <algorithm>

namespace ds {
namespace core {

template <typename T>
class LruCache {
public:
    LruCache() = default;
    LruCache(size_t size) : size_{size} {}

    void touch(const T& v) {
        auto it = find(cache_.begin(), cache_.end(), v);
        if (it != cache_.end()) {
            // Just relocate existing pointer to front
            auto ptr = move(*it);
            cache_.erase(it);
            cache_.push_front(move(ptr));
        } else {
            // Add the pointer
            cache_.push_front(v);
            if (cache_.size() >= size_) {
                cache_.pop_back();
            }
        }
    }

    void remove(const T& v) {
        cache_.erase(std::find(cache_.begin(), cache_.end(), v));
    }

    void clear() {
        cache_.clear();
    }

private:
    std::list<T> cache_;
    size_t size_ = 32;
};

}}

#endif // cache_H
