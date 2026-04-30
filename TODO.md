# TODO

Work list inherited from upstream, with current status.

## Features

- **Compression support** — ✅ done in 2.2.0. `.gz` and `.xz` on Write + Verify (GUI + CLI), streaming decode with a multi-threaded xz decoder.
- **Reformat device** — not implemented. Brings an SD card back to a clean FAT32 layout after it's been written with a Linux / Pi image. Low priority: SDA Card Formatter and the Windows Disk Management snap-in already cover this.
- **GPT support for `Read Only Allocated Partitions`** — currently MBR-only. GPT-partitioned disks are detected (protective MBR with type 0xEE) and fall back to a full disk read with an info dialog. Native GPT support would parse the GPT primary header at LBA 1, walk partition entries (typically 128 × 128 bytes at the LBA from the header), find the highest-ending partition, and read both the start of the disk through that point AND the GPT backup header + partition entries at the disk tail (so the image is byte-exact restore-able with a valid backup GPT). ~150-200 lines including CRC validation. Worth doing if GPT-partitioned SD cards become common — currently rare on the SD / USB target media this tool focuses on.
