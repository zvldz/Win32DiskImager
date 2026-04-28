# Changelog

## 2026-XX-XX

### Version 2.2.2

#### Convenience
- After a successful Write (or chained Write + Verify), the volume is auto-ejected via `IOCTL_STORAGE_EJECT_MEDIA` so the card shows as "Safely Removed" in Windows — no need to click the tray icon before pulling it. Standalone Read and standalone Verify are left untouched (the user may still want the card mounted afterwards). Eject is best-effort — silent on bus types that don't support it. Helper added as `ejectVolume()` in `disk.h`. Both success dialogs ("Write Successful", "Write & Verify Successful") now also tell the user "Card can be safely removed." in italics; the CLI prints the same line.

#### Diagnostics
- Verify-failure message now reports both the absolute sector / byte offset **and** a relative position — e.g. `Verification failed at sector 16777216 of 30523392 (55% / 8.00 GB of 14.6 GB)`. Hitting the fail right around an even GB boundary on an SD card whose advertised capacity is much larger is the classic signature of a counterfeit card with a remapped controller; the diagnostic makes that obvious at a glance. Applied symmetrically in the GUI (`mainwindow.cpp::verifyFailMessage`) and the CLI (`cli_main.cpp::cmdVerify`).

#### Verify reliability
- Verify now opens the device handle with `FILE_FLAG_NO_BUFFERING`, symmetric with the Write side. Previously the Write side bypassed the OS block cache but the Verify read went through it, so a stale page from before the just-finished Write could mask the real device data.
- When Verify is chained from a successful Write, it now issues `FlushFileBuffers` on the volume and waits 1.5 s before starting reads. SD controllers batch-flush their internal cache and FTL state to NAND lazily; without this delay, reads close to the end of the image had a higher chance of catching the controller mid-flush — the recurring "fail just before 100%" pattern. Same behavior in GUI and CLI.
- Per-chunk retry on memcmp mismatch: up to 3 re-reads with a 500 ms wait between attempts, both in GUI (re-issuing `readSectorDataFromHandle`) and CLI (seeking back via `SetFilePointerEx(FILE_CURRENT, -length)` and re-reading). Recovers from transient flakiness — typical when an SD card sits in a passive microSD→SD adapter, where SD-bus signal integrity degrades and reads occasionally come back wrong on the first try. A real, persistent mismatch still surfaces as a `Verify Failure` with the diagnostic position info above.

## 2026-04-28

### Version 2.2.1

#### GUI
- Browse dialog now opens directly in the folder of the currently selected Image File (whether the path was picked from history, dropped onto the field or typed). Native `QFileDialog` on Windows would frequently ignore `selectFile(fullPath)` and fall back to its own last-used directory; the fix splits the input into `setDirectory(folder)` + `selectFile(name)` so the dialog lands in the right place even on first open.

#### Reliability
- Long-running Write / Read / Verify operations now suppress system idle-sleep via `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)` for as long as the operation is in flight. A multi-minute SD-card write on a laptop with a 10-minute sleep timer no longer gets cut in half. Implemented as a tiny RAII guard (`src/keepawake.h`) so any exit path — success, error, cancel, exception — automatically releases the request. Applied symmetrically in the GUI (`on_b{Write,Read,Verify}_clicked`) and the CLI (`cmd{Write,Read,Verify}`).

## 2026-04-23

### Version 2.2.0

#### Compressed image input (.gz / .xz)
- New `ImageReader` interface (`src/imagereader.h`) with three implementations: `RawImageReader` (plain `.img`), `GzImageReader` (zlib via `gzopen_w` / `gzread`) and `XzImageReader` (liblzma via `lzma_stream_decoder` / `LZMA_CONCATENATED`). Factory in `imagereader.cpp` sniffs the first 6 bytes — magic bytes beat file extension, so a `.img` that's actually `.img.xz` still works.
- Write and Verify in the GUI now drive off the reader; size checks use `uncompressedSize()` from the format's metadata (xz stream index, gz ISIZE trailer). When the size cannot be determined (rare — gz > 4 GB without a credible ISIZE), the loop iterates to EOF and aborts the moment device capacity is reached, instead of silently truncating.
- The truncate pre-scan (warning that the extra bytes past the device size DOES / does not contain data) only runs for raw sources — compressed streams default to the pessimistic warning since they don't allow cheap random access.
- Progress bar has two modes: sectors-written when uncompressed size is known, compressed-bytes percentage otherwise. Either way the bar moves smoothly from 0 to 100 — same fallback RPi Imager / balenaEtcher use.
- Browse-file filter in the GUI becomes `Disk images (*.img *.iso *.gz *.xz)` by default; raw-only and compressed-only filters are also offered.
- The CLI gets a parallel, Qt-free `ImageSource` hierarchy (in `cli_main.cpp`) using the same magic-byte sniffing. `cmdWrite` and `cmdVerify` accept `.gz` / `.xz` transparently.

