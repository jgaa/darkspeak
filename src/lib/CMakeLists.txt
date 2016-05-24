project(darkspeak_api)
set(ACTUAL_SOURCES
        ImManager.cpp
        BuddyImpl.cpp
        TorChatEngine.cpp
        TorChatConnection.cpp
        Connection.cpp
        TorChatPeer.cpp
    )
set(HEADERS
   ${DARKSPEAK_ROOT_DIR}/darkspeak/Api.h
   ${DARKSPEAK_ROOT_DIR}/darkspeak/ImManager.h
   )

if (WIN32)
    ADD_MSVC_PRECOMPILED_HEADER(${WARFTPD_ROOT_DIR}/include/warftpd/warftpd.h pch.cpp ACTUAL_SOURCES)
    add_definitions(-DWARFTPD_PCH)
    set(SOURCES ${ACTUAL_SOURCES} pch.cpp ${HEADERS} ${RESFILES})
else()
    set(SOURCES ${ACTUAL_SOURCES})
endif()

add_library(darkspeak_api ${SOURCES})
add_dependencies(darkspeak_api
    war_base
    war_log
    war_tasks
    )
set_target_properties(darkspeak_api PROPERTIES DEBUG_OUTPUT_NAME darkspeak_apiD)
