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

#include "gzimagereader.h"
#include "partitions.h"

#include <QFile>
#include <QObject>
#include <vector>
#include <zlib.h>

GzImageReader::~GzImageReader()
{
    if (m_gz) {
        gzclose(static_cast<gzFile>(m_gz));
    }
}

bool GzImageReader::open(const QString &path, QString *err)
{
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (err) *err = probe.errorString();
        return false;
    }
    m_compressedSize = (quint64)probe.size();
    probe.close();

    // ISIZE wraps at 4 GiB so it can't be trusted. Instead, peek the
    // decompressed start of the image and read the disk size from its
    // GPT primary header (or MBR partition entries as fallback). Same
    // pattern Etcher uses (lib/source-destination/gzip.ts: max of
    // ISIZE and partition-table-derived size). Returns 0 when neither
    // is present — caller falls back to compressed-bytes progress.
    m_uncompressed = 0;
    {
        constexpr size_t PEEK_BYTES = 17 * 1024;  // covers MBR + GPT primary + 32-LBA entries
        gzFile gPeek = gzopen_w(reinterpret_cast<const wchar_t *>(path.utf16()), "rb");
        if (gPeek) {
            std::vector<uint8_t> buf(PEEK_BYTES);
            int got = gzread(gPeek, buf.data(), (unsigned)PEEK_BYTES);
            gzclose(gPeek);
            if (got > 0) {
                m_uncompressed = estimateImageSizeBytes(buf.data(), (uint64_t)got, 512);
            }
        }
    }

    gzFile g = gzopen_w(reinterpret_cast<const wchar_t *>(path.utf16()), "rb");
    if (!g) {
        if (err) *err = QObject::tr("Unable to open gzip image.");
        return false;
    }
    m_gz = g;
    return true;
}

qint64 GzImageReader::read(void *buf, qsizetype maxBytes)
{
    if (!m_gz || maxBytes <= 0) return 0;
    int got = gzread(static_cast<gzFile>(m_gz), buf, (unsigned)maxBytes);
    if (got < 0) {
        int ec = 0;
        const char *msg = gzerror(static_cast<gzFile>(m_gz), &ec);
        m_err = msg ? QString::fromUtf8(msg)
                    : QObject::tr("gzip decode error (%1).").arg(ec);
        return -1;
    }
    const z_off_t off = gzoffset(static_cast<gzFile>(m_gz));
    if (off >= 0) m_compressedPos = (quint64)off;
    return (qint64)got;
}
