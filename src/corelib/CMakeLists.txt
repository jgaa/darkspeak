project(core LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    src/filemanager.cpp
    src/identitymanager.cpp
    src/contactmanager.cpp
    src/identity.cpp
    src/conversation.cpp
    src/file.cpp
    src/hashtask.cpp
    src/logutil.cpp
    src/conversationmanager.cpp
    src/message.cpp
    src/messagemanager.cpp
    src/contact.cpp
    src/database.cpp
    src/protocolmanager.cpp
    src/dsengine.cpp
    include/ds/conversationmanager.h
    include/ds/bytes.h
    include/ds/filemanager.h
    include/ds/identitymanager.h
    include/ds/file.h
    include/ds/database.h
    include/ds/contact.h
    include/ds/conversation.h
    include/ds/protocolmanager.h
    include/ds/identity.h
    include/ds/message.h
    include/ds/dsengine.h
    include/ds/userinfo.h
    include/ds/transporthandle.h
    include/ds/hashtask.h
    include/ds/peerconnection.h
    include/ds/task.h
    include/ds/contactmanager.h
    include/ds/registry.h
    include/ds/logutil.h
    include/ds/update_helper.h
    include/ds/messagemanager.h
    include/ds/errors.h
    include/ds/lru_cache.h
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
#add_dependencies(${PROJECT_NAME} corelib)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Quick Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql Qt5::Svg)
