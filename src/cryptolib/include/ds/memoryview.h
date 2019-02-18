#ifndef MEMORYVIEW_H
#define MEMORYVIEW_H

#include <cstddef>
#include <stdexcept>

#include <QByteArray>

namespace ds {
namespace crypto {

/*! A container-like construct that reflects data from
 * another object.
 */
template <typename T = char, typename Tptr = T*>
class MemoryView {
public:
    using value_type = T;

    MemoryView() = default;

    template<typename Tc>
    MemoryView(Tc& v)
        : data_{reinterpret_cast<Tptr>(v.data())}, len_{static_cast<size_t>(v.size())} {}

    template<typename Tc>
    MemoryView(const Tc& v)
        : data_{reinterpret_cast<Tptr>(v.data())}, len_{static_cast<size_t>(v.size())} {}

    MemoryView(void *d, size_t len)
        : data_{static_cast<Tptr>(d)}, len_{len} {}

    template<typename Tc>
    MemoryView(void *d, size_t len)
        : data_{static_cast<Tptr>(d)}, len_{len} {}

    bool operator == (const MemoryView& v) const noexcept {
        if (size() == v.size()) {
            return memcmp(data_, v.data_, len_) == 0;
        }
        return false;
    }

    void assign(Tptr data, const size_t len) {
        data_ = data;
        len_ = len;
    }

    template <typename Tc>
    void assign(Tc& v) {
        data_ = v.data();
        len_ = v.size();
    }

    bool empty() const noexcept {
        return len_ == 0;
    }

    bool isEmpty() const noexcept {
        return empty();
    }

    size_t size() const noexcept {
        return len_;
    }

    const T* cdata() const noexcept {
        return data_;
    }

    T* data() noexcept {
        return data_;
    }

    const T* data() const noexcept {
        return data_;
    }

    T& at(size_t pos) {
        if (pos >= len_) {
            throw std::out_of_range("at()");
        }
        return data_[pos];
    }

    T at(size_t pos) const {
        if (pos >= len_) {
            throw std::out_of_range("at()");
        }
        return data_[pos];
    }

    T* begin() noexcept {
        return data_;
    }

    T* end() noexcept {
        return data_ + len_;
    }

    const T* begin() const noexcept {
        return data_;
    }

    const T* end() const noexcept {
        return data_ + len_;
    }

    const T* cbegin() const noexcept {
        return data_;
    }

    const T* cend() const noexcept {
        return data_ + len_;
    }

    QByteArray toByteArray() const {
        return {reinterpret_cast<const char *>(data_),
                    static_cast<int>(len_)};
    }

    Tptr data_ = {};
    size_t len_ = {};
};


}} // namespaces

#endif // MEMORYVIEW_H
