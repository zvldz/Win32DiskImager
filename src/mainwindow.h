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
 *  Copyright (C) 2009-2014 ImageWriter developers                    *
 *                 https://sourceforge.net/projects/win32diskimager/  *
 **********************************************************************/

#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <QtWidgets>
#include <QClipboard>
#include <windows.h>
#include <memory>
#include "ui_mainwindow.h"
#include "elapsedtimer.h"

class MainWindow : public QMainWindow, public Ui::MainWindow
{
    Q_OBJECT
    public:
        static MainWindow* getInstance() {
            // !NOT thread safe  - first call from main only
            if (!instance)
                instance = new MainWindow();
            return instance;
        }
        static MainWindow* getInstanceIfAvailable() {
            // getInstance crashes if invoked during init to get MessageBox-parent
            // thus, we simply return instance (NULL, if not yet created) to avoid app crash.
            return instance;
        }

        ~MainWindow();
        void closeEvent(QCloseEvent *event);
        enum Status {STATUS_IDLE=0, STATUS_READING, STATUS_WRITING, STATUS_VERIFYING, STATUS_EXIT, STATUS_CANCELED};
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        bool nativeEvent(const QByteArray &type, void *vMsg, qintptr *result) override;
#else
        bool nativeEvent(const QByteArray &type, void *vMsg, long *result) override;
#endif
    protected slots:
        void on_tbBrowse_clicked();
        void on_bCancel_clicked();
        void on_bWrite_clicked();
        void on_bRead_clicked();
        void on_bVerify_clicked();
        void onImageFileEditingFinished();
        void on_bHashCopy_clicked();
private slots:
        void on_cboxHashType_IdxChg();
        void on_bHashGen_clicked();
protected:
        MainWindow(QWidget* = NULL);
private:
        static MainWindow* instance;
        // find attached devices
        void getLogicalDrives();
        void setReadWriteButtonState();
        void saveSettings();
        void loadSettings();
        void initializeHomeDir();
        void updateHashControls();
        void loadImageFileHistory();
        void addImageFileToHistory(const QString &path);

        static const int MAX_RECENT_IMAGE_FILES = 20;

        HANDLE hVolume;
        HANDLE hFile;
        HANDLE hRawDisk;
        static const unsigned short ONE_SEC_IN_MS = 1000;
        unsigned long long sectorsize;
        int status;
        char *sectorData;
        char *sectorData2; //for verify
        QElapsedTimer update_timer;
        ElapsedTimer *elapsed_timer = NULL;
        QClipboard *clipboard;
        void generateHash(char *filename, int hashish);
        QString myHomeDir;
        QString myFileType;
        QStringList myFileTypeList;
};

#endif // MAINWINDOW_H
