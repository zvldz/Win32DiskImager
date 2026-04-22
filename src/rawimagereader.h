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

#ifndef RAWIMAGEREADER_H
#define RAWIMAGEREADER_H

#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "imagereader.h"
#include <QString>
#include <windows.h>

// Uncompressed .img files. Plain ReadFile; uncompressedSize == compressedSize.
class RawImageReader : public ImageReader
{
public:
    RawImageReader();
    ~RawImageReader() override;

    // Open the file. Returns false and sets *err on failure.
    bool open(const QString &path, QString *err);

    qint64  read(void *buf, qsizetype maxBytes) override;
    quint64 uncompressedSize() const override { return m_size; }
    quint64 compressedPos()    const override { return m_pos;  }
    quint64 compressedSize()   const override { return m_size; }
    QString errorString()      const override { return m_err;  }

private:
    HANDLE  m_handle = INVALID_HANDLE_VALUE;
    quint64 m_size   = 0;
    quint64 m_pos    = 0;
    QString m_err;
};

#endif // RAWIMAGEREADER_H
