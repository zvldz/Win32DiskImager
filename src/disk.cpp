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
#include <QDateTime>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QTextStream>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>          // _aligned_malloc / _aligned_free
#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>          // SHChangeNotify + SHCNE_* constants
#include "disk.h"
#include "mainwindow.h"

// Dev-only diag log: appends every lock / acquire / I/O transition
// to wdi_diag.log next to the exe so we can see exactly where a
// Write or Read on a problem card is failing. Compiled out entirely
// in release builds (no file ever created, zero call overhead).
static void diagLog(const QString &msg)
{
#ifdef WDI_DEV_BUILD
    QFile f(QCoreApplication::applicationDirPath() + "/wdi_diag.log");
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream s(&f);
        s << QDateTime::currentDateTime().toString(Qt::ISODateWithMs)
          << "  [disk] " << msg << "\n";
    }
#else
    Q_UNUSED(msg);
#endif
}

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

// Tells Explorer to drop its view of a drive letter — prevents the
// "Insert a disk" dialog while we strip the letter, and lets Explorer
// release any open handles before our subsequent IOCTLs. Matches the
// notifyShellDriveRemoved helper in RPi Imager
// (src/windows/diskpart_util.cpp).
static void notifyShellDriveRemoved(const QString &driveLetter)
{
    if (driveLetter.isEmpty()) return;
    QString drivePath = driveLetter;
    if (!drivePath.endsWith('\\')) drivePath += '\\';
    LPCWSTR pathW = reinterpret_cast<LPCWSTR>(drivePath.utf16());
    SHChangeNotify(SHCNE_MEDIAREMOVED, SHCNF_PATH, pathW, NULL);
    SHChangeNotify(SHCNE_DRIVEREMOVED, SHCNF_PATH, pathW, NULL);
}

// Tells Explorer that a freshly cleaned disk is back and may have
// new volumes. Fires after the scratch-handle prep IOCTLs settle so
// Explorer rebuilds its drive list without trying to access a
// half-torn-down partition table.
static void notifyShellDriveAdded()
{
    SHChangeNotify(SHCNE_DRIVEADD, SHCNF_IDLIST, NULL, NULL);
    SHChangeNotify(SHCNE_MEDIAINSERTED, SHCNF_IDLIST, NULL, NULL);
}

// FSCTL_LOCK_VOLUME with 8 × geometric backoff starting at 100 ms.
// No UI dialog — caller decides whether a missed lock is fatal. Matches
// the LOCK retry loop in RPi Imager (diskpart_util.cpp:126).
static bool lockVolumeBackoff(HANDLE handle, DWORD *outLastErr)
{
    DWORD junk = 0;
    DWORD lastErr = 0;
    int delayMs = 100;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (DeviceIoControl(handle, FSCTL_LOCK_VOLUME,
                            NULL, 0, NULL, 0, &junk, NULL)) {
            if (outLastErr) *outLastErr = 0;
            return true;
        }
        lastErr = GetLastError();
        if (attempt < 7) Sleep(delayMs);
        delayMs *= 2;
    }
    if (outLastErr) *outLastErr = lastErr;
    return false;
}

bool ejectVolume(HANDLE handle)
{
    DWORD junk;
    // Silent on failure: not all bus types support eject, and we don't
    // want to block the success dialog over a best-effort convenience.
    return DeviceIoControl(handle, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &junk, NULL) != 0;
}

