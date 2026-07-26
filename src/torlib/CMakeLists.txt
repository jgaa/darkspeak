project(tor LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/torctlsocket.cpp
    src/torcontroller.cpp
    src/tormgr.cpp
    src/toraddress.cpp
    include/ds/serviceproperties.h
    include/ds/torctlsocket.h
    include/ds/tormgr.h
    include/ds/torconfig.h
    include/ds/torcontroller.h
    include/ds/toraddress.h
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_link_libraries(${PROJECT_NAME} PUBLIC core Qt6::Core Qt6::Network)
