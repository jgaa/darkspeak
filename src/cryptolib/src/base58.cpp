#include <string>
#include <array>
#include <vector>
#include <string.h>

#include <sodium.h>

#include "ds/base58.h"

namespace ds {
namespace crypto {

namespace {

static constexpr int8_t b58digits_map[] = {
    -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
    -1,-1,-1,-1,-1,-1,-1,-1, -1,-1,-1,-1,-1,-1,-1,-1,
    -1, 0, 1, 2, 3, 4, 5, 6,  7, 8,-1,-1,-1,-1,-1,-1,
    -1, 9,10,11,12,13,14,15, 16,-1,17,18,19,20,21,-1,
    22,23,24,25,26,27,28,29, 30,31,32,-1,-1,-1,-1,-1,
    -1,33,34,35,36,37,38,39, 40,41,42,43,-1,44,45,46,
    47,48,49,50,51,52,53,54, 55,56,57,-1,-1,-1,-1,-1,
};

static constexpr char b58digits_ordered[] = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

} // anonymous namespace

bool b58tobin_(void *bin, size_t *binszp, const std::string b58) {
    size_t binsz = *binszp;
    const auto *b58u = reinterpret_cast<const unsigned char *>(b58.c_str());
    auto *binu = reinterpret_cast<unsigned char *>(bin);
    size_t outisz = (binsz + 3) / 4;
    //uint32_t outi[outisz];
    std::vector<uint32_t> outi(outisz);
    uint64_t t = 0;
    uint32_t c = 0;
    size_t i = 0, j = 0;
    uint8_t bytesleft = binsz % 4;
    uint32_t zeromask = bytesleft ? (0xffffffff << (bytesleft * 8)) : 0;
    unsigned zerocount = 0;
    const auto b58sz = b58.size();

    // Leading zeros, just count
    for (i = 0; i < b58sz && b58u[i] == '1'; ++i)
        ++zerocount;

    for ( ; i < b58sz; ++i)
    {
        if (b58u[i] & 0x80)
            // High-bit set on invalid digit
            return false;
        if (b58digits_map[b58u[i]] == -1)
            // Invalid base58 digit
            return false;
        c = (unsigned)b58digits_map[b58u[i]];
        for (j = outisz; j--; )
        {
            t = (static_cast<uint64_t>(outi[j])) * 58 + c;
            c = (t & 0x3f00000000) >> 32;
            outi[j] = t & 0xffffffff;
        }
        if (c)
            // Output number too big (carry to the next int32)
            return false;
        if (outi[0] & zeromask)
            // Output number too big (last int32 filled too far)
            return false;
    }

    j = 0;
    switch (bytesleft) {
        case 3:
            *(binu++) = (outi[0] &   0xff0000) >> 16;
            [[fallthrough]];
        case 2:
            *(binu++) = (outi[0] &     0xff00) >>  8;
            [[fallthrough]];
        case 1:
            *(binu++) = (outi[0] &       0xff);
            ++j;
            [[fallthrough]];
        default:
            break;
    }

    for (; j < outisz; ++j)
    {
        *(binu++) = (outi[j] >> 0x18) & 0xff;
        *(binu++) = (outi[j] >> 0x10) & 0xff;
        *(binu++) = (outi[j] >>    8) & 0xff;
        *(binu++) = (outi[j] >>    0) & 0xff;
    }

    // Count canonical base58 byte count
    binu = reinterpret_cast<unsigned char *>(bin);
    for (i = 0; i < binsz; ++i)
    {
        if (binu[i])
            break;
        --*binszp;
    }
    *binszp += zerocount;

    return true;
}

bool b58enc_(std::string& b58, const void *data, size_t binsz)
{
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
            b58.at(ix) = '1';
        }
    }
    for (i = zcount; j < size; ++i, ++j)
        b58[i] = b58digits_ordered[buf[j]];
    b58.resize(i);

    return true;
}


}} // namespaces
