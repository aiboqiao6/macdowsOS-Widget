QT += core gui widgets network opengl openglwidgets concurrent testlib
CONFIG += testcase console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = widget_regression
DEFINES += NOMINMAX
INCLUDEPATH += ../src
INCLUDEPATH += ../third_party/WebView2/include
win32 {
    CONFIG(debug, debug|release): WIDGET_WEBVIEW_DEST = $$OUT_PWD/debug
    else: WIDGET_WEBVIEW_DEST = $$OUT_PWD/release
    QMAKE_POST_LINK += $$QMAKE_COPY $$shell_quote($$shell_path($$PWD/../third_party/WebView2/bin/WebView2Loader.dll)) $$shell_quote($$shell_path($$WIDGET_WEBVIEW_DEST/WebView2Loader.dll))
}
RESOURCES += ../resources.qrc ../src/rendering/shaders.qrc
win32-msvc {
    QMAKE_CXXFLAGS += /utf-8
    LIBS += -lBthprops -lOle32 -lPropsys -lUser32 -lGdi32 -lPsapi
}

SOURCES += ../src/network/updatechecker.cpp \
    ../src/ui/settingsdialog.cpp \
    ../src/app/fontsettings.cpp \
    ../src/widgets/customwidget.cpp \
    ../src/widgets/desktopwidget_custom.cpp \
    ../src/ui/htmlwidgetview.cpp \
    ../src/ui/widgeteditordialog.cpp \
    ../src/media/systemmediasession.cpp \
    ../src/network/cardrequest.cpp \
    ../src/platform/powerdevices.cpp \
    ../src/rendering/cleartypetext.cpp \
    ../src/rendering/textpaintdevice.cpp \
    ../src/rendering/desktopcapture.cpp \
    ../src/rendering/liquidglasswidget.cpp \
    ../src/rendering/nativewindows.cpp \
    ../src/rendering/qtglassflowscene.cpp \
    ../src/rendering/renderdiagnostics.cpp \
    ../src/ui/firstrunwizard.cpp \
    ../src/ui/gridplacementoverlay.cpp \
    ../src/ui/liquidglassmenu.cpp \
    ../src/ui/scaleselector.cpp \
    ../src/ui/weathercitydialog.cpp \
    ../src/ui/widgetlibrarydialog.cpp \
    ../src/widgets/cards/batterycard.cpp \
    ../src/widgets/cards/clockcard.cpp \
    ../src/widgets/cards/dictionarycard.cpp \
    ../src/widgets/cards/weathercard.cpp \
    ../src/widgets/data/dictionarydata.cpp \
    ../src/widgets/data/weatherdata.cpp \
    ../src/widgets/desktopwidget.cpp \
    ../src/widgets/desktopwidget_interaction.cpp \
    ../src/widgets/desktopwidget_paint.cpp \
    ../src/widgets/desktopwidget_placement.cpp \
    ../src/widgets/desktopwidget_power.cpp \
    ../src/widgets/desktopwidget_preview.cpp \
    ../src/widgets/desktopwidget_settings.cpp \
    ../src/widgets/desktopwidget_storage.cpp \
    ../src/widgets/desktopwidget_tray.cpp \
    ../src/widgets/musiccard.cpp \
    ../src/widgets/countdowncard.cpp \
    ../src/widgets/countdowneditor.cpp \
    ../src/widgets/widgetgeometry.cpp \
    widget_regression.cpp

HEADERS += ../src/network/updatechecker.h \
    ../src/ui/settingsdialog.h \
    ../src/app/fontsettings.h \
    ../src/widgets/customwidget.h \
    ../src/ui/htmlwidgetview.h \
    ../src/ui/widgeteditordialog.h \
    ../src/config.h \
    ../src/media/systemmediasession.h \
    ../src/network/cardrequest.h \
    ../src/platform/powerdevices.h \
    ../src/rendering/cleartypetext.h \
    ../src/rendering/textpaintdevice.h \
    ../src/ui/textcontrols.h \
    ../src/rendering/graphicsquality.h \
    ../src/rendering/glpixeltransfer.h \
    ../src/rendering/liquidglassstyle.h \
    ../src/rendering/qtglassflowscene.h \
    ../src/rendering/renderdiagnostics.h \
    ../src/ui/gridplacementoverlay.h \
    ../src/ui/scaleselector.h \
    ../src/ui/weathercitydialog.h \
    ../src/ui/widgetlibrarydialog.h \
    ../src/widgets/cardmetrics.h \
    ../src/widgets/musiccard.h \
    ../src/widgets/countdowncard.h \
    ../src/widgets/weatherformat.h \
    ../src/widgets/widgetgeometry.h
