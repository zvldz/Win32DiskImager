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

#include <QtWidgets>
#include <QCoreApplication>
#include <QFileInfo>
#include <QDirIterator>
#include <QClipboard>
#include <algorithm>         // std::max for chunk-size clamp
#include <cstdio>
#include <cstdlib>
#include <malloc.h>          // _aligned_free (paired with _aligned_malloc in disk.cpp)
#include <windows.h>
#include <winioctl.h>
#include <dbt.h>
#include <shlobj.h>
#include <iostream>
#include <sstream>

#include "disk.h"
#include "imagereader.h"
#include "iopipeline.h"
#include "keepawake.h"
#include "rawimagereader.h"
#include "mainwindow.h"
#include "elapsedtimer.h"

#include <thread>

// Compact "2TB" / "29GB" / "512MB" for the device combo. No space and
// no fractional part — the combo width is fixed by the pinned window
// size, so every saved character matters. Uses binary units (the
// convention Windows itself displays).
static QString formatDeviceSize(unsigned long long bytes)
{
    if (bytes == 0) return QString();
    constexpr double KB = 1024.0;
    constexpr double MB = KB * 1024.0;
    constexpr double GB = MB * 1024.0;
    constexpr double TB = GB * 1024.0;
    const double b = (double)bytes;
    if (b >= TB) return QString::number(b / TB, 'f', 0) + "TB";
    if (b >= GB) return QString::number(b / GB, 'f', 0) + "GB";
    if (b >= MB) return QString::number(b / MB, 'f', 0) + "MB";
    return QString::number(b / KB, 'f', 0) + "KB";
}

// Centered-button, rich-text completion dialog. Used by all four success
// paths (Write / Read / Verify / Write+Verify). Rich text renders the
// bold labels; centerButtons puts the single OK under the middle of
// the dialog instead of flush-left where QMessageBox::information puts
// it when content is wide (e.g. the Write+Verify table).
//
// "%ZEBRA%" in text is replaced with a palette-derived row color — the
// QApplication palette's AlternateBase is often out of sync with the
// style's actual rendering on Windows (stays light even when the dialog
// is drawn dark), so we derive the shade from the box's Window color
// at render time.
static void showComplete(QWidget *parent, const QString &title, QString text)
{
    // NoIcon removes the big left-side icon so the content (including the
    // centered Write+Verify table) is truly centered relative to the
    // dialog. The info indicator moves to the title bar via setWindowIcon,
    // keeping the "this is a success dialog" visual cue.
    QMessageBox box(QMessageBox::NoIcon, title, QString(), QMessageBox::Ok, parent);
    box.setWindowIcon(box.style()->standardIcon(QStyle::SP_MessageBoxInformation));
    if (text.contains(QStringLiteral("%ZEBRA%"))) {
        const QColor base = box.palette().color(QPalette::Window);
        const QColor alt = (base.lightness() < 128) ? base.lighter(130)
                                                    : base.darker(108);
        text.replace(QStringLiteral("%ZEBRA%"), alt.name());
    }
    box.setText(text);
    box.setTextFormat(Qt::RichText);
    box.setStyleSheet("QDialogButtonBox { qproperty-centerButtons: true; }");
    // QMessageBox's label defaults to AlignLeft, which prevents inline
    // <center> from reaching block-level children (the Write+Verify
    // table). Force AlignHCenter on the label so the entire content —
    // both the heading and the table — is centered in the dialog.
    for (QLabel *lbl : box.findChildren<QLabel*>())
        lbl->setAlignment(Qt::AlignHCenter);
    box.exec();
}

// Format "2m 15s" / "1h 5m 30s" from a milliseconds value. Used by the
// completion dialogs to report how long Read / Write / (Write + Verify)
// actually took.
static QString formatElapsedMs(qint64 ms)
{
    if (ms < 0) ms = 0;
    qint64 s = ms / 1000;
    const qint64 h = s / 3600;
    const qint64 m = (s / 60) % 60;
    s %= 60;
    if (h > 0) return QString("%1h %2m %3s").arg(h).arg(m).arg(s);
    if (m > 0) return QString("%1m %2s").arg(m).arg(s);
    return QString("%1s").arg(s);
}

// I/O chunk target in bytes. The sector count per iteration is derived from
// this so a 512-byte-sector device still transfers 4 MB per syscall instead
// of the legacy 512 KB. Matches what RPi Imager / Etcher do on Windows.
static const unsigned long long CHUNK_BYTES = 4ULL * 1024ULL * 1024ULL;

MainWindow* MainWindow::instance = NULL;

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent)
{
    setupUi(this);
    // Reserve 8 chars of combo width *before* pinning the window
    // size. The full "[X:\] XXGB" label is elided in the combo but
    // still renders complete in the dropdown. Without this reservation
    // the fixed-size window was frozen around an empty combo, so
    // inserting a card at runtime (DBT_DEVICEARRIVAL) couldn't widen
    // the combo at all.
    cboxDevice->setMinimumContentsLength(8);
    cboxDevice->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    setFixedSize(size());
    if (QLineEdit *fileEdit = leFile->lineEdit()) {
        connect(fileEdit, &QLineEdit::editingFinished,
                this, &MainWindow::onImageFileEditingFinished);
    }
    // Live-update Read/Write/Verify enabled state as the user types — otherwise
    // the buttons only refresh on Enter / focus-out (editingFinished), which
    // raced with Tab and left Tab unable to reach a button that was about to
    // become enabled.
    connect(leFile, &QComboBox::editTextChanged,
            this, [this](const QString &) { setReadWriteButtonState(); });
    loadImageFileHistory();
    elapsed_timer = new ElapsedTimer();
    statusbar->addPermanentWidget(elapsed_timer);   // "addpermanent" puts it on the RHS of the statusbar
    getLogicalDrives();
    status = STATUS_IDLE;
    progressbar->reset();
    clipboard = QApplication::clipboard();
    statusbar->showMessage(tr("Waiting for a task."));
    hVolume = INVALID_HANDLE_VALUE;
    hFile = INVALID_HANDLE_VALUE;
    hRawDisk = INVALID_HANDLE_VALUE;
    if (QCoreApplication::arguments().count() > 1)
    {
        QString fileLocation = QApplication::arguments().at(1);
        QFileInfo fileInfo(fileLocation);
        leFile->setEditText(fileInfo.absoluteFilePath());
    }
    // Add supported hash types.
    cboxHashType->addItem("MD5",QVariant(QCryptographicHash::Md5));
    cboxHashType->addItem("SHA1",QVariant(QCryptographicHash::Sha1));
    cboxHashType->addItem("SHA256",QVariant(QCryptographicHash::Sha256));
    connect(this->cboxHashType, SIGNAL(currentIndexChanged(int)), SLOT(on_cboxHashType_IdxChg()));
    updateHashControls();
    setReadWriteButtonState();
    sectorData = NULL;
    sectorsize = 0ul;

    loadSettings();
    if (myHomeDir.isEmpty()){
        initializeHomeDir();
    }

    // Qt's QFileDialog is case-insensitive on Windows, so one pattern per
    // extension is enough — no need for *.img *.IMG etc.
    const QString defaultFilter = tr("Disk images (*.img *.iso *.gz *.xz)");
    if (myFileType.isEmpty()) {
        myFileType = defaultFilter;
    }
    myFileTypeList << defaultFilter
                   << tr("Raw images (*.img *.iso)")
                   << tr("Compressed images (*.gz *.xz)")
                   << "*.*";
}

