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

#include "rawimagereader.h"

#include <QObject>

RawImageReader::RawImageReader() = default;

RawImageReader::~RawImageReader()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_handle);
    }
}

bool RawImageReader::open(const QString &path, QString *err)
{
    m_handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                           GENERIC_READ,
                           FILE_SHARE_READ,
                           nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (m_handle == INVALID_HANDLE_VALUE) {
        if (err) *err = QObject::tr("Unable to open image file for reading.");
        return false;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(m_handle, &sz)) {
        if (err) *err = QObject::tr("Unable to query image file size.");
        CloseHandle(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
        return false;
    }
    m_size = static_cast<quint64>(sz.QuadPart);
    m_pos  = 0;
    return true;
}

qint64 RawImageReader::read(void *buf, qsizetype maxBytes)
{
    if (m_handle == INVALID_HANDLE_VALUE || maxBytes <= 0) {
        return 0;
    }
    DWORD got = 0;
    if (!ReadFile(m_handle, buf, static_cast<DWORD>(maxBytes), &got, nullptr)) {
        m_err = QObject::tr("Failed to read from image file.");
        return -1;
    }
    m_pos += got;
    return static_cast<qint64>(got);
}
