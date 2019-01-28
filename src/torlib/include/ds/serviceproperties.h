#ifndef SERVICEPROPERTIES_H
#define SERVICEPROPERTIES_H

#include <QMetaType>
#include <QByteArray>

namespace ds {
namespace tor {

class TorCtlReply;

struct ServiceProperties {
    QByteArray id; // Our id
    QByteArray service_id; // Onion site, without the .onion postfix
    QByteArray key_type; // Key type, currently RSA1024 or ED25519-V3
    QByteArray key; // Tor service private key, in binary format.
    uint16_t service_port = {}; // Service port on the Tor network
    uint16_t app_port = {}; // Local port for Tor to forward connections to
};

}} // namespaces

Q_DECLARE_METATYPE(ds::tor::ServiceProperties);

#endif // SERVICEPROPERTIES_H
