
import QtQuick 2.4

ListView {
    height: parent.height
    width: parent.width
    delegate: ContactDelegate { id: contactDelegate }
    model: contactsModel
}
