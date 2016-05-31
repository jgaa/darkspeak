import QtQuick 2.4
import QtQuick.Controls 1.3
import QtQuick.Layouts 1.2
import QtQuick.Dialogs 1.2
import com.jgaa.darkspeak 1.0

Dialog {
    id: root
    standardButtons: StandardButton.Save | StandardButton.Cancel
    title: "Settings"
    property SettingsData settings: null
    property int dlg_width: main_window.width > 300 ? 300 : main_window.width
    property int dlg_height: main_window.height > 370 ? 370 : main_window.height
    width: dlg_width
    height: dlg_height


    TabView {
        id: ownInfoView
        width: dlg_width - 6
        height: dlg_height - 30

        Tab {
            title: "You"
            OwnInfoTab {}
        }

        Tab {
            title: "Tor"
            TorSettingsTab{}
        }
    }


    onButtonClicked: {
        if (clickedButton === StandardButton.Save) {

            var own_info = ownInfoView.getTab(0).item
            console.log("Saving settings") // + ownInfoView.getTab(0).item.status)

            settings.handle = own_info.handle
            settings.status = own_info.status
            settings.nickname = own_info.nickname
            settings.profileText = own_info.profileText

            var tor = ownInfoView.getTab(1).item
            settings.torIncomingHost = tor.incomingHost
            if (!isNaN(tor.incomingPort)) {
                settings.torIncomingPort = parseInt(tor.outgoingHost);
            }
            settings.torOutgoingHost = tor.incomingHost
            if (!isNaN(tor.outgoingPort)) {
                settings.torOutgoingPort = parseInt(tor.outgoingPort);
            }

        } else {
            console.log("Cancelled" + clickedButton)
            settings = null;
            settings = darkRoot.settings()
        }
    }
}


