
#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak-gui.h"

#include "SettingsData.h"

using namespace std;
using namespace darkspeak;
using namespace war;

SettingsData::SettingsData(QObject* parent): QObject(parent)
{
}


SettingsData::SettingsData(const std::shared_ptr<darkspeak::Config>& config,
                           QObject *parent)
: QObject(parent), config_{config}
{

}

QString SettingsData::handle() const
{
    if (config_)
        return {config_->Get(Config::HANDLE, "").c_str()};
    return {};
}

void SettingsData::setHandle(QString newHandle)
{
    auto current = handle();

    if (current != newHandle) {
        config_->Set(Config::HANDLE, newHandle.toStdString());
        emit handleChanged(newHandle);
    }

}

QString SettingsData::nickname() const
{
    if (config_)
        return {config_->Get(Config::PROFILE_NAME, "").c_str()};
    return {};
}

void SettingsData::setNickname(QString newNickname)
{
    auto current = nickname();

    if (current != newNickname) {
        config_->Set(Config::PROFILE_NAME, newNickname.toStdString());
        emit nicknameChanged(newNickname);
    }
}

QString SettingsData::profileText() const
{
    if (config_)
        return {config_->Get(Config::PROFILE_INFO, "").c_str()};
    return {};
}

void SettingsData::setProfileText(QString text)
{
    auto current = profileText();

    if (current != text) {
        config_->Set(Config::PROFILE_INFO, text.toStdString());
        emit profileTextChanged(text);
    }
}

int SettingsData::status() const
{
    if (config_)
        return {config_->Get<int>(Config::PROFILE_STATUS,
            int(Config::PROFILE_STATUS_DEFAULT))};
    return 0;
}

void SettingsData::setStatus(int newStatus)
{
    auto current = status();

    if (current != newStatus) {
        config_->Set(Config::PROFILE_STATUS, newStatus);
        emit statusChanged(newStatus);
    }
}

QString SettingsData::torOutgoingHost() const
{
    return {config_->Get(Config::TOR_HOSTNAME, Config::TOR_HOSTNAME_DEFAULT).c_str()};
}

void SettingsData::setTorOutgoingHost(QString newTorOutgoingHost)
{
    if (torOutgoingHost() == newTorOutgoingHost)
            return;

    config_->Set(Config::TOR_HOSTNAME, newTorOutgoingHost.toStdString());
    emit torOutgoingHostChanged(newTorOutgoingHost);
}

unsigned int SettingsData::torOutgoingPort() const
{
    return config_->Get<unsigned>(Config::TOR_PORT, Config::TOR_PORT_DEFAULT);
}


void SettingsData::setTorOutgoingPort(unsigned int newTorOutgoingPort)
{
    if (torOutgoingPort() == newTorOutgoingPort)
        return;

    config_->Set<unsigned>(Config::TOR_PORT, newTorOutgoingPort);
    emit torOutgoingPortChanged(newTorOutgoingPort);
}

QString SettingsData::torIncomingHost() const
{
    return {config_->Get(Config::SERVICE_HOSTNAME, Config::SERVICE_HOSTNAME_DEFAULT).c_str()};
}

void SettingsData::setTorIncomingHost(QString newTorIncomingHost)
{
    if (torIncomingHost() == newTorIncomingHost)
            return;

    config_->Set(Config::SERVICE_HOSTNAME, newTorIncomingHost.toStdString());
    emit torIncomingHostChanged(newTorIncomingHost);
}

unsigned int SettingsData::torIncomingPort() const
{
    return config_->Get<unsigned>(Config::SERVICE_PORT, Config::SERVICE_PORT_DEFAULT);
}

void SettingsData::setTorIncomingPort(unsigned int newTorIncomingPort)
{
    if (torIncomingPort() == newTorIncomingPort)
            return;

    config_->Set<unsigned>(Config::SERVICE_PORT, newTorIncomingPort);
    emit torIncomingPortChanged(newTorIncomingPort);
}

