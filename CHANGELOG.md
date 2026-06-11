# Changelog

## 2026-XX-XX

### Version 2.4.0

#### Image formats
- **`.zst` (zstd) compression support** — write and verify directly from a zstd-compressed image. Adopted by Armbian, NixOS SD images, and Arch ARM, and increasingly common across the Linux-board world for its ~15× faster decode than `.xz` at near-identical compression. Magic-byte detected like `.gz` / `.xz`, no extension required. Progress + ETA derive from a GPT/MBR peek of the decompressed stream front (falls back to compressed-bytes pacing for raw images without a partition table). Both GUI and CLI.

#### GUI
- Read / Write / Verify buttons stay disabled while an operation is in progress. Previously editing the Image File field (or picking another history entry) re-enabled them mid-operation.

#### Reliability
- Write, Read, and Verify now work reliably on cards Windows had previously mounted — multi-FAT layouts from OpenHD / Raspberry Pi OS / radxa images, or anything with Windows-recognisable partitions. These operations could previously fail with "Access Denied" or "Device not ready" mid-write, or finish but fail Verify, on Windows 11 25H2+. Cards that Windows hasn't touched (fresh radxa / bare cards without a recognised filesystem) keep the fast path.
- **`.gz` images ≥ 4 GB now write the full image** instead of silently stopping at 2 GB / 0 bytes. The gzip trailer carries the uncompressed size mod 2^32, and we were trusting it — so a 6 GB radxa Debian image (ISIZE wraps to 2 GB) finished Write + Verify in seconds but Linux refused to boot because the rootfs tail was missing. Now `.gz` is always streamed to EOF (same approach Balena Etcher / Pi Imager take). Progress switches to compressed-bytes percentage for `.gz`; `.xz` is unaffected — its 64-bit stream index is reliable for any size.

#### Read Only Allocated Partitions
- Now also works on GPT-partitioned disks (radxa, OpenHD, anything that boots a Linux board with a GPT layout). Previously these fell back to a full-disk read with an "MBR-only" notice — typical 6 GB image on a 32 GB card produced a 32 GB raw file before compression. The output image is now a **self-consistent truncated disk image** per UEFI spec: a normalised GPT primary header, the partition data, and a freshly built backup GPT at the tail — so tools like `gdisk -l image.img`, `parted`, or loop-mounting see a clean, complete image without warnings about a missing backup. After re-flashing to a larger card the standard distro first-boot resize (`sgdisk -e` / `growpart`) still extends the GPT to fill the new media. Both GUI and CLI (`--allocated-only`).

#### Progress / ETA
- Time-remaining estimate during `.gz` Write and Verify is now correct. Previously the ETA extrapolated against full disk capacity — a 1-minute write of a 6 GB `.gz` into a 64 GB card displayed "11 minutes remaining" throughout. Now ETA paces off compressed bytes consumed, the same metric the progress bar uses for unknown-size sources, and converges within a few seconds of the start.

#### Resource usage
- xz decoder memory now scales with available RAM (~25% of free physical, clamped 256 MB – 4 GB). High-end machines still spin up the full multi-threaded decoder for maximum throughput; low-RAM machines no longer swap-thrash on `xz -9` images. Override with the `WDI_XZ_MEMLIMIT_MB` env var. Applies to both GUI and CLI.

## 2026-05-27

### Version 2.3.1

#### Reliability
- Fix occasional "Device Error 55: The specified network resource or device is no longer available" at the start of Write / Read / Verify on some SD card readers. The reader briefly resets when its volume is dismounted; the operation now survives that and proceeds without needing the user to retry.

#### GUI
- Image File history dropdown ✕ button is now bolder, larger, and has a hover highlight — easier to see and to click. Click area now reliably hits even on entries with long paths.

## 2026-05-15

### Version 2.3.0

#### Device selection
- The Device dropdown (GUI) and CLI `list` now show **bare disks** — cards with no recognised filesystem or no drive letter assigned by Windows finally show up and can be written, read, or verified. Empty multi-card-reader slots are hidden.
- New label format: `Disk N [E:\, F:\] 32GB` (or `Disk N 32GB` for bare disks). Multi-letter cards show every letter in the dropdown; long labels elide gracefully.
- Multi-partition cards now lock every volume on the target disk during raw operations — sibling partitions can no longer be touched by Google Drive / Windows Search / antivirus during a Write or Verify.
- Inserting a bare disk now refreshes the dropdown automatically (previously only formatted volumes triggered a refresh).

#### CLI
- `--device` accepts both `--device 1` (matches `list` output) and `--device E:` (legacy). The numeric form is required for bare disks.
- `list` output now reads `Disk N (PhysicalDriveN, removable, 32 GB) [E:, F:]` with a usage hint at the bottom.

