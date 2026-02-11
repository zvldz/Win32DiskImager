# Win32DiskImager

Win32DiskImager is a Windows utility for writing raw disk images (`.img` and related files) to removable media (SD cards, USB flash drives) and reading devices back into image files.

This repository contains:
- A Qt-based GUI application (`Win32DiskImager.exe`)
- A native Windows CLI application (`Win32DiskImager-cli.exe`)

## Important Safety Notes

- This tool performs raw writes to physical disks.
- Selecting the wrong device can overwrite data on that device.
- Use it only when you are sure the selected target is correct.
- For CLI `read`, `write`, and `verify`, run from an elevated (Administrator) terminal.

## Features

- Major improvement: Windows 11 compatibility (`WINVER`/`_WIN32_WINNT` set to `0x0A00`) and updated library/toolchain support (Qt 6 + modern MinGW builds)
- Write image file to a physical device
- Read physical device to image file
- Verify image against device
- Optional read limit in CLI (`--bytes`)
- Optional allocated-only read mode in CLI (`--allocated-only`, MBR-based)
- Image hash generation in GUI (MD5, SHA1, SHA256)
- Multi-language UI translations (`src/lang/*.ts`)

## Requirements

### Runtime

- Windows
- Administrator rights for raw disk operations

### Build Toolchain

- Qt qmake toolchain for GUI (`src/DiskImager.pro`)
- MinGW-w64 (`g++`, `mingw32-make`)
- Optional static Qt build for fully static GUI builds

The provided batch files expect a typical MSYS2 MinGW path:
- `C:\msys64\mingw64\bin`

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
1. Choose an image file.
2. Select a target device.
3. Choose `Write`, `Read`, or `Verify`.

The GUI also supports:
- Drag/drop file path input
- Hash generation and copy
- Auto-refresh of removable devices

### CLI

Usage:

```text
Win32DiskImager-cli.exe list
Win32DiskImager-cli.exe write  --device E: --image C:\path\image.img
Win32DiskImager-cli.exe read   --device E: --image C:\path\backup.img [--bytes N] [--allocated-only]
Win32DiskImager-cli.exe verify --device E: --image C:\path\image.img
```

Examples:

```bat
Win32DiskImager-cli.exe list
Win32DiskImager-cli.exe write --device E: --image C:\images\raspi.img
Win32DiskImager-cli.exe read --device E: --image C:\images\backup.img --allocated-only
Win32DiskImager-cli.exe verify --device E: --image C:\images\raspi.img
```

## Environment Variables (GUI)

- `DiskImagesDir`: default folder used by the file picker
- `DiskImagerFiles`: custom file filter list for image selection dialog (`;;` separated)

## Repository Layout

- `src/` - main GUI and CLI sources, resources, translations
- `src/DiskImager.pro` - GUI qmake project
- `src/DiskImagerCli.pro` - CLI qmake project
- `compile.bat` - GUI build helper
- `compile-cli.bat` - CLI build helper
- `compile-gui-static.bat` - static GUI build helper
- `setup.iss` - Inno Setup installer script
- `bin/` - build outputs and packaged zip artifacts

## License

- Project license: GNU GPL v2 (see `GPL-2`)
- Qt binaries/libraries used by builds may be under LGPL terms (see `LGPL-2.1`)
- Additional legal text: `License.txt`

## Historical Notes

Legacy docs are still included:
- `README.txt`
- `DEVEL.txt`
- `CHANGELOG.md` and `Changelog.txt`
