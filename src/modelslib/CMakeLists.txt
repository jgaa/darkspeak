project(models LANGUAGES CXX)

add_library(${PROJECT_NAME} STATIC
    include/ds/notificationsmodel.h
    include/ds/logmodel.h
    include/ds/contactsmodel.h
    include/ds/model_util.h
    include/ds/messagesmodel.h
    include/ds/filesmodel.h
    include/ds/strategy.h
    include/ds/imageprovider.h
    include/ds/manager.h
    include/ds/conversationsmodel.h
    include/ds/identitynamevalidator.h
    src/messagesmodel.cpp
    src/conversationsmodel.cpp
    src/imageprovider.cpp
    src/identitynamevalidator.cpp
    src/logmodel.cpp
    src/contactsmodel.cpp
    src/manager.cpp
    src/filesmodel.cpp
    src/notificationsmodel.cpp
    )
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
add_dependencies(${PROJECT_NAME} prot tor core crypto)
target_link_libraries(${PROJECT_NAME} PRIVATE Qt5::Quick Qt5::Widgets Qt5::Core Qt5::Network Qt5::Sql Qt5::Svg)
