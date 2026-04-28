# Win32DiskImager

Win32DiskImager is a Windows utility for writing raw disk images (`.img`, `.iso`) to removable media (SD cards, USB flash drives) and reading devices back into image files. Compressed images (`.gz`, `.xz`) are decompressed on the fly — no need to expand to a temp file first.

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
- Compressed image input (`.gz`, `.xz`) for Write and Verify — format detected by magic bytes, not file extension. Multi-threaded xz decoder used when available.
- Pipelined I/O: a decoder thread runs in parallel with the device I/O, so on `.xz` sources the SD card stays the bottleneck instead of decompression.
- Auto-verify after Write — GUI shows a single combined success dialog, CLI runs `verify` automatically (opt out with `--no-verify`).
- Direct, write-through I/O on the destination handle (`FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH`) — reported MB/s reflects real device throughput, and "Write successful" only appears once data has actually landed on the device.
- System idle-sleep suppressed for the duration of Write / Read / Verify, so a multi-minute SD card operation isn't interrupted by a laptop's sleep timer.
- Device dropdown / `list` output shows each removable target with its capacity (`[E:\] 58GB` / `E: (removable, PhysicalDrive2, 29.3 GB)`).
- CLI progress with ASCII bar, MB/s and ETA; GUI progress bar reflects actual sectors written even for compressed sources of known size.
- Recently-used **Image File history** in the GUI (editable drop-down, up to 20 entries; saved on successful Read / Write, persisted in `HKCU\Software\Win32DiskImager\ImageFileHistory`).
- Image hash generation in the GUI (MD5, SHA1, SHA256).
- Optional read limit in CLI (`--bytes`) and allocated-only read mode (`--allocated-only`, MBR-based).
- Multi-language UI translations (`src/lang/*.ts`).

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

## Usage

### GUI

Run `Win32DiskImager.exe`, then:
1. Choose an image file (`.img`, `.iso`, `.gz`, or `.xz`).
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
Win32DiskImager-cli.exe write  --device E: --image C:\path\image.img[.gz|.xz] [--no-verify]
Win32DiskImager-cli.exe read   --device E: --image C:\path\backup.img [--bytes N] [--allocated-only]
Win32DiskImager-cli.exe verify --device E: --image C:\path\image.img[.gz|.xz]
```

`write` / `verify` accept plain `.img`, gzipped `.img.gz` and
xz-compressed `.img.xz` — the format is detected by magic bytes, not the
file extension. `write` runs a verify pass afterwards by default; pass
`--no-verify` to skip it.

Examples:

```bat
Win32DiskImager-cli.exe list
Win32DiskImager-cli.exe write  --device E: --image C:\images\raspi.img.xz
Win32DiskImager-cli.exe read   --device E: --image C:\images\backup.img --allocated-only
Win32DiskImager-cli.exe verify --device E: --image C:\images\raspi.img
```

## Environment Variables (GUI)

- `DiskImagesDir`: default folder used by the file picker
- `DiskImagerFiles`: custom file filter list for image selection dialog (`;;` separated)

## Repository Layout

- `src/` - GUI and CLI sources, resources, translations
- `src/DiskImager.pro` - GUI qmake project
- `src/DiskImagerCli.pro` - CLI qmake project
- `compile.bat` / `compile-cli.bat` / `compile-gui-static.bat` - local build helpers
- `_detect-toolchain.bat` - Qt / MinGW auto-discovery used by the above
- `setup.iss` - Inno Setup installer script
- `.github/workflows/release.yml` - CI that publishes the static GUI / CLI / installer

## License

- Project license: GNU GPL v2 (see `GPL-2`)
- Qt is statically linked in the released GUI binary under LGPL-2.1 terms
  (see `LGPL-2.1`). Qt source is available at
  https://download.qt.io/official_releases/qt/; the release build uses the
  `mingw-w64-x86_64-qt6-static` package from MSYS2. All our source is in
  this repository so you can relink against a different Qt if desired.
- Third-party components (Qt, zlib, liblzma, and libraries brought in
  transitively by static Qt) and the full LGPL compliance statement are
  documented in `THIRD_PARTY_LICENSES.md`.
- Additional legal text: `License.txt`