#### Pipelined I/O
- New `src/iopipeline.h` — a bounded (4-chunk) producer/consumer queue used by both the GUI and the CLI. A decoder thread fills the queue with `IoChunk`s; the main thread drains them to the device.
- Write and Verify in both front-ends now overlap decompression with disk I/O. On a `.xz` source, prior versions hit a ~15-16 MB/s ceiling on Write because decode and `WriteFile` ran sequentially; with the pipeline the SD card is the bottleneck again (typically 30-40+ MB/s on V30, closer to spec on faster cards). Verify gets the same speedup since the decoder runs while the main thread reads the device and runs `memcmp`.
- Cancel propagates via `ChunkQueue::requestAbort()`. The decoder thread is always `join()`ed before the operation function returns. Errors flow back to the main thread as `IoChunk::err`, never as a dead producer.

#### Auto-verify after Write
- GUI: new `Verify after Write` checkbox next to `Read Only Allocated Partitions`, **default on**. After a successful Write, control chains into `on_bVerify_clicked()` so the user gets one combined `Verify Successful` dialog. Unchecking restores the legacy "Write only, click Verify separately" flow.
- CLI: `write` runs `verify` afterwards by default — same as RPi Imager. Pass `--no-verify` to opt out. `printUsage()` documents the flag.
- Write hands the still-locked, still-dismounted volume directly to the chained Verify instead of unlocking + re-opening. Closes the tiny window where Google Drive / Windows Search / antivirus latched onto the freshly-written volume and made the Verify pass's first IOCTL fail with transient 5/55 errors. GUI uses a class-level flag (`m_verifyInheritsLock`); CLI threads the handle through `cmdWrite`'s new out-param and `cmdVerify`'s new inherit-volume param.

#### CLI polish
- Banner: `Win32DiskImager CLI X.Y.Z` printed once before any command runs (`list` / `write` / `read` / `verify`).
- `list` now filters by storage bus type, mirroring the GUI's `checkDriveType`: `DRIVE_REMOVABLE && BusType != SATA`, or `DRIVE_FIXED && BusType in {USB, SD, MMC}`. System SSDs (C:, D:) and virtual filesystems (Google Drive, OneDrive) no longer show up — they aren't valid write targets and listing them invited footguns.
- `list` now also prints device capacity per drive: `E: (removable, PhysicalDrive2, 32 GB)`. Size is queried via `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` and formatted as KB / MB / GB / TB depending on magnitude.
- Manifest changed from `requireAdministrator` to `asInvoker`. `list`, `--help` and `--version` no longer trigger UAC. `write` / `read` / `verify` still need elevation; `isRunningAsAdmin()` produces a clear "relaunch from an elevated terminal" message instead.
- Progress printer rewritten — fixed-format `%5.1f%%` percentage, 20-char ASCII bar (`[######....]`), right-justified speed, and an ETA derived from running average. Unknown-size fallback prints `Processed: NNN.NN MB  Speed: NN.N MB/s`.
- `--bytes` parsing: same as before. `--allocated-only` for `read` unchanged.

