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

#ifndef GZIMAGEREADER_H
#define GZIMAGEREADER_H

#include "imagereader.h"
#include <QString>

// zlib handle is a void * typedef (gzFile_s *); keep it as void * here so
// zlib.h isn't pulled into the rest of the project through this header.

class GzImageReader : public ImageReader
{
public:
    GzImageReader() = default;
    ~GzImageReader() override;

    bool open(const QString &path, QString *err);

    qint64  read(void *buf, qsizetype maxBytes) override;
    quint64 uncompressedSize() const override { return m_uncompressed; }
    quint64 compressedPos()    const override { return m_compressedPos; }
    quint64 compressedSize()   const override { return m_compressedSize; }
    QString errorString()      const override { return m_err; }

private:
    void   *m_gz             = nullptr;   // gzFile
    quint64 m_uncompressed   = 0;         // 0 if unreliable (ISIZE wrap on >4 GB)
    quint64 m_compressedSize = 0;
    quint64 m_compressedPos  = 0;
    QString m_err;
};

#endif // GZIMAGEREADER_H
