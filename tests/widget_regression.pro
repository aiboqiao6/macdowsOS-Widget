QT += core gui widgets network opengl openglwidgets concurrent testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = widget_regression
DEFINES += NOMINMAX
INCLUDEPATH += ../src ../third_party/QtGlassFlow/src
SOURCES += widget_regression.cpp \
    ../src/widgets/batterywidget.cpp \
    ../src/ui/widgetlibrarydialog.cpp \
    ../src/rendering/liquidglasswidget.cpp \
    ../src/rendering/nativewindows.cpp \
    ../src/rendering/desktopcapture.cpp \
    ../third_party/QtGlassFlow/src/qtglassflowscene.cpp
HEADERS += ../src/ui/widgetlibrarydialog.h \
    ../third_party/QtGlassFlow/src/qtglassflowscene.h
RESOURCES += ../resources.qrc ../third_party/QtGlassFlow/src/shaders.qrc
win32-msvc {
    QMAKE_CXXFLAGS += /utf-8
    LIBS += -lBthprops -lOle32 -lPropsys -lUser32 -lGdi32 -lPsapi
}
