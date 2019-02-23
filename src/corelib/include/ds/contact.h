#ifndef CONTACT_H
#define CONTACT_H

#include <memory>

#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QString>
#include <QString>
#include <QUuid>
#include <QVariant>
#include <QtGui/QImage>

#include "ds/dscert.h"
#include "ds/peerconnection.h"

namespace ds {
namespace core {

class Identity;

//Q_NAMESPACE
//enum InitiatedBy {
//    IB_ME,
//    IB_THEM
//};

//Q_ENUM_NS(InitiatedBy)

//enum ContactState {
//    CS_PENDING, // Never successfully connected to peers address
//    CS_WAITING_FOR_ACCEPTANCE, // We have sent addme, Waiting for ack
//    CS_ACCEPTED, // We have experienced connectins in both directions, and received and / or sent ack
//    CS_REJECTED,
//    CS_BLOCKED
//};

//Q_ENUM_NS(ContactState)

struct ContactData;

/*! Representation of a contact.
 *
 * The Contact is owned by an Identity instance
 *
 */
struct Contact : public QObject {
        Q_OBJECT

public:
    using ptr_t = std::shared_ptr<Contact>;
    using data_t = std::unique_ptr<ContactData>;

    enum OnlineStatus {
        DISCONNECTED, // We are not trying to connect so we don't know
        OFFLINE, // We are unable to connect
        CONNECTING,
        ONLINE
    };

    Q_ENUM(OnlineStatus)

    enum InitiatedBy {
        ME,
        THEM
    };

    Q_ENUM(InitiatedBy)

    struct Connection {
        Connection(const PeerConnection::ptr_t& peerVal, Contact& ownerVal)
            : peer{peerVal}, owner{ownerVal} {}

        ~Connection();

        const PeerConnection::ptr_t peer;
        OnlineStatus status = DISCONNECTED;
        Contact& owner;
    };

    enum ContactState {
        PENDING, // Never successfully connected to peers adsdress
        WAITING_FOR_ACCEPTANCE, // We have sent addme, Waiting for ack
        ACCEPTED, // We have experienced connectins in both directions, and received and / or sent ack
        REJECTED,
        BLOCKED
    };

    Q_ENUM(ContactState)


    Contact(QObject& parent,
            const int dbId, // -1 if the identity is new
            const bool online,
            data_t data);

