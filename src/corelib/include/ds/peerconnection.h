#ifndef PEERCONNECTION_H
#define PEERCONNECTION_H

#include <memory>

#include <QUuid>
#include <QObject>

#include "ds/dscert.h"

namespace ds{
namespace core {

/*! Representation of a incoming or outgoing connection to a contact.
 *
 *  Owned by the protocol module. Lifetime is purely managed as std::shared_ptr.
 */

class PeerConnection
        : public QObject
        , public std::enable_shared_from_this<PeerConnection>
{
    Q_OBJECT

public:
    using ptr_t = std::shared_ptr<PeerConnection>;

    PeerConnection() = default;

    enum Direction { INCOMING, OUTGOING };

    virtual QUuid getConnectionId() const = 0;
    virtual void authorize(bool allow) = 0;
    virtual Direction getDirection() const noexcept = 0;
    virtual crypto::DsCert::ptr_t getPeerCert() const noexcept = 0;
    virtual QUuid getIdentityId() const noexcept = 0;
    virtual void close() = 0;

signals:
    void connectedToPeer(PeerConnection *peer);
    void disconnectedFromPeer(PeerConnection *peer);
    void receivedData(const quint32 channel, const quint64 id, const QByteArray& data);
};

}}

Q_DECLARE_METATYPE(ds::core::PeerConnection *)

#endif // PEERCONNECTION_H
