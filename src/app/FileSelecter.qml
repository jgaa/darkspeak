import QtQuick

// Require use of QApplication, which crash when we load the main qml page
//import Qt.labs.platform 1.1
import QtQuick.Dialogs


FileDialog {
    currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
}
