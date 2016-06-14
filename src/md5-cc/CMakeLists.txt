project(darkspeak_api)
set(ACTUAL_SOURCES
        md5.cc
    )
set(HEADERS
   md5.hh
   )

if (WIN32)
    set(SOURCES ${ACTUAL_SOURCES} ${HEADERS} ${RESFILES})
else()
    set(SOURCES ${ACTUAL_SOURCES})
endif()

add_library(md5 ${SOURCES})

set_target_properties(md5 PROPERTIES DEBUG_OUTPUT_NAME md5D)
