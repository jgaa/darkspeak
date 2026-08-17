#ifndef IDENTITY_H
#define IDENTITY_H

#include <memory>

#include "ds/dscert.h"
#include "ds/errors.h"
#include "ds/protocolmanager.h"
#include "ds/contact.h"
#include "ds/conversation.h"

#include <QString>
#include <QStringList>
#include <QtGui/QImage>
#include <QUuid>
#include <QDateTime>
#include <QVariant>
#include <map>
#include <utility>

namespace ds {
namespace core {

struct IdentityData {
    QUuid uuid;
    QString name;
    QByteArray hash;
    QByteArray address;
    QByteArray addressData;
    QString notes;
    QString bio;
    QImage avatar;
    bool allowUtf8 = true;
    bool allowFiles = true;
    QStringList allowMimeTypes{QStringLiteral("*")};
    bool allowReactions = true;
    bool publicBio = true;
    bool publicAvatar = true;
    crypto::DsCert::ptr_t cert;
    bool autoConnect = true;
};

struct IdentityReq {
    QUuid uuid = QUuid::createUuid();
    QString name;
    QString notes;
    QString bio;
    QImage avatar;
    bool allowUtf8 = true;
    bool allowFiles = true;
    QStringList allowMimeTypes{QStringLiteral("*")};
    bool allowReactions = true;
    bool publicBio = true;
    bool publicAvatar = true;
    bool autoConnect = true;
};

class QmlIdentityReq : public QObject {
    Q_OBJECT
public:
    QmlIdentityReq() = default;
    QmlIdentityReq(const QmlIdentityReq&) = default;
    virtual ~QmlIdentityReq() = default;

    Q_PROPERTY(QString name READ name WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QString notes READ notes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(QString bio READ bio WRITE setBio NOTIFY bioChanged)
    Q_PROPERTY(bool autoConnect READ autoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)
    Q_PROPERTY(QImage avatar READ avatar WRITE setAvatar NOTIFY avatarChanged)
    Q_PROPERTY(bool allowUtf8 READ allowUtf8 WRITE setAllowUtf8 NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool allowFiles READ allowFiles WRITE setAllowFiles NOTIFY securityPreferencesChanged)
    Q_PROPERTY(QStringList allowMimeTypes READ allowMimeTypes WRITE setAllowMimeTypes NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool allowReactions READ allowReactions WRITE setAllowReactions NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool publicBio READ publicBio WRITE setPublicBio NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool publicAvatar READ publicAvatar WRITE setPublicAvatar NOTIFY securityPreferencesChanged)

    QString name() const { return value.name; }
    void setName(QString name) {
        if (name != value.name) {
            value.name = name;
            emit nameChanged();
        }
    }

    QString notes() const { return value.notes; }
    void setNotes(QString notes) {
        if (value.notes != notes) {
            value.notes = notes;
            emit notesChanged();
        }
    }

    QString bio() const { return value.bio; }
    void setBio(QString bio) {
        if (value.bio != bio) { value.bio = std::move(bio); emit bioChanged(); }
    }

    bool autoConnect() const { return value.autoConnect ;}
    void setAutoConnect(bool autoConnect) {
        if (value.autoConnect != autoConnect) {
            value.autoConnect = autoConnect;
            emit autoConnectChanged();
        }
    }

    QImage avatar() const noexcept { return value.avatar; }

    void setAvatar(QImage avatar) {
        if (value.avatar != avatar) {
            value.avatar = avatar;
            emit avatarChanged();
        }
    }

    bool allowUtf8() const { return value.allowUtf8; }
    void setAllowUtf8(bool enabled) { if (value.allowUtf8 != enabled) { value.allowUtf8 = enabled; emit securityPreferencesChanged(); } }
    bool allowFiles() const { return value.allowFiles; }
    void setAllowFiles(bool enabled) { if (value.allowFiles != enabled) { value.allowFiles = enabled; emit securityPreferencesChanged(); } }
    QStringList allowMimeTypes() const { return value.allowMimeTypes; }
    void setAllowMimeTypes(QStringList types) { if (value.allowMimeTypes != types) { value.allowMimeTypes = std::move(types); emit securityPreferencesChanged(); } }
    bool allowReactions() const { return value.allowReactions; }
    void setAllowReactions(bool enabled) { if (value.allowReactions != enabled) { value.allowReactions = enabled; emit securityPreferencesChanged(); } }
    bool publicBio() const { return value.publicBio; }
    void setPublicBio(bool enabled) { if (value.publicBio != enabled) { value.publicBio = enabled; emit securityPreferencesChanged(); } }
    bool publicAvatar() const { return value.publicAvatar; }
    void setPublicAvatar(bool enabled) { if (value.publicAvatar != enabled) { value.publicAvatar = enabled; emit securityPreferencesChanged(); } }

signals:
    void nameChanged();
    void notesChanged();
    void bioChanged();
    void autoConnectChanged();
    void avatarChanged();
    void securityPreferencesChanged();

public:
    IdentityReq value;
};


struct IdentityError {
    QUuid uuid;
    QString name;
    QString explanation;
};


class Identity : public QObject {
    Q_OBJECT


