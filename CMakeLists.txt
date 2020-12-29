cmake_minimum_required(VERSION 3.16)
project (ds VERSION 0.1.01 LANGUAGES CXX)

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

find_package(Qt6 COMPONENTS Core Quick QuickControls2 LinguistTools Widgets Network Sql Svg REQUIRED)

set(TS_FILES testqt_en_US.ts)

#target_compile_definitions(testqt
#  PRIVATE $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:QT_QML_DEBUG>)
#target_link_libraries(testqt
#  PRIVATE Qt6::Core Qt6::Quick)

qt6_create_translation(QM_FILES ${CMAKE_SOURCE_DIR} ${TS_FILES})

include_directories(
    src/corelib/include
    src/cryptolib/include
    src/modelslib/include
    src/protlib/include
    src/torlib/include
    dependencies/logfault/include
    )

add_subdirectory(src/cryptolib)
add_subdirectory(src/corelib)
add_subdirectory(src/torlib)
add_subdirectory(src/protlib)
add_subdirectory(src/modelslib)
add_subdirectory(src/app)
