cmake_minimum_required(VERSION 3.16)
project (ds VERSION 0.2.0 LANGUAGES CXX)

include(cmake/FindSodium.cmake)

if (sodium_INCLUDE_DIR MATCHES "NOTFOUND")
    message(FATAL_ERROR "Failed to locate required library libsodium")
endif()

set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.10 COMPONENTS Core Gui Qml Quick QuickControls2 LinguistTools Widgets Network Sql Svg Test REQUIRED)

#set(TS_FILES ds_en_US.ts)
#qt5_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})

add_subdirectory(src/cryptolib)
add_subdirectory(src/corelib)
add_subdirectory(src/torlib)
add_subdirectory(src/protlib)
add_subdirectory(src/modelslib)
add_subdirectory(src/app)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
