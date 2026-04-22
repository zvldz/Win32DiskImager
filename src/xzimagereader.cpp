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

#include "xzimagereader.h"

#include <QFile>
#include <QObject>
#include <algorithm>
#include <cstring>
#include <thread>

namespace {

constexpr size_t XZ_INBUF_SIZE = 64 * 1024;

// Decode the xz stream footer + index at the end of the file to recover the
// total uncompressed size. Standard `xz` CLI output always contains an index;
// streaming encoders occasionally skip it, in which case we return 0 and the
// progress UI falls back to compressed-bytes percentage.
quint64 probeXzUncompressedSize(const QString &path, quint64 fileSize)
{
    if (fileSize < 2 * LZMA_STREAM_HEADER_SIZE) return 0;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0;

    // Stream footer (fixed 12 bytes) sits at the very end.
    unsigned char footer[LZMA_STREAM_HEADER_SIZE];
    if (!f.seek((qint64)fileSize - (qint64)LZMA_STREAM_HEADER_SIZE)) return 0;
    if (f.read(reinterpret_cast<char *>(footer), LZMA_STREAM_HEADER_SIZE)
            != (qint64)LZMA_STREAM_HEADER_SIZE) return 0;

    lzma_stream_flags flags;
    if (lzma_stream_footer_decode(&flags, footer) != LZMA_OK) return 0;

    const quint64 indexSize = flags.backward_size;
    if (indexSize == 0 || indexSize > fileSize - LZMA_STREAM_HEADER_SIZE) return 0;

    std::vector<unsigned char> idx((size_t)indexSize);
    if (!f.seek((qint64)(fileSize - LZMA_STREAM_HEADER_SIZE - indexSize))) return 0;
    if ((quint64)f.read(reinterpret_cast<char *>(idx.data()), (qint64)indexSize) != indexSize) return 0;

    lzma_index *index = nullptr;
    uint64_t memlimit = UINT64_MAX;
    size_t inPos = 0;
    if (lzma_index_buffer_decode(&index, &memlimit, nullptr,
                                 idx.data(), &inPos, (size_t)indexSize) != LZMA_OK) {
        return 0;
    }
    const quint64 total = (quint64)lzma_index_uncompressed_size(index);
    lzma_index_end(index, nullptr);
    return total;
}

} // namespace

XzImageReader::XzImageReader() = default;

XzImageReader::~XzImageReader()
{
    if (m_strmInit) lzma_end(&m_strm);
    if (m_fp) fclose(m_fp);
}

bool XzImageReader::open(const QString &path, QString *err)
{
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (err) *err = probe.errorString();
        return false;
    }
    m_compressedSize = (quint64)probe.size();
    probe.close();

    m_uncompressed = probeXzUncompressedSize(path, m_compressedSize);

    m_fp = _wfopen(reinterpret_cast<const wchar_t *>(path.utf16()), L"rb");
    if (!m_fp) {
        if (err) *err = QObject::tr("Unable to open xz image.");
        return false;
    }

    // Try the multi-threaded decoder first. On CPUs where single-thread
    // decode (~80 MB/s) would bottleneck a fast target (UHS-II / fast USB
    // SSD), this gives 2-4x throughput. No effect when the bottleneck is the
    // target device — decoder just waits in the I/O pipeline. Falls back to
    // single-threaded if unavailable (very old liblzma or OOM).
    lzma_mt mtOpts = {};
    mtOpts.flags              = LZMA_CONCATENATED;
    mtOpts.threads            = std::min(8u, std::max(2u, std::thread::hardware_concurrency()));
    mtOpts.memlimit_threading = UINT64_MAX;
    mtOpts.memlimit_stop      = UINT64_MAX;
    lzma_ret r = lzma_stream_decoder_mt(&m_strm, &mtOpts);
    if (r != LZMA_OK) {
        m_strm = LZMA_STREAM_INIT;
        r = lzma_stream_decoder(&m_strm, UINT64_MAX, LZMA_CONCATENATED);
    }
    if (r != LZMA_OK) {
        if (err) *err = QObject::tr("Failed to initialize xz decoder.");
        fclose(m_fp);
        m_fp = nullptr;
        return false;
    }
    m_strmInit = true;
    m_inBuf.resize(XZ_INBUF_SIZE);
    return true;
}

qint64 XzImageReader::read(void *buf, qsizetype maxBytes)
{
    if (!m_fp || maxBytes <= 0 || m_eof) return 0;

    m_strm.next_out  = static_cast<unsigned char *>(buf);
    m_strm.avail_out = (size_t)maxBytes;

    while (m_strm.avail_out > 0 && !m_eof) {
        if (m_strm.avail_in == 0 && !feof(m_fp)) {
            const size_t n = fread(m_inBuf.data(), 1, m_inBuf.size(), m_fp);
            if (ferror(m_fp)) {
                m_err = QObject::tr("Failed to read from xz image.");
                return -1;
            }
            m_strm.next_in  = m_inBuf.data();
            m_strm.avail_in = n;
            m_compressedPos += n;
        }

        const lzma_action action = feof(m_fp) ? LZMA_FINISH : LZMA_RUN;
        const lzma_ret rc = lzma_code(&m_strm, action);

        if (rc == LZMA_STREAM_END) {
            m_eof = true;
            break;
        }
        if (rc != LZMA_OK) {
            switch (rc) {
                case LZMA_FORMAT_ERROR: m_err = QObject::tr("Not a valid xz file."); break;
                case LZMA_DATA_ERROR:   m_err = QObject::tr("Corrupted xz data.");   break;
                case LZMA_BUF_ERROR:    m_err = QObject::tr("Unexpected end of xz stream."); break;
                default:                m_err = QObject::tr("xz decoder error (%1).").arg(int(rc)); break;
            }
            return -1;
        }
    }

    return (qint64)maxBytes - (qint64)m_strm.avail_out;
}
