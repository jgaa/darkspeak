#ifndef SAFEMEMORY_H
#define SAFEMEMORY_H

#include <QByteArray>
#include <stdexcept>
#include <cassert>
#include <string.h>
#include <sodium.h>

namespace ds {
namespace crypto {

/*! A container designed for secrets.
 *
 * Data in the contaier will be attempted to be locked
 * in memory (to prevent cryptographic keys etc. to
 * be written to swap). It will also safely erase the
 * data when the object is deleted.
 *
 * This class is designed for safety, not performance.
 */
template <typename T = char>
class SafeMemory {
public:
    using value_type = T;

    SafeMemory() = default;
    ~SafeMemory() {
        clear();
    }

    SafeMemory(const size_t bytes) {
        resize(bytes);
    }

    explicit SafeMemory(const QByteArray& v) {
        assign(reinterpret_cast<const T *>(v.constData()),
               static_cast<size_t>(v.size()));
    }

    SafeMemory(const T* data, const size_t bytes) {
        assign(data, bytes);
    }

    SafeMemory(const SafeMemory& v) {
        assign(v);
    }

    SafeMemory(SafeMemory&& v) {
        swap(v);
    }

    SafeMemory& operator = (const SafeMemory& v) {
        assign(v);
        return *this;
    }

    SafeMemory& operator = (SafeMemory&& v) {
        swap(v);
    }

    bool operator == (const SafeMemory& v) const noexcept {
        if (size() == v.size()) {
            return memcmp(data_, v.data_, len_) == 0;
        }
        return false;
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

    void clear() noexcept {
        if (!empty()) {
            sodium_free(data_);
            data_ = nullptr;
            len_ = {};
        }
    }

    void resize(const size_t bytes) {
        if (bytes > len_) {
            auto d = static_cast<T *>(sodium_malloc(sizeof(T) * bytes));
            if (d == nullptr) {
                throw std::bad_alloc();
            }
            if (len_) {
                memcpy(d, data_, len_);
                clear();
            }
            data_ = d;
        } else {
            const auto diff = bytes - len_;
            const auto upper = len_ - diff;
            if (diff) {
                sodium_memzero(data_ + upper, diff);
            }
        }
        len_ = bytes;
    }

    void assign(const T* data, const size_t bytes) {
        clear();
        append(data, bytes);
    }

    void assign(const SafeMemory& v) {
        clear();
        append(v.cdata(), v.size());
    }

    void append(const T* data, const size_t bytes) {
        if (!bytes) {
            return;
        }

        auto start = len_;
        resize(bytes);
        memcpy(data_ + start, data, bytes);
    }

    template <typename Tc>
    void append(const Tc& v) {
        append(reinterpret_cast<const T*>(v.data()), v.size());
    }

    void swap(SafeMemory& v) {
        auto d = data_;
        data_ = v.data_;
        v.data_ = d;

        auto len = len_;
        len_ = v.len_;
        v.len_ = len;
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

private:
    T *data_ = nullptr;
    size_t len_ = {};
};

}} // namespaces

#endif // SAFEMEMORY_H
