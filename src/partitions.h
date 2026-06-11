/**********************************************************************
 *  This program is free software; you can redistribute it and/or     *
 *  modify it under the terms of the GNU General Public License       *
 *  as published by the Free Software Foundation; either version 2    *
 *  of the License, or (at your option) any later version.            *
 *                                                                    *
 *  This program is distributed in the hope that it will be useful,   *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of    *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the     *
 *  GNU General Public License for more details.                      *
 *                                                                    *
 *  You should have received a copy of the GNU General Public License *
 *  along with this program; if not, see http://gnu.org/licenses/     *
 **********************************************************************/

#ifndef PARTITIONS_H
#define PARTITIONS_H

#include <cstdint>

// Pure parsers for MBR and GPT partition tables. No I/O, no Windows /
// Qt dependencies — callers read the raw sectors themselves and pass
// buffers in. Used by `Read Only Allocated Partitions` (GUI + CLI) to
// determine where the last allocated partition ends, so the rest of
// the disk (often a huge empty tail on freshly imaged cards) can be
// skipped on backup.
//
// Typical caller flow for a disk of unknown layout:
//   1) read sector 0, call scanMbrAllocated()
//      - Ok               → use maxEndingLba, done
//      - GptProtectiveMbr → standard GPT, go to step 2
//      - NoMbrSignature   → try GPT anyway (Linux images such as radxa
//                           ship sector 0 all zeros, no protective MBR)
//   2) read sector 1 (GPT primary header), call readGptEntryArrayLocation()
//      - false → invalid header, show fallback to caller
//      - true  → use returned (entriesLba, numEntries, entrySize) to
//                read partition-entry array and step 3
//   3) read the entry array bytes, call scanGptEntries()
//      - Ok            → use maxEndingLba, done
//      - other         → show fallback to caller
//
// All CRC32 checks use the IEEE 802.3 polynomial (zlib::crc32), as
// required by the UEFI spec.

enum class PartitionScanResult {
    Ok,                  // maxEndingLba populated, caller should use it
    NoMbrSignature,      // 0x55AA missing at sector 0 — try GPT next
                         // (Linux images such as radxa have sector 0 all zeros)
    GptProtectiveMbr,    // sector 0 carries a type 0xEE entry — try GPT next
    InvalidGptEntries,   // GPT partition-entry array CRC32 mismatch
    NoPartitions,        // GPT valid but all entries empty (zero PartitionTypeGUID)
};

// MBR-only scan over sector 0. Walks the four primary partition entries
// at 0x1BE..0x1FD and returns the largest start+count LBA in
// `maxEndingLba` when the disk is plain MBR. Returns NoMbrSignature
// when 0x55AA is missing (caller should try GPT) and GptProtectiveMbr
// when any primary entry carries type 0xEE (also a hint to try GPT).
PartitionScanResult scanMbrAllocated(const uint8_t *sector0,
                                     uint32_t sectorSize,
                                     uint64_t &maxEndingLba);

// Validates the GPT primary header at LBA 1: "EFI PART" signature plus
// header CRC32 computed over its first `HeaderSize` bytes (with the
// CRC32 field zeroed during the calc, per UEFI spec). On success fills
// out the location and CRC32 of the partition-entry array so the
// caller knows how many bytes to read next and can pass the expected
// CRC into scanGptEntries(). Returns false on signature / header CRC
// mismatch.
bool readGptEntryArrayLocation(const uint8_t *headerSector,
                               uint32_t sectorSize,
                               uint64_t &partitionEntryLba,
                               uint32_t &numEntries,
                               uint32_t &entrySize,
                               uint32_t &entriesCrc32);

// Validates the partition-entry array's CRC32 against the value
// carried in the header (passed in as `expectedEntriesCrc32`), then
// walks every entry and returns the largest EndingLBA across entries
// with a non-zero PartitionTypeGUID in `maxEndingLba`.
PartitionScanResult scanGptEntries(const uint8_t *entriesBuf,
                                   uint64_t entriesBufBytes,
                                   uint32_t numEntries,
                                   uint32_t entrySize,
                                   uint32_t expectedEntriesCrc32,
                                   uint64_t &maxEndingLba);

// Best-effort total image size estimate by parsing the first ~17 KiB
// of a raw image (typically the decompressed head of a .gz). Used to
// give the writer a reliable known-size for progress / ETA when the
// gzip ISIZE trailer can't be trusted (wraps at 4 GiB).
//
// Order of preference:
//   1. GPT primary header at LBA 1 — AlternateLBA field gives the
//      exact disk size in sectors; this is reliable for any image
//      dd'd from a GPT-partitioned disk (radxa, modern Pi OS, etc.)
//   2. MBR at LBA 0 — max(start + count) across the four primary
//      entries; lower bound only, doesn't account for padding or
//      trailing zero space after the last partition.
// Returns 0 when neither is parseable.
uint64_t estimateImageSizeBytes(const uint8_t *bootSectors,
                                uint64_t available,
                                uint32_t sectorSize);

// Normalize a GPT primary header in-place so a truncated allocated-
// only image is self-consistent. `maxEndingLba` = sector count of
// allocated partition data (output of scanGptEntries); `entrySectors`
// = ceil(numEntries × entrySize / sectorSize), i.e. how many sectors
// the backup partition entries occupy. The new image is sized
// (maxEndingLba + entrySectors + 1) sectors.
//
// Modifies in `header`:
//   AlternateLBA  ← maxEndingLba + entrySectors   (backup header LBA)
//   LastUsableLBA ← maxEndingLba - 1              (last data LBA)
//   Header CRC32  ← recomputed
//
// Returns false on signature / HeaderSize sanity failures, on
// entrySectors == 0, or when maxEndingLba < 34. On false the buffer
// is left untouched.
bool normalizeGptPrimaryHeader(uint8_t *header,
                               uint64_t maxEndingLba,
                               uint32_t entrySectors);

// Build a GPT backup header from a (normalized) primary header.
// Writes exactly HeaderSize bytes into `backup`; the caller is
// responsible for placing it inside a sector-sized buffer (zero-
// padded) and writing it at LBA (maxEndingLba + entrySectors) of
// the file. `entrySectors` matches the value passed to
// normalizeGptPrimaryHeader.
//
// The backup header has:
//   MyLBA              ← maxEndingLba + entrySectors
//   AlternateLBA       ← 1                  (primary header LBA)
//   PartitionEntryLBA  ← maxEndingLba       (backup entries position)
//   Header CRC32       ← recomputed
//
// All other fields (including PartitionEntryArrayCRC32) are inherited
// from the primary, since the backup entries are a byte-identical
// copy of the primary entries.
bool buildGptBackupHeader(uint8_t *backup, const uint8_t *primary,
                          uint64_t maxEndingLba,
                          uint32_t entrySectors);

#endif // PARTITIONS_H
