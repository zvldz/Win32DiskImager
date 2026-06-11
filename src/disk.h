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
QString getDriveLabel(const char *drv);
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

// Write-only pre-write disk preparation. Three disposable-handle
// stages that together convince Windows the disk is raw before the
// caller opens the main write handle:
//   1) per-letter unmount + DeleteVolumeMountPointW
//   2) scratch handle: FSCTL_ALLOW_EXTENDED_DASD_IO +
//      IOCTL_DISK_DELETE_DRIVE_LAYOUT (manual first-sector zero fallback)
//   3) scratch handle: IOCTL_DISK_UPDATE_PROPERTIES +
//      IOCTL_DISK_ARE_VOLUMES_READY + SHChangeNotify(DRIVEADD)
//
// All handles are closed before this returns, so the main write
// handle (opened next via openPhysicalDiskForWrite) never inherits
// the kernel's transient "settling" state from UPDATE_PROPERTIES —
// the Win11 25H2+ regression where the first WriteFile on the same
// handle would otherwise come back with ERROR_NOT_READY.
//
// Best-effort throughout: individual IOCTL or strip failures are
// logged but never abort. After this returns, drive letters are
// gone and the partition table has been wiped — call
// openPhysicalDiskForWrite to obtain the write handle.
//
// Returns true if any drive letters were processed (i.e. Windows
// had mounted volumes on the disk). The caller uses this signal to
// decide whether to follow up with runDiskpartClean — cards Windows
// touched need the deeper VDS-level settling diskpart provides; bare
// cards (no letters) work fine on the fast path alone.
bool stripLettersAndPrepDisk(const TargetDisk &td);

// Spawns `diskpart.exe` with a `select disk N / clean / rescan`
// script. Routes through the Virtual Disk Service for synchronous
// settling on Mount Manager + Volume Manager + PnP + bus driver —
// catches the Win11 25H2+ case where IOCTL_DISK_ARE_VOLUMES_READY
// returns READY before the device hardware is actually responsive
// (RPi Imager upstream issue #1489, unfixed there).
//
// Slow (~5-10 sec), so call only when stripLettersAndPrepDisk
// reported it processed letters. Best-effort: returns true on
// diskpart success, false otherwise — caller proceeds to open and
// write regardless, with the understanding that the device is more
// likely to be ready after this returns.
bool runDiskpartClean(int diskNumber);

// Read / standalone-Verify exclusive access. For each mounted letter:
// open the volume, LOCK with 8 × geometric backoff from 100 ms,
// DISMOUNT, and APPEND the handle to `volumes`. The handles stay
// open for the entire read session — that's what holds the
// LOCK_VOLUME so Windows can't re-mount or re-cache mid-read.
//
// Does NOT call DeleteVolumeMountPointW and does NOT touch the
// partition table. After releaseVolumeLocks closes the handles,
// Windows re-mounts the volumes naturally and drive letters
// reappear — the user's card is left exactly as they inserted it.
//
// `volumes` non-empty on entry ⇒ chained call (Verify after Write
// reusing inherited handles); the function is a no-op.
//
// Best-effort: failures on individual letters are logged but don't
// abort. For a letter-less card the loop is a no-op and the read
// proceeds with whatever protection the open handle alone provides.
bool lockVolumesForRawAccess(const TargetDisk &td, QList<HANDLE> &volumes);

// Pair to lockVolumesForRawAccess. UNLOCK + CloseHandle for every
// handle in `volumes`, then clear the list. After this Windows
// re-mounts the volumes and drive letters return (since the
// underlying volume signatures haven't changed). Safe to call on
// an empty list or after a partial acquire.
void releaseVolumeLocks(QList<HANDLE> &volumes);

// Opens \\.\PhysicalDriveN for raw write access with RPi Imager's
// flag and retry pattern (file_operations_windows.cpp:259 +
// downloadthread.cpp:329). First tries direct I/O flags
// (NO_BUFFERING | WRITE_THROUGH | SEQUENTIAL_SCAN); on
// ERROR_INVALID_PARAMETER falls back to buffered I/O
// (SEQUENTIAL_SCAN only) for USB readers that misreport sector
// geometry. On transient errors (ACCESS_DENIED, SHARING_VIOLATION,
// NOT_READY) retries 8 × geometric backoff from 250 ms — gives the
// kernel time to release the disk after the prep IOCTLs on
// Win11 25H2+. Worst-case wait ≈ 32 s.
//
// Share mode is FILE_SHARE_READ | FILE_SHARE_WRITE always — matches
// RPi Imager (OpenInternal at file_operations_windows.cpp:740) and
// allows other readers (Explorer probes, antivirus) to coexist
// without blocking the open.
//
// Returns INVALID_HANDLE_VALUE on permanent failure. No UI dialog —
// caller surfaces the error in the context-appropriate way
// (Write vs chained Verify vs Read).
HANDLE openPhysicalDiskForWrite(int diskNumber);

// Enumerates \\.\PhysicalDrive0..31, filters to writable removable / USB /
// SD / MMC targets (system SATA / NVMe disks rejected), and maps each disk
// to its mounted drive letters via IOCTL_STORAGE_GET_DEVICE_NUMBER. Empty
// multi-card-reader slots are skipped (IOCTL_STORAGE_CHECK_VERIFY). Probe
// failures on individual devices are silent — virtual / driver-based
// filesystems (Google Drive / OneDrive / Dokany / subst) get skipped
// without nag dialogs.
QList<TargetDisk> enumerateTargetDisks();

#endif // DISK_H
