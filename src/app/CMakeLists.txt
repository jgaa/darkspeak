project(darkspeak LANGUAGES CXX)

add_executable(${PROJECT_NAME}
    appearance.cpp
    appearance.h
    theme_data.h
    controls_style.h
    settings_session.cpp
    settings_session.h
    main.cpp
    desktop_integration.cpp
    desktop_integration.h
    filedialogmodel.cpp
    filedialogmodel.h
    tray_controller.cpp
    tray_controller.h
    qml.qrc
    )
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
if(DARKSPEAK_FLATPAK_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DARKSPEAK_FLATPAK_BUILD=1)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
    models prot tor core crypto
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::Sql Qt6::Svg Qt6::Widgets)
darkspeak_link_color_scheme(${PROJECT_NAME})

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(${PROJECT_NAME} PRIVATE
        desktop_notifications.cpp
        desktop_notifications.h)
    target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::DBus)
endif()

qt_add_qml_module(${PROJECT_NAME}
    URI DarkSpeak.Ui
    VERSION 1.0
    RESOURCE_PREFIX /
    NO_RESOURCE_TARGET_PATH
    QML_FILES
        main.qml
        AboutDialog.qml
        Log.qml
        Home.qml
        Contacts.qml
        Conversations.qml
        Chat.qml
        Identities.qml
        MainToolBar.qml
        EditIdentityDialog.qml
        EditContactDialog.qml
        AcceptContactDialog.qml
        BioDialog.qml
        EditUploadFileDialog.qml
        TorSettings.qml
        SettingsDialog.qml
        PathsSettings.qml
        LogSettings.qml
        RoundedImage.qml
        Header.qml
        FileSelecter.qml
        MessageComponent.qml
        FileComponent.qml
        FormattedTime.qml
        GeneralSettings.qml
        ResourceLimitsSettings.qml
        ResourceLimitRow.qml
        ImportTorServiceDialog.qml
        CustomWindowSizeDialog.qml
        UiDialog.qml
        EmptyState.qml
        StatusBadge.qml
        AppearanceSettings.qml
        PrimaryPage.qml
        ScaledToolButton.qml
        ScaleSelector.qml
        ScaleControl.qml
        MessageBubble.qml
        UiHarness.qml
)
