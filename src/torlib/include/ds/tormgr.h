#ifndef TORMGR_H
#define TORMGR_H

#include <memory>

#include <QObject>

#include "ds/torconfig.h"
#include "ds/torcontroller.h"

namespace ds {
namespace tor {

/*! Manages the access to a Tor server
 *
 */
class TorMgr : public QObject
{
    Q_OBJECT

public:
    struct OfflineError : public std::runtime_error
    {
        explicit OfflineError(const char *what) : std::runtime_error(what) {}
        explicit OfflineError(const QString& what) : std::runtime_error(what.toStdString()) {}
    };

    explicit TorMgr(const TorConfig& config);

    TorController *getController();

signals:
    // Connected to the Tor control channel
    void started();

    // Tor is online
    void online();

    // Tor is offline
    void offline();

    // stop() is complete
    void stopped();

    // Proxied from the controller, as it is hard for consumers to subscribe
    // to the controller. The manager will create new copntrollers if connectivity
    // with the Tor service is lost.
    void serviceCreated(const ServiceProperties& service);
    void serviceFailed(const QByteArray& id, const QByteArray& reason);
    void serviceStarted(const QByteArray& id);
    void serviceStopped(const QByteArray& id);
    void torStateUpdate(TorController::TorState state, int progress, const QString& summary);
    void stateUpdate(TorController::CtlState state);

public slots:
    /*! Start / connect to the Tor service */
    void start();

    /*! Disconnect from the Tor service */
    void stop();

    /*! Update the configuration
     *
     * Takes effect after start()
     */
    void updateConfig(const TorConfig& config);

    /*! Create a hidden service
     *
     * This will create and start a Tor hidden service.
     *
     * Signals:
     *  - serviceCreated and serviceStarted if sucessful
     *  - serviceFailed if the service was not created or failed to start.
     */
    void createService(const QByteArray& id);

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
    void stopService(const QByteArray& id);

private slots:
    void onTorStateUpdate(TorController::TorState state, int progress, const QString& summary);
    void onStateUpdate(TorController::CtlState state);
    void onServiceCreated(const ServiceProperties& service);
    void onServiceFailed(const QByteArray& id, const QByteArray& reason);
    void onServiceStarted(const QByteArray& id);
    void onServiceStopped(const QByteArray& id);

private:
    void startUseSystemInstance();

    TorConfig config_;
    std::shared_ptr<TorController> ctl_;
};

}} // namespaces

#endif // TORMGR_H
