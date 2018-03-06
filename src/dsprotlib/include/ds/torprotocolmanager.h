#ifndef TORPROTOCOLMANAGER_H
#define TORPROTOCOLMANAGER_H

#include "ds/protocolmanager.h"
#include "ds/tormgr.h"

namespace ds {
namespace prot {

class TorProtocolManager : public ds::core::ProtocolManager
{
public:
    TorProtocolManager(QSettings& settings);
    virtual ~TorProtocolManager() = default;

public slots:
    void sendMessage(const core::Message &) override;
    void start() override;
    void stop() override;
    void createTransportHandle(const core::TransportHandleReq &) override;
    void torMgrStarted();
    void torMgrOnline();
    void torMgrOffline();
    void torMgrStopped();

public:
    State getState() const override;

protected:
    void setState(State state);
    ds::tor::TorConfig getConfig() const;

    State state_ = State::OFFLINE;
    std::unique_ptr<::ds::tor::TorMgr> tor_;
    QSettings& settings_;

    // ProtocolManager interface
public slots:

};

}} // namespaces

#endif // TORPROTOCOLMANAGER_H
