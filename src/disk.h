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

#ifndef DISK_H
#define DISK_H

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include <QtWidgets>
#include <QString>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <winioctl.h>
#ifndef FSCTL_IS_VOLUME_MOUNTED
#define FSCTL_IS_VOLUME_MOUNTED  CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 10, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif // FSCTL_IS_VOLUME_MOUNTED

typedef struct _DEVICE_NUMBER
{
    DEVICE_TYPE  DeviceType;
    ULONG  DeviceNumber;
    ULONG  PartitionNumber;
} DEVICE_NUMBER, *PDEVICE_NUMBER;

// IOCTL control code
#define IOCTL_STORAGE_QUERY_PROPERTY   CTL_CODE(IOCTL_STORAGE_BASE, 0x0500, METHOD_BUFFERED, FILE_ANY_ACCESS)

// extraFlags is OR'd into CreateFileW's dwFlagsAndAttributes. Pass
// FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH on the destination handle
// of a Read/Write operation to bypass the Windows FS cache — this makes
// reported throughput match reality and guarantees data is on the device
// before the operation returns (required for correctness on sudden power
// loss / device yank). Buffers used with NO_BUFFERING must be sector-aligned
// (see readSectorDataFromHandle).
HANDLE getHandleOnFile(LPCWSTR filelocation, DWORD access, DWORD extraFlags = 0);
HANDLE getHandleOnDevice(int device, DWORD access, DWORD extraFlags = 0);
HANDLE getHandleOnVolume(int volume, DWORD access, DWORD extraFlags = 0);
QString getDriveLabel(const char *drv);
DWORD getDeviceID(HANDLE handle);
bool getLockOnVolume(HANDLE handle);
bool removeLockOnVolume(HANDLE handle);
bool unmountVolume(HANDLE handle);
bool isVolumeUnmounted(HANDLE handle);
char *readSectorDataFromHandle(HANDLE handle, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize);
bool writeSectorDataToHandle(HANDLE handle, char *data, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize);
unsigned long long getNumberOfSectors(HANDLE handle, unsigned long long *sectorsize);
unsigned long long getFileSizeInSectors(HANDLE handle, unsigned long long sectorsize);
bool spaceAvailable(char *location, unsigned long long spaceneeded);
// If sizeBytes is non-null and the probe succeeds, it receives the total
// device capacity in bytes (taken from IOCTL_DISK_GET_LENGTH_INFO on the
// volume handle). Callers that don't want the size can pass nullptr.
bool checkDriveType(char *name, ULONG *pid, unsigned long long *sizeBytes = nullptr);

#endif // DISK_H
