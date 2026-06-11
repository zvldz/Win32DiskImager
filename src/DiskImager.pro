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
QMAKE_CXXFLAGS += -Wall -Wextra
DEFINES -= UNICODE
QT += widgets network
contains(QT_CONFIG, static) {
    # MSYS2 qt6-static auto-includes the standard Windows plugins
    # (qwindows / qgif / qico / qjpeg / qnetworklistmanager /
    # qmodernwindowsstyle / qschannelbackend), so we only force the
    # one that isn't auto-added.
    QTPLUGIN += qcertonlybackend
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
           historydelegate.h \
           imagereader.h \
           rawimagereader.h \
           gzimagereader.h \
           xzimagereader.h \
           partitions.h \
           updatechecker.h

FORMS += mainwindow.ui

SOURCES += disk.cpp\
           main.cpp\
           mainwindow.cpp\
           droppablecombobox.cpp \
           elapsedtimer.cpp \
           historydelegate.cpp \
           imagereader.cpp \
           rawimagereader.cpp \
           gzimagereader.cpp \
           xzimagereader.cpp \
           partitions.cpp \
           updatechecker.cpp

# zlib + liblzma are pulled in for on-the-fly .gz / .xz image decompression.
LIBS += -lz -llzma

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
             ja\
             uk

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

# Qt's standard widget translations (Yes / No / OK / Cancel, QMessageBox
# default buttons, file-dialog labels). Bundled into the binary via
# translations.qrc alongside our own diskimager_*.qm. The qtbase_*.qm
# files come from Qt's install translations/ dir; copy them into
# src/lang/ at qmake time so rcc can pick them up. CI pre-stages these
# from the mingw-w64-x86_64-qt6-translations package (the qt6-static
# install itself has no translations dir), so the copy below is a
# best-effort fallback for local builds with a non-static Qt. Tamil
# (ta_IN) isn't listed — Qt itself doesn't ship qtbase_ta_IN.qm and
# translations.qrc doesn't reference it.
QTBASE_LANGUAGES = es it pl nl de fr zh_CN zh_TW ko ja uk
QT_TR_SRC = $$[QT_INSTALL_TRANSLATIONS]
for(loc, QTBASE_LANGUAGES) {
    src_qm = $$QT_TR_SRC/qtbase_$${loc}.qm
    dst_qm = $$PWD/lang/qtbase_$${loc}.qm
    !exists($$dst_qm):exists($$src_qm) {
        win32 {
            copy_cmd = copy /Y \"$$shell_path($$src_qm)\" \"$$shell_path($$dst_qm)\"
        } else {
            copy_cmd = cp -f \"$$src_qm\" \"$$dst_qm\"
        }
        system($$copy_cmd)|error("Failed to copy $$src_qm")
    }
    !exists($$dst_qm) {
        error("Missing Qt translation: $$dst_qm. Source $$src_qm is also absent — install Qt's translations component (mingw-w64-x86_64-qt6-translations on MSYS2) or copy qtbase_$${loc}.qm manually into src/lang/.")
    }
}
