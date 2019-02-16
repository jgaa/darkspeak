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
    void startService(const QUuid& service,
                      const crypto::DsCert::ptr_t& cert,
                      const QVariantMap& data) override;
    void stopService(const QUuid& service) override;
    QUuid connectTo(core::ConnectData cd) override;
    void disconnectFrom(const QUuid& service,
                        const QUuid& connection) override;
    void autorizeConnection(const QUuid& service,
                            const QUuid& connection,
                            const bool allow) override;

private slots:
    void onServiceCreated(const ds::tor::ServiceProperties& service);

public:
    State getState() const override;
    static const QByteArray& getName(const State state);

protected:
    void setState(State state);
    ds::tor::TorConfig getConfig() const;
    TorServiceInterface& getService(const QUuid& service);

    std::unique_ptr<::ds::tor::TorMgr> tor_;
    QSettings& settings_;
    State state_ = State::OFFLINE;

    std::map<QUuid, TorServiceInterface::ptr_t> services_;

    // ProtocolManager interface
public slots:
    uint64_t sendAddme(const core::AddmeReq& req) override;
    uint64_t sendAck(const core::AckMsg& ack) override;

    // ProtocolManager interface
public:
    QByteArray getPeerHandle(const QUuid &service, const QUuid &connectionId) override;
};

}} // namespaces

#endif // TORPROTOCOLMANAGER_H