    Q_PROPERTY(int id READ getId)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QString nickName READ getNickName WRITE setNickName NOTIFY nickNameChanged)
    Q_PROPERTY(QString group READ getGroup WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(QByteArray address READ getAddress WRITE setAddress NOTIFY addressChanged)
    Q_PROPERTY(QString notes READ getNotes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(QString avatar READ getAvatarUri NOTIFY avatarChanged)
    Q_PROPERTY(QUuid uuid READ getUuid)
    Q_PROPERTY(InitiatedBy whoInitiated READ getWhoInitiated)
    Q_PROPERTY(QDateTime created READ getCreated)
    Q_PROPERTY(QDateTime lastSeen READ getLastSeen NOTIFY lastSeenChanged)
    Q_PROPERTY(bool online READ isOnline WRITE setOnline NOTIFY onlineChanged)
    Q_PROPERTY(OnlineStatus onlineStatus READ getOnlineStatus NOTIFY onlineStatusChanged)
    Q_PROPERTY(QString onlineIcon READ getOnlineIcon WRITE setOnlineIcon NOTIFY onlineIconChanged)
    Q_PROPERTY(QByteArray b58identity READ getB58EncodedIdetity)
    Q_PROPERTY(QByteArray handle READ getHandle)
    Q_PROPERTY(bool autoConnect READ isAutoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)
    Q_PROPERTY(ContactState state READ getState WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QString addMeMessage READ getAddMeMessage WRITE setAddMeMessage NOTIFY addMeMessageChanged)
    Q_PROPERTY(bool peerVerified READ isPeerVerified WRITE setPeerVerified NOTIFY peerVerifiedChanged)
    Q_PROPERTY(Identity * identity READ getIdentity)

    Q_INVOKABLE void connectToContact();
    Q_INVOKABLE void disconnectFromContact();

    static Contact::ptr_t load(QObject& parent, const QUuid &uuid);

    int getId() const noexcept;
    QString getName() const noexcept;
    void setName(const QString& name);
    QString getNickName() const noexcept;
    void setNickName(const QString& name);
    QString getGroup() const noexcept;
    void setGroup(const QString& name);
    QByteArray getAddress() const noexcept;
    void setAddress(const QByteArray& address);
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
    QDateTime getLastSeen() const noexcept;
    void setLastSeen(const QDateTime& when);
    crypto::DsCert::ptr_t getCert() const noexcept;
    QByteArray getB58EncodedIdetity() const noexcept;
    QByteArray getHandle() const noexcept;
    bool isAutoConnect() const noexcept;
    void setAutoConnect(bool value);
    InitiatedBy getWhoInitiated() const noexcept;
    ContactState getState() const noexcept;
    void setState(const ContactState state);
    QString getAddMeMessage() const noexcept;
    void setAddMeMessage(const QString& msg);
    bool isPeerVerified() const noexcept;
    void setPeerVerified(const bool verified);
    QString getOnlineIcon() const noexcept;
    void setOnlineIcon(const QString& path);
    Identity *getIdentity() const;
    OnlineStatus getOnlineStatus() const noexcept;
    void setOnlineStatus(const OnlineStatus status);
    int getIdentityId() const noexcept;

    /*! Add the new Identity to the database. */
    void addToDb();

    /*! Delete from the database */
    void deleteFromDb();

    const char *getTableName() const noexcept { return "contact"; }

    void onAddmeRequest(const PeerAddmeReq& req);

signals:
    void nameChanged();
    void nickNameChanged();
    void groupChanged();
    void addressChanged();
    void notesChanged();
    void avatarChanged();
    void onlineChanged();
    void autoConnectChanged();
    void stateChanged();
    void addMeMessageChanged();
    void lastSeenChanged();
    void peerVerifiedChanged();
    void onlineIconChanged();
    void onlineStatusChanged();
    void sendAddMeLater();
    void sendAddmeAckLater();
    void processOnlineLater();

public slots:
    void onConnectedToPeer(const std::shared_ptr<PeerConnection>& peer);

private slots:
    void onDisconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer);
    void onSendAddMeLater();
    void onSendAddmeAckLater();
    void onProcessOnlineLater();
    void onReceivedAck(const PeerAck& ack);

private:
    static void bind(QSqlQuery& query, ContactData& data);

    int id_ = -1; // Database id
    bool online_ = false;
    data_t data_;
    QString onlineIcon_ = "qrc:///images/onion-bw.svg";
    OnlineStatus onlineStatus_ = DISCONNECTED;

    std::unique_ptr<Connection> connection_;
};

struct ContactData {

    // Reference to the database-id to the Identity that own this contact
    int identity = 0;

    // Our name for the contact. If unset, same as nickname
    QString name;

    // UUID for this contact. Used if we need a folder-name or file-name
    // uniqely for this contanct. We don't want to use a hash for this, as
    // potential contacts - for example the US government trying to
    // prove what wistleblower a journalist communicated with.
    QUuid uuid = QUuid::createUuid();

    // The contacts own, chosen nickname
    QString nickName;

    // A hash from the pubkey.
    // Used for database lookups.
    QByteArray hash;

    // Our notes about the contact
    QString notes;

    // The group we place the contact in. If unset; 'Default'
    QString group;

    // Cert derived from the contacts public key
    crypto::DsCert::ptr_t cert;

    // The users onion address and port
    QByteArray address;

    // An optional image the contact can chose
    QImage avatar;

    // When the contact was created
    QDateTime created;

    // Who sent the initial addme request?
    Contact::InitiatedBy whoInitiated = Contact::ME;

    // Updated when we receive data from the contact, rounded to minute.
    QDateTime lastSeen;

    // The current state for this contact
    Contact::ContactState state = Contact::PENDING;

    // Message to send with the addme request
    QString addMeMessage;

    // true if we want to connect automatically when the related Identity is online
    bool autoConnect = true;

    // True if we ever have connected to the remote peer's address
    bool peerVerified = false;
};


}} // namepsaces

Q_DECLARE_METATYPE(ds::core::Contact *)

#endif // CONTACT_H
