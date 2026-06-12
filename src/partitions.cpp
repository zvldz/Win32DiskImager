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

#include "partitions.h"

#include <cstring>
#include <vector>
#include <zlib.h>

// All multi-byte fields in MBR / GPT structures are little-endian on
// disk per Intel convention; the UEFI spec requires LE for GPT too.

static uint32_t readLe32(const uint8_t *p)
{
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t readLe64(const uint8_t *p)
{
    return  (uint64_t)readLe32(p)
         | ((uint64_t)readLe32(p + 4) << 32);
}

static void writeLe32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v        & 0xFFu);
    p[1] = (uint8_t)((v >> 8)  & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void writeLe64(uint8_t *p, uint64_t v)
{
    writeLe32(p,     (uint32_t)(v          & 0xFFFFFFFFULL));
    writeLe32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFULL));
}

// Semantic of maxEndingLba returned by both scanners:
//   one past the last LBA that is part of any allocated partition.
// I.e. the number of sectors the caller needs to read from offset 0 to
// cover every allocated partition. For MBR this is max(start + count);
// for GPT this is max(EndingLBA + 1) since GPT EndingLBA is inclusive.

PartitionScanResult scanMbrAllocated(const uint8_t *sector0,
                                     uint32_t sectorSize,
                                     uint64_t &maxEndingLba)
{
    maxEndingLba = 0;
    if (sectorSize < 512 || sector0 == nullptr) {
        return PartitionScanResult::NoMbrSignature;
    }
    // 0x55AA at the end of the boot sector is the standard MBR signature.
    if (sector0[0x1FE] != 0x55 || sector0[0x1FF] != 0xAA) {
        return PartitionScanResult::NoMbrSignature;
    }
    // A type 0xEE primary entry is the GPT protective MBR — caller
    // should re-try via the GPT path.
    for (int i = 0; i < 4; ++i) {
        if (sector0[0x1BE + 16 * i + 4] == 0xEE) {
            return PartitionScanResult::GptProtectiveMbr;
        }
    }
    // Walk the four primary partition entries.
    for (int i = 0; i < 4; ++i) {
        const int base = 0x1BE + 16 * i;
        const uint32_t start = readLe32(sector0 + base + 8);
        const uint32_t count = readLe32(sector0 + base + 12);
        const uint64_t end = (uint64_t)start + (uint64_t)count;
        if (end > maxEndingLba) {
            maxEndingLba = end;
        }
    }
    return PartitionScanResult::Ok;
}

bool readGptEntryArrayLocation(const uint8_t *headerSector,
                               uint32_t sectorSize,
                               uint64_t &partitionEntryLba,
                               uint32_t &numEntries,
                               uint32_t &entrySize,
                               uint32_t &entriesCrc32)
{
    if (headerSector == nullptr || sectorSize < 92) {
        return false;
    }
    // Signature "EFI PART" at offset 0.
    if (std::memcmp(headerSector, "EFI PART", 8) != 0) {
        return false;
    }
    const uint32_t headerSize = readLe32(headerSector + 12);
    if (headerSize < 92 || headerSize > sectorSize) {
        return false;
    }
    const uint32_t storedCrc = readLe32(headerSector + 16);

    // UEFI spec: CRC32 is computed over the first HeaderSize bytes of
    // the header with the HeaderCRC32 field itself zeroed. Copy out so
    // we don't mutate the caller's buffer.
    std::vector<uint8_t> buf(headerSize);
    std::memcpy(buf.data(), headerSector, headerSize);
    std::memset(buf.data() + 16, 0, 4);
    const uint32_t computedCrc =
        (uint32_t)crc32(0L, buf.data(), (uInt)headerSize);
    if (computedCrc != storedCrc) {
        return false;
    }

    partitionEntryLba = readLe64(headerSector + 72);
    numEntries        = readLe32(headerSector + 80);
    entrySize         = readLe32(headerSector + 84);
    entriesCrc32      = readLe32(headerSector + 88);

    // UEFI requires SizeOfPartitionEntry to be a power of two >= 128.
    // Sanity-cap NumberOfPartitionEntries × SizeOfPartitionEntry at
    // 16 MiB so a corrupted header can't ask us to read gigabytes.
    if (entrySize < 128 || (entrySize & (entrySize - 1)) != 0) {
        return false;
    }
    if ((uint64_t)numEntries * entrySize > 16ULL * 1024 * 1024) {
        return false;
    }
    return true;
}

