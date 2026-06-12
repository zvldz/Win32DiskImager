# Win32DiskImager

Win32DiskImager is a Windows utility for writing raw disk images (`.img`, `.iso`) to removable media (SD cards, USB flash drives) and reading devices back into image files. Compressed images (`.gz`, `.xz`, `.zst`) are decompressed on the fly — no need to expand to a temp file first.

This repository contains:
- A Qt-based GUI application (`Win32DiskImager.exe`)
- A native Windows CLI application (`Win32DiskImager-cli.exe`)

## Important Safety Notes

- This tool performs raw writes to physical disks.
- Selecting the wrong device can overwrite data on that device.
- Use it only when you are sure the selected target is correct.
- For CLI `read`, `write`, and `verify`, run from an elevated (Administrator) terminal.

## Features

- Write image file to a physical device, read physical device into an image file, verify image against device.
- Compressed image input (`.gz`, `.xz`, `.zst`) for Write and Verify — format detected by magic bytes, not file extension. Multi-threaded xz decoder used when available; zstd decoder is ~15× faster than xz at near-identical compression ratios (the format Armbian / NixOS / Arch ARM have moved to).
- Pipelined I/O: a decoder thread runs in parallel with the device I/O, so on `.xz` sources the SD card stays the bottleneck instead of decompression.
- Auto-verify after Write — GUI shows a single combined success dialog, CLI runs `verify` automatically (opt out with `--no-verify`).
- Direct, write-through I/O on the destination handle (`FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`) — reported MB/s reflects real device throughput, and "Write successful" only appears once data has actually landed on the device.
- System idle-sleep suppressed for the duration of Write / Read / Verify, so a multi-minute SD card operation isn't interrupted by a laptop's sleep timer.
- Device dropdown / `list` output enumerates physical disks directly (`\\.\PhysicalDrive0..31`), so **bare disks** — unformatted cards or disks Windows hasn't assigned a letter to — show up alongside the lettered ones. GUI label: `Disk 2 [E:\] 58GB` (or `Disk 2 58GB` for a bare disk). CLI `list`: `Disk 2 (PhysicalDrive2, removable, 58.2 GB) [E:]` / `... [no letter]`. Lock + dismount covers every mounted volume on the target disk, not just one.
- CLI `--device` accepts both the canonical numeric form (`--device 1`, matches `list` output, required for bare disks) and the legacy letter form (`--device E:`).
- CLI progress with ASCII bar, MB/s and ETA; GUI progress bar reflects actual sectors written even for compressed sources of known size.
- Recently-used **Image File history** in the GUI (editable drop-down, up to 20 entries; saved on successful Read / Write, persisted in `HKCU\Software\Win32DiskImager\ImageFileHistory`). Each entry has a ✕ button that removes it after confirmation.
- **Auto-update checker** in the GUI: weekly background poll against the GitHub releases API, plus an on-demand `Check for Updates...` entry in the window's system menu (right-click on the title bar). When a new version is available, the dialog offers to download `Win32DiskImager-setup-X.Y.Z.exe` and run it — Inno Setup handles whether this is an upgrade in place or a fresh install. Releases without an installer asset attached fall back to opening the GitHub release page.
- Image hash generation in the GUI (MD5, SHA1, SHA256).
- Optional read limit in CLI (`--bytes`) and allocated-only read mode (`--allocated-only`) — supports both MBR and GPT. On GPT disks the truncated image is rewritten with a self-consistent backup header + backup entry array and a patched protective MBR, so the result is a valid UEFI-compliant disk image (gdisk / fdisk / parted accept it without warnings).
- **Multi-language GUI** — 12 locales (Chinese (Simplified / Traditional), Dutch, French, German, Italian, Japanese, Korean, Polish, Spanish, Tamil, Ukrainian) baked into the binary via Qt's resource system. Qt's standard widget strings (Yes / No / OK / Cancel) are bundled alongside, so dialog buttons are localised too. Override the system locale with the `WDI_LANG` env var (see below).

## Requirements

### Runtime

- Windows
- Administrator rights for raw disk operations

### Build Toolchain

- Qt qmake toolchain for GUI (`src/DiskImager.pro`)
- MinGW-w64 (`g++`, `mingw32-make`)
- Optional static Qt build for fully static GUI builds

The provided batch files auto-detect Qt and MinGW in common locations
(MSYS2 `C:\msys64\mingw64\bin`, the Qt online installer under `C:\Qt`).
Set `QT_BIN` and / or `MINGW_BIN` to override.

## Usage

### GUI

Run `Win32DiskImager.exe`, then:
1. Choose an image file (`.img`, `.iso`, `.gz`, `.xz`, or `.zst`).
2. Select a target device — the dropdown shows each removable drive with its capacity.
3. Choose `Write`, `Read`, or `Verify`.

