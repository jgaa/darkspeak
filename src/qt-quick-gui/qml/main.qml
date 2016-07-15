
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

    ListModel {
            id: filesMock

            ListElement { name: "test.zip"; icon: "qrc:/images/FileDownload.svg"; buddy_name: "jgaa"; percent: 100; size: "123b" }
            ListElement { name: "nicecat.zip"; icon: "qrc:/images/FileDownload.svg"; buddy_name: "jgaa"; percent: 70; size: "67m" }
            ListElement { name: "nicedog.zip"; icon: "qrc:/images/FileUpload.svg"; buddy_name: "gakke"; percent: 0; size: "2.45g"}
        }

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
//           var chatComponent = Qt.createComponent("ChatView.qml")
//           var chat = chatComponent.createObject(
//               main_pane, {"model":model});
//           chat.model = model
//           push(chat)
//           state = currentItem.stateName
           openWindow("ChatView.qml", model)
       }

       function openTransfersWindow(model) {
           openWindow("FileTransfersView.qml", model)
       }

       function openWindow(qmlFile, model) {
           var uiComponent = Qt.createComponent(qmlFile)
           if( uiComponent.status === Component.Error ) {
                   console.debug("Error: "+ uiComponent.errorString());
               return;
           }
           var uiWin = uiComponent.createObject(main_pane, {"model":model});
           uiWin.model = model
           push(uiWin)
           state = currentItem.stateName
       }

       states: [
           State {
               name: "chat"
               PropertyChanges { target: mainToolBar; state: "chat"}
           },
           State {
               name: "transfers"
               PropertyChanges { target: mainToolBar; state: "transfers"}
           }
       ]
   }
}

