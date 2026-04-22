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
#include "rawimagereader.h"

// Factory: today only the raw path is wired up. A later commit adds magic-
// byte sniffing here to return GzImageReader / XzImageReader for compressed
// inputs without touching any caller.
std::unique_ptr<ImageReader> ImageReader::open(const QString &path, QString *err)
{
    auto reader = std::make_unique<RawImageReader>();
    if (!reader->open(path, err)) {
        return nullptr;
    }
    return reader;
}
