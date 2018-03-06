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
    explicit TorMgr(const TorConfig& config);

    TorController *getController() { return ctl_.get(); }

signals:
    // Connected to the Tor control channel
    void started();

    // Tor is online
    void online();

    // Tor is offline
    void offline();

    // stop() is complete
    void stopped();

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

private slots:
    void onTorCtlStopped();
    void onTorCtlAutenticated();
    void onTorCtlstateUpdate(TorController::CtlState state);

private:
    void startUseSystemInstance();

    TorConfig config_;
    std::shared_ptr<TorController> ctl_;
};

}} // namespaces

#endif // TORMGR_H