#### Misc
- GUI status-bar MB/s now formatted as `%.2f` (e.g. `85.32 MB/s`) instead of the default 6-significant-digit Qt rendering (`85.3214567`). CLI progress line is formatted the same way.
- `cli_main.cpp` direct I/O on the destination handle (`FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`) plus `_aligned_malloc` chunk buffer — same correctness fix the GUI got in 2.1.1.
- `disk.cpp`: the legacy 32-bit `SetFilePointer` replaced with `SetFilePointerEx` in `readSectorDataFromHandle` / `writeSectorDataToHandle`. Closes a latent issue for images ≥ 2 GB where the old API treated the low-part as a signed `LONG`.
- Multi-threaded xz decoder (`lzma_stream_decoder_mt`) with up to 8 threads, falling back to single-threaded when unavailable. For modern CPUs on SD targets this changes nothing (SD is the bottleneck), but on older CPUs or fast targets (UHS-II SD, USB 3.0 SSD) it prevents decode from becoming the bottleneck.
- Translations (.ts) refreshed for all 11 locales (de, es, fr, it, ja, ko, nl, pl, ta_IN, zh_CN, zh_TW): the completion-dialog strings gained rich-text HTML markup in 2.2.0 and required new translations; obsolete entries from earlier releases purged via `lupdate -no-obsolete`.

