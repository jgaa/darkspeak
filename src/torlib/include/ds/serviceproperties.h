#ifndef SERVICEPROPERTIES_H
#define SERVICEPROPERTIES_H

#include <QMetaType>
#include <QByteArray>
#include <QString>
#include <QUuid>

#include "ds/toraddress.h"

namespace ds {
namespace tor {

struct TorCtlReply;

struct ServiceProperties {
    QUuid uuid;
    QString name;
    QString service_id; // Tor v3 identifier, without the .onion postfix
    QString key_type = QStringLiteral("ED25519-V3");
    QString key; // Tor service private key, in binary format.
    uint16_t service_port = {}; // Service port on the Tor network
    uint16_t app_port = {}; // Local port for Tor to forward connections to
};

}} // namespaces

Q_DECLARE_METATYPE(ds::tor::ServiceProperties);

#endif // SERVICEPROPERTIES_H
