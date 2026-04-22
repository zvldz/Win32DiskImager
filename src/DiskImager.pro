###################################################################
#  This program is free software; you can redistribute it and/or
#  modify it under the terms of the GNU General Public License
#  as published by the Free Software Foundation; either version 2
#  of the License, or (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program; if not, see http://gnu.org/licenses/
#
#
#  Copyright (C) 2009, Justin Davis <tuxdavis@gmail.com>
#  Copyright (C) 2009-2017 ImageWriter developers
#                 https://sourceforge.net/projects/win32diskimager/
###################################################################
TEMPLATE = app
TARGET = Win32DiskImager
DEPENDPATH += .
INCLUDEPATH += .
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
RCC_DIR = $$OUT_PWD/rcc
UI_DIR = $$OUT_PWD/ui
#CONFIG += release
QMAKE_LFLAGS += -static -static-libgcc -static-libstdc++
DEFINES -= UNICODE
QT += widgets
contains(QT_CONFIG, static) {
    # Link the plugins required for a single-binary static Qt build.
    QTPLUGIN += qwindows
    QTPLUGIN += qgif
    QTPLUGIN += qico
    QTPLUGIN += qjpeg
    QTPLUGIN += qnetworklistmanager
    QTPLUGIN += qmodernwindowsstyle
    QTPLUGIN += qcertonlybackend
    QTPLUGIN += qschannelbackend
    # Static library link order matters; repeat these at the end to resolve
    # transitive symbols from harfbuzz/freetype.
    QMAKE_LIBS += -lgraphite2
    QMAKE_LIBS += -lusp10
    QMAKE_LIBS += -lbz2
    QMAKE_LIBS += -lrpcrt4
}
DEFINES += WINVER=0x0A00
DEFINES += _WIN32_WINNT=0x0A00
QMAKE_TARGET_PRODUCT = "Win32 Image Writer"
QMAKE_TARGET_DESCRIPTION = "Image Writer for Windows to write USB and SD images"
QMAKE_TARGET_COPYRIGHT = "Copyright (C) 2009-2017 Windows ImageWriter Team"

# Input
HEADERS += disk.h\
           mainwindow.h\
           droppablecombobox.h \
           elapsedtimer.h \
           imagereader.h \
           rawimagereader.h

FORMS += mainwindow.ui

SOURCES += disk.cpp\
           main.cpp\
           mainwindow.cpp\
           droppablecombobox.cpp \
           elapsedtimer.cpp \
           imagereader.cpp \
           rawimagereader.cpp

RESOURCES += gui_icons.qrc translations.qrc

RC_FILE = DiskImager.rc

LANGUAGES  = es\
             it\
             pl\
             nl\
             de\
             fr\
             zh_CN\
             zh_TW\
             ta_IN\
             ko\
             ja

defineReplace(prependAll) {
 for(a,$$1):result += $$2$${a}$$3
 return($$result)
}

TRANSLATIONS = $$prependAll(LANGUAGES, $$PWD/lang/diskimager_, .ts)

TRANSLATIONS_FILES =

LRELEASE_EXEC = lrelease
win32:exists($$[QT_HOST_BINS]/lrelease-qt6.exe): LRELEASE_EXEC = $$[QT_HOST_BINS]/lrelease-qt6.exe
else:win32:exists($$[QT_HOST_BINS]/lrelease-qt5.exe): LRELEASE_EXEC = $$[QT_HOST_BINS]/lrelease-qt5.exe
else:win32:exists($$[QT_HOST_BINS]/lrelease.exe): LRELEASE_EXEC = $$[QT_HOST_BINS]/lrelease.exe
for(tsfile, TRANSLATIONS) {
    qmfile = $$tsfile
    qmfile ~= s,.ts$,.qm,
    command = $$LRELEASE_EXEC $$tsfile -qm $$qmfile
    system($$command)|error("Failed to run: $$command")
    TRANSLATIONS_FILES += $$qmfile
}
