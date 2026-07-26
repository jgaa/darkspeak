#include "ds/base32.h"

#include <QCryptographicHash>

#include <cstdint>
#include <stdexcept>

namespace ds::crypto {
namespace {
constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
}

QByteArray onionv3decode(const QByteArray& src)
{
    if (src.size() != 56) {
        throw std::runtime_error("onionv3decode: source size must be 56");
    }
    QByteArray out;
    uint32_t accumulator = 0;
    int bits = 0;
    for (const auto ch : src.toLower()) {
        const auto index = QByteArray(alphabet).indexOf(ch);
        if (index < 0) {
            throw std::runtime_error("onionv3decode: invalid base32 character");
        }
        accumulator = (accumulator << 5) | static_cast<uint32_t>(index);
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            out.append(static_cast<char>((accumulator >> bits) & 0xff));
            accumulator &= bits ? ((1u << bits) - 1u) : 0u;
        }
    }
    if (out.size() != 35) {
        throw std::runtime_error("onionv3decode: invalid decoded size");
    }
    return out;
}

QByteArray onionv3encode(const QByteArray& src)
{
    if (src.size() != 35) {
        throw std::runtime_error("onionv3encode: source size must be 35");
    }
    QByteArray out(56, '\0');
    uint32_t accumulator = 0;
    int bits = 0;
    int index = 0;
    for (const auto byte : src) {
        accumulator = (accumulator << 8) | static_cast<unsigned char>(byte);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out[index++] = alphabet[(accumulator >> bits) & 31];
            accumulator &= bits ? ((1u << bits) - 1u) : 0u;
        }
    }
    if (bits > 0) {
        out[index++] = alphabet[(accumulator << (5 - bits)) & 31];
    }
    if (index != out.size()) {
        throw std::runtime_error("onionv3encode: invalid encoded size");
    }
    return out;
}

bool onionv3isValid(const QByteArray& src) noexcept
{
    try {
        const auto decoded = onionv3decode(src);
        if (static_cast<unsigned char>(decoded.at(34)) != 3) {
            return false;
        }
        QByteArray checksumInput(".onion checksum", 15);
        checksumInput += decoded.left(32);
        checksumInput += static_cast<char>(3);
        const auto digest = QCryptographicHash::hash(checksumInput, QCryptographicHash::Sha3_256);
        return decoded.mid(32, 2) == digest.left(2);
    } catch (...) {
        return false;
    }
}

} // namespace ds::crypto
