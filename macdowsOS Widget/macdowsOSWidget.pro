QT += core gui widgets network opengl openglwidgets
win32:LIBS += -lbthprops -lole32 -lpropsys
CONFIG += c++17 windows
TEMPLATE = app
TARGET = macdowsOSWidget

SOURCES += main.cpp \
           batterywidget.cpp \
           widgetlibrarydialog.cpp \
           liquidglasswidget.cpp \
           third_party/QtGlassFlow/src/qtglassflowscene.cpp
HEADERS += batterywidget.h \
           widgetlibrarydialog.h \
           liquidglasswidget.h \
           third_party/QtGlassFlow/src/qtglassflowscene.h
RESOURCES += third_party/QtGlassFlow/src/shaders.qrc
INCLUDEPATH += third_party/QtGlassFlow/src
