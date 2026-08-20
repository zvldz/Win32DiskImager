# TODO

Open work items. Completed tasks are removed once shipped — see CHANGELOG.md for what landed in each release.

## Future tuning

- **Prep-pipeline tuning (low-priority polish).** The current Win11 25H2+ write fix combines five defences — per-letter unmount, `diskpart` subprocess settling on cards Windows had mounted, exclusive `FILE_SHARE_READ` open, whole-disk `FSCTL_LOCK_VOLUME`, and the held first buffer (which since 2.4.1 stays unwritten through the whole verify pass, not just the write). They all ship together because each one landed only after the previous turned out not to be enough. The same root cause is open as raspberrypi/rpi-imager#1489 in the upstream Raspberry Pi Imager (introduced when 2.0.2 switched from `diskpart` to direct IOCTLs); their maintainer has acknowledged it but not shipped a fix in mainline as of 2026-06. Open questions worth a focused session if someone feels like polishing:
  - **Caching is the promising one.** Remember "this drive number was settled X seconds ago" within a session, invalidated by `WM_DEVICECHANGE`, so back-to-back Write → Verify → re-write cycles on the same card don't re-pay the diskpart cost. Cheap, and it targets the case where the wait is most irritating.
  - Dropping `diskpart` outright looks unlikely to work. The open-side retry (8 × geometric backoff from 250 ms in `openPhysicalDiskForWrite`) and the `ERROR_NOT_READY` retry both predate the diskpart fallback — they were already in the tree when it turned out they weren't enough. And `delayFirstBuffer`, which landed after, addresses a different failure mode: it stops mountmgr auto-mounting mid-write, but the first physical `WriteFile` still lands early (at offset = chunk), so it does nothing for `ERROR_NOT_READY`.
  - First step either way: `runDiskpartClean` has no timer — its log line says "elapsed inferred from log timestamps". Add a `QElapsedTimer` and log real milliseconds, split between vds.exe startup and `rescan`, so the tuning has numbers behind it.
  - If upstream Pi Imager ships a fix for #1489, port the idea and see if it lets us simplify.
  The pre-squash branch (`2.3.2-pre-squash-backup`) preserves the per-fix commit chain if anyone wants to audit which defence caught which failure mode.

- **Find out what SD Card Formatter actually does.** It formats a card cleanly in about a second, including its "full format" mode, where our path takes 10-15 seconds whenever anything goes sideways — and it clearly does not follow the rufus / RPi Imager / Etcher recipe that ours is modelled on. The tool is closed-source, so the way to learn this is to run it under Process Monitor or API Monitor and record the real call sequence: whether it touches `IOCTL_DISK_SET_DRIVE_LAYOUT_EX` at all, whether it goes through `FormatEx` in fmifs.dll or `IVdsService`, how it handles a card that has no drive letter, and what it does *instead* of dismount-and-settle.
  Worth doing because the findings may not stop at Format: our whole pre-write preparation is inherited from RPi Imager, whose own maintainers have the unresolved #1489 for the same root cause. If SD Card Formatter gets exclusive access without the dance we perform, Write could get faster too.

## Features

- **Format device — done in 2.4.2, one gap left.** The button lays down a single MBR partition spanning the card and formats it (FAT32 up to 32 GB, exFAT above). Verified on 8 GB and 15 GB cards; **the exFAT branch has never actually run** — no card larger than 32 GB has been through it. Also untested straight after writing a Linux image, where the card carries GPT and several partitions.
