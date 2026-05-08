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
// Issues IOCTL_STORAGE_EJECT_MEDIA on a volume that's already locked +
// dismounted. Used after a successful Write so the user can pull the
// card immediately (the volume shows as "Safely Removed" in Windows).
// Silent on failure — eject is best-effort, never blocking.
bool ejectVolume(HANDLE handle);
char *readSectorDataFromHandle(HANDLE handle, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize);
bool writeSectorDataToHandle(HANDLE handle, char *data, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize);
unsigned long long getNumberOfSectors(HANDLE handle, unsigned long long *sectorsize);
unsigned long long getFileSizeInSectors(HANDLE handle, unsigned long long sectorsize);
bool spaceAvailable(char *location, unsigned long long spaceneeded);

// Snapshot of one writable physical disk: \\.\PhysicalDriveN identity,
// total capacity, and the list of mounted drive letters (empty for a
// bare disk that Windows did not assign a letter — unformatted card,
// foreign partition layout, manually unmounted volume).
struct TargetDisk
{
    DWORD       diskNumber  = 0;
    quint64     sizeBytes   = 0;
    QStringList letters;        // entries like "E:", possibly empty
};

// Enumerates \\.\PhysicalDrive0..31, filters to writable removable / USB /
// SD / MMC targets (system SATA / NVMe disks rejected), and maps each disk
// to its mounted drive letters via IOCTL_STORAGE_GET_DEVICE_NUMBER. Empty
// multi-card-reader slots are skipped (IOCTL_STORAGE_CHECK_VERIFY). Probe
// failures on individual devices are silent — virtual / driver-based
// filesystems (Google Drive / OneDrive / Dokany / subst) get skipped
// without nag dialogs.
QList<TargetDisk> enumerateTargetDisks();

#endif // DISK_H
