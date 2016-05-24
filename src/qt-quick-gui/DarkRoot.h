#pragma once

#include <QtCore>

#include "darkspeak/darkspeak.h"
#include "darkspeak/Api.h"

#include "ContactsModel.h"

// Todo: Make signals for online/offline state change
// Todo: Make it possible to edit my_info_;

using namespace std;
using namespace darkspeak;
using namespace war;
/*! Interface to QML.
 *
 * The gui app has one instance of this class. QML use it
 * to instatiate models and access properties of the IM layer.
 */
class DarkRoot : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString programName READ programName)
public:
    explicit DarkRoot(darkspeak::Api& api, QObject *parent = nullptr);

    QString programName() const {
        return "DarkSpeak";
    }

    Q_INVOKABLE ContactsModel* contactsModel();

    Q_INVOKABLE void goOnline();
    Q_INVOKABLE void goOffline();
    Q_INVOKABLE void copyToClipboard(QString text);

    darkspeak::Api::Info& GetInfo() { return my_info_; }

private:
    darkspeak::Api& api_;
    darkspeak::Api::Info my_info_;
};