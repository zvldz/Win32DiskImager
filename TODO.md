# TODO

Open work items. Completed tasks are removed once shipped — see CHANGELOG.md for what landed in each release.

## Future tuning

- **Prep-pipeline tuning (low-priority polish).** The current Win11 25H2+ write fix combines four defences — per-letter unmount, `diskpart` subprocess settling on cards Windows had mounted, exclusive `FILE_SHARE_READ` open, and held-first-buffer write. All four currently ship together because we landed each one only after seeing the previous one wasn't enough. The same root cause is open as raspberrypi/rpi-imager#1489 in the upstream Raspberry Pi Imager (introduced when 2.0.2 switched from `diskpart` to direct IOCTLs); their maintainer has acknowledged it but not shipped a fix in mainline as of 2026-06. Open questions worth a focused session if someone feels like polishing:
  - Is the `diskpart` subprocess (which costs 15-20 sec on mounted cards) still necessary now that delayed-first-buffer is in place, or would the open-side retry alone catch the same window?
  - Can we cache "this drive number was settled X seconds ago" within a session so back-to-back writes to the same card don't re-pay the diskpart cost?
  - If upstream Pi Imager ships a fix for #1489, port the idea and see if it lets us simplify.
  The pre-squash branch (`2.3.2-pre-squash-backup`) preserves the per-fix commit chain if anyone wants to audit which defence caught which failure mode.

## Features

- **Reformat device** — not implemented. Brings an SD card back to a clean FAT32 layout after it's been written with a Linux / Pi image. Low priority: SDA Card Formatter and the Windows Disk Management snap-in already cover this.
