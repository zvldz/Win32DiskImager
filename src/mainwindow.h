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
#include <QList>
#include <windows.h>
#include <memory>
#include "ui_mainwindow.h"
#include "elapsedtimer.h"
#include "disk.h"   // TargetDisk struct

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
        // Confirms with the user, then removes one Image File history
        // entry from both the combo and the registry-backed list.
        // Wired up to HistoryItemDelegate::removeRequested.
        void onHistoryRemoveRequested(int row);
        // UpdateChecker signal handlers — see updatechecker.h.
        void onUpdateAvailable(const QString &tag, const QString &installerUrl);
        void onNoUpdateAvailable();
        void onUpdateCheckFailed(const QString &error);
protected:
        MainWindow(QWidget* = NULL);
private:
        static MainWindow* instance;
        // find attached devices
        void getLogicalDrives();
        void setReadWriteButtonState();
        void downloadAndRunInstaller(const QString &url, const QString &version);
        // Closes any open volume / hFile / hRawDisk (with lock release on
        // each volume) and resets the UI back to idle. Replaces the long
        // identical cleanup blocks that used to be duplicated in every
        // early-return path of Write / Read / Verify.
        void cleanupHandlesAndUI();
        void saveSettings();
        void loadSettings();
        void initializeHomeDir();
        void updateHashControls();
        void loadImageFileHistory();
        void addImageFileToHistory(const QString &path);

        static const int MAX_RECENT_IMAGE_FILES = 20;

        // All mounted volume handles for the selected target disk, each
        // already locked + dismounted. Empty for a bare disk (no letter,
        // no recognised FS), which then needs no per-volume housekeeping —
        // we go straight to hRawDisk.
        QList<HANDLE> m_volumes;
        // Snapshot of writable disks shown in the cboxDevice combo, indexed
        // by combo row. Source of truth for the diskNumber + letters of
        // whichever entry the user picked — replaces the old letter-parsing
        // out of the combo's display text.
        QList<TargetDisk> m_targetDisks;
        HANDLE hFile;
        HANDLE hRawDisk;
        static const unsigned short ONE_SEC_IN_MS = 1000;
        unsigned long long sectorsize;
        int status;
        char *sectorData;
        char *sectorData2; //for verify
        QElapsedTimer update_timer;
        ElapsedTimer *elapsed_timer = NULL;
        // Non-zero while a Write has just finished and is about to chain
        // into an auto-verify. Verify's completion dialog uses it to report
        // Write + Verify + Total time instead of just Verify's own elapsed.
        qint64 m_writeElapsedMs = 0;
        // Set up in the constructor; lives for the whole window lifetime
        // and emits update{Available,NotAvailable,Failed}.
        class UpdateChecker *m_updateChecker = nullptr;
        QClipboard *clipboard;
        void generateHash(char *filename, int hashish);
        QString myHomeDir;
        QString myFileType;
        QStringList myFileTypeList;
};

#endif // MAINWINDOW_H
