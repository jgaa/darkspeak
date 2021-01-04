project(darkspeak LANGUAGES CXX)

add_executable(${PROJECT_NAME}
    main.cpp
    qml.qrc
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} models prot tor core crypto)
target_link_libraries(${PROJECT_NAME} PRIVATE
    models prot tor core crypto
    Qt5::QuickControls2 Qt5::Quick Qt5::Widgets
    Qt5::Core Qt5::Network Qt5::Sql Qt5::Svg
    sodium)
