import QtQuick 2.0

// Require use of QApplication, which crash when we load the main qml page
//import Qt.labs.platform 1.1
import QtQuick.Dialogs 1.3


FileDialog {
    folder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
}
