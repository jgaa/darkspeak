# Can also be configured independently, without Tor, the OS vault, or application libraries.
cmake_minimum_required(VERSION 3.21)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(darkspeak_ui_tests LANGUAGES CXX)
    enable_testing()
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
endif()
find_package(Qt6 6.10 REQUIRED COMPONENTS Core Gui Widgets Qml Quick QuickControls2 QuickTest Test Network Sql)
get_filename_component(DS_UI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include(${DS_UI_ROOT}/cmake/KdeColorScheme.cmake)
endif()
add_library(darkspeak_ui_test_support STATIC
    ${DS_UI_ROOT}/src/corelib/src/eventjournal.cpp
    ${DS_UI_ROOT}/src/corelib/include/ds/eventjournal.h
    ${DS_UI_ROOT}/src/app/appearance.cpp
    ${DS_UI_ROOT}/src/app/appearance.h
    ${DS_UI_ROOT}/src/app/settings_session.cpp
    ${DS_UI_ROOT}/src/app/settings_session.h
    ${DS_UI_ROOT}/src/app/desktop_integration.cpp
    ${DS_UI_ROOT}/src/app/desktop_integration.h)
target_compile_features(darkspeak_ui_test_support PUBLIC cxx_std_20)
target_include_directories(darkspeak_ui_test_support PUBLIC ${DS_UI_ROOT}/src/app ${DS_UI_ROOT}/src/corelib/include ${DS_UI_ROOT}/dependencies/logfault/include)
target_link_libraries(darkspeak_ui_test_support PUBLIC Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Qml Qt6::Network Qt6::Sql)
darkspeak_link_color_scheme(darkspeak_ui_test_support)
add_executable(darkspeak_ui_test quick_test.cpp mock_ui_model.h ${DS_UI_ROOT}/src/app/qml.qrc)
target_link_libraries(darkspeak_ui_test PRIVATE darkspeak_ui_test_support Qt6::QuickTest Qt6::QuickControls2)
target_compile_definitions(darkspeak_ui_test PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}/qml")
add_test(NAME ui_components COMMAND darkspeak_ui_test)
set_tests_properties(ui_components PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QT_QUICK_CONTROLS_STYLE=org.kde.desktop" TIMEOUT 30)
add_executable(darkspeak_ui_preferences_test preferences_test.cpp)
target_link_libraries(darkspeak_ui_preferences_test PRIVATE darkspeak_ui_test_support Qt6::Test)
darkspeak_link_color_scheme(darkspeak_ui_preferences_test)
add_test(NAME ui_preferences COMMAND darkspeak_ui_preferences_test)
set_tests_properties(ui_preferences PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 20)
find_package(Qt6 6.10 REQUIRED COMPONENTS Sql)
add_executable(darkspeak_history_query_test history_query_test.cpp)
target_compile_features(darkspeak_history_query_test PRIVATE cxx_std_20)
target_include_directories(darkspeak_history_query_test PRIVATE ${DS_UI_ROOT}/src/modelslib/include)
target_link_libraries(darkspeak_history_query_test PRIVATE Qt6::Core Qt6::Sql Qt6::Test)
add_test(NAME ui_history_query COMMAND darkspeak_history_query_test)
add_executable(darkspeak_ui_preview preview.cpp mock_ui_model.h)
target_compile_definitions(darkspeak_ui_preview PRIVATE DS_UI_SOURCE_DIR="${DS_UI_ROOT}/src/app")
target_link_libraries(darkspeak_ui_preview PRIVATE darkspeak_ui_test_support Qt6::Quick Qt6::QuickControls2)

# Storage tests also run standalone, without building the application/core stack.
add_executable(event_journal_test ../event_journal_test.cpp)
target_compile_features(event_journal_test PRIVATE cxx_std_20)
target_link_libraries(event_journal_test PRIVATE darkspeak_ui_test_support Qt6::Test)
add_test(NAME event_journal COMMAND event_journal_test)
set_tests_properties(event_journal PROPERTIES TIMEOUT 30)
