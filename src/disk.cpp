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
#include <QMap>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>          // _aligned_malloc / _aligned_free
#include <windows.h>
#include <winioctl.h>
#include "disk.h"
#include "mainwindow.h"

HANDLE getHandleOnFile(LPCWSTR filelocation, DWORD access, DWORD extraFlags)
{
    HANDLE hFile;
    hFile = CreateFileW(filelocation, access, (access == GENERIC_READ) ? FILE_SHARE_READ : 0, NULL, (access == GENERIC_READ) ? OPEN_EXISTING:CREATE_ALWAYS, extraFlags, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
    {
        wchar_t *errormessage=NULL;
        ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0,
                         (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("File Error"), QObject::tr("An error occurred when attempting to get a handle on the file.\n"
                                                              "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return hFile;
}
DWORD getDeviceID(HANDLE hVolume)
{
    VOLUME_DISK_EXTENTS sd;
    DWORD bytesreturned;
    if (!DeviceIoControl(hVolume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &sd, sizeof(sd), &bytesreturned, NULL))
    {
        wchar_t *errormessage=NULL;
        ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0,
                         (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Volume Error"),
                              QObject::tr("An error occurred when attempting to get information on volume.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return sd.Extents[0].DiskNumber;
}

HANDLE getHandleOnDevice(int device, DWORD access, DWORD extraFlags)
{
    HANDLE hDevice;
    QString devicename = QString("\\\\.\\PhysicalDrive%1").arg(device);
    hDevice = CreateFileW(reinterpret_cast<LPCWSTR>(devicename.utf16()), access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, extraFlags, NULL);
    if (hDevice == INVALID_HANDLE_VALUE)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Device Error"),
                              QObject::tr("An error occurred when attempting to get a handle on the device.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return hDevice;
}

HANDLE getHandleOnVolume(int volume, DWORD access, DWORD extraFlags)
{
    HANDLE hVolume;
    wchar_t volumename[] = L"\\\\.\\A:";
    volumename[4] += volume;
    hVolume = CreateFileW(volumename, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, extraFlags, NULL);
    if (hVolume == INVALID_HANDLE_VALUE)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Volume Error"),
                              QObject::tr("An error occurred when attempting to get a handle on the volume.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return hVolume;
}

bool getLockOnVolume(HANDLE handle)
{
    DWORD bytesreturned;
    BOOL bResult = FALSE;
    DWORD lastError = ERROR_SUCCESS;
    const int maxRetries = 100; // up to ~10s total wait
    const DWORD retryDelayMs = 100;

    for (int attempt = 0; attempt < maxRetries; ++attempt)
    {
        bResult = DeviceIoControl(handle, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &bytesreturned, NULL);
        if (bResult)
        {
            return true;
        }

        lastError = GetLastError();
        if ((lastError != ERROR_ACCESS_DENIED) &&
            (lastError != ERROR_SHARING_VIOLATION) &&
            (lastError != ERROR_LOCK_VIOLATION))
        {
            break;
        }

        if (attempt + 1 < maxRetries)
        {
            Sleep(retryDelayMs);
        }
    }

    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, lastError, 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Lock Error"),
                              QObject::tr("An error occurred when attempting to lock the volume.\n"
                                          "Error %1: %2\n\n"
                                          "Please close any File Explorer windows, terminals, or applications using this drive and try again.")
                              .arg(lastError).arg(errText));
        LocalFree(errormessage);
    }
    return (bResult);
}

bool removeLockOnVolume(HANDLE handle)
{
    DWORD junk;
    BOOL bResult;
    bResult = DeviceIoControl(handle, FSCTL_UNLOCK_VOLUME, NULL, 0, NULL, 0, &junk, NULL);
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Unlock Error"),
                              QObject::tr("An error occurred when attempting to unlock the volume.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return (bResult);
}

bool unmountVolume(HANDLE handle)
{
    DWORD junk;
    BOOL bResult;
    bResult = DeviceIoControl(handle, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &junk, NULL);
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Dismount Error"),
                              QObject::tr("An error occurred when attempting to dismount the volume.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return (bResult);
}

bool isVolumeUnmounted(HANDLE handle)
{
    DWORD junk;
    BOOL bResult;
    bResult = DeviceIoControl(handle, FSCTL_IS_VOLUME_MOUNTED, NULL, 0, NULL, 0, &junk, NULL);
    return (!bResult);
}

bool ejectVolume(HANDLE handle)
{
    DWORD junk;
    // Silent on failure: not all bus types support eject, and we don't
    // want to block the success dialog over a best-effort convenience.
    return DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &junk, NULL) != 0;
}

// Sector-aligned buffer: FILE_FLAG_NO_BUFFERING on the device/file handle
// requires both the buffer address and transfer size to be a multiple of the
// sector size. Freeing must go through _aligned_free (see callers).
char *readSectorDataFromHandle(HANDLE handle, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize)
{
    unsigned long bytesread;
    char *data = (char *)_aligned_malloc((size_t)(sectorsize * numsectors), (size_t)sectorsize);
    if (!data) {
        return NULL;
    }
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)(startsector * sectorsize);
    if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Read Error"),
                              QObject::tr("Seek failed at offset %1.\nError %2: %3")
                                  .arg(li.QuadPart).arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
        _aligned_free(data);
        return NULL;
    }
    if (!ReadFile(handle, data, (DWORD)(sectorsize * numsectors), &bytesread, NULL))
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Read Error"),
                              QObject::tr("An error occurred when attempting to read data from handle.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
        _aligned_free(data);
        data = NULL;
    }
    if (data && bytesread < (sectorsize * numsectors))
    {
            memset(data + bytesread,0,(sectorsize * numsectors) - bytesread);
    }
    return data;
}

bool writeSectorDataToHandle(HANDLE handle, char *data, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize)
{
    unsigned long byteswritten;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)(startsector * sectorsize);
    if (!SetFilePointerEx(handle, li, nullptr, FILE_BEGIN)) {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Write Error"),
                              QObject::tr("Seek failed at offset %1.\nError %2: %3")
                                  .arg(li.QuadPart).arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
        return false;
    }
    const BOOL bResult = WriteFile(handle, data, (DWORD)(sectorsize * numsectors), &byteswritten, NULL);
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Write Error"),
                              QObject::tr("An error occurred when attempting to write data to handle.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
        LocalFree(errormessage);
    }
    return (bResult);
}

unsigned long long getNumberOfSectors(HANDLE handle, unsigned long long *sectorsize)
{
    DWORD junk = 0;
    DISK_GEOMETRY_EX diskgeometry = {};
    BOOL bResult = FALSE;
    // Retry on transient errors — something else (Google Drive / OneDrive /
    // Explorer indexer, ...) can grab the disk for a second right after the
    // user inserts the card. Gives up after ~2 s.
    DWORD lastErr = 0;
    for (int attempt = 0; attempt < 10; ++attempt)
    {
        bResult = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                                  NULL, 0, &diskgeometry, sizeof(diskgeometry),
                                  &junk, NULL);
        if (bResult) break;
        lastErr = GetLastError();
        const bool transient = (lastErr == ERROR_DEV_NOT_EXIST
                             || lastErr == ERROR_ACCESS_DENIED
                             || lastErr == ERROR_SHARING_VIOLATION
                             || lastErr == ERROR_LOCK_VIOLATION
                             || lastErr == ERROR_INVALID_FUNCTION);
        if (!transient) break;
        Sleep(200);
    }
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, lastErr, 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Device Error"),
                              QObject::tr("An error occurred when attempting to get the device's geometry.\n"
                                          "Error %1: %2").arg(lastErr).arg(errText));
        LocalFree(errormessage);
        return 0;
    }
    if (sectorsize != NULL)
    {
        *sectorsize = (unsigned long long)diskgeometry.Geometry.BytesPerSector;
    }
    return (unsigned long long)diskgeometry.DiskSize.QuadPart / (unsigned long long)diskgeometry.Geometry.BytesPerSector;
}

unsigned long long getFileSizeInSectors(HANDLE handle, unsigned long long sectorsize)
{
    unsigned long long retVal = 0;
    if (sectorsize) // avoid divide by 0
    {
        LARGE_INTEGER filesize;
        if(GetFileSizeEx(handle, &filesize) == 0)
        {
            // error
            wchar_t *errormessage=NULL;
            FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
            QString errText = QString::fromUtf16((const char16_t *)errormessage);
            QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("File Error"),
                                  QObject::tr("An error occurred while getting the file size.\n"
                                              "Error %1: %2").arg(GetLastError()).arg(errText));
            LocalFree(errormessage);
            retVal = 0;
        }
        else
        {
            retVal = ((unsigned long long)filesize.QuadPart / sectorsize ) + (((unsigned long long)filesize.QuadPart % sectorsize )?1:0);
        }
    }
    return(retVal);
}

bool spaceAvailable(char *location, unsigned long long spaceneeded)
{
    ULARGE_INTEGER freespace;
    BOOL bResult;
    bResult = GetDiskFreeSpaceEx(location, NULL, NULL, &freespace);
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Free Space Error"),
                              QObject::tr("Failed to get the free space on drive %1.\n"
                                          "Error %2: %3\n"
                                          "Checking of free space will be skipped.").arg(location).arg(GetLastError()).arg(errText));
        return true;
    }
    return (spaceneeded <= freespace.QuadPart);
}

