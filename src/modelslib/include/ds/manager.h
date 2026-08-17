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

    enum StatusLevel {
        INFO = ds::core::DsEngine::INFO,
        WARNING = ds::core::DsEngine::WARNING,
        ERROR = ds::core::DsEngine::ERROR
    };
    Q_ENUM(StatusLevel)

    enum Pages {
        LOG,
        IDENTITIES,
        CONTACTS,
        HOME,
        CONVERSATIONS,
        CHAT
    };

    Q_ENUM(Pages)

    Q_PROPERTY(QString programName READ getProgramName CONSTANT)
    Q_PROPERTY(QString programNameAndVersion READ getProgramNameAndVersion CONSTANT)
    Q_PROPERTY(int currentPage READ getCurrentPage WRITE setCurrentPage NOTIFY currentPageChanged)

    Q_PROPERTY(int appState READ getAppState NOTIFY appStateChanged)
    Q_PROPERTY(int onlineState READ getOnlineState NOTIFY onlineStateChanged)
    Q_PROPERTY(int online READ isOnline NOTIFY onlineChanged)
    Q_PROPERTY(QString torVersion READ getTorVersion NOTIFY torVersionChanged)
    Q_PROPERTY(QString qtVersion READ getQtVersion CONSTANT)
    Q_PROPERTY(QString compiler READ getCompiler CONSTANT)
    Q_PROPERTY(QString buildDate READ getBuildDate CONSTANT)
    Q_PROPERTY(QUrl onlineStatusIcon READ getOnlineStatusIcon NOTIFY onlineStatusIconChanged)
    Q_PROPERTY(QString statusMessage READ getStatusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(StatusLevel statusLevel READ getStatusLevel NOTIFY statusMessageChanged)
    Q_PROPERTY(bool hasValidContactInClipboard READ hasValidContactInClipboard NOTIFY clipboardChanged)

    Q_INVOKABLE LogModel *logModel();
    Q_INVOKABLE ContactsModel *contactsModel();
    Q_INVOKABLE NotificationsModel *notificationsModel();
    Q_INVOKABLE ConversationsModel *conversationsModel();
    Q_INVOKABLE MessagesModel *messagesModel();
    Q_INVOKABLE FilesModel *filesModel();
    Q_INVOKABLE void textToClipboard(const QString& text);
    Q_INVOKABLE QString sanitizeBio(const QString& text) const;
    Q_INVOKABLE bool hasValidContactInClipboard() const;
    Q_INVOKABLE void createIdentityQrCode(const QString& identityName,
                                          const QString& identity);
    Q_INVOKABLE QString getIdentityText(const QString& identityName,
                                         const QString& identity) const;
    Q_INVOKABLE QVariantMap getIdenityFromClipboard() const;
    Q_INVOKABLE static QString urlToPath(const QString& url);
    Q_INVOKABLE static QString pathToUrl(const QString& path);

    Q_INVOKABLE void setTmpImageFromPath(const QString& path, const QSize& size = {128, 128});
    Q_INVOKABLE void setTmpImageFromImage(const QImage& image);
    Q_INVOKABLE void clearTmpImage();
    Q_INVOKABLE QImage getTmpImage() const;
    Q_INVOKABLE void deleteContact(const QUuid& uuid);
    Q_INVOKABLE void setStatusMessage(const QString& message,
                                      StatusLevel level = INFO);

    explicit Manager(const QString& logFile = {}, const QString& logLevel = {});

    static Manager& instance() noexcept {
        assert(instance_);
        return *instance_;
    }

public slots:
    AppState getAppState() const;
    OnlineState getOnlineState() const;
    bool isOnline() const;
    QString getTorVersion() const;
    QString getQtVersion() const;
    QString getCompiler() const;
    QString getBuildDate() const;
    QString getProgramName() const;
    QString getProgramNameAndVersion() const;
    int getCurrentPage();
    void setCurrentPage(int page);
    QUrl getOnlineStatusIcon() const;
    QString getStatusMessage() const;
    StatusLevel getStatusLevel() const;
    void goOnline();
    void goOffline();
    void onQuit();

signals:
    void appStateChanged(const AppState state);
    void onlineStateChanged(const OnlineState state);
    void currentPageChanged(int page);
    void onlineStatusIconChanged();
    void onlineChanged();
    void torVersionChanged();
    void statusMessageChanged();
    void clipboardChanged();

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
    QString status_message_;
    StatusLevel status_level_ = INFO;
};

}} // namespaces

#endif // MANAGER_H
