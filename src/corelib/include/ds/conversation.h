#ifndef CONVERSATION_H
#define CONVERSATION_H

#include <set>

#include <QDir>
#include <QObject>

#include "ds/contact.h"

namespace ds {
namespace core {

class Identity;

class Conversation : public QObject, public std::enable_shared_from_this<Conversation>
{
    Q_OBJECT
public:
    using ptr_t = std::shared_ptr<Conversation>;
    using participants_t = std::vector<Contact::ptr_t>;

    enum Type {
        PRIVATE_P2P
    };
    Q_ENUM(Type)

    Conversation(QObject& parent);
    Conversation(QObject& parent, QString name,
                 QString topic, Contact *participant);

    Q_PROPERTY(int id READ getId)
    Q_PROPERTY(Type type READ getType)
    Q_PROPERTY(QString name READ getName WRITE setName NOTIFY nameChanged)
    Q_PROPERTY(QUuid uuid READ getUuid)
    Q_PROPERTY(participants_t participants READ getParticipants NOTIFY participantsChanged)
    Q_PROPERTY(Contact *participant READ getFirstParticipant NOTIFY participantsChanged) // p2p
    Q_PROPERTY(QString topic READ getTopic WRITE setTopic NOTIFY topicChanged)
    Q_PROPERTY(QDateTime created READ getCreated)
    Q_PROPERTY(QDateTime lastActiviry READ getLastActivity NOTIFY lastActivityChanged)
    Q_PROPERTY(int unread READ getUnread WRITE setUnread NOTIFY unredChanged)

    Q_INVOKABLE void incrementUnread();
    Q_INVOKABLE void sendMessage(const QString& text);
    Q_INVOKABLE void sendFile(const QVariantMap& args);

    void incomingMessage(Contact *contact, const MessageData& data);
    void incomingFileOffer(Contact *contact, const PeerFileOffer& offer);

    int getId() const noexcept;
    QString getName() const noexcept;
    void setName(const QString& name);
    QUuid getUuid() const noexcept;
    participants_t getParticipants() const;
    Contact *getFirstParticipant() const;
    QString getTopic() const noexcept;
    void setTopic(const QString& topic);
    Type getType() const noexcept;
    QByteArray getHash() const noexcept;
    QDateTime getCreated() const noexcept;
    QDateTime getLastActivity() const noexcept;
    void setLastActivity(const QDateTime& when);
    void touchLastActivity();
    int getUnread() const noexcept;
    void setUnread(const int value);
    int getIdentityId() const noexcept;
    Identity *getIdentity() const;
    bool haveParticipant(const Contact& contact) const;
    QString getFilesLocation() const;


    /*! Add the new Conversation to the database. */
    void addToDb();

    /*! Delete from the database */
    void deleteFromDb();

    static Conversation::ptr_t load(QObject& parent, const QUuid& uuid);
    static Conversation::ptr_t load(QObject& parent, int identity, const QByteArray& hash);

    const char *getTableName() const noexcept { return "conversation"; }

    // Only valid for PRIVATE_P2P conversations
    Contact::ptr_t getParticipant() const;

    static QByteArray calculateHash(const Contact& contact);

signals:
    void nameChanged();
    void participantsChanged();
    void topicChanged();
    void lastActivityChanged();
    void unredChanged();

public slots:

private:
    static QString getSelectStatement(const QString& where);

    static Conversation::ptr_t load(QObject& parent, const std::function<void(QSqlQuery&)>& prepare);

    int id_ = {};
    int identity_ = {};
    QString name_;
    QUuid uuid_;
    std::set<QUuid> participants_;
    mutable participants_t real_perticipants_;
    QString topic_;
    Type type_ = PRIVATE_P2P;
    QByteArray hash_;
    QDateTime created_;
    QDateTime lastActivity_;
    int unread_ = {};
};

}}

Q_DECLARE_METATYPE(ds::core::Conversation *)

#endif // CONVERSATION_H
