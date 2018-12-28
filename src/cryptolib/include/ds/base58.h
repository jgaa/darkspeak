#pragma once

#ifndef BASE58_H
#define BASE58_H

/* Copyright 2018 Jarle (jgaa) Aase
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the standard MIT license.  See LICENSE for more details.
 *
 * Based on code from Luke Dashjr
 * https://github.com/luke-jr/libbase58
 */

/*
 * Copyright 2012-2014 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the standard MIT license.  See COPYING for more details.
 */


#include <string>
#include <array>
#include <vector>
#include <string.h>

#include <sodium.h>

#include "ds/base58.h"

namespace ds {
namespace crypto {

bool b58tobin_(void *bin, size_t *binszp, const std::string b58);
bool b58enc_(std::string& b58, const void *data, size_t binsz);

template <typename T>
T b58tobin(const std::string& in, size_t bytes) {
    T out;
    out.resize(bytes);
    size_t real_size = out.size();

    if (b58tobin_(out.data(), &real_size, in)) {
        out.resize(real_size);
        return out;
    }

    return {};
}

template <typename T>
bool b58check(const T& in, const std::string& base58)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> hash;
    unsigned i;
    if (in.size() < 4)
        return false;
    crypto_hash_sha256(hash.data(),
                       reinterpret_cast<const unsigned char *>(in.data()), in.size() - 4);
    if (memcmp(in.data() + (in.size() - 4), hash.data(), 4)) {
        return false;
    }

    // Check number of zeros is correct AFTER verifying checksum (to avoid possibility of accessing base58str beyond the end)
    for (i = 0; in.at(i) == '\0' && base58.at(i) == '1'; ++i)
        ; // Just finding the end of zeros, nothing to do in loop

    if (in.at(i) == '\0' || base58.at(i) == '1') {
        return false;
    }

    return true;
}

template <typename T>
T b58tobin_check(const std::string& in, size_t bytes, std::initializer_list<uint8_t> ver) {

    auto full = b58tobin<T>(in, bytes + ver.size() + 4);

    if (!b58check(full, in)) {
        return {};
    }

    auto it = full.cbegin();
    for(auto v : ver) {
        if (v != static_cast<uint8_t>(*it)) {
            return {}; // Not the expected version
        }
        ++it;
    }

    T bin;
    for(; it != full.end() - 4; ++it) {
        bin.push_back(*it);
    }

    return bin;
}

template <typename T>
std::string b58enc(const T& in) {
    std::string out;

    if (b58enc_(out, in.data(), in.size())) {
        return out;
    }

    return {};
}

template <typename T>
std::string b58check_enc(const T& data, std::initializer_list<uint8_t> ver)
{
    const auto bufsize = data.size() + ver.size() + 4;
    std::vector<unsigned char> buf;
    buf.reserve(bufsize);

    for(auto const v : ver) {
        buf.push_back(v);
    }

    for(const auto c : data) {
        buf.push_back(c);
    }

    std::array<unsigned char, crypto_hash_sha256_BYTES> hash;
    crypto_hash_sha256(hash.data(), buf.data(), buf.size());
    std::copy(hash.begin(), hash.begin() + 4, std::back_inserter(buf));

    return b58enc(buf);
}


}} // namespaces

#endif // BASE58_H
