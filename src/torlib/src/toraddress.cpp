#include "ds/toraddress.h"

#include <QCryptographicHash>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace ds::tor {
namespace {
constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";

QByteArray decodeBase32(const QString& input)
{
    QByteArray output;
    uint32_t accumulator = 0;
    int bits = 0;
    for (const auto character : input) {
        const auto index = QByteArray(alphabet).indexOf(character.toLatin1());
        if (index < 0) {
            throw std::invalid_argument("Invalid Tor v3 base32 character");
        }
        accumulator = (accumulator << 5) | index;
        bits += 5;
        if (bits >= 8) {
            bits -= 8;
            output.append(static_cast<char>((accumulator >> bits) & 0xff));
            accumulator &= bits ? ((1u << bits) - 1u) : 0u;
        }
    }
    return output;
}
}

bool TorAddress::isValidServiceId(const QString& value) noexcept
{
    try {
        if (value.size() != 56 || value != value.toLower()) {
            return false;
        }
        const auto decoded = decodeBase32(value);
        if (decoded.size() != 35 || static_cast<unsigned char>(decoded.at(34)) != 3) {
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

TorAddress TorAddress::parse(const QString& raw)
{
    auto value = raw.trimmed();
    if (value.startsWith(QStringLiteral("onion:"), Qt::CaseInsensitive)) {
        value = value.mid(6);
    }

    const auto separator = value.lastIndexOf(':');
    if (separator <= 0 || separator == value.size() - 1 || value.indexOf(':') != separator) {
        throw std::invalid_argument("Tor address must be host:port");
    }

    auto host = value.left(separator);
    if (host.endsWith(QStringLiteral(".onion"), Qt::CaseInsensitive)) {
        host.chop(6);
    }
    host = host.toLower();

    bool ok = false;
    const auto port = value.mid(separator + 1).toUShort(&ok);
    if (!ok || port == 0 || !isValidServiceId(host)) {
        throw std::invalid_argument("Tor address is not a valid v3 address");
    }

    return {host, port};
}

QString TorAddress::host() const
{
    return service_id + QStringLiteral(".onion");
}

QString TorAddress::toString() const
{
    if (!isValidServiceId(service_id) || port == 0) {
        throw std::logic_error("Cannot format invalid Tor v3 address");
    }
    return host() + ':' + QString::number(port);
}

} // namespace ds::tor
