#ifndef TORCONFIG_H
#define TORCONFIG_H

#include <QHostAddress>

namespace ds {
namespace tor {

struct TorConfig {
    enum class Mode {
        SYSTEM, // use system or server
        PRIVATE // use our own instance of the tor server
    };

    ~TorConfig() {
        for(auto ch : ctl_passwd) {
            ch = ' ';
        }
        ctl_passwd.clear();
    }

    Mode mode = Mode::SYSTEM;
    uint16_t ctl_port = 9051;
    QHostAddress ctl_host = QHostAddress::LocalHost;
    QString ctl_passwd;
    QVector<QByteArray> allowed_auth_methods = {"HASHEDPASSWORD", "SAFECOOKIE"};
    uint16_t service_from_port = 1025;
    uint16_t service_to_port_ = 29999;
};

}} // namespaces

#endif // TORCONFIG_H
