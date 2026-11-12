#pragma once
#include <QAbstractListModel>
#include <QImage>
#include <QValidator>
#include <QDateTime>
#include "ds/mimepreferences.h"

class MockUiModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int delegates READ delegates NOTIFY delegatesChanged)
public:
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 10000; }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
        if (role == Qt::UserRole) return tr("Message %1 — a disposable UI fixture with a long identifier and selectable plain text: <b>not markup</b>.").arg(index.row());
        if (role == Qt::UserRole + 1) return index.row() % 2 == 1;
        return {};
    }
    QHash<int, QByteArray> roleNames() const override { return {{Qt::UserRole, "body"}, {Qt::UserRole + 1, "outgoing"}}; }
    int delegates() const { return delegates_; }
    Q_INVOKABLE void delegateCreated() { ++delegates_; emit delegatesChanged(); }
    Q_INVOKABLE void delegateDestroyed() { --delegates_; emit delegatesChanged(); }
signals:
    void delegatesChanged();
private:
    int delegates_ = 0;
};

class MockNavigation final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int currentPage MEMBER current_page NOTIFY currentPageChanged)
public:
    Q_PROPERTY(bool online READ offline CONSTANT)
    Q_PROPERTY(int onlineState READ onlineState CONSTANT)
    Q_PROPERTY(QString torVersion READ empty CONSTANT)
    Q_PROPERTY(QString statusMessage READ empty CONSTANT)
    Q_PROPERTY(int statusLevel READ onlineState CONSTANT)
    Q_PROPERTY(bool hasValidContactInClipboard READ offline CONSTANT)
    Q_PROPERTY(QString programNameAndVersion READ empty CONSTANT)
    Q_PROPERTY(QString qtVersion READ empty CONSTANT)
    Q_PROPERTY(QString compiler READ empty CONSTANT)
    Q_PROPERTY(QString buildDate READ empty CONSTANT)
    Q_PROPERTY(QString onlineStatusIcon READ empty CONSTANT)
    enum Pages { LOG, IDENTITIES, CONTACTS, HOME, CONVERSATIONS, CHAT, EVENTS };
    Q_ENUM(Pages)
    enum Levels { INFO, WARNING, ERROR };
    Q_ENUM(Levels)
    bool offline() const { return false; }
    int onlineState() const { return 0; }
    QString empty() const { return {}; }
    Q_INVOKABLE void goOnline() {}
    Q_INVOKABLE void goOffline() {}
    int current_page = 3;
    Q_INVOKABLE void navigate(int page) { if (current_page != page) { current_page = page; emit currentPageChanged(); } }
signals:
    void currentPageChanged();
};

class MockWindow final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int width MEMBER width CONSTANT)
    Q_PROPERTY(int height MEMBER height CONSTANT)
public:
    int width = 1000;
    int height = 700;
};

class MockBackendModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QObject *current READ current NOTIFY currentRowChanged)
    Q_PROPERTY(int currentRow MEMBER current_row NOTIFY currentRowChanged)
    Q_PROPERTY(QString search MEMBER search NOTIFY searchChanged)
    Q_PROPERTY(QString draft MEMBER draft NOTIFY draftChanged)
    Q_PROPERTY(bool loading MEMBER loading NOTIFY historyChanged)
    Q_PROPERTY(bool hasOlder MEMBER has_older NOTIFY historyChanged)
    Q_PROPERTY(bool hasNewer MEMBER has_newer NOTIFY historyChanged)
    Q_PROPERTY(QString historyError MEMBER history_error NOTIFY historyChanged)
    Q_PROPERTY(int maximumLevel MEMBER maximum_level NOTIFY searchChanged)
