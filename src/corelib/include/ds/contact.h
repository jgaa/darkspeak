#ifndef CONTACT_H
#define CONTACT_H

#include <memory>
#include <deque>
#include <set>

#include <QDateTime>
#include <QString>
#include <QUuid>
#include <QVariant>
#include <QtGui/QImage>

#include "ds/dscert.h"
#include "ds/peerconnection.h"
#include "ds/message.h"

class QSqlQuery;

namespace ds {
namespace core {

class Identity;
class Conversation;
struct ContactData;
class File;

/*! Representation of a contact.
 *
 * The Contact is owned by an Identity instance
 *
 */
struct Contact : public QObject, public std::enable_shared_from_this<Contact> {
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
        Contact& owner;
    };

    enum ContactState {
        PENDING, // Never successfully connected to peers adsdress
        WAITING_FOR_ACCEPTANCE, // We have sent addme, Waiting for ack
        ACCEPTED, // We have experienced connectins in both directions, and received and / or sent ack
        REJECTED,
        BLOCKED // They blocked us
    };

    Q_ENUM(ContactState)


    Contact(const int dbId, // -1 if the identity is new
            const bool online,
            data_t data);

    ~Contact() override;

    Q_PROPERTY(int id READ getId CONSTANT)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QString nickName READ getNickName WRITE setNickName NOTIFY nickNameChanged)
    Q_PROPERTY(QString group READ getGroup WRITE setGroup NOTIFY groupChanged)
    Q_PROPERTY(QByteArray address READ getAddress WRITE setAddress NOTIFY addressChanged)
    Q_PROPERTY(QString notes READ getNotes WRITE setNotes NOTIFY notesChanged)
    Q_PROPERTY(QImage avatar READ getAvatar WRITE setAvatar NOTIFY avatarChanged)
    Q_PROPERTY(QString avatarUrl READ getAvatarUrl NOTIFY avatarUrlChanged)
    Q_PROPERTY(QUuid uuid READ getUuid CONSTANT)
    Q_PROPERTY(InitiatedBy whoInitiated READ getWhoInitiated CONSTANT)
    Q_PROPERTY(QDateTime created READ getCreated CONSTANT)
    Q_PROPERTY(QDateTime lastSeen READ getLastSeen NOTIFY lastSeenChanged)
    Q_PROPERTY(bool online READ isOnline WRITE setOnline NOTIFY onlineChanged)
    Q_PROPERTY(OnlineStatus onlineStatus READ getOnlineStatus NOTIFY onlineStatusChanged)
    Q_PROPERTY(QString onlineIcon READ getOnlineIcon WRITE setOnlineIcon NOTIFY onlineIconChanged)
    Q_PROPERTY(QByteArray b58identity READ getB58EncodedIdetity CONSTANT)
    Q_PROPERTY(QByteArray handle READ getHandle CONSTANT)
    Q_PROPERTY(bool autoConnect READ isAutoConnect WRITE setAutoConnect NOTIFY autoConnectChanged)
    Q_PROPERTY(ContactState state READ getState WRITE setState NOTIFY stateChanged)
    Q_PROPERTY(QString addMeMessage READ getAddMeMessage WRITE setAddMeMessage NOTIFY addMeMessageChanged)
    Q_PROPERTY(bool peerVerified READ isPeerVerified WRITE setPeerVerified NOTIFY peerVerifiedChanged)
    Q_PROPERTY(Identity * identity READ getIdentity CONSTANT)
    Q_PROPERTY(bool sentAvatar READ isAvatarSent WRITE setSentAvatar NOTIFY sentAvatarChanged)
    Q_PROPERTY(bool blocked READ isBlocked WRITE setBlocked NOTIFY blockedChanged)
    Q_PROPERTY(bool iBlocked READ iBlocked WRITE setBlocked NOTIFY blockedChanged)
    Q_PROPERTY(bool theyBlocked READ theyBlocked WRITE setBlocked NOTIFY blockedChanged)
    Q_PROPERTY(bool sendBlockNotice READ getSendBlockNotice WRITE setSendBlockNotice NOTIFY sendBlockNoticeChanged)

    Q_INVOKABLE void connectToContact();
    Q_INVOKABLE void disconnectFromContact(bool manual = false);
    Q_INVOKABLE Conversation *getDefaultConversation();

    static Contact::ptr_t load(const QUuid &uuid);