MainWindow::~MainWindow()
{
    saveSettings();
    if (hRawDisk != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hRawDisk);
        hRawDisk = INVALID_HANDLE_VALUE;
    }
    if (hFile != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hFile);
        hFile = INVALID_HANDLE_VALUE;
    }
    if (hVolume != INVALID_HANDLE_VALUE)
    {
        CloseHandle(hVolume);
        hVolume = INVALID_HANDLE_VALUE;
    }
    if (sectorData != NULL)
    {
        _aligned_free(sectorData);
        sectorData = NULL;
    }
    if (sectorData2 != NULL)
    {
        _aligned_free(sectorData2);
        sectorData2 = NULL;
    }
    if (elapsed_timer != NULL)
    {
        delete elapsed_timer;
        elapsed_timer = NULL;
    }
    if (cboxHashType != NULL)
    {
       cboxHashType->clear();
    }
}


void MainWindow::saveSettings()
{
    QSettings userSettings("HKEY_CURRENT_USER\\Software\\Win32DiskImager", QSettings::NativeFormat);
    userSettings.beginGroup("Settings");
    userSettings.setValue("ImageDir", myHomeDir);
    userSettings.setValue("FileType", myFileType);
    userSettings.endGroup();
}

void MainWindow::loadSettings()
{
    QSettings userSettings("HKEY_CURRENT_USER\\Software\\Win32DiskImager", QSettings::NativeFormat);
    userSettings.beginGroup("Settings");
    myHomeDir = userSettings.value("ImageDir").toString();
    myFileType = userSettings.value("FileType").toString();
}

void MainWindow::loadImageFileHistory()
{
    QSettings userSettings("HKEY_CURRENT_USER\\Software\\Win32DiskImager", QSettings::NativeFormat);
    const QStringList history = userSettings.value("ImageFileHistory/Items").toStringList();

    const QString preserved = leFile->currentText();
    QSignalBlocker blocker(leFile);
    leFile->clear();
    for (const QString &entry : history) {
        leFile->addItem(entry);
    }
    leFile->setEditText(preserved);
}

void MainWindow::addImageFileToHistory(const QString &path)
{
    if (path.isEmpty()) {
        return;
    }

    QSettings userSettings("HKEY_CURRENT_USER\\Software\\Win32DiskImager", QSettings::NativeFormat);
    QStringList history = userSettings.value("ImageFileHistory/Items").toStringList();

    // Case-insensitive dedup keeps registry stable on Windows; the newest
    // entry wins the preferred path casing.
    history.erase(std::remove_if(history.begin(), history.end(),
                                 [&path](const QString &e) {
                                     return e.compare(path, Qt::CaseInsensitive) == 0;
                                 }),
                  history.end());
    history.prepend(path);
    while (history.size() > MAX_RECENT_IMAGE_FILES) {
        history.removeLast();
    }

    userSettings.setValue("ImageFileHistory/Items", history);

    // Reflect the new order in the combo-box without losing the edit text.
    QSignalBlocker blocker(leFile);
    const QString preserved = leFile->currentText();
    leFile->clear();
    for (const QString &entry : history) {
        leFile->addItem(entry);
    }
    leFile->setEditText(preserved);
}

void MainWindow::initializeHomeDir()
{
    myHomeDir = QDir::homePath();
    if (myHomeDir.isNull()){
        myHomeDir = qgetenv("USERPROFILE");
    }
    /* Get Downloads the Windows way */
    QString downloadPath = qgetenv("DiskImagesDir");
    if (downloadPath.isEmpty()) {
        PWSTR pPath = NULL;
        static GUID downloads = {0x374de290, 0x123f, 0x4565, {0x91, 0x64, 0x39,
                                 0xc4, 0x92, 0x5e, 0x46, 0x7b}};
        if (SHGetKnownFolderPath(downloads, 0, 0, &pPath) == S_OK) {
            downloadPath = QDir::fromNativeSeparators(QString::fromWCharArray(pPath));
            LocalFree(pPath);
            if (downloadPath.isEmpty() || !QDir(downloadPath).exists()) {
                downloadPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            }
        }
    }
    if (downloadPath.isEmpty())
        downloadPath = QDir::currentPath();
    myHomeDir = downloadPath;
}

