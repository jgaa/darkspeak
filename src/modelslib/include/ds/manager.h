#ifndef MANAGER_H
#define MANAGER_H


#include <QSettings>
#include <QMetaType>
#include <QtCore>

#include "ds/dscert.h"
#include "ds/dsengine.h"
#include "ds/identity.h"

#ifndef PROGRAM_VERSION
    #define PROGRAM_VERSION "develop"
#endif

namespace ds {
namespace models {

class Manager : public QObject
{
    Q_OBJECT
public:
    using AppState = ds::core::DsEngine::State;
    using OnlineState = ds::core::ProtocolManager::State;

    Q_PROPERTY(QString programName READ getProgramName)
    Q_PROPERTY(QString programNameAndVersion READ getProgramNameAndVersion)
    Q_PROPERTY(int currentPage READ getCurrentPage WRITE setCurrentPage NOTIFY currentPageChanged)

    Q_PROPERTY(int appState READ getAppState NOTIFY appStateChanged)
    Q_PROPERTY(int onlineState READ getOnlineState NOTIFY onlineStateChanged)
    Q_PROPERTY(QUrl onlineStatusIcon READ getOnlineStatusIcon NOTIFY onlineStatusIconChanged)

    Q_INVOKABLE

    Manager();

public slots:
    AppState getAppState() const;
    OnlineState getOnlineState() const;
    QString getProgramName() const;
    QString getProgramNameAndVersion() const;
    int getCurrentPage();
    void setCurrentPage(int page);
    QUrl getOnlineStatusIcon() const;
    Q_INVOKABLE void goOnline();
    Q_INVOKABLE void goOffline();

signals:
    void appStateChanged(const AppState state);
    void onlineStateChanged(const OnlineState state);
    void currentPageChanged(int page);
    void onlineStatusIconChanged();

private:
    AppState app_state_ = ds::core::DsEngine::State::INITIALIZING;
    OnlineState online_state_ = ds::core::ProtocolManager::State::OFFLINE;
    std::unique_ptr<ds::core::DsEngine> engine_;
    int page_ = 3; // Home
};

}} // namespaces

#endif // MANAGER_H
