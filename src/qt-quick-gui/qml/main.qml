
import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Dialogs 1.1
import com.jgaa.darkspeak 1.0
import QtQuick.Layouts 1.2
import QtQuick.Extras 1.4
import QtPositioning 5.5
import org.kde.private.kquickcontrols 2.0

ApplicationWindow {
    id: main_window
    visible: true
    width: 300
    height: 400
    title: qsTr("DarkSpeak")

    toolBar: MainToolBar { id: mainToolBar }

    Connections {
        target: contactsModel

        onIncomingFile: {
            console.log("Incoming file" + fileName)

            var component = Qt.createComponent("IncomingFileRequest.qml")
            if( component.status != Component.Ready ) {
                if( component.status == Component.Error )
                    console.debug("Error:"+ component.errorString() );
                return; // or maybe throw
            }
            var dialog = component.createObject(
               main_window,
               {
                   "fileName": fileName,
                   "fileId": fileId,
                   "contact": contact
               });
            dialog.process()
        }
    }

    StackView {
       id: main_pane
       initialItem: ContactsListView {
           id: contacts
           anchors.fill: parent
           property string stateName: ""
       }
       anchors.fill: parent

       function popWindow() {
           pop()
           state = currentItem.stateName
           console.log("dept is: " + depth);
       }

       function openChatWindow(model) {
           var chatComponent = Qt.createComponent("ChatView.qml")
           var chat = chatComponent.createObject(
               main_pane, {"model":model});
           chat.model = model
           push(chat)
           state = currentItem.stateName
       }

       states: [
           State {
               name: "chat"
               PropertyChanges { target: mainToolBar; state: "chat"}
           }
       ]
   }
}

