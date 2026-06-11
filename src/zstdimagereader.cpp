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

#include "zstdimagereader.h"
#include "partitions.h"

#include <QFile>
#include <QObject>
#include <zstd.h>

namespace {

// Peek the first ~17 KB of the decompressed image and parse the GPT /
// MBR at the front. Returns 0 if no recognisable partition table —
// caller's progress UI falls back to compressed-bytes percentage. Same
// trick gzimagereader uses; zstd's frame header has an optional
// content-size field but real-world images don't always set it, so
// peek-and-parse is the reliable signal.
quint64 probeZstdUncompressedSize(const QString &path)
{
    FILE *fp = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"rb");
    if (!fp) return 0;

    ZSTD_DStream *zds = ZSTD_createDStream();
    if (!zds) { fclose(fp); return 0; }
    ZSTD_initDStream(zds);

    constexpr size_t PEEK_BYTES = 17 * 1024;   // covers MBR + GPT primary + 32-LBA entries
    std::vector<unsigned char> outBuf(PEEK_BYTES);
    std::vector<unsigned char> inBuf(ZSTD_DStreamInSize());

    ZSTD_inBuffer  in  = { inBuf.data(), 0, 0 };
    ZSTD_outBuffer out = { outBuf.data(), PEEK_BYTES, 0 };

    bool fileEof = false;
    while (out.pos < out.size && !fileEof) {
        if (in.pos >= in.size) {
            size_t n = fread(inBuf.data(), 1, inBuf.size(), fp);
            if (n == 0) {
                fileEof = true;
            } else {
                in.size = n;
                in.pos  = 0;
            }
        }
        size_t ret = ZSTD_decompressStream(zds, &out, &in);
        if (ZSTD_isError(ret)) break;
        if (ret == 0 && fileEof) break;
    }

    quint64 result = 0;
    if (out.pos > 0) {
        result = estimateImageSizeBytes(outBuf.data(), (uint64_t)out.pos, 512);
    }

    ZSTD_freeDStream(zds);
    fclose(fp);
    return result;
}

} // namespace

ZstdImageReader::~ZstdImageReader()
{
    if (m_zds) ZSTD_freeDStream(static_cast<ZSTD_DStream *>(m_zds));
    if (m_fp)  fclose(m_fp);
}

bool ZstdImageReader::open(const QString &path, QString *err)
{
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (err) *err = probe.errorString();
        return false;
    }
    m_compressedSize = (quint64)probe.size();
    probe.close();

    m_uncompressed = probeZstdUncompressedSize(path);

    m_fp = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"rb");
    if (!m_fp) {
        if (err) *err = QObject::tr("Unable to open zstd image.");
        return false;
    }

    ZSTD_DStream *zds = ZSTD_createDStream();
    if (!zds) {
        if (err) *err = QObject::tr("Failed to initialize zstd decoder.");
        fclose(m_fp);
        m_fp = nullptr;
        return false;
    }
    ZSTD_initDStream(zds);
    m_zds = zds;
    m_inBuf.resize(ZSTD_DStreamInSize());
    return true;
}

qint64 ZstdImageReader::read(void *buf, qsizetype maxBytes)
{
    if (!m_fp || maxBytes <= 0 || m_eof) return 0;

    auto *zds = static_cast<ZSTD_DStream *>(m_zds);
    ZSTD_inBuffer  in  = { m_inBuf.data(), m_inSize, m_inPos };
    ZSTD_outBuffer out = { buf, (size_t)maxBytes, 0 };

    while (out.pos < out.size && !m_eof) {
        if (in.pos >= in.size && !feof(m_fp)) {
            const size_t n = fread(m_inBuf.data(), 1, m_inBuf.size(), m_fp);
            if (ferror(m_fp)) {
                m_err = QObject::tr("Failed to read from zstd image.");
                return -1;
            }
            in.src  = m_inBuf.data();
            in.size = n;
            in.pos  = 0;
            m_compressedPos += n;
        }

        const size_t beforeOut = out.pos;
        const size_t beforeIn  = in.pos;

        const size_t ret = ZSTD_decompressStream(zds, &out, &in);
        if (ZSTD_isError(ret)) {
            m_err = QObject::tr("zstd decode error: %1")
                        .arg(QString::fromUtf8(ZSTD_getErrorName(ret)));
            return -1;
        }

        // Frame fully decoded + no more input ahead = clean EOF.
        if (ret == 0 && in.pos >= in.size && feof(m_fp)) {
            m_eof = true;
            break;
        }

        // No-progress detector: nothing consumed, nothing produced, file
        // exhausted. Treat as EOF — equivalent to a truncated input;
        // surfacing it here keeps the main write loop from spinning.
        if (out.pos == beforeOut && in.pos == beforeIn && feof(m_fp)
                && in.pos >= in.size) {
            m_eof = true;
            break;
        }
    }

    m_inPos  = in.pos;
    m_inSize = in.size;
    return (qint64)out.pos;
}
