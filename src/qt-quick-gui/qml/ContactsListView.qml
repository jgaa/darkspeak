
import QtQuick 2.4

ListView {
    delegate: ContactDelegate { id: contactDelegate }
    model: contactsModel
}
