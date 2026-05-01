/**********************************************************************
 *  HistoryItemDelegate implementation — see historydelegate.h.
 **********************************************************************/

#include "historydelegate.h"

#include <QPainter>
#include <QMouseEvent>

HistoryItemDelegate::HistoryItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{}

QRect HistoryItemDelegate::closeButtonRect(const QStyleOptionViewItem &option)
{
    // Square area on the right edge, sized to the row height.
    const int sz = option.rect.height();
    return QRect(option.rect.right() - sz + 1, option.rect.top(),
                 sz - 1, option.rect.height());
}

void HistoryItemDelegate::paint(QPainter *p,
                                const QStyleOptionViewItem &option,
                                const QModelIndex &index) const
{
    // Reserve the right edge for the close button so the entry text
    // doesn't underflow the ✕.
    QStyleOptionViewItem opt = option;
    const int sz = option.rect.height();
    opt.rect.adjust(0, 0, -sz, 0);
    QStyledItemDelegate::paint(p, opt, index);

    // Render a muted ✕ glyph on the right of the row.
    const QRect r = closeButtonRect(option);
    p->save();
    QPen pen = p->pen();
    pen.setColor(option.palette.color(QPalette::Disabled, QPalette::WindowText));
    p->setPen(pen);
    p->drawText(r, Qt::AlignCenter, QStringLiteral("✕"));
    p->restore();
}

bool HistoryItemDelegate::editorEvent(QEvent *event,
                                      QAbstractItemModel *model,
                                      const QStyleOptionViewItem &option,
                                      const QModelIndex &index)
{
    // Intercept clicks that land on the ✕: emit, don't let the click
    // propagate to row-selection.
    if (event->type() == QEvent::MouseButtonRelease ||
        event->type() == QEvent::MouseButtonPress)
    {
        auto *me = static_cast<QMouseEvent *>(event);
        if (closeButtonRect(option).contains(me->pos())) {
            if (event->type() == QEvent::MouseButtonRelease) {
                emit removeRequested(index.row());
            }
            return true;  // consume both press and release on the ✕
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}