// given a drive letter (ending in a slash), return the label for that drive
// TODO make this more robust by adding input verification
QString getDriveLabel(const char *drv)
{
    QString retVal;
    int szNameBuf = MAX_PATH + 1;
    char *nameBuf = NULL;
    if( (nameBuf = (char *)calloc(szNameBuf, sizeof(char))) != 0 )
    {
        ::GetVolumeInformationA(drv, nameBuf, szNameBuf, NULL,
                                        NULL, NULL, NULL, 0);
    }

    // if malloc fails, nameBuf will be NULL.
    // if GetVolumeInfo fails, nameBuf will contain empty string
    // if all succeeds, nameBuf will contain label
    if(nameBuf == NULL)
    {
        retVal = QString("");
    }
    else
    {
        retVal = QString(nameBuf);
        free(nameBuf);
    }

    return(retVal);
}

// Build the disk-number → mounted-letter map for enumerateTargetDisks().
// Walks GetLogicalDrives() and asks each \\.\X: for IOCTL_STORAGE_GET_DEVICE_NUMBER.
// Letters that don't map to a physical disk (network shares, subst drives,
// virtual filesystems like Google Drive / OneDrive) are silently skipped.
static QMap<DWORD, QStringList> buildDiskLetterMap()
{
    QMap<DWORD, QStringList> map;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if ((mask & (1u << i)) == 0) continue;
        wchar_t volPath[] = L"\\\\.\\A:";
        volPath[4] = static_cast<wchar_t>(L'A' + i);
        HANDLE hVol = CreateFileW(volPath, FILE_READ_ATTRIBUTES,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER sdn = {};
        DWORD got = 0;
        if (DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                            NULL, 0, &sdn, sizeof(sdn), &got, NULL)
            && sdn.DeviceType == FILE_DEVICE_DISK)
        {
            map[sdn.DeviceNumber].append(
                QString(QChar(QLatin1Char('A' + i))) + ":");
        }
        CloseHandle(hVol);
    }
    return map;
}

