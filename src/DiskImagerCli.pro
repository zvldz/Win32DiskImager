TEMPLATE = app
TARGET = Win32DiskImager-cli
DEPENDPATH += .
INCLUDEPATH += .
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
RCC_DIR = $$OUT_PWD/rcc
UI_DIR = $$OUT_PWD/ui
CONFIG += console c++17
CONFIG -= app_bundle qt
QMAKE_LFLAGS += -static -static-libgcc -static-libstdc++
DEFINES += WINVER=0x0A00
DEFINES += _WIN32_WINNT=0x0A00

SOURCES += cli_main.cpp
RC_FILE = DiskImagerCli.rc
