#ifndef DS_PROTOCOLCODEC_H
#define DS_PROTOCOLCODEC_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>
#include <sodium.h>

#include "darkspeak.qpb.h"

namespace ds::prot::codec {

inline constexpr quint32 protocolVersion = 3;
inline constexpr qsizetype maxRecordBytes = 1'048'576;
inline constexpr qsizetype minPeerRecordBytes = 4'096;

// The generated QtProtobuf serializer is deliberately kept behind this small
// boundary. Callers must validate the decoded object before dispatching it.
QByteArray serialize(const ds::protocol::Envelope& envelope);
bool deserialize(QByteArrayView bytes, ds::protocol::Envelope& envelope);

QByteArray serialize(const ds::protocol::TransportEnvelope& envelope);
bool deserialize(QByteArrayView bytes, ds::protocol::TransportEnvelope& envelope);

QByteArray serialize(const ds::protocol::ContactDescriptor& descriptor);
bool deserialize(QByteArrayView bytes, ds::protocol::ContactDescriptor& descriptor);

bool validRecordLength(quint32 length) noexcept;

ds::protocol::Envelope makeCapabilities(const QStringList& names,
                                        quint32 maxRecordBytes);
bool validateCapabilities(const ds::protocol::Envelope& envelope,
                          QString* error = nullptr);

} // namespace ds::prot::codec

#endif // DS_PROTOCOLCODEC_H
