# TODO

Open work items. Completed tasks are removed once shipped — see CHANGELOG.md for what landed in each release.

## Future tuning

- **Prep-pipeline tuning (low-priority polish).** The current Win11 25H2+ write fix combines four defences — per-letter unmount, `diskpart` subprocess settling on cards Windows had mounted, exclusive `FILE_SHARE_READ` open, and held-first-buffer write. All four currently ship together because we landed each one only after seeing the previous one wasn't enough. The same root cause is open as raspberrypi/rpi-imager#1489 in the upstream Raspberry Pi Imager (introduced when 2.0.2 switched from `diskpart` to direct IOCTLs); their maintainer has acknowledged it but not shipped a fix in mainline as of 2026-06. Open questions worth a focused session if someone feels like polishing:
  - **Caching is the promising one.** Remember "this drive number was settled X seconds ago" within a session, invalidated by `WM_DEVICECHANGE`, so back-to-back Write → Verify → re-write cycles on the same card don't re-pay the diskpart cost. Cheap, and it targets the case where the wait is most irritating.
  - Dropping `diskpart` outright looks unlikely to work. The open-side retry (8 × geometric backoff from 250 ms in `openPhysicalDiskForWrite`) and the `ERROR_NOT_READY` retry both predate the diskpart fallback — they were already in the tree when it turned out they weren't enough. And `delayFirstBuffer`, which landed after, addresses a different failure mode: it stops mountmgr auto-mounting mid-write, but the first physical `WriteFile` still lands early (at offset = chunk), so it does nothing for `ERROR_NOT_READY`.
  - First step either way: `runDiskpartClean` has no timer — its log line says "elapsed inferred from log timestamps". Add a `QElapsedTimer` and log real milliseconds, split between vds.exe startup and `rescan`, so the tuning has numbers behind it.
  - If upstream Pi Imager ships a fix for #1489, port the idea and see if it lets us simplify.
  The pre-squash branch (`2.3.2-pre-squash-backup`) preserves the per-fix commit chain if anyone wants to audit which defence caught which failure mode.

## Features

- **Reformat device** — not implemented. Brings an SD card back to a clean FAT32 layout after it's been written with a Linux / Pi image. Low priority: SDA Card Formatter and the Windows Disk Management snap-in already cover this.
