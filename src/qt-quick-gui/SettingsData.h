#pragma once

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak/Config.h"

#include <QtCore>
#include <QString>

class ChatMessagesModel;
class ContactsModel;
class ImageProvider;

class SettingsData : public QObject
{
    Q_OBJECT

    // Personal info
    Q_PROPERTY(QString handle READ handle WRITE setHandle NOTIFY handleChanged)
    Q_PROPERTY(QString nickname READ nickname WRITE setNickname NOTIFY nicknameChanged)
    Q_PROPERTY(int status READ status WRITE setStatus NOTIFY statusChanged)
    Q_PROPERTY(QString profileText READ profileText WRITE setProfileText NOTIFY profileTextChanged)
    Q_PROPERTY(QUrl avatar READ avatar WRITE setAvatar NOTIFY avatarChanged)

    // Tor
    Q_PROPERTY(QString torIncomingHost READ torIncomingHost WRITE setTorIncomingHost NOTIFY torIncomingHostChanged)
    Q_PROPERTY(unsigned int torIncomingPort READ torIncomingPort WRITE setTorIncomingPort NOTIFY torIncomingPortChanged)
    Q_PROPERTY(QString torOutgoingHost READ torOutgoingHost WRITE setTorOutgoingHost NOTIFY torOutgoingHostChanged)
    Q_PROPERTY(unsigned int torOutgoingPort READ torOutgoingPort WRITE setTorOutgoingPort NOTIFY torOutgoingPortChanged)

public:
    SettingsData(QObject *parent = nullptr);
    SettingsData(const std::shared_ptr<darkspeak::Config>& config,
                 ImageProvider *provider,
                 QObject *parent);

    unsigned int torOutgoingPort() const;
    unsigned int torIncomingPort() const;
    QString torOutgoingHost() const;
    QString torIncomingHost() const;

public slots:
    QString handle() const;
    void setHandle(QString handle);
    QString nickname() const;
    void setNickname(QString nickname);
    int status() const;
    void setStatus(int status);
    QString profileText() const;
    void setProfileText(QString text);
    QUrl avatar() const;
    void setAvatar(QUrl url);
    void setTorOutgoingPort(unsigned int torOutgoingPort);
    void setTorIncomingPort(unsigned int torIncomingPort);
    void setTorOutgoingHost(QString torOutgoingHost);
    void setTorIncomingHost(QString torIncomingHost);

signals:
    void handleChanged(const QString&);
    void nicknameChanged(const QString&);
    void statusChanged(int);
    void profileTextChanged(const QString&);
    void avatarChanged();
    void torOutgoingPortChanged(unsigned int torOutgoingPort);
    void torIncomingPortChanged(unsigned int torIncomingPort);
    void torOutgoingHostChanged(QString torOutgoingHost);

    void torIncomingHostChanged(QString torIncomingHost);

private:
    static std::vector<char> GetArgb(const std::shared_ptr<QImage>& image);

    std::shared_ptr<darkspeak::Config> config_;
    ImageProvider *image_provider_;
    mutable bool loaded_avatar_ = false;
};
