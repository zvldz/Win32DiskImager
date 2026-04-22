# Third-party components

The released GUI binary (`Win32DiskImager.exe`) is built with a **static**
Qt 6 toolchain, so the components listed below are linked into the exe and
are redistributed together with this project. The CLI binary
(`Win32DiskImager-cli.exe`) does not link Qt, but uses zlib and liblzma for
`.gz` / `.xz` decompression.

Direct dependencies used by the application:

| Component    | License                             | Upstream                   |
|--------------|-------------------------------------|----------------------------|
| Qt 6         | LGPL-2.1 (see `LGPL-2.1`)           | https://www.qt.io/         |
| zlib         | zlib license (permissive)           | https://zlib.net/          |
| xz / liblzma | Public domain + BSD-style (parts)   | https://tukaani.org/xz/    |

Libraries brought in transitively by static Qt at link time (each under its
own open-source license — mostly permissive, a few LGPL; see each project
for details):

libtiff, libjpeg-turbo, libpng, libwebp, libdeflate, jbigkit, lerc, zstd,
bzip2, brotli, libb2, OpenSSL, PCRE2, glib2, graphite2, HarfBuzz, FreeType,
double-conversion, md4c.

## LGPL compliance (static Qt)

Qt is distributed under LGPL-2.1. LGPL-2.1 section 6 requires that users
can relink the distributed binary against a modified version of Qt. This
project satisfies that by:

1. **Making all our source code available** — this Git repository
   (https://github.com/zvldz/Win32DiskImager) contains every `.cpp`, `.h`,
   `.pro`, `.rc` and build script we ship.
2. **Documenting the build** — `compile-gui-static.bat` +
   `.github/workflows/release.yml` describe exactly how the published
   binary is produced. The release build uses the
   `mingw-w64-x86_64-qt6-static` package from
   [MSYS2](https://www.msys2.org/); upstream Qt source is available at
   https://download.qt.io/official_releases/qt/.
3. **Bundling the LGPL-2.1 text** — the installer drops `LGPL-2.1`
   alongside the exe; it is also in the repository root.

To rebuild against a modified Qt, clone the repository and either replace
the Qt source in the MSYS2 environment or set `QT_STATIC_BIN` to your own
static Qt `bin` directory (see `compile-gui-static.bat`).

## Additional attribution

* **zlib** — "Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler";
  canonical source and license text at https://zlib.net/.
* **xz-utils** — parts by Lasse Collin and contributors, most of the code
  is public domain; see the xz-utils source tree for the full breakdown.

## Project provenance

This project is a fork of a fork. The upstream copyright notices in each
modified source file are preserved.

* **Original upstream**: Win32 Disk Imager by Justin Davis and the
  ImageWriter developers, hosted historically at
  https://sourceforge.net/projects/win32diskimager/.
* **Intermediate fork**: "Native Version" by Stefan S. (ripper121),
  which migrated the project to Qt 6 and MSYS2-based MinGW builds,
  and added native Win32 scaffolds.
* **This fork**: continues from the Native Version fork with further
  reliability, performance and usability work — see `CHANGELOG.md`.
