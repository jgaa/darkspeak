#ifndef REGISTRY_H
#define REGISTRY_H

// See https://lastviking.eu/singleton_objects.html

#include <map>
#include <memory>
#include <string>

namespace ds {
namespace core {

template <typename keyT, typename valueT>
class Registry {
public:
    Registry() = default;

    std::shared_ptr<valueT> fetch(const keyT& key) const {
        auto it = registry_.find(key);
        if (it != registry_.end()) {
            if (auto ptr = it->second.lock()) {
                return ptr;
            }

            // Expired.
            registry_.erase(it);
        }

        return {};
    }

    void add(const keyT& key, const std::shared_ptr<valueT>& value) {
        registry_[key] = value;
    }

    void clean() {
        for(auto it = registry_.begin(); it != registry_.end();) {
            if (it->second.expired()) {
                static_assert(__cplusplus >= 201103L, "Require at least C++ 11");
                it = registry_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t size() const {
        return registry_.size();
    }

private:
    mutable std::map<keyT, std::weak_ptr<valueT>> registry_;
};

}}

#endif // REGISTRY_H
