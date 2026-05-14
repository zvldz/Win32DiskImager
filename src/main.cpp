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
    // otherwise fall back to the OS locale. Search "translations/" next
    // to the executable via applicationDirPath — relative paths would
    // resolve against the launcher's cwd, which for console launches
    // like `C:\Users\foo> "C:\Program Files\...\Win32DiskImager.exe"`
    // is not the install directory. Note: under requireAdministrator
    // elevation Windows replaces the inherited environment with a
    // fresh one from the elevated user's profile, so a process-local
    // `set WDI_LANG=uk` in an unprivileged shell is lost — use
    // `setx WDI_LANG uk` or launch from an already-elevated shell.
    const QString lang  = qEnvironmentVariable("WDI_LANG",
                                                QLocale::system().name());
    const QString trDir = QCoreApplication::applicationDirPath() + "/translations";

    // Qt's standard widget translations (Yes / No / OK / Cancel,
    // QMessageBox default button labels, file-dialog titles, ...).
    // Static Qt 6 doesn't expose qtbase_*.qm via QLibraryInfo, so we
    // bundle them next to our own .qm files at build time.
    QTranslator qtTranslator;
    if (qtTranslator.load("qtbase_" + lang, trDir))
        app.installTranslator(&qtTranslator);

    // Our own UI strings.
    QTranslator appTranslator;
    if (appTranslator.load("diskimager_" + lang, trDir))
        app.installTranslator(&appTranslator);

    MainWindow* mainwindow = MainWindow::getInstance();
    mainwindow->show();
    return app.exec();
}
