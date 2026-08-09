#ifndef TORCONFIG_H
#define TORCONFIG_H

#include <QHostAddress>
#include <QDir>
#include <QString>
#include <QStringList>

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

    Mode mode = Mode::PRIVATE;
    QString tor_command = QStringLiteral("tor");
    uint16_t ctl_port = 9051;
    QHostAddress ctl_host = QHostAddress::LocalHost;
    QString ctl_socket;
    QString ctl_passwd;
    QVector<QByteArray> allowed_auth_methods = {"HASHEDPASSWORD", "SAFECOOKIE"};
    QHostAddress socks_host = QHostAddress::LocalHost;
    uint16_t socks_port = 9050;
    QString socks_socket;
    uint16_t service_from_port = 1025;
    uint16_t service_to_port = 29999;
    QHostAddress app_host = QHostAddress::LocalHost;
    //uint16_t app_port = 29998;

    static QStringList privateArguments(const QString& dataDir,
                                        const QString& controlSocket,
                                        const QString& socksSocket,
                                        quint16 socksPort)
    {
        const auto cookie = QDir(dataDir).filePath(QStringLiteral("control_auth_cookie"));
        return {
            QStringLiteral("--DataDirectory"), dataDir,
            QStringLiteral("--SocksPort"), QStringLiteral("unix:%1").arg(socksSocket),
            QStringLiteral("--SocksPort"), QStringLiteral("127.0.0.1:%1").arg(socksPort),
            QStringLiteral("--ControlPort"), QStringLiteral("unix:%1").arg(controlSocket),
            QStringLiteral("--CookieAuthentication"), QStringLiteral("1"),
            QStringLiteral("--CookieAuthFile"), cookie,
            QStringLiteral("--CookieAuthFileGroupReadable"), QStringLiteral("0"),
            QStringLiteral("--ControlSocketsGroupWritable"), QStringLiteral("0"),
            QStringLiteral("--UnixSocksGroupWritable"), QStringLiteral("0")
        };
    }
};

}} // namespaces

#endif // TORCONFIG_H
