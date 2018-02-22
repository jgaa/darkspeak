#ifndef TORMGR_H
#define TORMGR_H

#include <QObject>
#include <QHostAddress>

namespace ds {
namespace tor {

/*! Manages the access to a Tor server
 *
 */
class TorMgr : public QObject
{
    Q_OBJECT

    struct Config {
        enum class Mode {
            SYSTEM, // use system or server
            PRIVATE // use our own instance of the tor server
        };

        Mode mode = Mode::SYSTEM;
        uint16_t ctl_port = {};
        QHostAddress ctl_host = QHostAddress::LocalHost;
        QString ctl_passwd;
    };
public:
    explicit TorMgr(const Config& config, QObject *parent = nullptr);

signals:

public slots:
    void start();


private:
    void startUseSystemInstance();

    Config config_;
};

}} // namespaces

#endif // TORMGR_H
