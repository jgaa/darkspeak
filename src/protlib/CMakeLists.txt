project(prot LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/torsocketlistener.cpp
    src/torserviceinterface.cpp
    src/dsclient.cpp
    src/protmanager.cpp
    src/torprotocolmanager.cpp
    src/dsserver.cpp
    src/imageutil.cpp
    src/connectionsocket.cpp
    src/peer.cpp
    src/protocolcodec.cpp
    include/ds/dsserver.h
    include/ds/protmanager.h
    include/ds/peer.h
    include/ds/connectionsocket.h
    include/ds/imageutil.h
    include/ds/torprotocolmanager.h
    include/ds/dsclient.h
    include/ds/handshake.h
    include/ds/protocolcodec.h
    include/ds/torsocketlistener.h
    include/ds/torserviceinterface.h
)

qt_add_protobuf(${PROJECT_NAME}
    PROTO_FILES ${CMAKE_SOURCE_DIR}/proto/darkspeak.proto
)

set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 20)
target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_SOURCE_DIR}/dependencies/logfault/include
            ${CMAKE_CURRENT_SOURCE_DIR}/qtprotobuf_compat)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_20)
if(DARKSPEAK_FLATPAK_BUILD)
    target_compile_definitions(${PROJECT_NAME} PRIVATE DARKSPEAK_FLATPAK_BUILD=1)
endif()
target_link_libraries(${PROJECT_NAME} PUBLIC core crypto tor Qt6::Core Qt6::Gui Qt6::Network Qt6::Protobuf Qt6::Quick Qt6::Sql Qt6::Svg Qt6::Widgets)
