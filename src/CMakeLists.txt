
add_subdirectory(lib)

if (WITH_QT_QUICK_GUI)
    message(STATUS "Building with QT Quick GUI")
    add_subdirectory(qt-quick-gui)
endif()
