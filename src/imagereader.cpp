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

#include "imagereader.h"
#include "gzimagereader.h"
#include "rawimagereader.h"
#include "xzimagereader.h"

#include <QFile>

// Sniff the first handful of bytes and hand back a reader that speaks the
// detected format. We pick by magic bytes, not extension — users sometimes
// rename `.img.xz` → `.img` thinking it doesn't matter.
std::unique_ptr<ImageReader> ImageReader::open(const QString &path, QString *err)
{
    QFile probe(path);
    if (!probe.open(QIODevice::ReadOnly)) {
        if (err) *err = probe.errorString();
        return nullptr;
    }
    unsigned char magic[6] = {0};
    const qint64 n = probe.read(reinterpret_cast<char *>(magic), 6);
    probe.close();

    const bool isGz = (n >= 2) && magic[0] == 0x1F && magic[1] == 0x8B;
    const bool isXz = (n >= 6) && magic[0] == 0xFD && magic[1] == '7'
                   && magic[2] == 'z' && magic[3] == 'X'
                   && magic[4] == 'Z' && magic[5] == 0x00;

    if (isGz) {
        auto r = std::make_unique<GzImageReader>();
        if (!r->open(path, err)) return nullptr;
        return r;
    }
    if (isXz) {
        auto r = std::make_unique<XzImageReader>();
        if (!r->open(path, err)) return nullptr;
        return r;
    }
    auto r = std::make_unique<RawImageReader>();
    if (!r->open(path, err)) return nullptr;
    return r;
}
