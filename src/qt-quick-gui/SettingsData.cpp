
#include "log/WarLog.h"

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"
#include "darkspeak-gui.h"

#include "SettingsData.h"
#include "ImageProvider.h"

using namespace std;
using namespace darkspeak;
using namespace war;

SettingsData::SettingsData(QObject* parent): QObject(parent)
{
}


SettingsData::SettingsData(const std::shared_ptr<darkspeak::Config>& config,
                           ImageProvider *provider,
                           QObject *parent)
: QObject(parent), config_{config}, image_provider_{provider}
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

QUrl SettingsData::avatar() const {
    static const string avatar_url{"image://buddy/"};
    static const string myself {"myself"};

    auto png_path = config_->Get(Config::PROFILE_AVATAR_PATH,
            Config::PROFILE_AVATAR_PATH_DEFAULT);

    if (!loaded_avatar_
        && boost::filesystem::is_regular(png_path.c_str())) {
        auto avatar = make_shared<QImage>(png_path.c_str());
        if (!avatar->isNull()) {
            image_provider_->add(myself, avatar);
            loaded_avatar_ = true;
        }
    }

    if (image_provider_->haveImage(myself)) {
        return QUrl(Convert(avatar_url + myself));
    }
    return QUrl(Convert(avatar_url + "default"));
}

//The url is always to a temporary image in the image provider
void SettingsData::setAvatar(QUrl url)
{
    auto path = url.toString().toStdString();
    auto pos = path.find_last_of('/');
    if (pos != path.npos) {
        auto key = path.substr(pos + 1);
        if (!path.empty()) {
            image_provider_->rename(key, "myself");
            image_provider_->save("myself",
                                  config_->Get(Config::PROFILE_AVATAR_PATH,
                                               Config::PROFILE_AVATAR_PATH_DEFAULT));
            auto argb = GetArgb(image_provider_->get("myself"));
            config_->Set(Config::PROFILE_AVATAR_ARGB,
                Config::EncodeArgb(argb));

            emit avatarChanged();
        }
    }
}

std::vector<char> SettingsData::GetArgb(const std::shared_ptr<QImage>& image)
{
    std::vector<char> argb;

    if (!image->isNull()) {
        auto height = image->height();
        auto width = image->width();
        argb.reserve(height * width);

        for(decltype(width) x = 0; x < width; ++x) {
            for(decltype(height) y = 0; y < height; ++y) {
                argb.push_back(image->pixel(x, y));
            }
        }
    }

    return argb;
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