#### Status / UI feedback
- Progress bar shows the current operation inline: `Writing: 42%`, `Reading: 78%`, `Verifying: 15%`. Important now that Auto-verify-after-Write resets the bar to 0 and starts a second pass — the user no longer has to guess which phase is running.
- Status-bar line is prefixed too: `Writing: 85.32 MB/s`, `Verifying: 92.10 MB/s`. An initial `"Writing..."` / `"Reading..."` / `"Verifying..."` lands before the first MB/s tick so the user sees the operation start.
- CLI prints an explicit `Writing:` / `Reading:` / `Verifying:` header on its own line before the progress bar, and inserts a blank line between a Write and its auto-chained Verify.
- GUI: removed the fake resize grip in the bottom-right corner. The window is pinned via `setFixedSize`, but Qt still drew the diagonal grip in the status-bar and flipped the cursor to the resize arrow on hover. `sizeGripEnabled=false` on the statusbar widget kills the affordance.
- GUI: Read / Write / Verify button state now refreshes on every keystroke in the Image File combo (`QComboBox::editTextChanged`), not only on `editingFinished`. Pressing Tab right after typing a path no longer skips past buttons that were about to become enabled.
- GUI: Device dropdown entries now include capacity: `[E:\] 58GB`. Size is queried via `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` and rendered in compact binary units (no decimal, no space) so a worst-case `[X:\] 999TB` fits. `checkDriveType` was extended with an optional `sizeBytes` out-param; the combo's add/remove handlers use `Qt::MatchStartsWith` so lookup-by-drive-letter still works with the appended size. The combo reserves a 13-character minimum contents length before the window is pinned, so inserting a card at runtime (`DBT_DEVICEARRIVAL`) doesn't truncate the label.
- GUI: completion dialogs (`Write Successful`, `Read Successful`, `Verify Successful`, `Write & Verify Successful`) use rich-text HTML — bold `Elapsed:` label, and the Write + Verify + Total breakdown renders as a centered zebra-striped table (row color taken from the dialog's own palette so it reads well in both light and dark themes). A shared `showComplete()` helper centers the OK button under the dialog and moves the info icon from the content area to the title bar, so the table aligns with the dialog's actual center instead of the content column's.

#### Reliability
- `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` is now retried through a short transient-error window (10 × 200 ms). Google Drive / OneDrive / the Explorer indexer can briefly grab a freshly-inserted SD card, during which the IOCTL returns `ERROR_DEV_NOT_EXIST (55)` or `ERROR_ACCESS_DENIED (5)`. Previously the tool bailed out with `"Failed to get disk geometry"` and the user had to retry by hand. Applied to both `cli_main.cpp::getDiskGeometry` and `disk.cpp::getNumberOfSectors`.

## 2026-04-22

### Version 2.1.1

#### I/O correctness and throughput
- Direct I/O on the destination handle (hRawDisk for Write, hFile for Read) with `FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`. Matches the behavior of Raspberry Pi Imager / balenaEtcher: the Windows FS cache is bypassed, the status-bar MB/s now reflects real device throughput, and `"Done"` only appears once data has physically landed on the device — previously a sudden power loss or device removal in the post-`"Done"` flush window could leave the image corrupted.
- Sector-aligned chunk buffer via `_aligned_malloc` / `_aligned_free` (required by `NO_BUFFERING`). Replaces `new[]` / `delete[]` in `readSectorDataFromHandle` and all per-chunk free sites in `on_bRead` / `on_bWrite` / `on_bVerify` and the destructor.
- Adaptive chunk size: 4 MB target regardless of sector size. On 512-byte-sector media the per-syscall transfer grows from the legacy 512 KB (1024 × 512 B) to 4 MB; 4K-sector media keep their existing 4 MB chunk.

## 2026-04-22

### Version 2.1.0

#### Reliability
- Silenced the drive-enumeration probe in `GetDisksProperty` / `checkDriveType`. No more `"Error 122: The data area passed to a system call is too small"` popups at startup from virtual filesystems (Google Drive, OneDrive, Dokany, VeraCrypt, Subst, RamDisk, ...) that don't implement `IOCTL_STORAGE_QUERY_PROPERTY`. Failures in the probe path now return `false` silently so the caller simply skips the device; dialogs remain only on paths triggered by an explicit user action.
- Removed the dead-code fallback branch in `GetDisksProperty` (the `if (bResult && GetLastError() == ERROR_INVALID_FUNCTION)` check was logically contradictory and served only to suppress a dialog we're no longer showing).

#### GUI features
- Image File field became an editable drop-down with a 20-entry **history**. New entries are added on successful Read / Write and persisted under `HKCU\Software\Win32DiskImager\ImageFileHistory`. Pick from the list and edit the selected path as needed; drag-and-drop of files into the field still works.
- Sub-string autocomplete on the Image File drop-down (case-insensitive, `Qt::MatchContains`).
- Removed the hard 65 px `maximumSize` on the Hash combo — Qt 6.11's `qmodernwindowsstyle` draws a wider drop-arrow than 6.10 and was clipping "None".
- `DroppableLineEdit` (QLineEdit subclass) replaced with `DroppableComboBox` (editable `QComboBox` subclass) that keeps the drag-and-drop behavior.

#### Version management
- Introduced `src/version.h` as the **single source of truth**. Bumping `APP_VERSION_MAJOR` / `APP_VERSION_MINOR` / `APP_VERSION_PATCH` propagates via the C preprocessor to:
  - `DiskImager.rc` / `DiskImagerCli.rc` — `FILEVERSION`, `PRODUCTVERSION`, and the string `FileVersion` / `ProductVersion` values,
  - `main.cpp` — GUI title-bar `setApplicationDisplayName(APP_VERSION)`,
  - `cli_main.cpp` — `--version` output,
  - `setup.iss` — reads `PRODUCT_VERSION` back out of the built exe via `GetStringFileInfo`, so the installer filename and metadata track automatically.

#### Installer (setup.iss)
- Now bundles the CLI exe (`Win32DiskImager-cli.exe`) alongside the GUI.
- New optional task: **"Add Win32DiskImager-cli to system PATH"**. Appends `{app}` to machine-wide `Path`; duplicate-safe via `NeedsAddPath` check function. `ChangesEnvironment=yes` broadcasts `WM_SETTINGCHANGE` on finish.
- Default install dir: `{autopf}\Win32DiskImager` (was `{pf32}\ImageWriter`) and Start-Menu group renamed to match.
- Explicit x64-only constraints (`ArchitecturesInstallIn64BitMode=x64compatible`, `ArchitecturesAllowed=x64compatible`).
- Dropped the `Release\*.dll` / `Release\platforms\*.dll` entries — the static GUI build embeds Qt, no runtime DLLs need deploying.
- Dropped the pre-Vista Quick Launch icon entry (dead code for modern Windows).

#### Build system / CI
- New GitHub Actions workflow `.github/workflows/release.yml`. One job on `windows-latest`:
  - Sets up MSYS2 with `mingw-w64-x86_64-qt6-static` plus link-time deps (libtiff, libjpeg-turbo, libpng, openssl, pcre2, glib2, harfbuzz, freetype, brotli, ...),
  - Builds GUI against static Qt 6 and CLI (no Qt, already static MinGW runtime),
  - Reads version from `src/version.h` via `awk`,
  - Stages `Release/` and runs Inno Setup,
  - Uploads three distinct artifacts (installer, GUI zip, CLI zip) and, on `v*` tags, publishes a GitHub Release with the files attached as raw assets.
- `_detect-toolchain.bat` helper for local builds. Auto-discovers Qt and MinGW across MSYS2 (`C:\msys64\mingw64`) and Qt online-installer layouts (`C:\Qt\<ver>\mingw*_64`, `C:\Qt\Tools\mingw*_64`). Honors `QT_BIN` / `MINGW_BIN` env overrides. Selects the preferred qmake flavor (`qmake6` > `qmake` > `qmake-qt5`).
- `compile.bat`, `compile-cli.bat`, `compile-gui-static.bat` rewritten on top of the helper — no more hard-coded `C:\msys64\mingw64\bin` path.
- `.pro` translations step: added a `lrelease.exe` fallback (in addition to `lrelease-qt6.exe` / `lrelease-qt5.exe`) so MSYS2 static Qt, which ships the binary without a version suffix, picks it up.
- Added `.gitignore` entries for `bin/`, `Output/`, `artifacts/`, `.claude/`, `CLAUDE.md`. Removed tracking of upstream-shipped `bin/*.zip`.

#### Documentation
- README: documented the Image File history drop-down.

## 2026-02-11

### Version 2.0
- Bumped GUI displayed version to `2.0` and Windows file/product version to `2.0.0`.
- Bumped CLI displayed version to `2.0` and Windows file/product version to `2.0.0`.
- Updated CLI icon resource to use the same application icon as GUI (`images\Win32DiskImager.ico`).

### Windows 11 compatibility and platform updates
- Raised Windows target defines to Windows 10+ (`WINVER`/`_WIN32_WINNT` = `0x0A00`).
- Updated application manifest for modern Windows compatibility:
  - Added supported OS GUIDs.
  - Enabled modern DPI awareness (`PerMonitorV2`).
  - Enabled long path awareness.
- Removed forced desktop OpenGL and enabled high-DPI behavior for Qt startup.
- Fixed Windows device-change handling logic (`DBTF_NET` flag usage).
- Updated `nativeEvent` signatures for Qt5/Qt6 compatibility.

### Build system and toolchain
- Migrated and validated builds on MSYS2/MinGW with Qt 6.10.1.
- Refreshed bundled runtime/deployment libraries from current MSYS2 toolchain.
- Added out-of-source build layout under `bin\` to keep `src\` clean.
- Updated `compile.bat` to build GUI into `bin\gui-build` with output in `bin\`.
- Updated `compile-cli.bat` to build CLI into `bin\cli-build` with output in `bin\`.
- Updated `clean.bat` to clean/remove build outputs from `bin\*`.
- Updated project files to keep generated files out of source tree (`OBJECTS_DIR`, `MOC_DIR`, `RCC_DIR`, `UI_DIR` now target `$$OUT_PWD`).

### Standalone binaries and deployment
- Built CLI as standalone with static MinGW runtime linking (`-static -static-libgcc -static-libstdc++`).
- Added static GUI build workflow script: `compile-gui-static.bat`.
- Added static Qt build handling in `src\DiskImager.pro` (`contains(QT_CONFIG, static)` with required plugin linkage).
- Added static link-order fixes for static Qt/MSYS2 builds (`-lgraphite2`, `-lusp10`, `-lbz2`, `-lrpcrt4`).
- Added script checks for static GUI builds: requires `QT_STATIC_BIN`, auto-adds MinGW toolchain path, and fails fast if static Qt libs are not present.
- Deployed required Qt/runtime DLLs and plugins into `bin\` for dynamic GUI distribution.
- Removed non-required Qt TLS backend from deployment (`qopensslbackend.dll`) when not needed.

### CLI feature set
- Added a dedicated CLI executable:
  - `Win32DiskImager-cli.exe`
- Implemented commands:
  - `list`
  - `read`
  - `write`
  - `verify`
- Added `--allocated-only` option for `read` (MBR partition-range backup behavior).
- Reworked CLI to be pure WinAPI/C++ (removed Qt dependency from CLI target).
- Added CLI admin manifest and runtime elevation checks for raw disk operations.

### Reliability and I/O improvements
- Added lock retries for volume lock (`FSCTL_LOCK_VOLUME`) with transient error handling.
- Improved lock error guidance shown to user.
- Opened volume handles for lock operations with `GENERIC_READ | GENERIC_WRITE`.
- Switched several raw handle opens to Unicode-safe WinAPI usage (`CreateFileW` paths).
- Fixed unsafe string copy/truncation patterns in disk path handling.
- Added error handling when hash file open fails.

### UI and packaging updates
- Disabled main window resizing.
- Adjusted progress bar layout behavior in the progress group panel.
- Added source-tree cleanup workflow by removing generated binaries/artifacts from repository layout.
- Standardized final executable outputs in `bin\` (`Win32DiskImager.exe`, `Win32DiskImager-cli.exe`).



Release 1.0.0
=============
Add Verify Image function
Added SHA1 and SHA256 checksums
Read Only Allocated Partitions
Save last opened folder
Additional language translations and translation files location change
Detect and disable USB Floppy (for now)
Updated Innosetup for Windows 10 compatibility
Updated QT/MinGW for Windows 10 compatibility/bug fixes

Bug fixes:
LP: 1285238 - Need to check filename text box for valid filename (not just a directory).
LP: 1323876 - Installer doesn't create the correct permissions on install
LP: 1330125 - Multi-partition SD card only partly copied
SF:  7 - Windows 8 x64 USB floppy access denied. Possibly imaging C drive
SF:  8 - Browse Dialog doesnt open then crashes application
SF:  9 - Cannot Read SD Card
SF: 13 - 0.9.5 version refuses to open read-only image
SF: 15 - Open a image for write, bring window in the background
SF: 27 - Error1: Incorrect function
SF: 35 - Mismatch between allocating and deleting memory buffer
SF: 39 - Miswrote to SSD
SF: 40 - Disk Imager scans whole %USERPROFILE% on start
SF: 45 - Translation files adustment

Release 0.9.5
=============
Update copyright headers, bump for point release
Fixed broken translation (caused build error).
Converted build environment to QT 5.2/Mingw 4.8
Added Italian translation (LP#1270388)
Added translations.
Start work on installer with Innosetup.


Release 0.9
===========
Added custom file dialog window.

Bug fixes:
LP:1118217 - can not select SD card to write image to.
LP:1191156 - File-open dialog does not accept non-existing *.img files as targets.


Release 0.8
===========
Added drag and drop - Drag a file from Windows Explorer and drop it in the
text window.
Added DiskImagesDir environment variable for default images directory.
Improved startup Downloads directory search.
Add copy button for MD5.

Bug fixes:
LP:1080880 Add a copy button for MD5.
LP:1157849 - Partial sectors not written.
LP:1117840 Initial startup can take a long time (+20s) due to the "Downloads" find routine.
LP:1118349 Application does not start.
LP:1157849 Partial sectors not written.
SF:1 Application doesn't start. No window appears.
SF:3 No support of Russian characters in image file path
SF:5 Very long app opening time


Release 0.7
===========
Added default directory path pointing to the user's download directory.
Fixed permission settings to require administrator privileges in Vista/7
Reinstated DiskImager.rc rc file (qmake method lost too many options - may revisit later).
Make MD5Sum selectable for copying.  Fixes LP:1080880.
Add version info to the main window.
Cleanup, move winver resources to project file.
Renamed base translation for English.
More translation updates, minor cleanup.
Added translation capabilities.  Cleaned up some code formatting (broke up long lines).
Testing changes for UTF16 support.
Clean up mixed indentation types, some minor code cleanup.  no functional changes.
Updating the 'driveLabel' stuff to use QString rather than char * on a suggestion from Roland Rudenauer

Bug fixes:
Fixed LP:1100228 "An error occurred while accessing the device. Error1: Incorrect function"
Fixed LP:1102608 "Cannot load images from network shares"
Fixed LP:1095038 "Error message encoding is messed up"
Fixed LP:984510  "Cannot select Device"
Fixed LP:923719  "An error occurred while accessing the device. Error1: Incorrect function"
Fixed LP:985080  "C: with windows 7 partition erased with all data" (NOT FULLY VERIFIED)
fixing memory leak
