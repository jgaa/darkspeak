#ifndef MANAGER_H
#define MANAGER_H


#include <QSettings>
#include <QMetaType>
#include <QtCore>

#include "ds/dscert.h"
#include "ds/dsengine.h"
#include "ds/identity.h"
#include "ds/logmodel.h"
#include "ds/contactsmodel.h"
#include "ds/notificationsmodel.h"
#include "ds/conversationsmodel.h"
#include "ds/messagesmodel.h"
#include "ds/filesmodel.h"

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

    enum Pages {
        LOG,
        IDENTITIES,
        CONTACTS,
        HOME,
        CONVERSATIONS,
        CHAT
    };

    Q_ENUM(Pages)

    Q_PROPERTY(QString programName READ getProgramName)
    Q_PROPERTY(QString programNameAndVersion READ getProgramNameAndVersion)
    Q_PROPERTY(int currentPage READ getCurrentPage WRITE setCurrentPage NOTIFY currentPageChanged)

    Q_PROPERTY(int appState READ getAppState NOTIFY appStateChanged)
    Q_PROPERTY(int onlineState READ getOnlineState NOTIFY onlineStateChanged)
    Q_PROPERTY(int online READ isOnline NOTIFY onlineChanged)
    Q_PROPERTY(QUrl onlineStatusIcon READ getOnlineStatusIcon NOTIFY onlineStatusIconChanged)

    Q_INVOKABLE LogModel *logModel();
    Q_INVOKABLE ContactsModel *contactsModel();
    Q_INVOKABLE NotificationsModel *notificationsModel();
    Q_INVOKABLE ConversationsModel *conversationsModel();
    Q_INVOKABLE MessagesModel *messagesModel();
    Q_INVOKABLE FilesModel *filesModel();
    Q_INVOKABLE void textToClipboard(QString text);
    Q_INVOKABLE QVariantMap getIdenityFromClipboard() const;
    Q_INVOKABLE static QString urlToPath(QString url);
    Q_INVOKABLE static QString pathToUrl(QString path);

    Q_INVOKABLE void setTmpImageFromPath(QString path, const QSize& size = {128, 128});
    Q_INVOKABLE void setTmpImageFromImage(QImage& image);
    Q_INVOKABLE void clearTmpImage();
    Q_INVOKABLE QImage getTmpImage() const;

    Manager();

    static Manager& instance() noexcept {
        assert(instance_);
        return *instance_;
    }

public slots:
    AppState getAppState() const;
    OnlineState getOnlineState() const;
    bool isOnline() const;
    QString getProgramName() const;
    QString getProgramNameAndVersion() const;
    int getCurrentPage();
    void setCurrentPage(int page);
    QUrl getOnlineStatusIcon() const;
    void goOnline();
    void goOffline();

signals:
    void appStateChanged(const AppState state);
    void onlineStateChanged(const OnlineState state);
    void currentPageChanged(int page);
    void onlineStatusIconChanged();
    void onlineChanged();

private:
    static Manager *instance_;
    AppState app_state_ = ds::core::DsEngine::State::INITIALIZING;
    OnlineState online_state_ = ds::core::ProtocolManager::State::OFFLINE;
    std::unique_ptr<ds::core::DsEngine> engine_;
    std::unique_ptr<LogModel> log_;
    std::unique_ptr<ContactsModel> contacts_;
    std::unique_ptr<NotificationsModel> notifications_;
    std::unique_ptr<ConversationsModel> conversationsModel_;
    std::unique_ptr<MessagesModel> messagesModel_;
    std::unique_ptr<FilesModel> filesModel_;
    int page_ = 3; // Home
    std::unique_ptr<QImage> tmpImage_;
};

}} // namespaces

#endif // MANAGER_H