// Write-only pre-write disk preparation. Three stages, all on
// disposable handles which are closed before the caller opens the
// main write handle. This is what keeps the eventual write handle
// out of the kernel's transient "settling" state after
// IOCTL_DISK_UPDATE_PROPERTIES (Win11 25H2+ regression: first
// WriteFile on the same handle would otherwise return ERROR_NOT_READY).
//
// Stage 1 — per-letter unmount + strip:
//   For each mounted letter: open volume → LOCK 8 × backoff →
//   DISMOUNT → UNLOCK → close → DeleteVolumeMountPointW +
//   SHChangeNotify. Letters are stripped completely so fastfat.sys
//   can't re-attach when our write stream emits a recognisable FAT
//   signature. Matches DiskpartUtil::unmountVolumes in RPi Imager
//   (src/windows/diskpart_util.cpp:69).
//
// Stage 2 — scratch handle "cleanDiskFast":
//   FSCTL_ALLOW_EXTENDED_DASD_IO + IOCTL_DISK_DELETE_DRIVE_LAYOUT
//   (manual first-sector zero fallback). Matches diskpart_util.cpp:199.
//
// Stage 3 — scratch handle "rescanDisk":
//   IOCTL_DISK_UPDATE_PROPERTIES + IOCTL_DISK_ARE_VOLUMES_READY
//   (one blocking call, Sleep(500) fallback) + SHChangeNotify(DRIVEADD).
//   Matches diskpart_util.cpp:296.
//
// Best-effort throughout — individual failures are logged but never
// abort. The combined effect of letter stripping and DELETE_LAYOUT
// makes the disk effectively raw from Windows' point of view by the
// time the caller opens the main write handle.
bool stripLettersAndPrepDisk(const TargetDisk &td)
{
    diagLog(QString("stripLettersAndPrepDisk: disk=%1 letters=[%2]")
                .arg(td.diskNumber).arg(td.letters.join(",")));

    int lettersProcessed = 0;

    // === Stage 1: per-letter unmount + DeleteVolumeMountPointW ===
    for (const QString &lt : td.letters) {
        if (lt.isEmpty()) continue;
        const int volumeIdx = lt.at(0).toUpper().toLatin1() - 'A';
        if (volumeIdx < 0 || volumeIdx > 25) continue;

        // First notify: tell Explorer the volume is going away before
        // we touch it. Helps Explorer release handles voluntarily.
        notifyShellDriveRemoved(lt);

        wchar_t volumeName[] = L"\\\\.\\A:";
        volumeName[4] = static_cast<wchar_t>('A' + volumeIdx);
        HANDLE h = CreateFileW(volumeName, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            diagLog(QString("  %1 CreateFile FAIL err=%2 (already unmounted?), still trying letter strip")
                        .arg(lt).arg(GetLastError()));
        } else {
            DWORD lockErr = 0;
            if (lockVolumeBackoff(h, &lockErr)) {
                diagLog(QString("  %1 FSCTL_LOCK_VOLUME OK").arg(lt));
            } else {
                diagLog(QString("  %1 FSCTL_LOCK_VOLUME FAIL err=%2 after 8 retries, continuing")
                            .arg(lt).arg(lockErr));
            }

            DWORD junk = 0;
            if (DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME,
                                NULL, 0, NULL, 0, &junk, NULL)) {
                diagLog(QString("  %1 FSCTL_DISMOUNT_VOLUME OK").arg(lt));
            } else {
                diagLog(QString("  %1 FSCTL_DISMOUNT_VOLUME FAIL err=%2, continuing")
                            .arg(lt).arg(GetLastError()));
            }

            // Unlock so the letter strip below doesn't fight a still-held
            // lock from a different process. We're closing the handle
            // immediately afterward so failures here don't matter.
            DeviceIoControl(h, FSCTL_UNLOCK_VOLUME,
                            NULL, 0, NULL, 0, &junk, NULL);
            CloseHandle(h);
        }

        QString mountPoint = lt.toUpper();
        if (!mountPoint.endsWith('\\')) mountPoint += '\\';
        const BOOL stripOk = DeleteVolumeMountPointW(
            reinterpret_cast<LPCWSTR>(mountPoint.utf16()));
        diagLog(QString("  DeleteVolumeMountPointW %1 = %2 err=%3")
                    .arg(mountPoint).arg(stripOk ? "OK" : "FAIL")
                    .arg(stripOk ? 0 : GetLastError()));

        // Second notify: now that the letter is gone, tell Explorer to
        // update its drive list.
        notifyShellDriveRemoved(lt);
        ++lettersProcessed;

        // Brief pause between volumes so the kernel can process the
        // dismount event before we touch the next one (RPi Imager
        // does the same — diskpart_util.cpp:182).
        Sleep(100);
    }

    diagLog(QString("  stage 1 done: %1 letters processed").arg(lettersProcessed));

    // === Stage 2: scratch handle "cleanDiskFast" — clear partition table ===
    const QString devicename =
        QStringLiteral("\\\\.\\PhysicalDrive%1").arg(td.diskNumber);
    LPCWSTR namePtr = reinterpret_cast<LPCWSTR>(devicename.utf16());

    HANDLE hScratch = CreateFileW(namePtr,
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
    if (hScratch == INVALID_HANDLE_VALUE) {
        diagLog(QString("  scratch1 CreateFile FAIL err=%1, skipping clean+rescan")
                    .arg(GetLastError()));
        // Stage 1 may still have stripped letters; reflect that so the
        // caller can decide to fall back to diskpart subprocess.
        return lettersProcessed > 0;
    }

    DWORD junk = 0;
    // FSCTL_ALLOW_EXTENDED_DASD_IO: best-effort. RPi Imager calls this
    // on every disk handle; failures are logged but not fatal.
    DeviceIoControl(hScratch, FSCTL_ALLOW_EXTENDED_DASD_IO,
                    NULL, 0, NULL, 0, &junk, NULL);

    BOOL ok = DeviceIoControl(hScratch, IOCTL_DISK_DELETE_DRIVE_LAYOUT,
                              NULL, 0, NULL, 0, &junk, NULL);
    if (ok) {
        diagLog("  IOCTL_DISK_DELETE_DRIVE_LAYOUT OK");
    } else {
        const DWORD err = GetLastError();
        // ERROR_INVALID_FUNCTION (1) / ERROR_FILE_NOT_FOUND (2): no
        // partition table to delete — fine. Other errors: fall back
        // to manually zeroing the first sector (RPi Imager does the
        // same — diskpart_util.cpp:249).
        if (err == ERROR_INVALID_FUNCTION || err == ERROR_FILE_NOT_FOUND) {
            diagLog(QString("  IOCTL_DISK_DELETE_DRIVE_LAYOUT no-op (err=%1)").arg(err));
        } else {
            diagLog(QString("  IOCTL_DISK_DELETE_DRIVE_LAYOUT FAIL err=%1, zeroing first sector manually")
                        .arg(err));
            LARGE_INTEGER zero = {};
            SetFilePointerEx(hScratch, zero, NULL, FILE_BEGIN);
            char emptyMBR[512] = {0};
            DWORD bytesWritten = 0;
            if (WriteFile(hScratch, emptyMBR, sizeof(emptyMBR),
                          &bytesWritten, NULL)
                && bytesWritten == sizeof(emptyMBR)) {
                diagLog("  manual first-sector zero OK");
            } else {
                diagLog(QString("  manual first-sector zero FAIL err=%1")
                            .arg(GetLastError()));
            }
        }
    }
    CloseHandle(hScratch);

    // === Stage 3: scratch handle "rescanDisk" — refresh OS view ===
    // Done on a separate handle so the "settling" state from
    // UPDATE_PROPERTIES + ARE_VOLUMES_READY doesn't bleed into the
    // handle the writer will subsequently use.
    hScratch = CreateFileW(namePtr,
                           GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (hScratch == INVALID_HANDLE_VALUE) {
        diagLog(QString("  scratch2 CreateFile FAIL err=%1, skipping rescan")
                    .arg(GetLastError()));
        notifyShellDriveAdded();
        return lettersProcessed > 0;
    }

    if (DeviceIoControl(hScratch, IOCTL_DISK_UPDATE_PROPERTIES,
                        NULL, 0, NULL, 0, &junk, NULL)) {
        diagLog("  IOCTL_DISK_UPDATE_PROPERTIES OK");
    } else {
        diagLog(QString("  IOCTL_DISK_UPDATE_PROPERTIES FAIL err=%1, continuing")
                    .arg(GetLastError()));
    }

    // IOCTL_DISK_ARE_VOLUMES_READY (Win8+) — ONE blocking call. The
    // IOCTL itself blocks until volumes finish coming online or being
    // torn down. RPi Imager doesn't poll it (diskpart_util.cpp:352);
    // we used to, which doubled the wait without helping. Falls back
    // to a fixed 500 ms sleep on older Windows that doesn't ship it.
#ifndef IOCTL_DISK_ARE_VOLUMES_READY
#define IOCTL_DISK_ARE_VOLUMES_READY CTL_CODE(IOCTL_DISK_BASE, 0x0087, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif
    if (DeviceIoControl(hScratch, IOCTL_DISK_ARE_VOLUMES_READY,
                        NULL, 0, NULL, 0, &junk, NULL)) {
        diagLog("  IOCTL_DISK_ARE_VOLUMES_READY OK");
    } else {
        diagLog(QString("  IOCTL_DISK_ARE_VOLUMES_READY FAIL err=%1, sleeping 500ms")
                    .arg(GetLastError()));
        Sleep(500);
    }
    CloseHandle(hScratch);

    // Tell Explorer the disk is back with a fresh layout. After this,
    // the re-scan in the caller sees zero letters for the cleaned
    // disk — which is the expected state before opening the main
    // write handle.
    notifyShellDriveAdded();
    diagLog(QString("stripLettersAndPrepDisk: done (returning hadLetters=%1)")
                .arg(lettersProcessed > 0));
    return lettersProcessed > 0;
}

// Spawns `diskpart.exe` and runs "select disk N / clean / rescan".
//
// Why we need this on top of stripLettersAndPrepDisk: direct IOCTLs
// (DELETE_DRIVE_LAYOUT + UPDATE_PROPERTIES + ARE_VOLUMES_READY) don't
// wait deep enough on cards that Windows had mounted recently — on
// Win11 25H2+ the kernel returns ARE_VOLUMES_READY before the SD
// controller / USB bus has finished its post-dismount activity, so
// the first WriteFile comes back with ERROR_NOT_READY. RPi Imager
// upstream issue #1489 documents this regression (open since switch
// to direct IOCTLs in 2.0.2, no fix in mainline yet).
//
// diskpart routes through the Virtual Disk Service (vds.exe), which
// synchronously waits for Mount Manager + Volume Manager + PnP +
// bus driver settlement — a deeper handshake than user-mode IOCTLs
// expose. Both OpenHD-ImageWriter (fork of older RPi Imager) and
// balenaEtcher use this same diskpart subprocess approach.
//
// Costs ~5-10 seconds per call, so we only invoke it as a follow-up
// to stripLettersAndPrepDisk when that function reported it processed
// any letters (≈ Windows had mounted volumes ≈ controller may be in
// "in-flight" state). Letter-less cards (fresh radxa) skip this and
// stay on the fast path.
//
// Returns true on diskpart exit code 0, false otherwise. Best-effort —
// failure is logged but the caller proceeds to open and write anyway.
bool runDiskpartClean(int diskNumber)
{
    diagLog(QString("runDiskpartClean: disk=%1 (VDS-level settling via diskpart subprocess)")
                .arg(diskNumber));

    QProcess proc;
    proc.setProgram("diskpart");
    proc.start();
    if (!proc.waitForStarted(5000)) {
        diagLog(QString("  diskpart failed to start: %1").arg(proc.errorString()));
        return false;
    }

    const QString script =
        QStringLiteral("select disk %1\r\nclean\r\nrescan\r\nexit\r\n").arg(diskNumber);
    proc.write(script.toLatin1());
    proc.closeWriteChannel();

    if (!proc.waitForFinished(60000)) {
        diagLog("  diskpart timed out after 60s, killing");
        proc.kill();
        proc.waitForFinished(2000);
        return false;
    }

    const int rc = proc.exitCode();
    diagLog(QString("  diskpart exit code=%1 (elapsed inferred from log timestamps)").arg(rc));
    return rc == 0;
}

// Read/Verify-side exclusive access. For each mounted letter on the
// disk: open the volume, LOCK with 8 × geometric backoff, DISMOUNT,
// and APPEND the handle to `volumes`. The handle stays open for the
// duration of the operation — that's what holds the LOCK_VOLUME and
// prevents Windows from re-mounting / re-caching mid-read.
//
// Unlike stripLettersAndPrepDisk, this does NOT call
// DeleteVolumeMountPointW and does NOT touch the partition table.
// After releaseVolumeLocks closes the handles, Windows re-mounts the
// volumes naturally — drive letters return without user action
// (the underlying volume signatures haven't changed, so mountmgr
// reassigns the same letters).
//
// Best-effort: failures on individual letters are logged and the
// remaining letters are still processed. For a freshly-inserted card
// with no recognisable FS (radxa GPT without protective MBR) the
// letter list may be empty and this becomes a no-op — that's fine,
// there's nothing for Windows to write to anyway.
//
// `volumes` non-empty on entry means a chained call (e.g. Verify
// after Write reusing inherited handles); skip entirely.
bool lockVolumesForRawAccess(const TargetDisk &td, QList<HANDLE> &volumes)
{
    diagLog(QString("lockVolumesForRawAccess: disk=%1 letters=[%2] volumes-on-entry=%3")
                .arg(td.diskNumber).arg(td.letters.join(",")).arg(volumes.size()));

    if (!volumes.isEmpty()) {
        diagLog("  chained call (volumes non-empty), skipping");
        return true;
    }

    for (const QString &lt : td.letters) {
        if (lt.isEmpty()) continue;
        const int volumeIdx = lt.at(0).toUpper().toLatin1() - 'A';
        if (volumeIdx < 0 || volumeIdx > 25) continue;

        wchar_t volumeName[] = L"\\\\.\\A:";
        volumeName[4] = static_cast<wchar_t>('A' + volumeIdx);
        HANDLE h = CreateFileW(volumeName, GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) {
            diagLog(QString("  %1 CreateFile FAIL err=%2, continuing")
                        .arg(lt).arg(GetLastError()));
            continue;
        }

        DWORD lockErr = 0;
        if (lockVolumeBackoff(h, &lockErr)) {
            diagLog(QString("  %1 FSCTL_LOCK_VOLUME OK").arg(lt));
        } else {
            diagLog(QString("  %1 FSCTL_LOCK_VOLUME FAIL err=%2 after 8 retries, continuing")
                        .arg(lt).arg(lockErr));
        }

        DWORD junk = 0;
        if (DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME,
                            NULL, 0, NULL, 0, &junk, NULL)) {
            diagLog(QString("  %1 FSCTL_DISMOUNT_VOLUME OK").arg(lt));
        } else {
            diagLog(QString("  %1 FSCTL_DISMOUNT_VOLUME FAIL err=%2, continuing")
                        .arg(lt).arg(GetLastError()));
        }

        volumes.append(h);
        diagLog(QString("  %1 handle kept (lock held for read session)").arg(lt));
    }

    diagLog(QString("  lockVolumesForRawAccess END: volumes.size=%1")
                .arg(volumes.size()));
    return true;
}

