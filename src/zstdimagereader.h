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

#ifndef ZSTDIMAGEREADER_H
#define ZSTDIMAGEREADER_H

#include "imagereader.h"
#include <QString>
#include <cstdio>
#include <cstddef>
#include <vector>

// zstd handle stays as void * here so zstd.h isn't pulled into the rest
// of the project through this header (same pattern as gzimagereader's
// gzFile).
class ZstdImageReader : public ImageReader
{
public:
    ZstdImageReader() = default;
    ~ZstdImageReader() override;

    bool open(const QString &path, QString *err);

    qint64  read(void *buf, qsizetype maxBytes) override;
    quint64 uncompressedSize() const override { return m_uncompressed; }
    quint64 compressedPos()    const override { return m_compressedPos; }
    quint64 compressedSize()   const override { return m_compressedSize; }
    QString errorString()      const override { return m_err; }

private:
    FILE                       *m_fp             = nullptr;
    void                       *m_zds            = nullptr;   // ZSTD_DStream *
    bool                        m_eof            = false;
    std::vector<unsigned char>  m_inBuf;
    size_t                      m_inPos          = 0;
    size_t                      m_inSize         = 0;
    quint64                     m_uncompressed   = 0;         // 0 if no GPT/MBR found
    quint64                     m_compressedSize = 0;
    quint64                     m_compressedPos  = 0;
    QString                     m_err;
};

#endif // ZSTDIMAGEREADER_H
