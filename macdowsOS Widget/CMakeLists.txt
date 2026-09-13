cmake_minimum_required(VERSION 3.21)
project(macdowsOSWidget VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network OpenGL OpenGLWidgets)

qt_add_executable(macdowsOSWidget WIN32
    main.cpp
    batterywidget.cpp
    batterywidget.h
    widgetlibrarydialog.cpp
    widgetlibrarydialog.h
    liquidglasswidget.cpp
    liquidglasswidget.h
    third_party/QtGlassFlow/src/qtglassflowscene.cpp
    third_party/QtGlassFlow/src/qtglassflowscene.h
    third_party/QtGlassFlow/src/shaders.qrc
)

target_include_directories(macdowsOSWidget PRIVATE
    third_party/QtGlassFlow/src
)

target_link_libraries(macdowsOSWidget PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::OpenGL
    Qt6::OpenGLWidgets
)

if (WIN32)
    target_compile_definitions(macdowsOSWidget PRIVATE NOMINMAX)
    target_link_libraries(macdowsOSWidget PRIVATE Bthprops Ole32 Propsys)

    # Keep the Release folder self-contained so the exe can be launched from
    # Explorer without a separate Qt installation on PATH.
    get_target_property(_qt6_qmake_executable Qt6::qmake IMPORTED_LOCATION)
    if (NOT _qt6_qmake_executable)
        get_target_property(_qt6_qmake_executable Qt6::qmake IMPORTED_LOCATION_RELEASE)
    endif()
    if (_qt6_qmake_executable)
        get_filename_component(_qt6_bin_dir "${_qt6_qmake_executable}" DIRECTORY)
        find_program(_qt6_windeployqt windeployqt HINTS "${_qt6_bin_dir}")
    endif()
    if (_qt6_windeployqt)
        add_custom_command(TARGET macdowsOSWidget POST_BUILD
            COMMAND "${_qt6_windeployqt}" --no-translations --compiler-runtime
                    "$<TARGET_FILE:macdowsOSWidget>"
            COMMENT "Deploying Qt runtime next to macdowsOSWidget.exe"
            VERBATIM
        )
    endif()
endif()

install(TARGETS macdowsOSWidget RUNTIME DESTINATION .)