// Pair to lockVolumesForRawAccess. UNLOCK + close each volume handle,
// then clear the list. After this, Windows re-mounts the volumes and
// drive letters reappear (since the underlying data hasn't changed).
// Safe to call on an empty list or after a partial acquire.
void releaseVolumeLocks(QList<HANDLE> &volumes)
{
    diagLog(QString("releaseVolumeLocks: volumes.size=%1").arg(volumes.size()));
    for (HANDLE h : volumes) {
        if (h != INVALID_HANDLE_VALUE) {
            DWORD junk = 0;
            DeviceIoControl(h, FSCTL_UNLOCK_VOLUME,
                            NULL, 0, NULL, 0, &junk, NULL);
            CloseHandle(h);
        }
    }
    volumes.clear();
}

// Opens \\.\PhysicalDriveN for raw write access. Two preference
// axes, tried in priority order on every retry iteration:
//
// Share-mode preference (OpenHD-ImageWriter pattern, winfile.cpp:33):
//   primary:   FILE_SHARE_READ only — EXCLUSIVE write access. While
//              we hold the handle, mountmgr / fastfat.sys can't open
//              it for write, so they can't auto-mount partitions
//              that appear in our write stream mid-operation. This
//              is what prevents the err=5 ACCESS_DENIED race we
//              were hitting around offset ~60 MB on cards Windows
//              actively monitors (e.g. after diskpart rescan).
//   fallback:  FILE_SHARE_READ | FILE_SHARE_WRITE — if exclusive
//              gives SHARING_VIOLATION (rare: something else opened
//              the disk with permissive share before us).
//
// I/O-flag preference (RPi Imager pattern):
//   primary:   NO_BUFFERING | WRITE_THROUGH | SEQUENTIAL_SCAN —
//              direct I/O so MB/s reflects real device throughput
//              and "Done" only after data lands.
//   fallback:  SEQUENTIAL_SCAN — buffered, for USB readers that
//              reject direct I/O with ERROR_INVALID_PARAMETER.
//
// Modern RPi Imager mainline uses permissive sharing + RPi flag set
// and has issue #1489 (Win11 25H2+ failure). Combining OpenHD's
// share-mode strategy with RPi's flag strategy gives us both
// honest MB/s and protection from the mountmgr auto-mount race.
//
// On transient errors (ACCESS_DENIED, NOT_READY) — not counting
// SHARING_VIOLATION, which signals "try permissive share next" —
// retries 8 × geometric backoff from 250 ms (~32 s worst-case).
//
// Returns INVALID_HANDLE_VALUE on permanent failure. No UI dialog —
// caller surfaces the error in operation-appropriate way.
HANDLE openPhysicalDiskForWrite(int diskNumber)
{
    const QString devicename =
        QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    LPCWSTR namePtr = reinterpret_cast<LPCWSTR>(devicename.utf16());
    const DWORD exclusiveShare  = FILE_SHARE_READ;
    const DWORD permissiveShare = FILE_SHARE_READ | FILE_SHARE_WRITE;
    const DWORD primaryFlags =
        FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH | FILE_FLAG_SEQUENTIAL_SCAN;
    const DWORD fallbackFlags = FILE_FLAG_SEQUENTIAL_SCAN;

    auto tryOpen = [&](DWORD share, DWORD flags) -> HANDLE {
        return CreateFileW(namePtr, GENERIC_READ | GENERIC_WRITE,
                           share, NULL, OPEN_EXISTING, flags, NULL);
    };

    DWORD delayMs = 250;
    DWORD lastErr = 0;

    for (int attempt = 0; attempt < 8; ++attempt) {
        // 1) Exclusive share + direct I/O — best case
        HANDLE h = tryOpen(exclusiveShare, primaryFlags);
        if (h != INVALID_HANDLE_VALUE) {
            diagLog(QString("openPhysicalDiskForWrite OK exclusive+direct (attempt %1)")
                        .arg(attempt));
            return h;
        }
        lastErr = GetLastError();

        // 2) Exclusive share + buffered I/O — device rejects direct I/O
        if (lastErr == ERROR_INVALID_PARAMETER) {
            h = tryOpen(exclusiveShare, fallbackFlags);
            if (h != INVALID_HANDLE_VALUE) {
                diagLog(QString("openPhysicalDiskForWrite OK exclusive+buffered (attempt %1)")
                            .arg(attempt));
                return h;
            }
            lastErr = GetLastError();
        }

        // 3) Permissive share + direct I/O — exclusive lost
        if (lastErr == ERROR_SHARING_VIOLATION) {
            diagLog(QString("openPhysicalDiskForWrite exclusive denied (attempt %1), relaxing share")
                        .arg(attempt));
            h = tryOpen(permissiveShare, primaryFlags);
            if (h != INVALID_HANDLE_VALUE) {
                diagLog(QString("openPhysicalDiskForWrite OK permissive+direct (attempt %1) — mountmgr race risk")
                            .arg(attempt));
                return h;
            }
            lastErr = GetLastError();

            // 4) Permissive share + buffered I/O — last resort
            if (lastErr == ERROR_INVALID_PARAMETER) {
                h = tryOpen(permissiveShare, fallbackFlags);
                if (h != INVALID_HANDLE_VALUE) {
                    diagLog(QString("openPhysicalDiskForWrite OK permissive+buffered (attempt %1)")
                                .arg(attempt));
                    return h;
                }
                lastErr = GetLastError();
            }
        }

        // Transient errors that the kernel may clear within seconds.
        // SHARING_VIOLATION here means even permissive share failed —
        // that's an actual conflict worth waiting on.
        const bool transient = (lastErr == ERROR_ACCESS_DENIED
                             || lastErr == ERROR_SHARING_VIOLATION
                             || lastErr == ERROR_NOT_READY);
        if (!transient) {
            diagLog(QString("openPhysicalDiskForWrite FAIL non-transient err=%1 attempt=%2")
                        .arg(lastErr).arg(attempt));
            return INVALID_HANDLE_VALUE;
        }

        diagLog(QString("openPhysicalDiskForWrite transient err=%1 attempt=%2 sleep=%3ms")
                    .arg(lastErr).arg(attempt).arg(delayMs));
        if (attempt < 7) Sleep(delayMs);
        delayMs *= 2;
    }

    diagLog(QString("openPhysicalDiskForWrite FAIL after 8 retries err=%1")
                .arg(lastErr));
    return INVALID_HANDLE_VALUE;
}

