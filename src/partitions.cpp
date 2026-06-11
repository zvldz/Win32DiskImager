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
            // Header validated. AlternateLBA at offset 32-39 is the
            // sector where the backup GPT lives, i.e. disk_size - 1.
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