uint64_t estimateImageSizeBytes(const uint8_t *bootSectors,
                                uint64_t available,
                                uint32_t sectorSize)
{
    if (bootSectors == nullptr || sectorSize == 0) return 0;
    if (available < (uint64_t)sectorSize * 2) return 0;
    // Try GPT primary header at LBA 1.
    const uint8_t *gptHdr = bootSectors + sectorSize;
    if (std::memcmp(gptHdr, "EFI PART", 8) == 0) {
        uint64_t entriesLba = 0;
        uint32_t numEntries = 0, entrySize = 0, entriesCrc32 = 0;
        if (readGptEntryArrayLocation(gptHdr, sectorSize,
                                      entriesLba, numEntries,
                                      entrySize, entriesCrc32)) {
            // Primary preference: max ending LBA across allocated
            // partition entries. AlternateLBA in the header points to
            // the backup GPT, i.e. disk_size - 1 — which equals the
            // image size only for factory images. For images that were
            // produced by a Read-allocated-only pass on a card whose
            // GPT had been rewritten to match physical capacity
            // (Linux first-boot resize / sgdisk -e / VDS auto-repair),
            // AlternateLBA over-reports by up to 4x. Partition entries
            // are not touched by any of those resize paths, so they're
            // the reliable signal for "where the data ends".
            const uint64_t entriesByteOffset =
                entriesLba * (uint64_t)sectorSize;
            const uint64_t entriesByteSize =
                (uint64_t)numEntries * (uint64_t)entrySize;
            if (entriesByteOffset + entriesByteSize <= available) {
                uint64_t maxEndingLba = 0;
                PartitionScanResult res = scanGptEntries(
                    bootSectors + entriesByteOffset, entriesByteSize,
                    numEntries, entrySize, entriesCrc32, maxEndingLba);
                if (res == PartitionScanResult::Ok && maxEndingLba > 0) {
                    return (maxEndingLba + 1ULL) * (uint64_t)sectorSize;
                }
            }
            // Fallback: AlternateLBA = disk_size - 1. Used when the
            // entries don't fit in the peek buffer or fail CRC32 —
            // off by at most the size of the backup GPT (~16 KB) for
            // factory images.
            const uint64_t altLba = readLe64(gptHdr + 32);
            if (altLba > 0) {
                return (altLba + 1ULL) * (uint64_t)sectorSize;
            }
        }
    }
    // Fall back to MBR — max(start + count) across primary entries.
    uint64_t maxLba = 0;
    PartitionScanResult mbr = scanMbrAllocated(bootSectors, sectorSize, maxLba);
    if (mbr == PartitionScanResult::Ok && maxLba > 0) {
        return maxLba * (uint64_t)sectorSize;
    }
    return 0;
}

PartitionScanResult scanGptEntries(const uint8_t *entriesBuf,
                                   uint64_t entriesBufBytes,
                                   uint32_t numEntries,
                                   uint32_t entrySize,
                                   uint32_t expectedEntriesCrc32,
                                   uint64_t &maxEndingLba)
{
    maxEndingLba = 0;
    if (entriesBuf == nullptr || numEntries == 0 || entrySize == 0) {
        return PartitionScanResult::InvalidGptEntries;
    }
    const uint64_t expectedBytes = (uint64_t)numEntries * (uint64_t)entrySize;
    if (entriesBufBytes < expectedBytes) {
        return PartitionScanResult::InvalidGptEntries;
    }
    // CRC32 of the entry array per UEFI spec — over exactly
    // NumberOfPartitionEntries × SizeOfPartitionEntry bytes.
    const uint32_t actualCrc =
        (uint32_t)crc32(0L, entriesBuf, (uInt)expectedBytes);
    if (actualCrc != expectedEntriesCrc32) {
        return PartitionScanResult::InvalidGptEntries;
    }
    // An entry is "in use" iff its 16-byte PartitionTypeGUID is non-zero.
    bool anyAllocated = false;
    for (uint32_t i = 0; i < numEntries; ++i) {
        const uint8_t *e = entriesBuf + (uint64_t)i * entrySize;
        bool empty = true;
        for (int b = 0; b < 16; ++b) {
            if (e[b] != 0) { empty = false; break; }
        }
        if (empty) {
            continue;
        }
        // EndingLBA is inclusive — convert to "one past last" so the
        // result has the same semantics as the MBR scanner.
        const uint64_t end = readLe64(e + 40) + 1ULL;
        if (end > maxEndingLba) {
            maxEndingLba = end;
        }
        anyAllocated = true;
    }
    if (!anyAllocated) {
        return PartitionScanResult::NoPartitions;
    }
    return PartitionScanResult::Ok;
}