The GUI also supports:
- `Verify after Write` checkbox (default on) — chains a verify pass automatically after a successful write.
- Drag/drop file path input.
- Image File history drop-down (last 20 entries).
- Hash generation and copy.
- Auto-refresh of removable devices.

### CLI

Usage:

```text
Win32DiskImager-cli.exe list
Win32DiskImager-cli.exe write  --device <N|E:> --image C:\path\image.img[.gz|.xz|.zst] [--no-verify]
Win32DiskImager-cli.exe read   --device <N|E:> --image C:\path\backup.img [--bytes N] [--allocated-only]
Win32DiskImager-cli.exe verify --device <N|E:> --image C:\path\image.img[.gz|.xz|.zst]
Win32DiskImager-cli.exe check-updates
```

`--device` accepts a physical disk number (canonical, matches `list`
output — required for bare disks with no recognised filesystem / no
assigned drive letter) or a drive letter (legacy, works for any
mounted target).

`write` / `verify` accept plain `.img`, gzipped `.img.gz`,
xz-compressed `.img.xz` and zstd-compressed `.img.zst` — the format is
detected by magic bytes, not the file extension. `write` runs a verify
pass afterwards by default; pass `--no-verify` to skip it.
`check-updates` queries the GitHub releases API and prints either "You
are running the latest version" or the new tag plus a link to its
release page.

Examples:

```bat
Win32DiskImager-cli.exe list
Win32DiskImager-cli.exe write  --device 1 --image C:\images\raspi.img.xz
Win32DiskImager-cli.exe read   --device E: --image C:\images\backup.img --allocated-only
Win32DiskImager-cli.exe verify --device 1 --image C:\images\raspi.img
```

## Environment Variables (GUI)

- `DiskImagesDir`: default folder used by the file picker
- `DiskImagerFiles`: custom file filter list for image selection dialog (`;;` separated)
- `WDI_LANG`: override the system locale for the GUI's translation lookup. Useful for
  testing a non-system language or forcing English on a localised Windows. Example
  (PowerShell): `$env:WDI_LANG = "uk"; & "C:\Program Files\Win32DiskImager\Win32DiskImager.exe"`.
  **UAC caveat**: launching the exe from an unprivileged shell triggers a UAC prompt,
  and Windows replaces the inherited environment for the elevated process with a fresh
  one — a process-local `set WDI_LANG=uk` is then lost. Use `setx WDI_LANG uk` (persists
  in `HKCU\Environment`, picked up by new processes) or launch from an already-elevated
  shell so no UAC prompt fires.

## Repository Layout

- `src/` - GUI and CLI sources, resources, translations
- `src/DiskImager.pro` - GUI qmake project
- `src/DiskImagerCli.pro` - CLI qmake project
- `compile.bat` / `compile-cli.bat` / `compile-gui-static.bat` - local build helpers
- `_detect-toolchain.bat` - Qt / MinGW auto-discovery used by the above
- `setup.iss` - Inno Setup installer script
- `.github/workflows/release.yml` - CI that publishes the static GUI / CLI / installer

## Build

### GUI build

From repository root:

```bat
compile.bat
```

Output:
- `bin\Win32DiskImager.exe`

### CLI build

From repository root:

```bat
compile-cli.bat
```

Output:
- `bin\Win32DiskImager-cli.exe`

### Static GUI build (Qt static toolchain)

Set `QT_STATIC_BIN` first, then run:

```bat
set QT_STATIC_BIN=C:\Qt\6.x.x\mingw_64_static\bin
compile-gui-static.bat
```

Output:
- `bin\Win32DiskImager.exe`

## License

- Project license: GNU GPL v2 (see `GPL-2`)
- Qt is statically linked in the released GUI binary under LGPL-2.1 terms
  (see `LGPL-2.1`). Qt source is available at
  https://download.qt.io/official_releases/qt/; the release build uses the
  `mingw-w64-x86_64-qt6-static` package from MSYS2. All our source is in
  this repository so you can relink against a different Qt if desired.
- Third-party components (Qt, zlib, liblzma, libzstd, and libraries
  brought in transitively by static Qt) and the full LGPL compliance
  statement are documented in `THIRD_PARTY_LICENSES.md`.
- Additional legal text: `License.txt`

## Acknowledgments

This project is a fork of a fork:
- Original upstream: [Win32 Disk Imager](https://sourceforge.net/projects/win32diskimager/) by Justin Davis and the ImageWriter contributors (2009-2017).
- Intermediate fork: [Win32DiskImager — Native Version](https://github.com/ripper121/Win32DiskImager) by Stefan S. ([ripper121](https://github.com/ripper121)), which modernised the build for Qt 6 / MinGW and added Windows 10/11 compatibility plus several reliability fixes.
- This fork continues from the Native Version with further GUI / CLI improvements (compressed image input, pipelined I/O, auto-verify, auto-eject, auto-update checker, ...). Full provenance and license attribution: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
