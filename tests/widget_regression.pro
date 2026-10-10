QT += core gui widgets network opengl openglwidgets concurrent testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = widget_regression
DEFINES += NOMINMAX
INCLUDEPATH += ../src
SOURCES += widget_regression.cpp \
    ../src/widgets/desktopwidget.cpp \
    ../src/ui/firstrunwizard.cpp \
    ../src/ui/widgetlibrarydialog.cpp \
    ../src/ui/liquidglassmenu.cpp \
    ../src/rendering/liquidglasswidget.cpp \
    ../src/rendering/nativewindows.cpp \
    ../src/rendering/renderdiagnostics.cpp \
    ../src/rendering/desktopcapture.cpp \
    ../src/rendering/qtglassflowscene.cpp
HEADERS += ../src/config.h \
    ../src/rendering/renderdiagnostics.h \
    ../src/ui/widgetlibrarydialog.h \
    ../src/rendering/qtglassflowscene.h
RESOURCES += ../resources.qrc ../src/rendering/shaders.qrc
win32-msvc {
    QMAKE_CXXFLAGS += /utf-8
    LIBS += -lBthprops -lOle32 -lPropsys -lUser32 -lGdi32 -lPsapi
}

SOURCES += ../src/ui/scaleselector.cpp ../src/media/systemmediasession.cpp ../src/widgets/musiccard.cpp ../src/rendering/cleartypetext.cpp
HEADERS += ../src/media/systemmediasession.h ../src/ui/scaleselector.h ../src/widgets/musiccard.h ../src/rendering/cleartypetext.h

SOURCES += ../src/widgets/desktopwidget_paint.cpp ../src/widgets/desktopwidget_data.cpp ../src/widgets/desktopwidget_settings.cpp
HEADERS += ../src/widgets/cardmetrics.h ../src/widgets/weatherformat.h
