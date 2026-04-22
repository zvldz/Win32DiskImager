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

#include <QFile>
#include <QObject>
#include <zlib.h>

namespace {

// Read the gzip ISIZE trailer — the uncompressed size mod 2^32. When the
// original input was ≥ 4 GB the field has wrapped and is unreliable; the
// caller treats a zero return as "size unknown" and falls back to compressed-
// bytes progress.
quint64 readGzIsize(const QString &path, quint64 compressedSize)
{
    if (compressedSize < 4) return 0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;
    if (!f.seek((qint64)compressedSize - 4)) return 0;
    unsigned char b[4];
    if (f.read(reinterpret_cast<char *>(b), 4) != 4) return 0;
    const quint32 isize = (quint32)b[0]
                        | ((quint32)b[1] << 8)
                        | ((quint32)b[2] << 16)
                        | ((quint32)b[3] << 24);
    // A credible gz never decompresses to fewer bytes than its compressed
    // form — if ISIZE is below the compressed size the counter has wrapped.
    if (isize < compressedSize) return 0;
    return isize;
}

} // namespace

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

    m_uncompressed = readGzIsize(path, m_compressedSize);

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