    int getId() const noexcept;
    QString getName() const noexcept;
    void setName(QString name);
    QString getNickName() const noexcept;
    void setNickName(QString name);
    QString getGroup() const noexcept;
    void setGroup(const QString& name);
    QByteArray getAddress() const noexcept;
    void setAddress(const QByteArray& address);
    QString getNotes() const noexcept;
    void setNotes(QString notes);
    QImage getAvatar() const noexcept;
    QString getAvatarUrl() const noexcept;
    void setAvatar(const QImage& avatar);
    bool isAvatarSent() const noexcept;
    void setSentAvatar(const bool value);
    bool isOnline() const noexcept;
    void setOnline(const bool value);
    QUuid getUuid() const noexcept;
    QByteArray getHash() const noexcept;
    QDateTime getCreated() const noexcept;
    QDateTime getLastSeen() const noexcept;
    void setLastSeen(const QDateTime& when);
    void touchLastSeen();
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
    bool wasManuallyDisconnected() const noexcept;
    void setManuallyDisconnected(bool state);
    bool isBlocked() const;
    bool iBlocked() const;
    bool theyBlocked() const;
    void setBlocked(bool value);
    bool getSendBlockNotice() const;
    void setSendBlockNotice(bool value);

    void queueMessage(const Message::ptr_t& message);
    void queueFile(const std::shared_ptr<File>& file);
    void sendAvatar(const QImage& avatar);

    /*! Add the new Identity to the database. */
    void addToDb();

    /*! Delete from the database */
    void deleteFromDb();

    const char *getTableName() const noexcept { return "contact"; }

    void onAddmeRequest(const PeerAddmeReq& req);
    //void onReceivedMessage(const PeerMessage& msg, Conversation *conversation = {});
    void sendAck(const QString& what, const QString& status, const QString& data = {});
    static bool validateNick(const QString& nickName);
    void sendUserInfo();

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
    //void processOnlineLater();
    void manuallyDisconnectedChanged();
    void sentAvatarChanged();
    void avatarUrlChanged();
    void blockedChanged();
    void sendBlockNoticeChanged();

public slots:
    void onConnectedToPeer(const std::shared_ptr<PeerConnection>& peer);
    void onIncomingPeer(const std::shared_ptr<PeerConnection>& peer);

private slots:
    void onDisconnectedFromPeer(const std::shared_ptr<PeerConnection>& peer);
    void onSendAddMeLater();
    void onSendAddmeAckLater();
    void onReceivedAck(const PeerAck& ack);
    bool procesMessageQueue();
    bool processFilesQueue();
    bool processFileBlocks();
    void onReceivedMessage(const PeerMessage& msg);
    void onReceivedUserInfo(const PeerUserInfo& uinfo);
    void onReceivedFileOffer(const PeerFileOffer& msg);
    void onReceivedAvatar(const PeerSetAvatarReq& avatar);
    void onOutputBufferEmptied();

private:
    static void bind(QSqlQuery& query, ContactData& data);
    void loadMessageQueue();
    void loadFileQueue();
    void queueTransfer(const std::shared_ptr<File>& file);
    void clearFileQueues();
    void prepareForNewConnection();
    void scheduleProcessOnlineLater();
    void processOnlineLater();
    void sendBlockNotification();

    // Sends reject message if the conversation is not the default and don't exist.
    Conversation *getRequestedOrDefaultConversation(const QByteArray& hash,
                                                    PeerConnection& peer,
                                                    const QString& what,
                                                    const QByteArray& id);

    int id_ = -1; // Database id
    bool online_ = false;
    bool loadedMessageQueue_ = false;
    bool loadedFileQueue_ = false;
    data_t data_;
    QString onlineIcon_ = "qrc:///images/onion-bw.svg";
    OnlineStatus onlineStatus_ = DISCONNECTED;
    bool sentAvatarPendingAck_ = false;
    bool avatarUrlChanging_ = false;

    std::unique_ptr<Connection> connection_;
    std::deque<Message::ptr_t> messageQueue_;
    std::deque<Message::ptr_t> unconfirmedMessageQueue_; // Waiting for ack
    std::deque<std::shared_ptr<File>> fileQueue_;
    std::set<std::shared_ptr<File>> transferringFileQueue_; // Currently transferring, (we have slots)
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

    // True if the user manually disconnected the contact
    // Auto-connect will be disabled for the duration of the application-session.
    bool manuallyDisconnected = false;

    // If set, this overrides the default download-path for the contact
    QString downloadPath;

    bool sentAvatar = false;

    // We bloc the contact
    bool isBlocked = false;

    // Tell them they are blocked
    bool sendBlockNotice = true;
};


}} // namepsaces

Q_DECLARE_METATYPE(ds::core::Contact *)


#endif // CONTACT_H
