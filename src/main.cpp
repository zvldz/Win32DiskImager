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
    // otherwise fall back to the OS locale. All .qm files are bundled
    // into the .exe via translations.qrc, so the lookup goes through
    // the Qt resource system at ":/translations" — no install-dir
    // layout to babysit, no cwd-dependent paths.
    //
    // UAC caveat: requireAdministrator elevation replaces the inherited
    // environment with a fresh one from the elevated user's profile.
    // A process-local `set WDI_LANG=uk` in an unprivileged shell is
    // dropped by the elevation prompt — use `setx WDI_LANG uk` to
    // persist it in HKCU\Environment, or launch from an already-
    // elevated shell so no UAC prompt fires and the env is preserved.
    const QString lang = qEnvironmentVariable("WDI_LANG",
                                              QLocale::system().name());

    // Qt's standard widget strings (Yes / No / OK / Cancel, QMessageBox
    // default button labels, file-dialog titles, ...). Loaded from the
    // qtbase_<lang>.qm pulled out of mingw-w64-x86_64-qt6-translations
    // at build time. Tamil has no qtbase translation upstream — load
    // simply returns false and Qt falls back to English for those
    // standard strings.
    QTranslator qtTranslator;
    if (qtTranslator.load("qtbase_" + lang, ":/translations"))
        app.installTranslator(&qtTranslator);

    // Our own UI strings (diskimager_<lang>.qm).
    QTranslator appTranslator;
    if (appTranslator.load("diskimager_" + lang, ":/translations"))
        app.installTranslator(&appTranslator);

    MainWindow* mainwindow = MainWindow::getInstance();
    mainwindow->show();
    return app.exec();
}
