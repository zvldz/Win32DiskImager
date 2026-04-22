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
    DWORD junk;
    DISK_GEOMETRY_EX diskgeometry;
    BOOL bResult;
    bResult = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0, &diskgeometry, sizeof(diskgeometry), &junk, NULL);
    if (!bResult)
    {
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, GetLastError(), 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Device Error"),
                              QObject::tr("An error occurred when attempting to get the device's geometry.\n"
                                          "Error %1: %2").arg(GetLastError()).arg(errText));
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

// Probe used during drive enumeration. Failures are expected and normal for
// virtual/user-mode filesystems (Google Drive, OneDrive, Dokany, VeraCrypt,
// Subst, RamDisk, ...) that do not fully implement storage IOCTLs. Return
// FALSE silently so the caller can skip the device — never show a dialog
// from here, or the user gets a nag popup for every such drive at startup.
BOOL GetDisksProperty(HANDLE hDevice, PSTORAGE_DEVICE_DESCRIPTOR pDevDesc,
                      DEVICE_NUMBER *devInfo)
{
    STORAGE_PROPERTY_QUERY Query;
    DWORD dwOutBytes;

    Query.PropertyId = StorageDeviceProperty;
    Query.QueryType = PropertyStandardQuery;

    if (!::DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
            &Query, sizeof(STORAGE_PROPERTY_QUERY), pDevDesc,
            pDevDesc->Size, &dwOutBytes, (LPOVERLAPPED)NULL))
    {
        return FALSE;
    }

    if (!::DeviceIoControl(hDevice, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            NULL, 0, devInfo, sizeof(DEVICE_NUMBER), &dwOutBytes,
            (LPOVERLAPPED)NULL))
    {
        return FALSE;
    }

    return TRUE;
}

// some routines fail if there's no trailing slash in a name,
// 		others fail if there is.  So this routine takes a name (trailing
// 		slash or no), and creates 2 versions - one with the slash, and one w/o
//
// 		CALLER MUST FREE THE 2 RETURNED STRINGS
bool slashify(char *str, char **slash, char **noSlash)
{
    bool retVal = false;
    int strLen = strlen(str);
    if ( strLen > 0 )
    {
        if ( *(str + strLen - 1) == '\\' )
        {
            // trailing slash exists
            if (( (*slash = (char *)calloc( (strLen + 1), sizeof(char))) != NULL) &&
                    ( (*noSlash = (char *)calloc(strLen, sizeof(char))) != NULL))
            {
                memcpy(*slash, str, strLen + 1); // include NUL terminator
                memcpy(*noSlash, *slash, strLen - 1);
                (*noSlash)[strLen - 1] = '\0';
                retVal = true;
            }
        }
        else
        {
            // no trailing slash exists
            if ( ((*slash = (char *)calloc( (strLen + 2), sizeof(char))) != NULL) &&
                 ((*noSlash = (char *)calloc( (strLen + 1), sizeof(char))) != NULL) )
            {
                memcpy(*noSlash, str, strLen + 1); // include NUL terminator
                snprintf(*slash, strLen + 2, "%s\\", *noSlash);
                retVal = true;
            }
        }
    }
    return(retVal);
}

bool GetMediaType(HANDLE hDevice)
{
    DISK_GEOMETRY diskGeo;
    DWORD cbBytesReturned;
    if (DeviceIoControl(hDevice, IOCTL_DISK_GET_DRIVE_GEOMETRY,NULL, 0, &diskGeo, sizeof(diskGeo), &cbBytesReturned, NULL))
    {
        if ((diskGeo.MediaType == FixedMedia) || (diskGeo.MediaType == RemovableMedia))
        {
            return true; // Not a floppy
        }
    }
    return false;
}

// Drive-enumeration probe: used from getLogicalDrives() and WM_DEVICECHANGE
// to decide whether a volume should appear in the target combo-box. Must
// stay silent on failure — any error dialog here fires once per non-target
// drive at startup (Google Drive, OneDrive, Dokany volumes, etc.).
bool checkDriveType(char *name, ULONG *pid)
{
    HANDLE hDevice;
    PSTORAGE_DEVICE_DESCRIPTOR pDevDesc;
    DEVICE_NUMBER deviceInfo;
    bool retVal = false;
    char *nameWithSlash;
    char *nameNoSlash;
    int driveType;
    DWORD cbBytesReturned;

    // some calls require no tailing slash, some require a trailing slash...
    if ( !(slashify(name, &nameWithSlash, &nameNoSlash)) )
    {
        return(retVal);
    }

    driveType = GetDriveType(nameWithSlash);
    switch( driveType )
    {
    case DRIVE_REMOVABLE: // The media can be removed from the drive.
    case DRIVE_FIXED:     // The media cannot be removed from the drive. Some USB drives report as this.
        hDevice = CreateFile(nameNoSlash, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE)
        {
            // Probe: driver-based devices (Google Drive / OneDrive / Dokany,
            // Subst, RamDisk, ...) legitimately refuse this open with
            // ACCESS_DENIED (5) or similar. Skip silently — dialogs belong
            // in code paths that react to an explicit user action.
        }
        else
        {
            int arrSz = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512 - 1;
            pDevDesc = (PSTORAGE_DEVICE_DESCRIPTOR)new BYTE[arrSz];
            pDevDesc->Size = arrSz;

            // get the device number if the drive is
            // removable or (fixed AND on the usb bus, SD, or MMC (undefined in XP/mingw))
            if(GetMediaType(hDevice) && GetDisksProperty(hDevice, pDevDesc, &deviceInfo) &&
                    ( ((driveType == DRIVE_REMOVABLE) && (pDevDesc->BusType != BusTypeSata))
                      || ( (driveType == DRIVE_FIXED) && ((pDevDesc->BusType == BusTypeUsb)
                      || (pDevDesc->BusType == BusTypeSd ) || (pDevDesc->BusType == BusTypeMmc )) ) ) )
            {
                // ensure that the drive is actually accessible
                // multi-card hubs were reporting "removable" even when empty
                if(DeviceIoControl(hDevice, IOCTL_STORAGE_CHECK_VERIFY2, NULL, 0, NULL, 0, &cbBytesReturned, (LPOVERLAPPED) NULL))
                {
                    *pid = deviceInfo.DeviceNumber;
                    retVal = true;
                }
                else
                // IOCTL_STORAGE_CHECK_VERIFY2 fails on some devices under XP/Vista, try the other (slower) method, just in case.
                {
                    CloseHandle(hDevice);
                    hDevice = CreateFile(nameNoSlash, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
                    if(DeviceIoControl(hDevice, IOCTL_STORAGE_CHECK_VERIFY, NULL, 0, NULL, 0, &cbBytesReturned, (LPOVERLAPPED) NULL))
                    {
                        *pid = deviceInfo.DeviceNumber;
                        retVal = true;
                    }
                }
            }

            delete[] pDevDesc;
            CloseHandle(hDevice);
        }

        break;
    default:
        retVal = false;
    }

    // free the strings allocated by slashify
    free(nameWithSlash);
    free(nameNoSlash);

    return(retVal);
}