// Arithmetic shared by normalizeGptPrimaryHeader / buildGptBackupHeader.
// Given:
//   maxEndingLba  = sector count of allocated partition data
//                   (the value scanGptEntries returns — one past the
//                   last partition data LBA)
//   entrySectors  = ceil(numEntries * entrySize / sectorSize)
//                   = number of sectors the backup entries take
// The truncated image layout is:
//
//   LBA 0 .. maxEndingLba - 1                            partition data
//   LBA maxEndingLba .. maxEndingLba + entrySectors - 1  backup partition entries
//   LBA maxEndingLba + entrySectors                       backup GPT header
//
// So:
//   backupHeaderLba   = maxEndingLba + entrySectors
//   backupEntriesLba  = maxEndingLba
//   lastUsableLba     = maxEndingLba - 1
//   imageSizeSectors  = maxEndingLba + entrySectors + 1
//
// For standard GPT (128 entries × 128 bytes on 512-byte sectors),
// entrySectors = 32. Pass that value here. For non-standard layouts
// (4K sectors, custom entry counts) compute it from numEntries /
// entrySize / sectorSize at the call site.

bool normalizeGptPrimaryHeader(uint8_t *header,
                               uint64_t maxEndingLba,
                               uint32_t entrySectors)
{
    if (header == nullptr) return false;
    if (maxEndingLba < 34) return false;   // need room for primary + entries + at least 1 partition data sector
    if (entrySectors == 0) return false;
    if (std::memcmp(header, "EFI PART", 8) != 0) return false;
    const uint32_t headerSize = readLe32(header + 12);
    if (headerSize < 92 || headerSize > 512) return false;

    const uint64_t backupHeaderLba = maxEndingLba + (uint64_t)entrySectors;
    writeLe64(header + 32, backupHeaderLba);        // AlternateLBA = backup header LBA
    writeLe64(header + 48, maxEndingLba - 1ULL);    // LastUsableLBA = last data sector
    // CRC32 over header with the CRC field zeroed
    writeLe32(header + 16, 0);
    const uint32_t crc = (uint32_t)crc32(0L, header, (uInt)headerSize);
    writeLe32(header + 16, crc);
    return true;
}

bool buildGptBackupHeader(uint8_t *backup, const uint8_t *primary,
                          uint64_t maxEndingLba,
                          uint32_t entrySectors)
{
    if (backup == nullptr || primary == nullptr) return false;
    if (maxEndingLba < 34) return false;
    if (entrySectors == 0) return false;
    if (std::memcmp(primary, "EFI PART", 8) != 0) return false;
    const uint32_t headerSize = readLe32(primary + 12);
    if (headerSize < 92 || headerSize > 512) return false;

    const uint64_t backupHeaderLba = maxEndingLba + (uint64_t)entrySectors;
    std::memcpy(backup, primary, headerSize);
    // Swap MyLBA / AlternateLBA for the backup copy.
    writeLe64(backup + 24, backupHeaderLba);        // MyLBA = backup header position
    writeLe64(backup + 32, 1ULL);                    // AlternateLBA = primary at LBA 1
    // LastUsableLBA must match the primary's normalized value — backup
    // is built independently of normalizeGptPrimaryHeader (caller may
    // pass an un-normalized primary), so update it here too. Per UEFI
    // spec, primary and backup share all fields except MyLBA /
    // AlternateLBA / CRC32.
    writeLe64(backup + 48, maxEndingLba - 1ULL);
    // PartitionEntryLBA points to the backup entries copy at maxEndingLba.
    writeLe64(backup + 72, maxEndingLba);
    // CRC32 over header with the CRC field zeroed
    writeLe32(backup + 16, 0);
    const uint32_t crc = (uint32_t)crc32(0L, backup, (uInt)headerSize);
    writeLe32(backup + 16, crc);
    return true;
}
