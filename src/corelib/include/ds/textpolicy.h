#pragma once

#include <QByteArrayView>
#include <QStringView>
#include <cstdint>

namespace ds::core::text_policy {

inline bool permittedAscii(QStringView text, bool multiline = true)
{
    for (const auto ch : text) {
        const auto code = ch.unicode();
        if (code > 126 || (code < 32 && !(multiline && (code == 9 || code == 10 || code == 13)))) return false;
    }
    return true;
}

// Inspect only the known text fields of protocol v3, before QtProtobuf creates
// QStrings. Byte fields (signatures, keys, chunks, hashes) must never be treated
// as text. Fixed nesting and a local field budget bound the work independently
// of peer-selected field numbers, lengths and repetition.
namespace detail {
enum class Node { envelope, bio_response, connection_request, connection_response, message, file_offer, user_info, bio };
inline bool varint(QByteArrayView bytes, qsizetype &offset, uint64_t &value)
{
    value = 0;
    for (unsigned shift = 0; shift < 70; shift += 7) {
        if (offset >= bytes.size()) return false;
        const auto byte = static_cast<unsigned char>(bytes[offset++]);
        if (shift == 63 && byte > 1) return false;
        value |= uint64_t(byte & 127) << shift;
        if (!(byte & 128)) return true;
    }
    return false;
}
inline bool scan(QByteArrayView bytes, Node node, unsigned &budget)
{
    qsizetype offset = 0;
    while (offset < bytes.size()) {
        if (budget == 0) return false;
        --budget;
        uint64_t tag = 0;
        if (!varint(bytes, offset, tag) || tag >> 3 == 0 || tag >> 3 > 0x1fffffff) return false;
        const auto field = tag >> 3;
        const auto wire = tag & 7;
        uint64_t length = 0;
        if (wire == 0) { if (!varint(bytes, offset, length)) return false; continue; }
        if (wire == 1 || wire == 5) {
            const qsizetype size = wire == 1 ? 8 : 4;
            if (bytes.size() - offset < size) return false;
            offset += size;
            continue;
        }
        if (wire != 2 || !varint(bytes, offset, length)
            || length > static_cast<uint64_t>(bytes.size() - offset)) return false;
        const auto value = bytes.sliced(offset, static_cast<qsizetype>(length));
        offset += static_cast<qsizetype>(length);
        bool text = false;
        bool multiline = false;
        qsizetype maximum = 0;
        switch (node) {
        case Node::envelope: {
            Node child;
            switch (field) {
            case 13: child = Node::bio_response; break;
            case 14: child = Node::connection_request; break;
            case 15: child = Node::connection_response; break;
            case 16: child = Node::message; break;
            case 17: child = Node::file_offer; break;
            case 20: child = Node::user_info; break;
            default: continue;
            }
            if (!scan(value, child, budget)) return false;
            continue;
        }
        case Node::bio_response:
            if (field == 1 && !scan(value, Node::bio, budget)) return false;
            continue;
        case Node::connection_request:
            text = field == 1 || field == 2;
            maximum = field == 1 ? 128 : 4096;
            multiline = field == 2;
            break;
        case Node::connection_response: text = field == 2; maximum = 4096; multiline = true; break;
        case Node::message: text = field == 4; maximum = 65536; multiline = true; break;
        case Node::file_offer: text = field == 3; maximum = 1024; break;
        case Node::user_info:
            if (field == 4 && !scan(value, Node::bio, budget)) return false;
            text = field == 1;
            maximum = 128;
            break;
        case Node::bio: text = field == 1; maximum = 4096; multiline = true; break;
        }
        if (!text) continue;
        if (value.size() > maximum) return false;
        for (const auto character : value) {
            const auto code = static_cast<unsigned char>(character);
            if (code > 126 || (code < 32 && !(multiline && (code == 9 || code == 10 || code == 13)))) return false;
        }
    }
    return true;
}
}
inline bool permittedAsciiEnvelope(QByteArrayView bytes)
{
    if (bytes.size() > 1'048'576) return false;
    unsigned budget = 4096;
    return detail::scan(bytes, detail::Node::envelope, budget);
}
}
