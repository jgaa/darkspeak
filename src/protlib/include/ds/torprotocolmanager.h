#ifndef TORPROTOCOLMANAGER_H
#define TORPROTOCOLMANAGER_H

#include "ds/protocolmanager.h"
#include "ds/tormgr.h"
#include "ds/torserviceinterface.h"

namespace ds {
namespace prot {

class TorProtocolManager : public ds::core::ProtocolManager
{
public:
    TorProtocolManager(QSettings& settings);

public slots:
    void sendMessage(const core::Message &) override;
    void start() override;
    void stop() override;
    void createTransportHandle(const core::TransportHandleReq &) override;
    void startService(const QByteArray& id, const QVariantMap& data) override;
    void stopService(const QByteArray& id) override;
    QUuid connectTo(const QByteArray &serviceId, const QByteArray &address) override;
    void disconnectFrom(const QByteArray &serviceId, const QUuid &uuid) override;

private slots:
    void onServiceCreated(const ds::tor::ServiceProperties& service);
    void onServiceFailed(const QByteArray& id, const QByteArray& reason);
    void onServiceStarted(const QByteArray& id);
    void onServiceStopped(const QByteArray& id);
    void torMgrStarted();
    void torMgrOnline();
    void torMgrOffline();
    void torMgrStopped();

public:
    State getState() const override;
    static const QByteArray& getName(const State state);

protected:
    void setState(State state);
    ds::tor::TorConfig getConfig() const;
    TorServiceInterface& getService(const QByteArray& serviceId);

    std::unique_ptr<::ds::tor::TorMgr> tor_;
    QSettings& settings_;
    State state_ = State::OFFLINE;

    std::map<QByteArray, TorServiceInterface::ptr_t> services_;
};

}} // namespaces

#endif // TORPROTOCOLMANAGER_H