QList<TargetDisk> enumerateTargetDisks()
{
    QList<TargetDisk> out;
    const QMap<DWORD, QStringList> letterMap = buildDiskLetterMap();

    for (DWORD n = 0; n < 32; ++n) {
        const QString path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(n);
        HANDLE hDisk = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                                   0,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hDisk == INVALID_HANDLE_VALUE) continue;

        // Bus-type filter: USB / SD / MMC always accepted, removable
        // accepted unless on the SATA bus (catches USB-attached HDDs in
        // odd reporting modes), fixed SATA / NVMe / RAID system disks
        // rejected. Probe failures are silent — no dialog on disks the
        // user can't or shouldn't pick.
        STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty,
                                     PropertyStandardQuery, {0} };
        BYTE buf[1024] = {0};
        DWORD got = 0;
        if (!DeviceIoControl(hDisk, IOCTL_STORAGE_QUERY_PROPERTY,
                             &q, sizeof(q), buf, sizeof(buf), &got, NULL)
            || got < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            CloseHandle(hDisk);
            continue;
        }
        const auto *desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR *>(buf);
        const BYTE busType   = static_cast<BYTE>(desc->BusType);
        const bool removable = (desc->RemovableMedia != FALSE);
        const bool accept =
            (busType == BusTypeUsb || busType == BusTypeSd || busType == BusTypeMmc)
            || (removable && busType != BusTypeSata);
        if (!accept) {
            CloseHandle(hDisk);
            continue;
        }

        // Hub-slot media check: a multi-card reader exposes one PhysicalDrive
        // per slot regardless of insertion. Without this filter empty slots
        // would litter the dropdown.
        if (!DeviceIoControl(hDisk, IOCTL_STORAGE_CHECK_VERIFY,
                             NULL, 0, NULL, 0, &got, NULL)) {
            CloseHandle(hDisk);
            continue;
        }

        DISK_GEOMETRY_EX dg = {};
        if (!DeviceIoControl(hDisk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                             NULL, 0, &dg, sizeof(dg), &got, NULL)) {
            CloseHandle(hDisk);
            continue;
        }
        CloseHandle(hDisk);

        TargetDisk td;
        td.diskNumber = n;
        td.sizeBytes  = static_cast<quint64>(dg.DiskSize.QuadPart);
        td.letters    = letterMap.value(n);
        out.append(td);
    }
    return out;
}
