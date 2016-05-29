
#include <QApplication>
#include <QClipboard>

#include "log/WarLog.h"

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
    return new SettingsData(config_, this);
}
