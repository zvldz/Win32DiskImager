/**********************************************************************
 *  This program is free software; you can redistribute it and/or     *
 *  modify it under the terms of the GNU General Public License       *
 *  as published by the Free Software Foundation; either version 2    *
 *  of the License, or (at your option) any later version.            *
 *                                                                    *
 *  This program is distributed in the hope that it will be useful,   *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 *  GNU General Public License for more details.                      *
 *                                                                    *
 *  You should have received a copy of the GNU General Public License *
 *  along with this program; if not, see http://gnu.org/licenses/     *
 *  ---                                                               *
 *  Copyright (C) 2009, Justin Davis <tuxdavis@gmail.com>             *
 *  Copyright (C) 2009-2017 ImageWriter developers                    *
 *                 https://sourceforge.net/projects/win32diskimager/  *
 **********************************************************************/

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <cstdlib>
#include <windows.h>
#include <winioctl.h>
#include "mainwindow.h"
#include "version.h"


int main(int argc, char *argv[])
{
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#endif
    QApplication app(argc, argv);
    app.setApplicationDisplayName(APP_VERSION);

    // Translation lookup: WDI_LANG env var wins (handy for testing a
    // non-system locale or forcing English on a localised Windows),
    // otherwise fall back to the OS locale.
    // Search "translations/" next to the executable via applicationDirPath
    // — relative paths would resolve against the launcher's cwd, which
    // for console launches like `C:\Users\foo> "C:\Program Files\...\Win32DiskImager.exe"`
    // is not the install directory and the .qm files would never be found.
    const QString envLang     = qEnvironmentVariable("WDI_LANG");
    const QString systemLang  = QLocale::system().name();
    const QString lang        = envLang.isEmpty() ? systemLang : envLang;
    const QString trDir       = QCoreApplication::applicationDirPath() + "/translations";
    QTranslator translator;
    const bool loaded = translator.load("diskimager_" + lang, trDir);
    if (loaded)
        app.installTranslator(&translator);

    // TEMP DEBUG — writes wdi_lang.log next to the exe so we can see why
    // a WDI_LANG override or the system-locale fallback isn't picking up
    // the expected .qm. Remove once translation loading is confirmed.
    {
        QFile log(QCoreApplication::applicationDirPath() + "/wdi_lang.log");
        if (log.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream s(&log);
            s << "--- " << QDateTime::currentDateTime().toString(Qt::ISODate) << " ---\n"
              << "WDI_LANG env  = [" << envLang << "]\n"
              << "system locale = [" << systemLang << "]\n"
              << "chosen lang   = [" << lang << "]\n"
              << "translations dir = [" << trDir << "]\n"
              << "QFile::exists(dir) = " << (QDir(trDir).exists() ? "yes" : "no") << "\n"
              << "QFile::exists(diskimager_" << lang << ".qm) = "
              << (QFile::exists(trDir + "/diskimager_" + lang + ".qm") ? "yes" : "no") << "\n"
              << "translator.load returned " << (loaded ? "true" : "false") << "\n\n";
        }
    }

    MainWindow* mainwindow = MainWindow::getInstance();
    mainwindow->show();
    return app.exec();
}
