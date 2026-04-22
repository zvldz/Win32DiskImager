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
 **********************************************************************/

#ifndef IMAGEREADER_H
#define IMAGEREADER_H

#include <QString>
#include <memory>

// Streaming source of an image payload. RawImageReader wraps an uncompressed
// .img file as-is. Future commits add GzImageReader / XzImageReader for
// on-the-fly decompression. Sequential, non-seekable by design — this matches
// the shape of compressed streams and is all the Write/Verify main loops need.
class ImageReader
{
public:
    virtual ~ImageReader() = default;

    // Fill up to maxBytes into buf. Returns:
    //   > 0  — number of bytes actually placed in buf (may be < maxBytes at tail)
    //   == 0 — clean EOF
    //   < 0  — I/O or decode error; see errorString()
    virtual qint64 read(void *buf, qsizetype maxBytes) = 0;

    // Total uncompressed image size in bytes. Returns 0 if the format does not
    // expose it cheaply (some xz streams, gz files > 4 GB where the ISIZE field
    // has wrapped). Progress UI falls back to compressedPos()/compressedSize()
    // in that case — both are always known.
    virtual quint64 uncompressedSize() const = 0;

    // Bytes of the source file consumed so far (monotonic, 0 at start).
    virtual quint64 compressedPos() const = 0;

    // On-disk size of the source file.
    virtual quint64 compressedSize() const = 0;

    // Human-readable reason for the last failure. Empty on success.
    virtual QString errorString() const = 0;

    // Open the source file, sniff the format, return a reader for it.
    // Returns nullptr and sets *err on failure.
    static std::unique_ptr<ImageReader> open(const QString &path, QString *err);
};

#endif // IMAGEREADER_H