void MainWindow::setReadWriteButtonState()
{
    bool fileSelected = !(leFile->currentText().isEmpty());
    bool deviceSelected = (cboxDevice->count() > 0);
    QFileInfo fi(leFile->currentText());

    // set read and write buttons according to status of file/device
    bRead->setEnabled(deviceSelected && fileSelected && (fi.exists() ? fi.isWritable() : true));
    bWrite->setEnabled(deviceSelected && fileSelected && fi.isReadable());
    bVerify->setEnabled(deviceSelected && fileSelected && fi.isReadable());
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    if (status == STATUS_READING)
    {
        if (QMessageBox::warning(this, tr("Exit?"), tr("Exiting now will result in a corrupt image file.\n"
                                                       "Are you sure you want to exit?"),
                                 QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            status = STATUS_EXIT;
        }
        event->ignore();
    }
    else if (status == STATUS_WRITING)
    {
        if (QMessageBox::warning(this, tr("Exit?"), tr("Exiting now will result in a corrupt disk.\n"
                                                       "Are you sure you want to exit?"),
                                 QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            status = STATUS_EXIT;
        }
        event->ignore();
    }
    else if (status == STATUS_VERIFYING)
    {
        if (QMessageBox::warning(this, tr("Exit?"), tr("Exiting now will cancel verifying image.\n"
                                                       "Are you sure you want to exit?"),
                                 QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            status = STATUS_EXIT;
        }
        event->ignore();
    }
}

void MainWindow::on_tbBrowse_clicked()
{
    // Use the location of already entered file
    QString fileLocation = leFile->currentText();
    QFileInfo fileinfo(fileLocation);

    // See if there is a user-defined file extension.
    QString fileTypeEnv = qgetenv("DiskImagerFiles");

    QStringList fileTypesList = fileTypeEnv.split(";;", Qt::SkipEmptyParts) + myFileTypeList;
    int index = fileTypesList.indexOf(myFileType);
    if (index != -1) {
        fileTypesList.move(index, 0);
    }

    // create a generic FileDialog
    QFileDialog dialog(this, tr("Select a disk image"));
    dialog.setNameFilters(fileTypesList);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setViewMode(QFileDialog::Detail);
    // Native QFileDialog on Windows often ignores selectFile() when given
    // a full path — it remembers the last-used directory instead. Split
    // the path explicitly: setDirectory + selectFile(name only) puts the
    // user into the file's actual folder regardless of native quirks.
    if (fileinfo.exists())
    {
        dialog.setDirectory(fileinfo.absoluteDir());
        dialog.selectFile(fileinfo.fileName());
    }
    else if (!fileLocation.isEmpty() && QDir(fileinfo.absolutePath()).exists())
    {
        // User typed (or pasted) a path whose directory exists but the
        // file itself doesn't yet — useful for Read targets.
        dialog.setDirectory(fileinfo.absolutePath());
        if (!fileinfo.fileName().isEmpty()) {
            dialog.selectFile(fileinfo.fileName());
        }
    }
    else
    {
        dialog.setDirectory(myHomeDir);
    }

    if (dialog.exec())
    {
        // selectedFiles returns a QStringList - we just want 1 filename,
        //	so use the zero'th element from that list as the filename
        fileLocation = (dialog.selectedFiles())[0];
        myFileType = dialog.selectedNameFilter();

        if (!fileLocation.isNull())
        {
            leFile->setEditText(fileLocation);
            QFileInfo newFileInfo(fileLocation);
            myHomeDir = newFileInfo.absolutePath();
        }
        setReadWriteButtonState();
        updateHashControls();
    }
}

void MainWindow::on_bHashCopy_clicked()
{
    QString hashSum(hashLabel->text());
    if ( !(hashSum.isEmpty()) )
    {
        clipboard->setText(hashSum);
    }
}

// generates the hash
void MainWindow::generateHash(char *filename, int hashish)
{
    hashLabel->setText(tr("Generating..."));
    QApplication::processEvents();

    QCryptographicHash filehash((QCryptographicHash::Algorithm)hashish);

    // may take a few secs - display a wait cursor
    QApplication::setOverrideCursor(QCursor(Qt::WaitCursor));

    QFile file(filename);
    if (!file.open(QFile::ReadOnly))
    {
        QMessageBox::critical(this, tr("File Error"), tr("Unable to open file for hashing."));
        QApplication::restoreOverrideCursor();
        hashLabel->clear();
        bHashCopy->setEnabled(false);
        return;
    }
    filehash.addData(&file);

    QByteArray hash = filehash.result();

    // display it in the textbox
    hashLabel->setText(hash.toHex());
    bHashCopy->setEnabled(true);
    // redisplay the normal cursor
    QApplication::restoreOverrideCursor();
}


// When the user commits an edit in the image-file field (Enter or focus out),
// refresh dependent controls. Connected explicitly in the ctor because
// QComboBox does not itself emit editingFinished — the inner QLineEdit does.
void MainWindow::onImageFileEditingFinished()
{
    setReadWriteButtonState();
    updateHashControls();
}

void MainWindow::on_bCancel_clicked()
{
    if ( (status == STATUS_READING) || (status == STATUS_WRITING) )
    {
        if (QMessageBox::warning(this, tr("Cancel?"), tr("Canceling now will result in a corrupt destination.\n"
                                                         "Are you sure you want to cancel?"),
                                 QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            status = STATUS_CANCELED;
        }
    }
    else if (status == STATUS_VERIFYING)
    {
        if (QMessageBox::warning(this, tr("Cancel?"), tr("Cancel Verify.\n"
                                                         "Are you sure you want to cancel?"),
                                 QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        {
            status = STATUS_CANCELED;
        }

    }
}

void MainWindow::on_bWrite_clicked()
{
    KeepAwake keepAwake;  // suppress idle sleep until this scope exits
    bool passfail = true;
    if (!leFile->currentText().isEmpty())
    {
        QFileInfo fileinfo(leFile->currentText());
        if (fileinfo.exists() && fileinfo.isFile() &&
                fileinfo.isReadable() && (fileinfo.size() > 0) )
        {
            if (leFile->currentText().at(0) == cboxDevice->currentText().at(1))
            {
                QMessageBox::critical(this, tr("Write Error"), tr("Image file cannot be located on the target device."));
                return;
            }

            // build the drive letter as a const char *
            //   (without the surrounding brackets)
            QString qs = cboxDevice->currentText();
            qs.replace(QRegularExpression("[\\[\\]]"), "");
            QByteArray qba = qs.toLocal8Bit();
            const char *ltr = qba.data();
            if (QMessageBox::warning(this, tr("Confirm overwrite"), tr("Writing to a physical device can corrupt the device.\n"
                                                                       "(Target Device: %1 \"%2\")\n"
                                                                       "Are you sure you want to continue?").arg(cboxDevice->currentText()).arg(getDriveLabel(ltr)),
                                     QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::No)
            {
                return;
            }
            status = STATUS_WRITING;
            bCancel->setEnabled(true);
            bWrite->setEnabled(false);
            bRead->setEnabled(false);
            bVerify->setEnabled(false);
            double mbpersec;
            unsigned long long i, lasti, availablesectors, numsectors;
            int volumeID = cboxDevice->currentText().at(1).toLatin1() - 'A';
            // int deviceID = cboxDevice->itemData(cboxDevice->currentIndex()).toInt();
            hVolume = getHandleOnVolume(volumeID, GENERIC_READ | GENERIC_WRITE);
            if (hVolume == INVALID_HANDLE_VALUE)
            {
                status = STATUS_IDLE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            DWORD deviceID = getDeviceID(hVolume);
            if (!getLockOnVolume(hVolume))
            {
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            if (!unmountVolume(hVolume))
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            hFile = getHandleOnFile(reinterpret_cast<LPCWSTR>(leFile->currentText().utf16()), GENERIC_READ);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            // Direct I/O on the destination: bypass FS cache so MB/s reflects
            // real device throughput and "Done" only appears once data has
            // landed. Buffer alignment is handled by readSectorDataFromHandle.
            hRawDisk = getHandleOnDevice(deviceID, GENERIC_WRITE,
                                         FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
            if (hRawDisk == INVALID_HANDLE_VALUE)
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            availablesectors = getNumberOfSectors(hRawDisk, &sectorsize);
            if (!availablesectors)
            {
                //For external card readers you may not get device change notification when you remove the card/flash.
                //(So no WM_DEVICECHANGE signal). Device stays but size goes to 0. [Is there special event for this on Windows??]
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                passfail = false;
                status = STATUS_IDLE;
                return;

            }
            const unsigned long long chunkSectors = sectorsize ? std::max<unsigned long long>(1ULL, CHUNK_BYTES / sectorsize) : 1ULL;

            // Open the image reader (sniffs magic bytes — Raw / Gz / Xz).
            QString readerErr;
            std::unique_ptr<ImageReader> reader = ImageReader::open(leFile->currentText(), &readerErr);
            if (!reader || reader->compressedSize() == 0)
            {
                QMessageBox::critical(this, tr("File Error"),
                                      reader ? tr("The selected image file is empty.") : readerErr);
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                status = STATUS_IDLE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }

            // knownSize == true when the reader can report the uncompressed
            // payload size (raw: file size; xz: via stream index; gz: via the
            // ISIZE footer when < 4 GB). Unknown size → we cap the main loop
            // at the device capacity and rely on the reader returning EOF.
            const quint64 imgBytes   = reader->uncompressedSize();
            const bool    knownSize  = (imgBytes > 0);
            numsectors = knownSize ? (imgBytes + sectorsize - 1) / sectorsize
                                   : availablesectors;

            if (knownSize && numsectors > availablesectors)
            {
                // For raw images we peek at the bytes past the device
                // capacity so the warning can say whether the truncated tail
                // carried data. Compressed sources don't allow cheap random
                // access → default to the pessimistic "DOES contain data".
                bool datafound = true;
                if (dynamic_cast<RawImageReader *>(reader.get()))
                {
                    datafound = false;
                    unsigned long long si = availablesectors;
                    while (si < numsectors && !datafound)
                    {
                        const unsigned long long next =
                            std::min<unsigned long long>(chunkSectors, numsectors - si);
                        char *sd = readSectorDataFromHandle(hFile, si, next, sectorsize);
                        if (!sd) break;
                        const unsigned long long limit = next * sectorsize;
                        for (unsigned long long j = 0; j < limit; ++j)
                        {
                            if (sd[j]) { datafound = true; break; }
                        }
                        _aligned_free(sd);
                        si += next;
                    }
                }
                std::ostringstream msg;
                msg << "More space required than is available:"
                    << "\n  Required: "   << numsectors      << " sectors"
                    << "\n  Available: "  << availablesectors << " sectors"
                    << "\n  Sector Size: " << sectorsize
                    << "\n\nThe extra space " << ((datafound) ? "DOES" : "does not") << " appear to contain data"
                    << "\n\nContinue Anyway?";
                if (QMessageBox::warning(this, tr("Not enough available space!"),
                                         tr(msg.str().c_str()),
                                         QMessageBox::Ok, QMessageBox::Cancel) == QMessageBox::Ok)
                {
                    numsectors = availablesectors;
                }
                else
                {
                    removeLockOnVolume(hVolume);
                    CloseHandle(hRawDisk);
                    CloseHandle(hFile);
                    CloseHandle(hVolume);
                    status = STATUS_IDLE;
                    hVolume = INVALID_HANDLE_VALUE;
                    hFile = INVALID_HANDLE_VALUE;
                    hRawDisk = INVALID_HANDLE_VALUE;
                    bCancel->setEnabled(false);
                    setReadWriteButtonState();
                    return;
                }
            }

            // hFile was only needed for the pre-scan above — the main loop
            // reads through the reader (which holds its own file handle or
            // decompressor state).
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;

            // Progress bar unit: sectors when size is known, compressed-byte
            // percentage otherwise. The bar stays smooth either way.
            progressbar->setRange(0, knownSize ? (numsectors == 0ul ? 100 : (int)numsectors) : 100);
            statusbar->showMessage(tr("Writing..."));
            lasti = 0ul;
            update_timer.start();
            elapsed_timer->start();

            // Producer/consumer pipeline: decoder thread fills the queue,
            // main thread drains to the device. Gets decode time off the
            // critical path so on .xz inputs the USB can keep pumping while
            // the next chunk is being decompressed. See src/iopipeline.h.
            ChunkQueue queue(4);  // ~16 MB in flight at 4 × 4 MB chunks
            const unsigned long long sectorSizeLocal = sectorsize;
            const unsigned long long chunkBytesLocal = chunkSectors * sectorsize;
            const unsigned long long sectorBudget   = numsectors;
            ImageReader *rawReaderPtr = reader.get();
            std::thread decoderThread([&queue, rawReaderPtr, chunkBytesLocal, sectorSizeLocal, sectorBudget]() {
                unsigned long long produced = 0;
                while (!queue.aborted()) {
                    if (produced >= sectorBudget) {
                        auto eof = std::make_unique<IoChunk>();
                        eof->eof = true;
                        queue.push(std::move(eof));
                        return;
                    }
                    auto c = std::make_unique<IoChunk>();
                    if (!c->allocate((size_t)chunkBytesLocal, (size_t)sectorSizeLocal)) {
                        c->err = "Out of memory allocating I/O buffer.";
                        queue.push(std::move(c));
                        return;
                    }
                    const unsigned long long want = std::min<unsigned long long>(
                        chunkBytesLocal, (sectorBudget - produced) * sectorSizeLocal);
                    qint64 got = rawReaderPtr->read(c->data, (qsizetype)want);
                    if (got < 0) {
                        c->err = rawReaderPtr->errorString().toStdString();
                        queue.push(std::move(c));
                        return;
                    }
                    if (got == 0) {
                        c->eof = true;
                        queue.push(std::move(c));
                        return;
                    }
                    if (got % (qint64)sectorSizeLocal) {
                        memset(c->data + got, 0, (size_t)(sectorSizeLocal - (got % sectorSizeLocal)));
                        got = ((got + (qint64)sectorSizeLocal - 1) / (qint64)sectorSizeLocal) * (qint64)sectorSizeLocal;
                    }
                    c->length = (size_t)got;
                    produced += (unsigned long long)got / sectorSizeLocal;
                    if (!queue.push(std::move(c))) return;  // aborted mid-push
                }
            });

            bool writeError  = false;
            QString writeErrMsg;
            i = 0ul;  // sector offset written to the device; was for(i=0; ...) before the pipeline refactor
            while (status == STATUS_WRITING)
            {
                std::unique_ptr<IoChunk> c = queue.pop();
                if (!c) break;  // queue aborted
                if (!c->err.empty()) { writeErrMsg = QString::fromStdString(c->err); writeError = true; break; }
                if (c->eof) break;

                const unsigned long long chunk = c->length / sectorsize;
                if (!writeSectorDataToHandle(hRawDisk, c->data, i, chunk, sectorsize))
                {
                    writeError = true;
                    break;
                }
                i += chunk;
                if (update_timer.elapsed() >= ONE_SEC_IN_MS)
                {
                    mbpersec = (((double)sectorsize * (i - lasti)) * ((float)ONE_SEC_IN_MS / update_timer.elapsed())) / 1024.0 / 1024.0;
                    statusbar->showMessage(tr("Writing: %1 MB/s").arg(mbpersec, 0, 'f', 2));
                    elapsed_timer->update(i, numsectors);
                    update_timer.start();
                    lasti = i;
                }
                if (knownSize) {
                    progressbar->setValue((int)i);
                } else {
                    const quint64 cSize = reader->compressedSize();
                    if (cSize > 0) progressbar->setValue((int)(reader->compressedPos() * 100ULL / cSize));
                }
                QCoreApplication::processEvents();
            }
            queue.requestAbort();
            decoderThread.join();

            if (writeError)
            {
                if (!writeErrMsg.isEmpty()) QMessageBox::critical(this, tr("File Error"), writeErrMsg);
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hRawDisk = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            // If auto-verify is about to run, hand the still-locked, still-
            // unmounted volume straight over to Verify. Otherwise there's a
            // gap between unlock and Verify's re-lock where Google Drive /
            // Windows Search / antivirus latches onto the freshly-written
            // volume and starts poking at it.
            const bool chainingVerify = (status != STATUS_CANCELED)
                                        && cbVerifyAfterWrite->isChecked();
            if (chainingVerify) {
                CloseHandle(hRawDisk);
                hRawDisk = INVALID_HANDLE_VALUE;
                m_verifyInheritsLock = true;
            } else {
                // Standalone Write success — eject so the user can pull the
                // card immediately. Eject must run while the volume is still
                // locked + dismounted, before removeLockOnVolume.
                if (status != STATUS_CANCELED) {
                    ejectVolume(hVolume);
                }
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hVolume);
                hRawDisk = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
            }
            if (status == STATUS_CANCELED){
                passfail = false;
            }
        }
        else if (!fileinfo.exists() || !fileinfo.isFile())
        {
            QMessageBox::critical(this, tr("File Error"), tr("The selected file does not exist."));
            passfail = false;
        }
        else if (!fileinfo.isReadable())
        {
            QMessageBox::critical(this, tr("File Error"), tr("You do not have permision to read the selected file."));
            passfail = false;
        }
        else if (fileinfo.size() == 0)
        {
            QMessageBox::critical(this, tr("File Error"), tr("The specified file contains no data."));
            passfail = false;
        }
        progressbar->reset();
        statusbar->showMessage(tr("Done."));
        bCancel->setEnabled(false);
        setReadWriteButtonState();
        if (passfail){
            addImageFileToHistory(leFile->currentText());
            const qint64 writeMs = elapsed_timer->ms();
            // Auto-verify chains into on_bVerify_clicked() so the user gets
            // one combined "Verify Successful" dialog instead of two.
            if (cbVerifyAfterWrite->isChecked()) {
                m_writeElapsedMs = writeMs;  // read back from Verify's completion dialog
                status = STATUS_IDLE;
                elapsed_timer->stop();
                on_bVerify_clicked();
                return;  // verify handles its own close-on-EXIT and timer stop
            }
            showComplete(this, tr("Complete"),
                tr("Write Successful.<br><br><b>Elapsed:</b> %1<br><br>"
                   "<i>Card can be safely removed.</i>").arg(formatElapsedMs(writeMs)));
        }

    }
    else
    {
        QMessageBox::critical(this, tr("File Error"), tr("Please specify an image file to use."));
    }
    if (status == STATUS_EXIT)
    {
        close();
    }
    status = STATUS_IDLE;
    elapsed_timer->stop();
}

void MainWindow::on_bRead_clicked()
{
    KeepAwake keepAwake;  // suppress idle sleep until this scope exits
    QString myFile;
    if (!leFile->currentText().isEmpty())
    {
        myFile = leFile->currentText();
        QFileInfo fileinfo(myFile);
        if (fileinfo.path()=="."){
            myFile=(myHomeDir + "/" + leFile->currentText());
        }
        // check whether source and target device is the same...
        if (myFile.at(0) == cboxDevice->currentText().at(1))
        {
            QMessageBox::critical(this, tr("Write Error"), tr("Image file cannot be located on the target device."));
            return;
        }
        // confirm overwrite if the dest. file already exists
        if (fileinfo.exists())
        {
            if (QMessageBox::warning(this, tr("Confirm Overwrite"), tr("Are you sure you want to overwrite the specified file?"),
                                     QMessageBox::Yes|QMessageBox::No, QMessageBox::No) == QMessageBox::No)
            {
                return;
            }
        }
        bCancel->setEnabled(true);
        bWrite->setEnabled(false);
        bRead->setEnabled(false);
        bVerify->setEnabled(false);
        status = STATUS_READING;
        double mbpersec;
        unsigned long long i, lasti, numsectors, filesize, spaceneeded = 0ull;
        int volumeID = cboxDevice->currentText().at(1).toLatin1() - 'A';
        hVolume = getHandleOnVolume(volumeID, GENERIC_READ | GENERIC_WRITE);
        if (hVolume == INVALID_HANDLE_VALUE)
        {
            status = STATUS_IDLE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        DWORD deviceID = getDeviceID(hVolume);
        if (!getLockOnVolume(hVolume))
        {
            CloseHandle(hVolume);
            status = STATUS_IDLE;
            hVolume = INVALID_HANDLE_VALUE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        if (!unmountVolume(hVolume))
        {
            removeLockOnVolume(hVolume);
            CloseHandle(hVolume);
            status = STATUS_IDLE;
            hVolume = INVALID_HANDLE_VALUE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        // Direct I/O on the destination (image file) — see Write path above.
        hFile = getHandleOnFile(reinterpret_cast<LPCWSTR>(myFile.utf16()), GENERIC_WRITE,
                                FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            removeLockOnVolume(hVolume);
            CloseHandle(hVolume);
            status = STATUS_IDLE;
            hVolume = INVALID_HANDLE_VALUE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        hRawDisk = getHandleOnDevice(deviceID, GENERIC_READ);
        if (hRawDisk == INVALID_HANDLE_VALUE)
        {
            removeLockOnVolume(hVolume);
            CloseHandle(hFile);
            CloseHandle(hVolume);
            status = STATUS_IDLE;
            hVolume = INVALID_HANDLE_VALUE;
            hFile = INVALID_HANDLE_VALUE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        numsectors = getNumberOfSectors(hRawDisk, &sectorsize);
        const unsigned long long chunkSectors = sectorsize ? std::max<unsigned long long>(1ULL, CHUNK_BYTES / sectorsize) : 1ULL;
        if(partitionCheckBox->isChecked())
        {
            // Read MBR partition table
            sectorData = readSectorDataFromHandle(hRawDisk, 0, 1ul, 512ul);
            numsectors = 1ul;
            // Read partition information
            for (i=0ul; i<4ul; i++)
            {
                uint32_t partitionStartSector = *((uint32_t*) (sectorData + 0x1BE + 8 + 16*i));
                uint32_t partitionNumSectors = *((uint32_t*) (sectorData + 0x1BE + 12 + 16*i));
                // Set numsectors to end of last partition
                if (partitionStartSector + partitionNumSectors > numsectors)
                {
                    numsectors = partitionStartSector + partitionNumSectors;
                }
            }
        }
        filesize = getFileSizeInSectors(hFile, sectorsize);
        if (filesize >= numsectors)
        {
            spaceneeded = 0ull;
        }
        else
        {
            spaceneeded = (unsigned long long)(numsectors - filesize) * (unsigned long long)(sectorsize);
        }
        if (!spaceAvailable(myFile.left(3).replace(QChar('/'), QChar('\\')).toLatin1().data(), spaceneeded))
        {
            QMessageBox::critical(this, tr("Write Error"), tr("Disk is not large enough for the specified image."));
            removeLockOnVolume(hVolume);
            CloseHandle(hRawDisk);
            CloseHandle(hFile);
            CloseHandle(hVolume);
            status = STATUS_IDLE;
            sectorData = NULL;
            hRawDisk = INVALID_HANDLE_VALUE;
            hFile = INVALID_HANDLE_VALUE;
            hVolume = INVALID_HANDLE_VALUE;
            bCancel->setEnabled(false);
            setReadWriteButtonState();
            return;
        }
        if (numsectors == 0ul)
        {
            progressbar->setRange(0, 100);
        }
        else
        {
            progressbar->setRange(0, (int)numsectors);
        }
        statusbar->showMessage(tr("Reading..."));
        lasti = 0ul;
        update_timer.start();
        elapsed_timer->start();
        for (i = 0ul; i < numsectors && status == STATUS_READING; i += chunkSectors)
        {
            sectorData = readSectorDataFromHandle(hRawDisk, i, (numsectors - i >= chunkSectors) ? chunkSectors:(numsectors - i), sectorsize);
            if (sectorData == NULL)
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            if (!writeSectorDataToHandle(hFile, sectorData, i, (numsectors - i >= chunkSectors) ? chunkSectors:(numsectors - i), sectorsize))
            {
                _aligned_free(sectorData);
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                sectorData = NULL;
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            _aligned_free(sectorData);
            sectorData = NULL;
            if (update_timer.elapsed() >= ONE_SEC_IN_MS)
            {
                mbpersec = (((double)sectorsize * (i - lasti)) * ((float)ONE_SEC_IN_MS / update_timer.elapsed())) / 1024.0 / 1024.0;
                statusbar->showMessage(tr("Reading: %1 MB/s").arg(mbpersec, 0, 'f', 2));
                update_timer.start();
                elapsed_timer->update(i, numsectors);
                lasti = i;
            }
            progressbar->setValue(i);
            QCoreApplication::processEvents();
        }
        removeLockOnVolume(hVolume);
        CloseHandle(hRawDisk);
        CloseHandle(hFile);
        CloseHandle(hVolume);
        hRawDisk = INVALID_HANDLE_VALUE;
        hFile = INVALID_HANDLE_VALUE;
        hVolume = INVALID_HANDLE_VALUE;
        progressbar->reset();
        statusbar->showMessage(tr("Done."));
        bCancel->setEnabled(false);
        setReadWriteButtonState();
        if (status == STATUS_CANCELED){
            QMessageBox::information(this, tr("Complete"), tr("Read Canceled."));
        } else {
            addImageFileToHistory(leFile->currentText());
            showComplete(this, tr("Complete"),
                tr("Read Successful.<br><br><b>Elapsed:</b> %1").arg(formatElapsedMs(elapsed_timer->ms())));
        }
        updateHashControls();
    }
    else
    {
        QMessageBox::critical(this, tr("File Info"), tr("Please specify a file to save data to."));
    }
    if (status == STATUS_EXIT)
    {
        close();
    }
    status = STATUS_IDLE;
    elapsed_timer->stop();
}

// Verify image with device
void MainWindow::on_bVerify_clicked()
{
    KeepAwake keepAwake;  // suppress idle sleep until this scope exits
    bool passfail = true;
    if (!leFile->currentText().isEmpty())
    {
        QFileInfo fileinfo(leFile->currentText());
        if (fileinfo.exists() && fileinfo.isFile() &&
                fileinfo.isReadable() && (fileinfo.size() > 0) )
        {
            if (leFile->currentText().at(0) == cboxDevice->currentText().at(1))
            {
                QMessageBox::critical(this, tr("Verify Error"), tr("Image file cannot be located on the target device."));
                return;
            }
            status = STATUS_VERIFYING;
            bCancel->setEnabled(true);
            bWrite->setEnabled(false);
            bRead->setEnabled(false);
            bVerify->setEnabled(false);
            double mbpersec;
            unsigned long long i, lasti, availablesectors, numsectors, result;
            int volumeID = cboxDevice->currentText().at(1).toLatin1() - 'A';
            DWORD deviceID;
            // If chained from auto-verify after Write, hVolume is already
            // open, locked and dismounted. Reusing it keeps third-party
            // watchers (Google Drive / indexer / antivirus) off the volume
            // through the Write → Verify boundary.
            if (m_verifyInheritsLock && hVolume != INVALID_HANDLE_VALUE) {
                m_verifyInheritsLock = false;
                deviceID = getDeviceID(hVolume);
            } else {
                hVolume = getHandleOnVolume(volumeID, GENERIC_READ | GENERIC_WRITE);
                if (hVolume == INVALID_HANDLE_VALUE)
                {
                    status = STATUS_IDLE;
                    bCancel->setEnabled(false);
                    setReadWriteButtonState();
                    return;
                }
                deviceID = getDeviceID(hVolume);
                if (!getLockOnVolume(hVolume))
                {
                    CloseHandle(hVolume);
                    status = STATUS_IDLE;
                    hVolume = INVALID_HANDLE_VALUE;
                    bCancel->setEnabled(false);
                    setReadWriteButtonState();
                    return;
                }
                if (!unmountVolume(hVolume))
                {
                    removeLockOnVolume(hVolume);
                    CloseHandle(hVolume);
                    status = STATUS_IDLE;
                    hVolume = INVALID_HANDLE_VALUE;
                    bCancel->setEnabled(false);
                    setReadWriteButtonState();
                    return;
                }
            }
            hFile = getHandleOnFile(reinterpret_cast<LPCWSTR>(leFile->currentText().utf16()), GENERIC_READ);
            if (hFile == INVALID_HANDLE_VALUE)
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            hRawDisk = getHandleOnDevice(deviceID, GENERIC_READ);
            if (hRawDisk == INVALID_HANDLE_VALUE)
            {
                removeLockOnVolume(hVolume);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hVolume = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            availablesectors = getNumberOfSectors(hRawDisk, &sectorsize);
            if (!availablesectors)
            {
                //For external card readers you may not get device change notification when you remove the card/flash.
                //(So no WM_DEVICECHANGE signal). Device stays but size goes to 0. [Is there special event for this on Windows??]
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                passfail = false;
                status = STATUS_IDLE;
                return;

            }
            const unsigned long long chunkSectors = sectorsize ? std::max<unsigned long long>(1ULL, CHUNK_BYTES / sectorsize) : 1ULL;

            // Open the image reader (sniffs magic bytes — Raw / Gz / Xz).
            QString verifyReaderErr;
            std::unique_ptr<ImageReader> reader = ImageReader::open(leFile->currentText(), &verifyReaderErr);
            if (!reader || reader->compressedSize() == 0)
            {
                QMessageBox::critical(this, tr("File Error"),
                                      reader ? tr("The selected image file is empty.") : verifyReaderErr);
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hFile);
                CloseHandle(hVolume);
                hRawDisk = INVALID_HANDLE_VALUE;
                hFile = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                status = STATUS_IDLE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }

            const quint64 imgBytes  = reader->uncompressedSize();
            const bool    knownSize = (imgBytes > 0);
            numsectors = knownSize ? (imgBytes + sectorsize - 1) / sectorsize
                                   : availablesectors;

            if (knownSize && numsectors > availablesectors)
            {
                bool datafound = true;
                if (dynamic_cast<RawImageReader *>(reader.get()))
                {
                    datafound = false;
                    unsigned long long si = availablesectors;
                    while (si < numsectors && !datafound)
                    {
                        const unsigned long long next =
                            std::min<unsigned long long>(chunkSectors, numsectors - si);
                        char *sd = readSectorDataFromHandle(hFile, si, next, sectorsize);
                        if (!sd) break;
                        const unsigned long long limit = next * sectorsize;
                        for (unsigned long long j = 0; j < limit; ++j)
                        {
                            if (sd[j]) { datafound = true; break; }
                        }
                        _aligned_free(sd);
                        si += next;
                    }
                }
                std::ostringstream msg;
                msg << "Size of image larger than device:"
                    << "\n  Image: "  << numsectors      << " sectors"
                    << "\n  Device: " << availablesectors << " sectors"
                    << "\n  Sector Size: " << sectorsize
                    << "\n\nThe extra space " << ((datafound) ? "DOES" : "does not") << " appear to contain data"
                    << "\n\nContinue Anyway?";
                if (QMessageBox::warning(this, tr("Size Mismatch!"),
                                         tr(msg.str().c_str()),
                                         QMessageBox::Ok, QMessageBox::Cancel) == QMessageBox::Ok)
                {
                    numsectors = availablesectors;
                }
                else
                {
                    removeLockOnVolume(hVolume);
                    CloseHandle(hRawDisk);
                    CloseHandle(hFile);
                    CloseHandle(hVolume);
                    status = STATUS_IDLE;
                    hVolume = INVALID_HANDLE_VALUE;
                    hFile = INVALID_HANDLE_VALUE;
                    hRawDisk = INVALID_HANDLE_VALUE;
                    bCancel->setEnabled(false);
                    setReadWriteButtonState();
                    return;
                }
            }

            // hFile was only useful for the pre-scan above — the Verify main
            // loop consumes the reader (which holds its own state) and reads
            // the device directly via readSectorDataFromHandle.
            CloseHandle(hFile);
            hFile = INVALID_HANDLE_VALUE;

            progressbar->setRange(0, knownSize ? (numsectors == 0ul ? 100 : (int)numsectors) : 100);
            statusbar->showMessage(tr("Verifying..."));
            update_timer.start();
            elapsed_timer->start();
            lasti = 0ul;

            // Same producer/consumer shape as Write. Decoder thread fills the
            // queue with decoded file chunks, the main thread reads the disk
            // and memcmp's. Decode runs in parallel with disk read + compare.
            ChunkQueue queue(4);
            const unsigned long long sectorSizeLocal = sectorsize;
            const unsigned long long chunkBytesLocal = chunkSectors * sectorsize;
            const unsigned long long sectorBudget   = numsectors;
            ImageReader *rawReaderPtr = reader.get();
            std::thread decoderThread([&queue, rawReaderPtr, chunkBytesLocal, sectorSizeLocal, sectorBudget]() {
                unsigned long long produced = 0;
                while (!queue.aborted()) {
                    if (produced >= sectorBudget) {
                        auto eof = std::make_unique<IoChunk>();
                        eof->eof = true;
                        queue.push(std::move(eof));
                        return;
                    }
                    auto c = std::make_unique<IoChunk>();
                    if (!c->allocate((size_t)chunkBytesLocal, (size_t)sectorSizeLocal)) {
                        c->err = "Out of memory allocating I/O buffer.";
                        queue.push(std::move(c));
                        return;
                    }
                    const unsigned long long want = std::min<unsigned long long>(
                        chunkBytesLocal, (sectorBudget - produced) * sectorSizeLocal);
                    qint64 got = rawReaderPtr->read(c->data, (qsizetype)want);
                    if (got < 0) {
                        c->err = rawReaderPtr->errorString().toStdString();
                        queue.push(std::move(c));
                        return;
                    }
                    if (got == 0) {
                        c->eof = true;
                        queue.push(std::move(c));
                        return;
                    }
                    if (got % (qint64)sectorSizeLocal) {
                        memset(c->data + got, 0, (size_t)(sectorSizeLocal - (got % sectorSizeLocal)));
                        got = ((got + (qint64)sectorSizeLocal - 1) / (qint64)sectorSizeLocal) * (qint64)sectorSizeLocal;
                    }
                    c->length = (size_t)got;
                    produced += (unsigned long long)got / sectorSizeLocal;
                    if (!queue.push(std::move(c))) return;
                }
            });

            bool verifyError = false;
            QString verifyErrMsg;
            i = 0ul;  // sector offset into the device; was for(i=0; ...) before the pipeline refactor
            while (status == STATUS_VERIFYING)
            {
                std::unique_ptr<IoChunk> c = queue.pop();
                if (!c) break;
                if (!c->err.empty()) { verifyErrMsg = QString::fromStdString(c->err); verifyError = true; break; }
                if (c->eof) break;

                const unsigned long long chunk = c->length / sectorsize;
                sectorData2 = readSectorDataFromHandle(hRawDisk, i, chunk, sectorsize);
                if (sectorData2 == NULL)
                {
                    QMessageBox::critical(this, tr("Verify Failure"), tr("Verification failed at sector: %1").arg(i));
                    verifyError = true;
                    break;
                }
                result = memcmp(c->data, sectorData2, c->length);
                if (result)
                {
                    QMessageBox::critical(this, tr("Verify Failure"), tr("Verification failed at sector: %1").arg(i));
                    passfail = false;
                    _aligned_free(sectorData2);
                    sectorData2 = NULL;
                    break;
                }
                _aligned_free(sectorData2);
                sectorData2 = NULL;
                i += chunk;
                if (update_timer.elapsed() >= ONE_SEC_IN_MS)
                {
                    mbpersec = (((double)sectorsize * (i - lasti)) * ((float)ONE_SEC_IN_MS / update_timer.elapsed())) / 1024.0 / 1024.0;
                    statusbar->showMessage(tr("Verifying: %1 MB/s").arg(mbpersec, 0, 'f', 2));
                    update_timer.start();
                    elapsed_timer->update(i, numsectors);
                    lasti = i;
                }
                if (knownSize) {
                    progressbar->setValue((int)i);
                } else {
                    const quint64 cSize = reader->compressedSize();
                    if (cSize > 0) progressbar->setValue((int)(reader->compressedPos() * 100ULL / cSize));
                }
                QCoreApplication::processEvents();
            }
            queue.requestAbort();
            decoderThread.join();

            if (verifyError)
            {
                if (!verifyErrMsg.isEmpty()) QMessageBox::critical(this, tr("File Error"), verifyErrMsg);
                removeLockOnVolume(hVolume);
                CloseHandle(hRawDisk);
                CloseHandle(hVolume);
                status = STATUS_IDLE;
                hRawDisk = INVALID_HANDLE_VALUE;
                hVolume = INVALID_HANDLE_VALUE;
                bCancel->setEnabled(false);
                setReadWriteButtonState();
                return;
            }
            // Eject only when this Verify was chained from a successful
            // Write — m_writeElapsedMs > 0 is the marker. Standalone
            // user-initiated Verify must NOT eject; the user may want to
            // do something else with the card afterwards.
            if (status != STATUS_CANCELED && m_writeElapsedMs > 0) {
                ejectVolume(hVolume);
            }
            removeLockOnVolume(hVolume);
            CloseHandle(hRawDisk);
            CloseHandle(hVolume);
            hRawDisk = INVALID_HANDLE_VALUE;
            hVolume = INVALID_HANDLE_VALUE;
            if (status == STATUS_CANCELED){
                passfail = false;
            }

        }
        else if (!fileinfo.exists() || !fileinfo.isFile())
        {
            QMessageBox::critical(this, tr("File Error"), tr("The selected file does not exist."));
            passfail = false;
        }
        else if (!fileinfo.isReadable())
        {
            QMessageBox::critical(this, tr("File Error"), tr("You do not have permision to read the selected file."));
            passfail = false;
        }
        else if (fileinfo.size() == 0)
        {
            QMessageBox::critical(this, tr("File Error"), tr("The specified file contains no data."));
            passfail = false;
        }
        progressbar->reset();
        statusbar->showMessage(tr("Done."));
        bCancel->setEnabled(false);
        setReadWriteButtonState();
        if (passfail){
            const qint64 verifyMs = elapsed_timer->ms();
            QString msg;
            if (m_writeElapsedMs > 0) {
                // Chained from an auto-verify — report both phases.
                const qint64 totalMs = m_writeElapsedMs + verifyMs;
                // %ZEBRA% placeholder is resolved inside showComplete()
                // based on the actual dialog palette — QApplication's
                // AlternateBase lies on Windows dark mode.
                msg = tr("Write &amp; Verify Successful.<br><br>"
                         "<center>"
                         "<table cellspacing=\"0\" cellpadding=\"6\">"
                         "<tr>"
                         "<td bgcolor=\"%ZEBRA%\"><b>Write:</b>&nbsp;&nbsp;</td>"
                         "<td bgcolor=\"%ZEBRA%\">%1</td>"
                         "</tr>"
                         "<tr>"
                         "<td><b>Verify:</b>&nbsp;&nbsp;</td>"
                         "<td>%2</td>"
                         "</tr>"
                         "<tr>"
                         "<td bgcolor=\"%ZEBRA%\"><b>Total:</b>&nbsp;&nbsp;</td>"
                         "<td bgcolor=\"%ZEBRA%\">%3</td>"
                         "</tr>"
                         "</table>"
                         "</center>"
                         "<br><i>Card can be safely removed.</i>")
                          .arg(formatElapsedMs(m_writeElapsedMs))
                          .arg(formatElapsedMs(verifyMs))
                          .arg(formatElapsedMs(totalMs));
                m_writeElapsedMs = 0;
            } else {
                msg = tr("Verify Successful.<br><br><b>Elapsed:</b> %1").arg(formatElapsedMs(verifyMs));
            }
            showComplete(this, tr("Complete"), msg);
        }
    }
    else
    {
        QMessageBox::critical(this, tr("File Error"), tr("Please specify an image file to use."));
    }
    if (status == STATUS_EXIT)
    {
        close();
    }
    status = STATUS_IDLE;
    elapsed_timer->stop();
}

// getLogicalDrives sets cBoxDevice with any logical drives found, as long
// as they indicate that they're either removable, or fixed and on USB bus
void MainWindow::getLogicalDrives()
{
    // GetLogicalDrives returns 0 on failure, or a bitmask representing
    // the drives available on the system (bit 0 = A:, bit 1 = B:, etc)
    unsigned long driveMask = GetLogicalDrives();
    int i = 0;
    ULONG pID;
    unsigned long long sizeBytes;

    cboxDevice->clear();

    while (driveMask != 0)
    {
        if (driveMask & 1)
        {
            // the "A" in drivename will get incremented by the # of bits
            // we've shifted
            char drivename[] = "\\\\.\\A:\\";
            drivename[4] += i;
            if (checkDriveType(drivename, &pID, &sizeBytes))
            {
                QString label = QString("[%1:\\]").arg(drivename[4]);
                const QString sz = formatDeviceSize(sizeBytes);
                if (!sz.isEmpty()) label += QStringLiteral(" ") + sz;
                cboxDevice->addItem(label, (qulonglong)pID);
            }
        }
        driveMask >>= 1;
        cboxDevice->setCurrentIndex(0);
        ++i;
    }
}

// support routine for winEvent - returns the drive letter for a given mask
//   taken from http://support.microsoft.com/kb/163503
char FirstDriveFromMask (ULONG unitmask)
{
    char i;

    for (i = 0; i < 26; ++i)
    {
        if (unitmask & 0x1)
        {
            break;
        }
        unitmask = unitmask >> 1;
    }

    return (i + 'A');
}

// register to receive notifications when USB devices are inserted or removed
// adapted from http://www.known-issues.net/qt/qt-detect-event-windows.html
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool MainWindow::nativeEvent(const QByteArray &type, void *vMsg, qintptr *result)
#else
bool MainWindow::nativeEvent(const QByteArray &type, void *vMsg, long *result)
#endif
{
    Q_UNUSED(type);
    MSG *msg = (MSG*)vMsg;
    if(msg->message == WM_DEVICECHANGE)
    {
        PDEV_BROADCAST_HDR lpdb = (PDEV_BROADCAST_HDR)msg->lParam;
        switch(msg->wParam)
        {
        case DBT_DEVICEARRIVAL:
            if (lpdb -> dbch_devicetype == DBT_DEVTYP_VOLUME)
            {
                PDEV_BROADCAST_VOLUME lpdbv = (PDEV_BROADCAST_VOLUME)lpdb;
                if ((lpdbv->dbcv_flags & DBTF_NET) == 0)
                {
                    char ALET = FirstDriveFromMask(lpdbv->dbcv_unitmask);
                    // add device to combo box (after sanity check that
                    // it's not already there, which it shouldn't be)
                    QString qsPrefix = QString("[%1:\\]").arg(ALET);
                    if (cboxDevice->findText(qsPrefix, Qt::MatchStartsWith) == -1)
                    {
                        ULONG pID;
                        unsigned long long sizeBytes;
                        char longname[] = "\\\\.\\A:\\";
                        longname[4] = ALET;
                        // checkDriveType gets the physicalID
                        if (checkDriveType(longname, &pID, &sizeBytes))
                        {
                            QString label = qsPrefix;
                            const QString sz = formatDeviceSize(sizeBytes);
                            if (!sz.isEmpty()) label += QStringLiteral(" ") + sz;
                            cboxDevice->addItem(label, (qulonglong)pID);
                            setReadWriteButtonState();
                        }
                    }
                }
            }
            break;
        case DBT_DEVICEREMOVECOMPLETE:
            if (lpdb -> dbch_devicetype == DBT_DEVTYP_VOLUME)
            {
                PDEV_BROADCAST_VOLUME lpdbv = (PDEV_BROADCAST_VOLUME)lpdb;
                if ((lpdbv->dbcv_flags & DBTF_NET) == 0)
                {
                    char ALET = FirstDriveFromMask(lpdbv->dbcv_unitmask);
                    //  find the device that was removed in the combo box,
                    //  and remove it from there....
                    //  "removeItem" ignores the request if the index is
                    //  out of range, and findText returns -1 if the item isn't found.
                    // Items now carry a size suffix ('[E:\\] 32 GB'), so
                    // match only the '[X:\\]' prefix.
                    cboxDevice->removeItem(cboxDevice->findText(QString("[%1:\\]").arg(ALET), Qt::MatchStartsWith));
                    setReadWriteButtonState();
                }
            }
            break;
        } // skip the rest
    } // end of if msg->message
    *result = 0; //get rid of obnoxious compiler warning
    return false; // let qt handle the rest
}

void MainWindow::updateHashControls()
{
    QFileInfo fileinfo(leFile->currentText());
    bool validFile = (fileinfo.exists() && fileinfo.isFile() &&
                      fileinfo.isReadable() && (fileinfo.size() >0));

    bHashCopy->setEnabled(false);
    hashLabel->clear();

    if (cboxHashType->currentIndex() != 0 && !leFile->currentText().isEmpty() && validFile)
    {
            bHashGen->setEnabled(true);
    }
    else
    {
        bHashGen->setEnabled(false);
    }

    // if there's a value in the md5 label make the copy button visible
    bool haveHash = !(hashLabel->text().isEmpty());
    bHashCopy->setEnabled(haveHash );
}

void MainWindow::on_cboxHashType_IdxChg()
{
    updateHashControls();
}

void MainWindow::on_bHashGen_clicked()
{
    generateHash(leFile->currentText().toLatin1().data(),cboxHashType->currentData().toInt());

}
