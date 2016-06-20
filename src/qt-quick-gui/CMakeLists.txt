
project(darkspeak-gui)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt5Widgets REQUIRED)
find_package(Qt5 COMPONENTS Quick Core)
include_directories(
    ${Qt5Quick_INCLUDES}
    ${Qt5Widgets_INCLUDES}
    ${Qt5Core_INCLUDES}
    )
add_definitions(${Qt5Quick_DEFINITIONS})
set(CMAKE_CXX_FLAGS "${Qt5Quick_EXECUTABLE_COMPILE_FLAGS}")

set(QML_SRC
    qml/main.qml
    qml/AddContactDlg.qml
    qml/ContactDelegate.qml
    qml/MainToolBar.qml
    qml/ContactsListView.qml
    qml/ChatView.qml
    qml/EditBuddy.qml
    qml/DataTextRo.qml
    qml/DataText.qml
    qml/OwnInfoTab.qml
    qml/SettingsDlg.qml
    qml/TorSettingsTab.qml
    qml/IncomingFileRequest.qml
    )

set(SOURCES
    main.cpp
    DarkRoot.cpp
    ContactsModel.cpp
    ChatMessagesModel.cpp
    ContactData.cpp
    SettingsData.cpp
    )

set (HEADERS
    DarkRoot.h
    ContactsModel.h
    ChatMessagesModel.h
    ContactData.h
    SettingsData.h
    )

qt5_add_resources(RESOURCES resources.qrc)
add_executable(darkspeak-gui ${GUI_TYPE} ${SOURCES} ${HEADERS} ${RESOURCES} ${QML_SRC})
set_target_properties(darkspeak-gui PROPERTIES DEBUG_OUTPUT_NAME darkspeak-guiD)
target_link_libraries(darkspeak-gui
    darkspeak_api
    darkspeak_md5
    ${DEFAULT_LIBRARIES}
    ${Qt5Core_LIBRARIES}
    ${Qt5Widgets_LIBRARIES}
    ${Qt5Quick_LIBRARIES}
    )
#ADD_CUSTOM_TARGET(darkspeak-gui SOURCES ${QML_SRC})

