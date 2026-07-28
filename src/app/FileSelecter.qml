import QtQuick

Item {
    function open(title, initialPath, filters) {
        return fileDialogs.openFile(title, initialPath, filters)
    }
}
