project(darkspeak LANGUAGES CXX)

add_executable(${PROJECT_NAME}
    main.cpp
    qml.qrc
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} models prot tor core crypto)
target_link_libraries(${PROJECT_NAME} PRIVATE
    models prot tor core crypto
    Qt6::QuickControls2 Qt6::Quick Qt6::Widgets
    Qt6::Core Qt6::Network Qt6::Sql Qt6::Svg
    sodium)
