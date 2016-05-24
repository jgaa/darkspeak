
#include <QApplication>
#include <QClipboard>

#include "log/WarLog.h"

#include "DarkRoot.h"



DarkRoot::DarkRoot(darkspeak::Api& api, QObject *parent)
    : QObject(parent), api_{api}
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
    api_.GoOnline(my_info_);
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
