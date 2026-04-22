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
 *                                                                    *
 *  You should have received a copy of the GNU General Public License *
 *  along with this program; if not, see http://gnu.org/licenses/     *
 **********************************************************************/

#include <QtWidgets>
#include "droppablecombobox.h"

DroppableComboBox::DroppableComboBox(QWidget *parent)
    : QComboBox(parent)
{
    setAcceptDrops(true);
    setEditable(true);
    setInsertPolicy(QComboBox::NoInsert);
    if (QCompleter *c = completer()) {
        c->setCompletionMode(QCompleter::PopupCompletion);
        c->setFilterMode(Qt::MatchContains);
        c->setCaseSensitivity(Qt::CaseInsensitive);
    }
}

void DroppableComboBox::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat("text/uri-list") ||
        event->mimeData()->hasFormat("text/plain"))
    {
        event->acceptProposedAction();
    }
}

void DroppableComboBox::dropEvent(QDropEvent *event)
{
    const QMimeData *data = event->mimeData();

    if (data->hasUrls())
    {
        const QList<QUrl> urlList = data->urls();
        if (!urlList.isEmpty())
        {
            const QString fName = urlList.first().toLocalFile();
            if (QFileInfo(fName).isFile())
            {
                setEditText(fName);
                event->acceptProposedAction();
                return;
            }
        }
        event->ignore();
    }
    else if (data->hasText())
    {
        setEditText(data->text());
        event->acceptProposedAction();
    }
    else
    {
        event->ignore();
    }
}
