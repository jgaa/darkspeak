project(darkspeak LANGUAGES CXX)

add_executable(${PROJECT_NAME}
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
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_17)
if(DARKSPEAK_FLATPAK_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DARKSPEAK_FLATPAK_BUILD=1)
endif()
target_link_libraries(${PROJECT_NAME} PRIVATE
    models prot tor core crypto
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2
    Qt6::Sql Qt6::Svg Qt6::Widgets)
