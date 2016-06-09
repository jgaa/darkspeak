
cmake_minimum_required(VERSION 3.0)
project (darkspeak)

set(EMBEDDED_WARLIB ON)

if (NOT DEFINED DARKSPEAK_ROOT_DIR)
    set(DARKSPEAK_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})
endif()

if (NOT DEFINED WITH_APIDOC)
    option(WITH_APIDOC "Generate Doxygen documentation" ON)
endif()

if (NOT DEFINED WITH_QT_QUICK_GUI)
    option(WITH_QT_QUICK_GUI "Enable Qt Quick GUI" ON)
endif()


if (NOT DEFINED DARKSPEAK_WITH_UNIT_TESTS)
	option(DARKSPEAK_WITH_UNIT_TESTS "Enable Unit Testing" ON)
endif()

if (NOT DEFINED WARLIB_ROOT_DIR)
    set(WARLIB_ROOT_DIR ${DARKSPEAK_ROOT_DIR}/externals/warlib)
endif()


message(STATUS "WARLIB_ROOT_DIR: ${WARLIB_ROOT_DIR}")
include(${WARLIB_ROOT_DIR}/cmake_scripts/boost.cmake)
include(${WARLIB_ROOT_DIR}/cmake_scripts/pch.cmake)

set(DEFAULT_LIBRARIES ${DEFAULT_LIBRARIES}
    war_tasks
    war_log
    war_base
    )


# We create a configuration file so that other code that
# include our header files gets the correct configuration.
set(CONF_PATH ${DARKSPEAK_ROOT_DIR}/include/darkspeak/ds_config.h)

message(STATUS "Using ${CMAKE_CXX_COMPILER}")

include(${WARLIB_ROOT_DIR}/cmake_scripts/boost.cmake)
include(${WARLIB_ROOT_DIR}/cmake_scripts/pch.cmake)
include(${WARLIB_ROOT_DIR}/cmake_scripts/doxygen.cmake)

if (UNIX)
# For now, assume we use g++/clang++
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG=1 -o3 ")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG=1 -D_DEBUG=1 -o0 -g ")
    add_definitions(-D__USE_LARGEFILE64=1 -D__USE_FILE_OFFSET64=1
        -Wall -fPIC -std=c++14 -pthread
    )
elseif(WIN32)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -DNDEBUG=1 ")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG=1 -D_DEBUG=1 ")
    # We will support windows from Windows Vista
    add_definitions(-D_WIN32_WINNT=0x0600 /EHsc )
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} /SAFESEH:NO")
endif()

set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib CACHE PATH "Destination location")
link_directories(${LIBRARY_OUTPUT_PATH})

include_directories(
    ${PROJECT_SOURCE_DIR}/include
    ${WARLIB_ROOT_DIR}/include
    )

add_subdirectory(${WARLIB_ROOT_DIR})
add_subdirectory(src)

if (DARKSPEAK_WITH_UNIT_TESTS)
    add_subdirectory(tests)
endif()


message(STATUS "Writing the current configuration to ${CONF_PATH}")
CONFIGURE_FILE(config.h.template ${CONF_PATH})

