# TODO

Work list inherited from upstream, with current status.

## Features

- **Compression support** — ✅ done in 2.2.0. `.gz` and `.xz` on Write + Verify (GUI + CLI), streaming decode with a multi-threaded xz decoder.
- **Reformat device** — not implemented. Brings an SD card back to a clean FAT32 layout after it's been written with a Linux / Pi image. Low priority: SDA Card Formatter and the Windows Disk Management snap-in already cover this.
