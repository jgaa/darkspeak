#ifndef MANAGER_H
#define MANAGER_H


#include <QSettings>
#include <QMetaType>
#include <QtCore>

#include "ds/dscert.h"
#include "ds/dsengine.h"
#include "ds/identity.h"

namespace ds {
namespace models {

class Manager : public QObject
{
    Q_OBJECT
public:
    using AppState = ds::core::DsEngine::State;
    using OnlineState = ds::core::ProtocolManager::State;

    Q_PROPERTY(QString programName READ programName)

    Q_ENUMS(AppState)
    Q_PROPERTY(AppState appState
        READ getAppState
        NOTIFY appStateChanged)

    Q_ENUMS(OnlineState)
    Q_PROPERTY(OnlineState onlineState
        READ getOnlineState
        NOTIFY onlineStateChanged)

    Manager();

public slots:
    AppState getAppState() const;
    OnlineState getOnlineState() const;
    QString programName() const;

signals:
    void appStateChanged(const AppState state);
    void onlineStateChanged(const OnlineState state);

private:
    AppState app_state_ = ds::core::DsEngine::State::INITIALIZING;
    OnlineState online_state_ = ds::core::ProtocolManager::State::OFFLINE;
    std::unique_ptr<ds::core::DsEngine> engine_;
};

}} // namespaces

#endif // MANAGER_H
