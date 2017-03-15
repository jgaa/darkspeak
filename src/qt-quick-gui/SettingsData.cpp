
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

    if (!loaded_avatar_) {

        shared_ptr<QImage> avatar;
        auto encoded = config_->Get(Config::PROFILE_AVATAR_RGBA, "");
        if (!encoded.empty()) {
            auto rgba = Config::DecodeRgba(encoded);
            avatar = make_shared<QImage>(64, 64, QImage::Format_ARGB32);
            int ix = 0;
            for(int y = 0; y < 64; ++y) {
                for (int x = 0; x < 64; ++x) {
                    const uint8_t r = static_cast<uint8_t>(rgba.at(ix++));
                    const uint8_t g = static_cast<uint8_t>(rgba.at(ix++));
                    const uint8_t b = static_cast<uint8_t>(rgba.at(ix++));
                    const uint8_t a = static_cast<uint8_t>(rgba.at(ix++));
                    avatar->setPixel(x, y, qRgba(r, g, b, a));
                }
            }
        }

        if (avatar && !avatar->isNull()) {
            image_provider_->add(myself, avatar);
            loaded_avatar_ = true;
        }
    }

    if (image_provider_->haveImage(myself)) {
        return QUrl(Convert(avatar_url + myself + ":" + to_string(++sequence_)));
    }
    return QUrl(Convert(avatar_url + "default"));
}

//The url is always to a temporary image in the image provider
void SettingsData::setAvatar(QUrl url)
{
    static const string default_name{"default"};
    auto path = url.toString().toStdString();
    auto pos = path.find_last_of('/');
    if (pos != path.npos) {
        auto key = path.substr(pos + 1);

        // Strip off the ':#' part added by the qml code to redraw the image
        auto coloumn_pos = key.find_last_of(':');
        if (coloumn_pos != path.npos) {
            key.resize(coloumn_pos);
        }

        if (!path.empty() && (key.compare(default_name) != 0)){
            image_provider_->rename(key, "myself");
            auto rgba = GetRgba(image_provider_->get("myself"));
            config_->Set(Config::PROFILE_AVATAR_RGBA,
                Config::EncodeRgba(rgba));

            emit avatarChanged();
        }
    }
}

std::vector<char> SettingsData::GetRgba(const std::shared_ptr<QImage>& image)
{
    std::vector<char> rgba;

    if (image && !image->isNull()) {
        auto height = image->height();
        auto width = image->width();
        rgba.reserve(height * width);

        for(decltype(width) y = 0; y < width; ++y) {
            for(decltype(height) x = 0; x < height; ++x) {
                auto pixel = image->pixel(x, y);
                rgba.push_back(qRed(pixel));
                rgba.push_back(qGreen(pixel));
                rgba.push_back(qBlue(pixel));
                rgba.push_back(qAlpha(pixel));
            }
        }
    }

    return rgba;
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

