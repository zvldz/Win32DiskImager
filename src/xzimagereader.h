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

#ifndef XZIMAGEREADER_H
#define XZIMAGEREADER_H

#include "imagereader.h"
#include <QString>
#include <cstdio>
#include <vector>
#include <lzma.h>

class XzImageReader : public ImageReader
{
public:
    XzImageReader();
    ~XzImageReader() override;

    bool open(const QString &path, QString *err);

    qint64  read(void *buf, qsizetype maxBytes) override;
    quint64 uncompressedSize() const override { return m_uncompressed; }
    quint64 compressedPos()    const override { return m_compressedPos; }
    quint64 compressedSize()   const override { return m_compressedSize; }
    QString errorString()      const override { return m_err; }

private:
    FILE                      *m_fp             = nullptr;
    lzma_stream                m_strm           = LZMA_STREAM_INIT;
    bool                       m_strmInit       = false;
    bool                       m_eof            = false;
    std::vector<unsigned char> m_inBuf;
    quint64                    m_uncompressed   = 0;   // 0 if the stream has no index
    quint64                    m_compressedSize = 0;
    quint64                    m_compressedPos  = 0;
    QString                    m_err;
};

#endif // XZIMAGEREADER_H