// Sector-aligned buffer: FILE_FLAG_NO_BUFFERING on the device/file handle
// requires both the buffer address and transfer size to be a multiple of the
// sector size. Freeing must go through _aligned_free (see callers).
char *readSectorDataFromHandle(HANDLE handle, unsigned long long startsector, unsigned long long numsectors, unsigned long long sectorsize)
{
    unsigned long bytesread = 0;
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
    // Geometric backoff symmetric to the WriteFile retry — same RPi
    // Imager pattern for the same Win11 25H2+ reason.
    BOOL bResult = FALSE;
    DWORD lastErr = 0;
    int retries = 0;
    DWORD backoffMs = 250;
    for (int attempt = 0; attempt < 8; ++attempt) {
        bResult = ReadFile(handle, data, (DWORD)(sectorsize * numsectors), &bytesread, NULL);
        if (bResult) break;
        lastErr = GetLastError();
        if (lastErr != ERROR_ACCESS_DENIED
         && lastErr != ERROR_NOT_READY
         && lastErr != ERROR_SHARING_VIOLATION
         && lastErr != ERROR_LOCK_VIOLATION
         && lastErr != ERROR_DEV_NOT_EXIST) {
            break;
        }
        diagLog(QString("ReadFile retry offset=%1 size=%2 attempt=%3 err=%4 sleep=%5ms")
                    .arg(li.QuadPart).arg(sectorsize * numsectors).arg(attempt).arg(lastErr).arg(backoffMs));
        retries++;
        if (attempt < 7) Sleep(backoffMs);
        backoffMs *= 2;
        SetFilePointerEx(handle, li, nullptr, FILE_BEGIN);
    }
    if (!bResult)
    {
        diagLog(QString("ReadFile FAIL offset=%1 size=%2 retries=%3 err=%4")
                    .arg(li.QuadPart).arg(sectorsize * numsectors).arg(retries).arg(lastErr));
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, lastErr, 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Read Error"),
                              QObject::tr("An error occurred when attempting to read data from handle.\n"
                                          "Error %1: %2").arg(lastErr).arg(errText));
        LocalFree(errormessage);
        _aligned_free(data);
        data = NULL;
    } else if (retries > 0) {
        diagLog(QString("ReadFile OK after %1 retries offset=%2").arg(retries).arg(li.QuadPart));
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
    // Retry on transient ERROR_ACCESS_DENIED. Even with FSCTL_LOCK_VOLUME
    // on the physical disk Windows occasionally grabs it for a moment to
    // update internal FS / partition state — particularly noticeable on
    // cards with multiple mountable partitions where one was dismounted
    // and another wasn't. WriteFile then comes back with Access Denied
    // mid-image even though we own the disk. A short retry rides over
    // those windows; non-Access-Denied errors are fatal as before.
    // Geometric backoff matching RPi Imager's pattern (src/downloadthread.cpp
    // line 333-364): 8 attempts × 250 ms × 2^n = ~32 s worst-case total.
    // Win11 25H2+ holds disk handles longer than older Windows after a
    // partition-table change; the previous 5 × 500 ms = 2.5 s wasn't
    // long enough on multi-FAT cards where mountmgr clings to the disk
    // for several seconds after our IOCTL_DISK_DELETE_DRIVE_LAYOUT.
    BOOL bResult = FALSE;
    DWORD lastErr = 0;
    int retries = 0;
    DWORD backoffMs = 250;
    for (int attempt = 0; attempt < 8; ++attempt) {
        bResult = WriteFile(handle, data, (DWORD)(sectorsize * numsectors), &byteswritten, NULL);
        if (bResult) break;
        lastErr = GetLastError();
        if (lastErr != ERROR_ACCESS_DENIED       // 5
         && lastErr != ERROR_NOT_READY            // 21 — driver still settling after layout change
         && lastErr != ERROR_SHARING_VIOLATION    // 32
         && lastErr != ERROR_LOCK_VIOLATION       // 33
         && lastErr != ERROR_DEV_NOT_EXIST) {     // 55
            break;
        }
        diagLog(QString("WriteFile retry offset=%1 size=%2 attempt=%3 err=%4 sleep=%5ms")
                    .arg(li.QuadPart).arg(sectorsize * numsectors).arg(attempt).arg(lastErr).arg(backoffMs));
        retries++;
        if (attempt < 7) Sleep(backoffMs);
        backoffMs *= 2;
        // WriteFile leaves the file pointer undefined on failure — re-seek
        // to the start of this chunk before retrying.
        SetFilePointerEx(handle, li, nullptr, FILE_BEGIN);
    }
    if (!bResult)
    {
        diagLog(QString("WriteFile FAIL offset=%1 size=%2 retries=%3 err=%4")
                    .arg(li.QuadPart).arg(sectorsize * numsectors).arg(retries).arg(lastErr));
        wchar_t *errormessage=NULL;
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, NULL, lastErr, 0, (LPWSTR)&errormessage, 0, NULL);
        QString errText = QString::fromUtf16((const char16_t *)errormessage);
        QMessageBox::critical(MainWindow::getInstanceIfAvailable(), QObject::tr("Write Error"),
                              QObject::tr("An error occurred when attempting to write data to handle.\n"
                                          "Error %1: %2").arg(lastErr).arg(errText));
        LocalFree(errormessage);
    } else if (retries > 0) {
        diagLog(QString("WriteFile OK after %1 retries offset=%2").arg(retries).arg(li.QuadPart));
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
        const QStringList letters = letterMap.value(n);

        // Filter logic:
        //  * For disks with mounted letters, defer to GetDriveTypeW per
        //    letter. Some SD card readers (internal laptop slots, some
        //    USB hubs) report RemovableMedia=FALSE and BusType=Unknown /
        //    RAID at the bus level, even though Windows correctly
        //    classifies the inserted card as DRIVE_REMOVABLE at the FS
        //    layer. The bus-level descriptor lies often enough that the
        //    FS classification is the more reliable source when we have
        //    a letter to ask about.
        //  * For bare disks (no letter — unformatted card, no FS Windows
        //    recognises) we only have the bus descriptor to go on:
        //    USB / SD / MMC always accepted, removable accepted unless
        //    on the SATA bus, system SATA / NVMe rejected.
        bool accept = false;
        if (!letters.isEmpty()) {
            for (const QString &lt : letters) {
                wchar_t root[] = L"X:\\";
                root[0] = static_cast<wchar_t>(lt.at(0).toUpper().toLatin1());
                const UINT dt = GetDriveTypeW(root);
                if ((dt == DRIVE_REMOVABLE && busType != BusTypeSata)
                    || (dt == DRIVE_FIXED
                        && (busType == BusTypeUsb
                            || busType == BusTypeSd
                            || busType == BusTypeMmc))) {
                    accept = true;
                    break;
                }
            }
        } else {
            accept =
                (busType == BusTypeUsb || busType == BusTypeSd || busType == BusTypeMmc)
                || (removable && busType != BusTypeSata);
        }
        if (!accept) {
            CloseHandle(hDisk);
            continue;
        }

        // Hub-slot media check: a multi-card reader exposes one PhysicalDrive
        // per slot regardless of insertion. CHECK_VERIFY2 is the
        // FILE_ANY_ACCESS variant — the strict CHECK_VERIFY requires
        // FILE_READ_ACCESS on the handle, which our 0-access probe handle
        // does not have, so it would fail on every disk with ERROR_ACCESS_DENIED.
#ifndef IOCTL_STORAGE_CHECK_VERIFY2
#define IOCTL_STORAGE_CHECK_VERIFY2 CTL_CODE(IOCTL_STORAGE_BASE, 0x0200, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
        if (!DeviceIoControl(hDisk, IOCTL_STORAGE_CHECK_VERIFY2,
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
        td.letters    = letters;
        out.append(td);
    }
    return out;
}
