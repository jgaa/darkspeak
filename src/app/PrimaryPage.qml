import QtQuick

Loader {
    id: host
    required property int pageNumber
    required property string pageSource
    property bool visited: false
    Component.onCompleted: {
        if (manager.currentPage === pageNumber)
            visited = true;
    }
    active: visited
    visible: manager.currentPage === pageNumber
    source: pageSource
    asynchronous: pageNumber !== 3
    Connections {
        target: manager
        function onCurrentPageChanged() {
            if (manager.currentPage === host.pageNumber)
                host.visited = true;
        }
    }
}
