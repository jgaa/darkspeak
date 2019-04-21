#ifndef BYTES_H
#define BYTES_H

#include <cassert>
#include <algorithm>

namespace ds {
namespace core {

template <typename valueT, typename bufT>
void valueToBytes(const valueT value, bufT& buffer) {

    // The buffer must be correctly sized prior to this operation
    assert(sizeof(value) == buffer.size());

    auto bytes = static_cast<const char *>(static_cast<const void *>(&value));
    //std::copy(bytes, bytes + sizeof(bufT), buffer.data());
    for(size_t i = 0; i < sizeof(value); ++i) {
        buffer.at(i) = bytes[i];
    }
}

template <typename valueT, typename bufT>
void appendValueToBytes(const valueT value, bufT& buffer) {
    auto bytes = static_cast<const char *>(static_cast<const void *>(&value));
    for(size_t i = 0; i < sizeof(valueT); ++i) {
        buffer += bytes[i];
    }
}

template <typename valueT, typename bufT>
valueT bytesToValue(bufT& buffer) {

    // The buffer must be correctly sized prior to this operation
    assert(sizeof(valueT) == buffer.size());

    auto value = static_cast<const valueT *>(static_cast<const void *>(buffer.data()));
    assert(value != nullptr);
    return *value;
}

}}


#endif // BYTES_H
