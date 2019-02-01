#ifndef TRANSPORTHANDLE_H
#define TRANSPORTHANDLE_H

#include <QString>
#include <QVariantMap>
#include <QtGui/QImage>
#include <QUuid>

namespace ds {
namespace core {

struct TransportHandle {
    QString identityName;
    QUuid uuid;

    /*! Handle in clear text
     *
     * This is whart we present to contacts
     */
    QByteArray handle;

    /*! Data needed by the protocol layer to initialize the transport
     *
     * For Tor hidden services this includes the private key, onion address and
     * port for the hidden service.
     */
    QVariantMap data;
};

struct TransportHandleReq {
    QString identityName;
    QUuid uuid;
};

struct TransportHandleError {
    QString identityName;
    QUuid uuid;
    QString explanation;
};

}} // namespaces

#endif // TRANSPORTHANDLE_H
