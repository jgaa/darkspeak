#include "ds/protocolcodec.h"

#include <QRegularExpression>
#include <QSet>
#include <QtProtobuf/qprotobufserializer.h>

#include <utility>

namespace ds::prot::codec {

namespace {

QProtobufSerializer& serializer()
{
    // QProtobufSerializer is stateless apart from its last error. Keeping one
    // instance per thread avoids sharing mutable error state between socket
    // callbacks while avoiding repeated allocations on the record path.
    thread_local QProtobufSerializer value;
    value.shouldPreserveUnknownFields(true);
    return value;
}

template<typename Message>
QByteArray encode(const Message& message)
{
    return serializer().serialize(&message);
}

template<typename Message>
bool decode(QByteArrayView bytes, Message& message)
{
    return serializer().deserialize(&message, bytes);
}

} // namespace

QByteArray serialize(const ds::protocol::Envelope& envelope)
{
    return encode(envelope);
}

bool deserialize(QByteArrayView bytes, ds::protocol::Envelope& envelope)
{
    return decode(bytes, envelope);
}

QByteArray serialize(const ds::protocol::TransportEnvelope& envelope)
{
    return encode(envelope);
}

bool deserialize(QByteArrayView bytes, ds::protocol::TransportEnvelope& envelope)
{
    return decode(bytes, envelope);
}

bool validRecordLength(const quint32 length) noexcept
{
    return length >= crypto_secretstream_xchacha20poly1305_ABYTES
            && length <= static_cast<quint32>(maxRecordBytes);
}

ds::protocol::Envelope makeCapabilities(const QStringList& names,
                                        const quint32 maxRecordBytes)
{
    ds::protocol::Capabilities capabilities;
    capabilities.setProtocolVersion(protocolVersion);
    capabilities.setCapabilities(names);
    capabilities.setMaxRecordBytes(maxRecordBytes);

    ds::protocol::Envelope envelope;
    envelope.setProtocolVersion(protocolVersion);
    envelope.setCapabilities(std::move(capabilities));
    return envelope;
}

bool validateCapabilities(const ds::protocol::Envelope& envelope, QString* error)
{
    const auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (envelope.protocolVersion() != protocolVersion || !envelope.hasCapabilities()) {
        return fail(QStringLiteral("invalid capabilities envelope"));
    }
    if (envelope.hasRequestId() || envelope.hasResponseToRequestId()) {
        return fail(QStringLiteral("capabilities cannot be correlated"));
    }

    const auto& capabilities = envelope.capabilities();
    if (capabilities.protocolVersion() != protocolVersion
            || !validRecordLength(capabilities.maxRecordBytes())
            || capabilities.maxRecordBytes() < static_cast<quint32>(minPeerRecordBytes)) {
        return fail(QStringLiteral("invalid capabilities limits"));
    }

    static const QRegularExpression namePattern{
        QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$")};
    if (capabilities.capabilities().size() > 32) {
        return fail(QStringLiteral("too many capabilities"));
    }

    QSet<QString> seen;
    for (const auto& name : capabilities.capabilities()) {
        if (!namePattern.match(name).hasMatch() || seen.contains(name)) {
            return fail(QStringLiteral("invalid or duplicate capability"));
        }
        seen.insert(name);
    }
    return true;
}

} // namespace ds::prot::codec
