#include "ds/protocolcodec.h"

#include <QRegularExpression>
#include <QCryptographicHash>
#include <dsfilemanager/transfercheckpoint.h>
#include "ds/resourcelimits.h"
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

QByteArray serialize(const ds::protocol::ContactDescriptor& descriptor)
{
    return encode(descriptor);
}

bool deserialize(QByteArrayView bytes, ds::protocol::ContactDescriptor& descriptor)
{
    return decode(bytes, descriptor);
}

bool validRecordLength(const quint32 length) noexcept
{
    return length >= crypto_secretstream_xchacha20poly1305_ABYTES
            && length <= static_cast<quint32>(maxRecordBytes);
}

ds::protocol::Envelope makeCapabilities(const QStringList& names,
                                        const quint32 maxRecordBytes, const quint32 checkpointIntervalBytes)
{
    ds::protocol::Capabilities capabilities;
    capabilities.setProtocolVersion(protocolVersion);
    capabilities.setCapabilities(names);
    capabilities.setMaxRecordBytes(maxRecordBytes);
    if(names.contains(QStringLiteral("file-checkpoint-v1")))
        capabilities.setCheckpointIntervalBytes(checkpointIntervalBytes ? checkpointIntervalBytes : dsfm::default_checkpoint_interval);

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
    const bool checkpoints=seen.contains(QStringLiteral("file-checkpoint-v1"));
    if(checkpoints ? !dsfm::validCheckpointInterval(capabilities.checkpointIntervalBytes()) : capabilities.checkpointIntervalBytes()!=0)
        return fail(QStringLiteral("invalid checkpoint capability limits"));
    return true;
}
bool validateCheckpointParameters(const ds::protocol::CheckpointParameters &p) {
    return p.version()==dsfm::checkpoint_format_version && dsfm::validCheckpointInterval(p.intervalBytes()) &&
        p.attemptId().size()==16 && p.attemptId()!=QByteArray(16,'\0') && p.nextCheckpointSequence()>0 && p.nextCheckpointSequence()<(1ULL<<60);
}
bool validateCheckpointReference(const ds::protocol::CheckpointReference &c, quint64 size, quint32 interval) {
    if(!dsfm::validCheckpointInterval(interval) || size>core::ResourceLimits::hardAcceptedFileBytes ||
        c.nextOffset()>size || c.prefixSha256().size()!=32 || c.sequence()>=(1ULL<<60) ||
        (c.nextOffset()==0 ? c.sequence()!=0 : c.sequence()==0) || (c.final() && c.nextOffset()!=size) ||
        (c.nextOffset()%interval!=0 && !c.final() && !c.pauseBoundary()))return false;
    return c.nextOffset()!=0 || c.prefixSha256()==QCryptographicHash::hash(QByteArray{},QCryptographicHash::Sha256);
}
dsfm::Result<dsfm::TransferAttempt> checkpointAttempt(const ds::protocol::CheckpointParameters &parameters,
    const ds::protocol::CheckpointReference &selected,quint64 size) {
    if(!validateCheckpointParameters(parameters) || !validateCheckpointReference(selected,size,parameters.intervalBytes()) ||
        parameters.nextCheckpointSequence()<=selected.sequence())return dsfm::Error{dsfm::ErrorCode::InvalidArgument};
    return dsfm::TransferAttempt{QUuid::fromRfc4122(parameters.attemptId()),parameters.intervalBytes(),parameters.nextCheckpointSequence(),
        {selected.sequence(),selected.nextOffset(),selected.prefixSha256(),selected.final(),selected.pauseBoundary()}};
}
ds::protocol::CheckpointParameters checkpointParameters(const dsfm::TransferAttempt &attempt) {
    ds::protocol::CheckpointParameters parameters;parameters.setVersion(dsfm::checkpoint_format_version);
    parameters.setIntervalBytes(attempt.interval);parameters.setAttemptId(attempt.id.toRfc4122());
    parameters.setNextCheckpointSequence(attempt.next_sequence);return parameters;
}
ds::protocol::CheckpointReference checkpointReference(const dsfm::DurableCheckpoint &checkpoint) {
    ds::protocol::CheckpointReference reference;reference.setSequence(checkpoint.sequence);reference.setNextOffset(checkpoint.offset);
    reference.setPrefixSha256(checkpoint.sha256);reference.setFinal(checkpoint.final);reference.setPauseBoundary(checkpoint.pause_boundary);
    return reference;
}
bool validateResumeCheckpoints(const ds::protocol::FileResume &resume) {
    if(resume.checkpointVersion()!=1 || !dsfm::validCheckpointInterval(resume.checkpointIntervalBytes()) ||
        resume.fileId().size()!=16 || resume.fileId()==QByteArray(16,'\0') || resume.sha256().size()!=32 ||
        resume.fileSize()>core::ResourceLimits::hardAcceptedFileBytes || resume.nextOffset()>resume.fileSize() ||
        resume.availableCheckpoints().isEmpty() || resume.availableCheckpoints().size()>dsfm::maximum_retained_checkpoints)return false;
    QSet<quint64> sequences;bool zero=false;
    for(const auto &reference:resume.availableCheckpoints()) {
        if(!validateCheckpointReference(reference,resume.fileSize(),resume.checkpointIntervalBytes()) || sequences.contains(reference.sequence()))return false;
        sequences.insert(reference.sequence());zero|=reference.nextOffset()==0;
    }
    return zero;
}
bool validateFileCheckpoint(const ds::protocol::FileCheckpoint &c, quint64 size, quint32 interval) {
    const auto phase=int(c.phase());
    return c.fileId().size()==16 && c.fileId()!=QByteArray(16,'\0') && c.attemptId().size()==16 && c.attemptId()!=QByteArray(16,'\0') &&
        c.expectedSha256().size()==32 && c.hasCheckpoint() && phase>=1 && phase<=3 && validateCheckpointReference(c.checkpoint(),size,interval);
}
bool validateFilePause(const ds::protocol::FilePause &p, quint64 size, quint32 interval) {
    const auto reason=int(p.reason());
    return p.fileId().size()==16 && p.fileId()!=QByteArray(16,'\0') && p.attemptId().size()==16 && p.attemptId()!=QByteArray(16,'\0') &&
        reason>=1 && reason<=6 && p.hasConfirmedCheckpoint() && validateCheckpointReference(p.confirmedCheckpoint(),size,interval);
}

} // namespace ds::prot::codec
