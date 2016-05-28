#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"

#include <QtCore>
#include <QString>

class ChatMessagesModel;
class ContactsModel;

class ContactData : public QObject
{
    Q_OBJECT

public:
    enum AnonymityLevel {
        TRIVIAL = 1,
        NORMAL,
        HIGH,
        CRITICAL
    };

private:
    Q_ENUMS(AnonymityLevel)

    Q_PROPERTY(QString handle READ getHandle NOTIFY handleChanged)
    Q_PROPERTY(QString profileName READ getProfileName NOTIFY profileNameChanged)
    Q_PROPERTY(QString profileText READ getProfileText NOTIFY profileTextChanged)
    Q_PROPERTY(QString ourNickname READ getOurNickname WRITE setOurNickname NOTIFY ourNicknameChanged)
    Q_PROPERTY(QString ourGeneralNotes READ getOurGeneralNotes WRITE setOurGeneralNotes NOTIFY ourGeneralNotesChanged)
    Q_PROPERTY(QDateTime createdTime READ getCreatedTime NOTIFY createdTimeChanged)
    Q_PROPERTY(QDateTime firstContact READ getFirstContact NOTIFY firstContactChanged)
    Q_PROPERTY(AnonymityLevel anonymity READ getAnonymity WRITE setAnonymity NOTIFY anonymityChanged)
    Q_PROPERTY(AnonymityLevel requiredAnonymity READ getRequiredAnonymity NOTIFY requiredAnonymityChanged)
    Q_PROPERTY(bool canSave READ getCanSave NOTIFY canSaveChanged)
    Q_PROPERTY(QString client READ getClient NOTIFY clientChanged)

public:
    ContactData();
    ContactData(const std::shared_ptr<darkspeak::Api::Buddy>& buddy,
                ContactsModel *parent);

    Q_INVOKABLE void save();
    Q_INVOKABLE void load();

public slots:
    QString getHandle() const;

    QString getProfileName() const;

    QString getProfileText() const;

    QString getOurNickname() const;
    void setOurNickname(const QString &value);

    QString getOurGeneralNotes() const;
    void setOurGeneralNotes(const QString &value);

    QDateTime getCreatedTime() const;

    QDateTime getFirstContact() const;

    QDateTime getLastSeen() const;

    AnonymityLevel getAnonymity() const;
    void setAnonymity(const AnonymityLevel &value);

    AnonymityLevel getRequiredAnonymity() const;

    bool getCanSave() const;

    QString getClient() const;

signals:
    void handleChanged();
    void profileNameChanged();
    void profileTextChanged();
    void createdTimeChanged();
    void firstContactChanged();
    void ourNicknameChanged();
    void ourGeneralNotesChanged();
    void anonymityChanged();
    void requiredAnonymityChanged();
    void canSaveChanged();
    void clientChanged();

private:
    // Visible data
    QString handle;
    QString profileName;
    QString profileText;
    QString ourNickname;
    QString ourGeneralNotes;
    QString client;
    QDateTime createdTime;
    QDateTime firstContact;
    QDateTime lastSeen;
    AnonymityLevel anonymity;
    AnonymityLevel requiredAnonymity;

    // Private
    std::shared_ptr<darkspeak::Api::Buddy> buddy_;
    ContactsModel *model_ = nullptr;
};