    struct Connected {
        Connected(const Contact::ptr_t& contact)
            : contact{contact} {}
        Contact::ptr_t contact;
    };

public:
    Identity(QObject& parent,
         const int dbId, // -1 if the identity is new
         const bool online,
         QDateTime created,
         IdentityData data);

    Q_PROPERTY(int id READ getId CONSTANT)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QByteArray hash READ getHash CONSTANT)
    Q_PROPERTY(QByteArray address READ getAddress WRITE setAddress NOTIFY addressChanged)
    Q_PROPERTY(QByteArray addressData READ getAddressData WRITE setAddressData NOTIFY addressDataChanged)
    Q_PROPERTY(QString notes READ getNotes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(QString bio READ getBio WRITE setBio NOTIFY bioChanged)
    Q_PROPERTY(QString probeBio READ getProbeBio NOTIFY probeBioChanged)
    Q_PROPERTY(QImage probeAvatar READ getProbeAvatar NOTIFY probeAvatarChanged)
    Q_PROPERTY(bool hasProbeAvatar READ hasProbeAvatar NOTIFY probeAvatarChanged)
    Q_PROPERTY(QImage avatar READ getAvatar WRITE setAvatar NOTIFY avatarChanged)
    Q_PROPERTY(QString avatarUrl READ getAvatarUrl NOTIFY avatarUrlChanged)
    Q_PROPERTY(QUuid uuid READ getUuid CONSTANT)
    Q_PROPERTY(QDateTime created READ getCreated CONSTANT)
    Q_PROPERTY(bool online READ isOnline WRITE setOnline NOTIFY onlineChanged)
    Q_PROPERTY(QByteArray b58identity READ getB58EncodedIdetity CONSTANT)
    Q_PROPERTY(QByteArray handle READ getHandle CONSTANT)
    Q_PROPERTY(bool autoConnect READ isAutoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)
    Q_PROPERTY(bool allowUtf8 READ getAllowUtf8 WRITE setAllowUtf8 NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool allowFiles READ getAllowFiles WRITE setAllowFiles NOTIFY securityPreferencesChanged)
    Q_PROPERTY(QStringList allowMimeTypes READ getAllowMimeTypes WRITE setAllowMimeTypes NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool allowReactions READ getAllowReactions WRITE setAllowReactions NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool publicBio READ getPublicBio WRITE setPublicBio NOTIFY securityPreferencesChanged)
    Q_PROPERTY(bool publicAvatar READ getPublicAvatar WRITE setPublicAvatar NOTIFY securityPreferencesChanged)

    Q_INVOKABLE void addContact(const QVariantMap& args);
    Q_INVOKABLE void probeContact(const QString& identityId);
    Q_INVOKABLE void cancelContactProbe();
    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();
    Q_INVOKABLE void changeTransport();
    Q_INVOKABLE void setNewTorService(const QString& address, int port, const QString privateKey);

    int getId() const noexcept;
    QString getName() const noexcept;
    void setName(const QString& name);
    QByteArray getAddress() const noexcept;
    void setAddress(const QByteArray& address);
    QByteArray getAddressData() const noexcept;
    void setAddressData(const QByteArray& data);
    QString getNotes() const noexcept;
    void setNotes(const QString& notes);
    QString getBio() const noexcept { return data_.bio; }
    void setBio(const QString& bio);
    QString getProbeBio() const noexcept;
    QImage getProbeAvatar() const noexcept;
    bool hasProbeAvatar() const noexcept;
    QImage getAvatar() const noexcept;
    QString getAvatarUrl() const noexcept;
    void setAvatar(const QImage& avatar);
    bool getAllowUtf8() const noexcept { return data_.allowUtf8; }
    bool getAllowFiles() const noexcept { return data_.allowFiles; }
    QStringList getAllowMimeTypes() const { return data_.allowMimeTypes; }
    bool getAllowReactions() const noexcept { return data_.allowReactions; }
    bool getPublicBio() const noexcept { return data_.publicBio; }
    bool getPublicAvatar() const noexcept { return data_.publicAvatar; }
    void setAllowUtf8(bool value);
    void setAllowFiles(bool value);
    void setAllowMimeTypes(QStringList value);
    void setAllowReactions(bool value);
    void setPublicBio(bool value);
    void setPublicAvatar(bool value);
    bool isOnline() const noexcept;
    void setOnline(const bool value);
    QUuid getUuid() const noexcept;
    QByteArray getHash() const noexcept;
    QDateTime getCreated() const noexcept;
    crypto::DsCert::ptr_t getCert() const noexcept;
    QByteArray getB58EncodedIdetity() const noexcept;
    QByteArray getHandle() const noexcept;
    bool isAutoConnect() const noexcept;
    void setAutoConnect(bool value);

    /*! Add the new Identity to the database. */
    void addToDb();

    /*! Delete from the database */
    void deleteFromDb();

    ProtocolManager& getProtocolManager();

    void onIncomingPeer(const std::shared_ptr<PeerConnection>& peer);
    Conversation::ptr_t convesationFromHash(const QByteArray& hash);

    const char *getTableName() const noexcept { return "identity"; }

    void registerConnection(const Contact::ptr_t& contact);
    void unregisterConnection(const QUuid& uuid);
    Contact::ptr_t contactFromHandle(const QString& handle);

public slots:
    void onAddmeRequest(const PeerAddmeReq& req);

signals:
    void nameChanged();
    void addressChanged();
    void addressDataChanged();
    void notesChanged();
    void bioChanged();
    void probeBioChanged();
    void probeAvatarChanged();
    void avatarChanged();
    void avatarUrlChanged();
    void onlineChanged();
    void autoConnectChanged();
    void securityPreferencesChanged();
    void processOnlineLater();

private slots:
    void onProcessOnlineLater();

private:
    void connectContacts();
    static int getRandomConnectDelay();
    void disconnectContacts();
    void forAllContacts(const std::function<void (const Contact::ptr_t&)>& fn );
    std::deque<QUuid> getAllContacts() const;

    Contact::ptr_t contactFromHash(const QByteArray& hash);
    Contact::ptr_t contactFromUuid(const QUuid& uuid);

    int id_ = -1; // Database id
    bool online_ = false;
    IdentityData data_;
    QDateTime created_;
    bool avatarUrlChanging_ = false;
    quint64 avatarVersion_ = 0;

    // Active Connections in any direction
    // Keeps connected Contacts in memory
    std::map<QUuid /* contact id */, std::unique_ptr<Connected>> connected_;
    Contact::ptr_t bioProbe_;
};

}} // identities

Q_DECLARE_METATYPE(ds::core::IdentityData)
Q_DECLARE_METATYPE(ds::core::IdentityReq)
Q_DECLARE_METATYPE(ds::core::IdentityError)

#endif // IDENTITY_H
