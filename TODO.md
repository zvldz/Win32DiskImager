# TODO

Open work items. Completed tasks are removed once shipped — see CHANGELOG.md for what landed in each release.

## Features

- **`.zst` (zstd) compression** — deferred to 2.4.0. Increasingly used in the Linux-board world (Armbian moved most images from `.xz` to `.zst`, NixOS SD images, Arch ARM); supported by Raspberry Pi Imager (since v1.7) and rufus (since v3.18), not by Balena Etcher. Decode is 15-20× faster than xz at ~90-95% of xz's compression ratio — practical sweet spot. Implementation: another `ImageReader` alongside `gzimagereader` / `xzimagereader`, ~150 lines using libzstd's streaming API (`ZSTD_DStream` / `ZSTD_decompressStream`). MSYS2 ships `mingw-w64-x86_64-zstd` static. Same magic-byte sniff in `ImageReader::open` (zstd magic = `28 B5 2F FD`). Mirror in CLI as `ZstdImageSource`. Progress: streaming decode like .xz, but zstd's frame index isn't as reliable as xz's stream index for total uncompressed size — treat as unknown-size for safety (same as our final `.gz` decision) until we can prove the frame header is always trustworthy on real-world images.
- **Reformat device** — not implemented. Brings an SD card back to a clean FAT32 layout after it's been written with a Linux / Pi image. Low priority: SDA Card Formatter and the Windows Disk Management snap-in already cover this.