public:
    int current_row = -1;
    int maximum_level = 6;
    QString search, draft, history_error;
    bool loading = false, has_older = false, has_newer = false;
    QObject *entity = nullptr;
    QObject *current() const { return entity; }
    void populate(QObject *value) { beginResetModel(); entity = value; endResetModel(); emit currentRowChanged(); emit currentIdentityChanged(); }
    int rowCount(const QModelIndex & = {}) const override { return entity ? 2 : 0; }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!entity || !index.isValid()) return {};
        const auto name = roleNames().value(role);
        if (name == "contact" || name == "identity" || name == "conversation" || name == "file") return QVariant::fromValue(entity);
        if (name == "display" || name == "content" || name == "message") return QStringLiteral("Fixture text <b>plain</b> with a long translated sentence.");
        if (name == "composedTime") return QDateTime::fromString("2026-11-11T12:00:00.000Z", Qt::ISODateWithMs);
        if (name == "stateName") return QStringLiteral("Received");
        if (name == "contactName" || name == "identityName" || name == "fileName") return QStringLiteral("Disposable fixture");
        if (name == "level") return 1;
        if (name == "direction") return entity->property("direction");
        if (name == "type") return index.row() == 0 ? message_type : message_type + 1;
        if (name == "messageState") return 0;
        if (name == "avatarUrl" || name == "nickName" || name == "requesterIdentityId") return QString{};
        if (name == "contactHeader") return false;
        if (name == "unreadCount") return 3;
        if (name == "identityId" || name == "conversationId") return 1;
        return {};
    }
    int message_type = 0;
    QHash<int, QByteArray> roleNames() const override {
        QHash<int, QByteArray> roles;
        int role = Qt::UserRole;
        for (const auto *name : {"contact", "identity", "conversation", "file", "type", "content", "stateName", "composedTime", "direction", "messageState", "display", "level", "message", "contactName", "identityName", "fileName", "contactHeader", "avatarUrl", "nickName", "unreadCount", "identityId", "conversationId", "requesterIdentityId"}) roles.insert(role++, name);
        return roles;
    }
    Q_INVOKABLE void setCurrentIdentity(int) {}
    Q_INVOKABLE void setConversation(QObject *) {}
    Q_INVOKABLE void setContact(QObject *) {}
    Q_INVOKABLE void loadLatest() {}
    Q_INVOKABLE void loadOlder() {}
    Q_INVOKABLE void loadNewer() {}
    Q_INVOKABLE void clearConversationNotifications(int) {}
    Q_INVOKABLE void refreshNotifications() {}
signals:
    void currentRowChanged();
    void currentIdentityChanged();
    void searchChanged();
    void draftChanged();
    void historyChanged();
    void pageLoaded(int direction);
};

class MockBackendTypes : public QObject {
    Q_OBJECT
public:
    enum Values { LOG, IDENTITIES, CONTACTS, HOME, CONVERSATIONS, CHAT,
                  ERROR, WARNING, INFO, N_ADDME, N_MESSAGE, MESSAGE, INCOMING, OUTGOING,
                  FS_OFFERED, FS_FAILED, FS_QUEUED, FS_TRANSFERRING, FS_HASHING,
                  FS_WAITING, FS_DONE, MS_REJECTED };
    Q_ENUM(Values)
};