#### Translations
- **Ukrainian** added (127 strings) — picks up automatically on a Ukrainian-locale Windows.
- Long-standing untranslated strings filled across `fr`, `pl`, and `ta_IN` — the Tamil locale had been shipping with most of the GUI in English since the upstream import.
- Standard Qt button labels (Yes / No / OK / Cancel, file-dialog labels) are now translated too — previously they stayed English regardless of locale. All 11 locales except Tamil (Qt itself doesn't ship a Tamil pack).
- All translation files are now baked into `Win32DiskImager.exe`. The portable zip becomes a true single-file deployment (drop the .exe anywhere and translations work); the installed directory loses the `translations\` subfolder.
- New `WDI_LANG` environment variable overrides the system locale for translation lookup. Use `setx WDI_LANG uk` to force a specific language regardless of the Windows display language (a plain `set WDI_LANG=uk` in the current shell is dropped by UAC when admin elevation kicks in).

#### Updates
- Dev builds (CI artifacts not from a release tag) now report themselves as `2.3.0-dev` instead of `2.3.0`. The update checker recognises a released `2.3.0` as newer than `2.3.0-dev` and offers the update — previously a dev build silently treated "same numbers" as "already up to date".

## 2026-05-01

### Version 2.2.5

Test release. Verifies that the auto-update flow shipped in 2.2.4 correctly downloads and runs the installer.

## 2026-05-01

### Version 2.2.4

#### GUI
- Auto-update now downloads and runs the installer in one step. Confirming the "Update available" dialog streams the installer to `%TEMP%`, hands off, and the app quits so the installer can replace its exe in place. If a release ships without an installer asset, the dialog falls back to opening the GitHub release page.

## 2026-05-01

### Version 2.2.3

#### CLI
- New `check-updates` command (also `--check-updates`) — prints either "You are running the latest version" or the new tag plus a link to the GitHub release page. Doesn't auto-run on any other command, so scripted invocations stay silent and offline.

## 2026-05-01

### Version 2.2.2

#### GUI
- Image File history dropdown gets a small ✕ on the right side of every entry. Click it to remove the entry from the history (with confirmation).
- Auto-update checker. Available three ways:
  - Right-click on the title bar → **Check for Updates...**
  - On startup, at most once per 7 days, the app checks silently and stays quiet unless a new version exists.
  - When a new version is found, the dialog offers an in-app download + installer hand-off (installer deployment) or opens the GitHub release page (portable / zip deployment). The CLI is unaffected.

#### Convenience
- After a successful Write (or chained Write + Verify), the card is auto-ejected — shows as "Safely Removed" in Windows so you can pull it right away. Standalone Read and standalone Verify leave the card mounted as before. Success dialogs add a "Card can be safely removed." line; the CLI prints the same.

#### Diagnostics
- Verify-failure message now shows both the absolute sector and a relative position — e.g. *"Verification failed at sector 16777216 of 30523392 (55% / 8.00 GB of 14.6 GB)"*. A failure right at an even-GB boundary on a card whose advertised capacity is much larger is the classic signature of a counterfeit card; the percentage makes that visible at a glance.

#### Read Only Allocated Partitions
- Falls back gracefully on devices without a recognisable MBR (raw cards) or with a GPT layout — info dialog (GUI) / stderr notice (CLI), then a full disk read. Previously these produced a malformed image. (GPT got native support in 2.3.2; the fallback path still handles unrecognised cards.)

#### Verify reliability
- Verify now bypasses the Windows FS cache (same as Write), so the read can't be served from a stale page that was cached before the just-finished Write.
- Verify-after-Write now flushes the volume and waits 1.5 s before starting reads. SD controllers batch-flush their internal state to NAND lazily; without this delay the recurring "fails just before 100%" pattern showed up.
- Per-chunk retry on a verify-mismatch: up to 3 re-reads with 500 ms wait. Recovers from transient flakiness typical when a card sits in a passive microSD → SD adapter with degraded SD-bus signal integrity. A real persistent mismatch still surfaces as a Verify Failure with the diagnostic above.

## 2026-04-28

### Version 2.2.1

#### GUI
- Browse dialog now opens directly in the folder of the currently selected Image File — whether the path was picked from history, dropped onto the field, or typed. Previously Windows always landed in its own last-used directory.

#### Reliability
- Long-running Write / Read / Verify no longer get interrupted by system sleep. The app suppresses idle-sleep for the duration of the operation, so a multi-minute SD-card write on a laptop with a 10-minute sleep timer finishes instead of being cut short.

## 2026-04-23

### Version 2.2.0

#### Compressed images (.gz / .xz)
- Write and Verify accept `.gz` and `.xz` images directly — no need to manually unpack first. Format is detected by magic bytes, so a `.img` that's actually `.img.xz` still works. Both GUI and CLI.
- Browse-file filter defaults to `Disk images (*.img *.iso *.gz *.xz)` with raw-only / compressed-only filters available.

#### Throughput
- Decompression overlaps with disk I/O: previously `.xz` Write capped at ~15-16 MB/s because decode and write ran sequentially; now the card is the bottleneck again (typically 30-40+ MB/s on V30 cards, closer to spec on faster ones). Verify gets the same speedup.

#### Auto-verify after Write
- New **Verify after Write** checkbox in the GUI (default on). After a successful Write the operation chains into Verify automatically and shows one combined success dialog. Uncheck to keep the legacy two-click flow.
- CLI: `write` runs `verify` afterwards by default (same as Raspberry Pi Imager). Pass `--no-verify` to opt out.
- The chain hands the locked, dismounted card directly to Verify without re-opening, closing a small window where Google Drive / Windows Search / antivirus could grab the freshly written card and cause Verify to start with a transient error.

#### CLI
- Banner with version printed once before each command.
- `list` now filters out system disks (C:\, D:) and virtual filesystems (Google Drive, OneDrive). Only valid write targets show up.
- `list` prints capacity per drive: `E: (removable, PhysicalDrive2, 32 GB)`.
- `list`, `--help`, `--version` no longer trigger UAC; only `write` / `read` / `verify` still need elevation. The elevation message now explicitly says "relaunch from an elevated terminal".
- Progress printer redrawn — fixed-width percentage + ASCII bar, MB/s, and ETA.

#### Status / UI feedback
- Progress bar shows the current phase inline: `Writing: 42%`, `Reading: 78%`, `Verifying: 15%`. Useful when chained Write + Verify resets the bar to 0 and starts a second pass.
- Status bar shows `Writing: 85.32 MB/s` / `Verifying: 92.10 MB/s` instead of just the number, and "..." appears before the first speed sample so the user sees the operation start.
- CLI prints an explicit `Writing:` / `Reading:` / `Verifying:` header before each progress bar.
- Device dropdown entries now include capacity: `[E:\] 58GB`.
- Removed the fake resize-grip in the bottom-right corner — the window can't be resized and the grip was misleading.
- Read / Write / Verify buttons re-evaluate on every keystroke in the Image File field — pressing Tab right after typing no longer skips past buttons that were about to become enabled.
- Completion dialogs now render the Write / Verify / Total elapsed-time table as a centered zebra-striped block, with bold labels. Reads cleanly in both light and dark Windows themes.

#### Reliability
- Disk-geometry probe is now retried through a short transient-error window. Previously a freshly-inserted card briefly held by Google Drive / OneDrive / the Explorer indexer bailed out the operation; now it just rides through.

#### Misc
- MB/s in the status bar formatted as `85.32` instead of `85.3214567`. CLI matches.
- Internal: 32-bit file pointer replaced with 64-bit — closes a latent issue with images ≥ 2 GB.

## 2026-04-22

### Version 2.1.1

#### Reliability
- Write / Read no longer rely on the Windows FS cache. "Done" only appears once data has physically landed on the device — previously pulling the card or losing power in the brief flush window after "Done" could leave the image corrupted. The status-bar MB/s now reflects real device throughput instead of cache speed.

#### Throughput
- Larger per-write chunks on standard 512-byte-sector media (4 MB instead of 512 KB). Faster cards see noticeable speedup; SD cards limited by their controller are unchanged.

## 2026-04-22

### Version 2.1.0

#### GUI
- Image File field is now an editable drop-down with a 20-entry history. New entries are added on successful Read / Write and persist across sessions; pick from the list, edit the selected path as needed. Drag-and-drop of files into the field still works. Sub-string autocomplete on the dropdown (case-insensitive).
- Hash type combo ("None / MD5 / SHA1 / SHA256") no longer clips "None" on Windows 11 (Qt 6.11 drew a wider drop-arrow than the fixed-size combo could hold).

#### Reliability
- No more "Error 122: The data area passed to a system call is too small" popups at startup. Virtual filesystems (Google Drive, OneDrive, Dokany, VeraCrypt, Subst, RamDisk, ...) that don't implement the storage probe IOCTL are now silently skipped during drive enumeration instead of showing a dialog.

#### Installer
- Now bundles the CLI executable (`Win32DiskImager-cli.exe`) alongside the GUI.
- New optional setup task: **"Add Win32DiskImager-cli to system PATH"**. With this checked, you can run `Win32DiskImager-cli` from any command prompt; safe to re-run an upgrade (no duplicate PATH entries).
- Default install dir renamed to `Program Files\Win32DiskImager` (was `Program Files (x86)\ImageWriter`); Start-Menu group renamed to match.

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
