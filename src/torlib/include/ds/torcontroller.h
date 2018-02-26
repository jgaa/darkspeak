#ifndef TORCONTROLLER_H
#define TORCONTROLLER_H

#include <memory>

#include "ds/torconfig.h"
#include "ds/torctlsocket.h"

namespace ds {
namespace tor {

class TorCtlReply;

class TorController : public QObject
{
    Q_OBJECT

public:
    enum class CtlState {
        DISCONNECTED,
        CONNECTING,
        AUTHENTICATING,
        CONNECTED,
        STOPPING,
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

    TorController(const TorConfig& config);

    CtlState getCtlState() const;
    TorState getTorState() const;

signals:
    void torStateUpdate(TorState state, int progress, const QString& summary);
    void stateUpdate(CtlState state);
    void autenticated();
    void authFailed(const QString& reason);

    // Emitted when we are authenticated and Tor is connected.
    void ready();

public slots:
    void start(); // Connect to Tor server
    void stop(); // Disconnect from Tor server

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
};

}} // namespaces

#endif // TORCONTROLLER_H
