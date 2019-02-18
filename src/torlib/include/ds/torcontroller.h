#ifndef TORCONTROLLER_H
#define TORCONTROLLER_H

#include <memory>
#include <random>

#include "ds/torconfig.h"
#include "ds/torctlsocket.h"
#include "ds/serviceproperties.h"

namespace ds {
namespace tor {

struct TorCtlReply;

class TorController : public QObject
{
    Q_OBJECT

public:
    enum class CtlState {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        ONLINE,
        STOPPING,
        STOPPED
    };

    enum class TorState {
        UNKNOWN,
        INITIALIZING,
        READY
    };

    struct SecurityError : public std::runtime_error
    {
        SecurityError(const char *what) : std::runtime_error(what) {}
    };

    struct AuthError : public std::runtime_error
    {
        AuthError(const char *what) : std::runtime_error(what) {}
    };

    struct IoError : public std::runtime_error
    {
        IoError(const char *what) : std::runtime_error(what) {}
    };

    struct CryptoError : public std::runtime_error
    {
        CryptoError(const char *what) : std::runtime_error(what) {}
    };

    struct TorError : public std::runtime_error
    {
        TorError(const char *what) : std::runtime_error(what) {}
    };

    struct ParseError : public std::runtime_error
    {
        ParseError(const char *what) : std::runtime_error(what) {}
    };

    struct NoSuchServiceError : public std::runtime_error
    {
        NoSuchServiceError(const char *what) : std::runtime_error(what) {}
    };

    TorController(const TorConfig& config);

    CtlState getCtlState() const;
    TorState getTorState() const;
    bool isConnected() const {
        const auto state = getCtlState();
        return (state == CtlState::CONNECTED)
                || (state == CtlState::ONLINE);

    }

signals:
    void torStateUpdate(TorState state, int progress, const QString& summary);
    void stateUpdate(CtlState state);
    void autenticated();
    void authFailed(const QString& reason);
    void serviceCreated(const ServiceProperties& service);
    void serviceFailed(const QUuid& service, const QByteArray& reason);
    void serviceStarted(const QUuid& service, const bool newService);
    void serviceStopped(const QUuid& service);

    // Emitted when we are authenticated and Tor is connected.
    void ready();

    // Emitted when the tor service has been shut down.
    void stopped();

public slots:
    void start(); // Connect to Tor server
    void stop(); // Disconnect from Tor server

    /*! Create a hidden service
     *
     * This will create and start a Tor hidden service.
     *
     * Signals:
     *  - serviceCreated and serviceStarted if sucessful
     *  - serviceFailed if the service was not created or failed to start.
     */
    void createService(const QUuid& service);

    /*! Start a hidden service
     *
     * This will start a Tor hidden service using the key provided in the service argument.
     *
     * Signals:
     *  - serviceStarted is sucessful
     *  - serviceFailed if the service failed to start.
     */
    void startService(const ServiceProperties& service);

    /*! Stop a Tor hidden service
     *
     * \param id The Tor service id to stop. Corresponds
     *      to ServiceProperties::id.
     *
     * Signals:
     *  - serviceStopped
     */
    void stopService(const QUuid& service);


private slots:
    void startAuth();
    void close();
    void clear(); // Disconnected - Clean up
    void torEvent(const TorCtlReply& reply);

protected:
    void setState(CtlState state);
    void setState(TorState state, int progress = -1, const QString& summary = {});
    void DoAuthentcate(const TorCtlReply& reply);
    void Authenticate(const QByteArray& data);
    void OnAuthReply(const TorCtlReply& reply);
    QByteArray GetCookie(const QString& path);
    QByteArray ComputeHmac(const QByteArray& key, const QByteArray& serverNonce);

private:
    CtlState ctl_state_ = CtlState::DISCONNECTED;
    TorState tor_state_ = TorState::UNKNOWN;
    std::unique_ptr<TorCtlSocket> ctl_;
    TorConfig config_;
    QByteArray client_nonce_;
    QByteArray cookie_;
    static const QByteArray tor_safe_serverkey_;
    static const QByteArray tor_safe_clientkey_;
    std::mt19937 rnd_eng_;
    QMap<QUuid, QByteArray> service_map_;
};

}} // namespaces

#endif // TORCONTROLLER_H
