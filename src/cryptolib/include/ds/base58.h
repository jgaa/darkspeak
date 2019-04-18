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

bool b58tobin_(void *bin, size_t *binszp, const std::string& b58);

template <typename T>
bool b58enc_(T& b58, const void *data, size_t binsz)
{
    static constexpr char b58digits_ordered[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

    const uint8_t *bin = reinterpret_cast<const unsigned char *>(data);;
    int carry = 0;
    size_t i = 0, j = 0, high = 0, zcount = 0;
    size_t size = 0;

    while (zcount < binsz && !bin[zcount])
        ++zcount;

    size = (binsz - zcount) * 138 / 100 + 1;
    std::vector<uint8_t> buf(size);

    for (i = zcount, high = size - 1; i < binsz; ++i, high = j)
    {
        for (carry = bin[i], j = size - 1; (j > high) || carry; --j)
        {
            carry += 256 * buf[j];
            buf[j] = carry % 58;
            carry /= 58;
        }
    }

    for (j = 0; j < size && !buf[j]; ++j);

    b58.resize(zcount + size - j);

    if (zcount) {
        for(size_t ix = 0; ix < zcount; ++ix) {
            b58[static_cast<int>(ix)] = '1';
        }
    }
    for (i = zcount; j < size; ++i, ++j)
        b58[static_cast<int>(i)] = b58digits_ordered[buf[j]];
    b58.resize(i);

    return true;
}


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
            throw std::runtime_error("Not the expected b58 version");
        }
        ++it;
    }

    T bin;
    for(; it != full.end() - 4; ++it) {
        bin.push_back(*it);
    }

    return bin;
}

template <typename Tout=std::string, typename T>
Tout b58enc(const T& in) {
    Tout out;

    if (b58enc_<Tout>(out, in.data(), in.size())) {
        return out;
    }

    return {};
}

template <typename Tout=std::string, typename T>
Tout b58check_enc(const T& data, std::initializer_list<uint8_t> ver)
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

    return b58enc<Tout>(buf);
}


}} // namespaces

#endif // BASE58_H