class MockEntity : public MockBackendTypes {
    Q_OBJECT
    Q_PROPERTY(int id MEMBER id CONSTANT)
    Q_PROPERTY(QString name MEMBER name NOTIFY changed)
    Q_PROPERTY(QString displayName MEMBER name NOTIFY changed)
    Q_PROPERTY(QString nickName MEMBER name NOTIFY changed)
    Q_PROPERTY(QString topic MEMBER topic NOTIFY changed)
    Q_PROPERTY(QString notes MEMBER notes NOTIFY changed)
    Q_PROPERTY(QString bio MEMBER bio NOTIFY changed)
    Q_PROPERTY(QString probeBio MEMBER bio NOTIFY changed)
    Q_PROPERTY(QString avatarUrl READ avatarUrl CONSTANT)
    Q_PROPERTY(QString handle MEMBER handle CONSTANT)
    Q_PROPERTY(QString address MEMBER address CONSTANT)
    Q_PROPERTY(QString b58identity MEMBER handle CONSTANT)
    Q_PROPERTY(QDateTime created READ date CONSTANT)
    Q_PROPERTY(QDateTime lastSeen READ date CONSTANT)
    Q_PROPERTY(bool online MEMBER online NOTIFY changed)
    Q_PROPERTY(bool peerVerified MEMBER peer_verified NOTIFY changed)
    Q_PROPERTY(bool canSendFiles MEMBER can_send_files NOTIFY changed)
    Q_PROPERTY(bool autoConnect MEMBER auto_connect NOTIFY changed)
    Q_PROPERTY(bool previewImages MEMBER preview_images NOTIFY changed)
    Q_PROPERTY(bool previewVideo MEMBER preview_video NOTIFY changed)
    Q_PROPERTY(bool allowUtf8 MEMBER allow_utf8 NOTIFY changed)
    Q_PROPERTY(bool allowFiles MEMBER allow_files NOTIFY changed)
    Q_PROPERTY(bool allowReactions MEMBER allow_reactions NOTIFY changed)
    Q_PROPERTY(bool publicBio MEMBER public_bio NOTIFY changed)
    Q_PROPERTY(bool publicAvatar MEMBER public_avatar NOTIFY changed)
    Q_PROPERTY(QStringList allowMimeTypes MEMBER mime_types NOTIFY changed)
    Q_PROPERTY(QImage avatar MEMBER avatar NOTIFY changed)
    Q_PROPERTY(bool hasBio READ hasBio CONSTANT)
    Q_PROPERTY(bool hasAvatar READ hasAvatar CONSTANT)
    Q_PROPERTY(bool hasProbeAvatar READ hasAvatar CONSTANT)
    Q_PROPERTY(QObject *identity MEMBER identity NOTIFY changed)
    Q_PROPERTY(QObject *participant MEMBER participant NOTIFY changed)
    Q_PROPERTY(QString contactName MEMBER name NOTIFY changed)
    Q_PROPERTY(int unread MEMBER unread NOTIFY changed)
    Q_PROPERTY(int direction MEMBER direction NOTIFY changed)
    Q_PROPERTY(int state MEMBER state NOTIFY changed)
    Q_PROPERTY(int size MEMBER size CONSTANT)
    Q_PROPERTY(qreal progress MEMBER progress NOTIFY changed)
    Q_PROPERTY(bool iBlocked MEMBER blocked NOTIFY changed)
public:
    int id = 1, unread = 3, direction = INCOMING, state = FS_OFFERED, size = 4096;
    qreal progress = .5;
    QString name = QStringLiteral("Alice <b>plain text</b>"), topic = QStringLiteral("A conversation with long translated labels"), notes;
    QString bio = QStringLiteral("A disposable bio fixture."), handle = QString(64, QChar('x'));
    QString address = QString(56, QChar('a')) + QStringLiteral(".onion");
    QStringList mime_types{QStringLiteral("*")};
    QImage avatar;
    bool online = true, peer_verified = true, can_send_files = true, auto_connect = true;
    bool preview_images = false, preview_video = false, allow_utf8 = true, allow_files = true;
    bool allow_reactions = true, public_bio = true, public_avatar = true, blocked = false;
    QObject *identity = this, *participant = this;
    QString avatarUrl() const { return {}; }
    QDateTime date() const { return QDateTime::fromString("2026-11-11T12:00:00.000Z", Qt::ISODateWithMs); }
    bool hasBio() const { return true; }
    bool hasAvatar() const { return false; }
    Q_INVOKABLE bool validMimeText(const QString &text) const { return ds::core::parseMimePreferences(text).has_value(); }
    Q_INVOKABLE bool setMimeText(const QString &text) { return validMimeText(text); }
    Q_INVOKABLE void cancelContactProbe() {}
signals:
    void changed();
    void probeAvatarChanged();
};

class MockIdentityValidator : public QValidator {
    Q_OBJECT
    Q_PROPERTY(QString currentName MEMBER current_name NOTIFY changed)
public:
    QString current_name;
    State validate(QString &, int &) const override { return Acceptable; }
signals:
    void changed();
};
