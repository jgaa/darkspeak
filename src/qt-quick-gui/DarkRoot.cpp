
#include <QApplication>
#include <QClipboard>

#include "log/WarLog.h"
#include "war_uuid.h"

#include "darkspeak-gui.h"
#include "DarkRoot.h"
#include "SettingsData.h"


DarkRoot::DarkRoot(darkspeak::Api& api,
                   const std::shared_ptr<darkspeak::Config>& config,
                   QObject *parent)
    : QObject(parent), api_{api}, config_{config}
{
}

ContactsModel *DarkRoot::contactsModel()
{
    // Leave ownership to QML
    LOG_DEBUG_FN << "Insiatiating new ContactsModel.";
    return new ContactsModel(api_);
}

void DarkRoot::goOnline()
{
    api_.GoOnline();
}

void DarkRoot::goOffline()
{
    api_.Disconnect(true);
}

void DarkRoot::copyToClipboard(QString text)
{
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}

SettingsData* DarkRoot::settings()
{
    return new SettingsData(config_, image_provider_, this);
}

void DarkRoot::acceptFile(const QString buddyHandle, const QString fileId)
{
    AcceptFileTransferData aftd;
    aftd.buddy_id = buddyHandle.toStdString();
    aftd.uuid = get_uuid_from_string(fileId.toStdString());
    api_.AcceptFileTransfer(aftd);
}

void DarkRoot::rejectFile(const QString buddyHandle, const QString fileId)
{
    AcceptFileTransferData aftd;
    aftd.buddy_id = buddyHandle.toStdString();
    aftd.uuid = get_uuid_from_string(fileId.toStdString());
    api_.RejectFileTransfer(aftd);
}

//Make a 64 x 64 version of the image
QUrl DarkRoot::prepareAvatar(QUrl url)
{
    QImage original(url.path());
    QImage scaled = original.scaled(64, 64,
                        Qt::KeepAspectRatioByExpanding,
                        Qt::SmoothTransformation);

    int center_x = max(0, (scaled.width() / 2) - 32);
    int center_y = max(0, (scaled.height() / 2) - 32);
    auto avatar = make_shared<QImage>(scaled.copy(center_x, center_y, 64, 64));

    image_provider_->add("myself~", avatar);
    static const string avatar_url{"image://buddy/myself~"};
    return QUrl(Convert(avatar_url));
}
