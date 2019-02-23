#ifndef IDENTITY_H
#define IDENTITY_H

#include <memory>

#include "ds/dscert.h"
#include "ds/errors.h"
#include "ds/protocolmanager.h"
#include "ds/contact.h"

#include <QString>
#include <QtGui/QImage>
#include <QUuid>
#include <QDateTime>
#include <QVariant>
#include <map>

namespace ds {
namespace core {

struct IdentityData {
    QUuid uuid;
    QString name;
    QByteArray hash;
    QByteArray address;
    QByteArray addressData;
    QString notes;
    QImage avatar;
    crypto::DsCert::ptr_t cert;
    bool autoConnect = true;
};

struct IdentityReq {
    QUuid uuid = QUuid::createUuid();
    QString name;
    QString notes;
    QImage avatar;
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
    Q_PROPERTY(bool autoConnect READ autoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)

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

    bool autoConnect() const { return value.autoConnect ;}
    void setAutoConnect(bool autoConnect) {
        if (value.autoConnect != autoConnect) {
            value.autoConnect = autoConnect;
            emit autoConnectChanged();
        }
    }

signals:
    void nameChanged();
    void notesChanged();
    void autoConnectChanged();

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
        Contact::ptr_t contact;
        bool established = false;
        bool outgoing = false;
    };

public:
    Identity(QObject& parent,
         const int dbId, // -1 if the identity is new
         const bool online,
         const QDateTime& created,
         IdentityData data);

    Q_PROPERTY(int id READ getId)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QByteArray hash READ getHash)
    Q_PROPERTY(QByteArray address READ getAddress WRITE setAddress NOTIFY addressChanged)
    Q_PROPERTY(QByteArray addressData READ getAddressData WRITE setAddressData NOTIFY addressDataChanged)
    Q_PROPERTY(QString notes READ getNotes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(QString avatar READ getAvatarUri NOTIFY avatarChanged)
    Q_PROPERTY(QUuid uuid READ getUuid)
    Q_PROPERTY(QDateTime created READ getCreated)
    Q_PROPERTY(bool online READ isOnline WRITE setOnline NOTIFY onlineChanged)
    Q_PROPERTY(QByteArray b58identity READ getB58EncodedIdetity)
    Q_PROPERTY(QByteArray handle READ getHandle)
    Q_PROPERTY(bool autoConnect READ isAutoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)

    Q_INVOKABLE void addContact(const QVariantMap& args);
    Q_INVOKABLE void startService();
    Q_INVOKABLE void stopService();

    Q_INVOKABLE void changeTransport();

    int getId() const noexcept;
    QString getName() const noexcept;
    void setName(const QString& name);
    QByteArray getAddress() const noexcept;
    void setAddress(const QByteArray& address);
    QByteArray getAddressData() const noexcept;
    void setAddressData(const QByteArray& data);
    QString getNotes() const noexcept;
    void setNotes(const QString& notes);
    QImage getAvatar() const noexcept;
    QString getAvatarUri() const noexcept;
    void setAvatar(const QImage& avatar);
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

    const char *getTableName() const noexcept { return "identity"; }

public slots:
    void onAddmeRequest(const PeerAddmeReq& req);

signals:
    void nameChanged();
    void addressChanged();
    void addressDataChanged();
    void notesChanged();
    void avatarChanged();
    void onlineChanged();
    void autoConnectChanged();

private:
    Contact::ptr_t contactFromHandle(const QByteArray& handle);
    Contact::ptr_t contactFromHash(const QByteArray& hash);
    Contact::ptr_t contactFromUuid(const QUuid& uuid);

    int id_ = -1; // Database id
    bool online_ = false;
    IdentityData data_;
    QDateTime created_;

    // Active Connections in any direction
    // Keeps connected Contacts in memory
    std::map<QUuid /* connection id */, std::unique_ptr<Connected>> connected_;
};

}} // identities

//Q_DECLARE_OPAQUE_POINTER(ds::core::Identity)
Q_DECLARE_METATYPE(ds::core::Identity *)
Q_DECLARE_METATYPE(ds::core::IdentityData)
Q_DECLARE_METATYPE(ds::core::IdentityReq)
Q_DECLARE_METATYPE(ds::core::IdentityError)

#endif // IDENTITY_H
