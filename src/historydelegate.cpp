/**********************************************************************
 *  HistoryItemDelegate implementation — see historydelegate.h.
 **********************************************************************/

#include "historydelegate.h"

#include <QAbstractItemView>
#include <QMouseEvent>
#include <QPainter>

HistoryItemDelegate::HistoryItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{}

void HistoryItemDelegate::attachTo(QAbstractItemView *view)
{
    m_view = view;
    if (!view) return;
    view->setItemDelegate(this);
    // QComboBox installs its own event filter on the popup's viewport
    // and treats any mouse press there as a row-selection. Intercept
    // the press *before* it gets there so a click on our ✕ doesn't
    // double as a "select this entry" action.
    view->viewport()->installEventFilter(this);
}

QRect HistoryItemDelegate::closeButtonRect(const QRect &rowRect)
{
    // Square area on the right edge, sized to the row height.
    const int sz = rowRect.height();
    return QRect(rowRect.right() - sz + 1, rowRect.top(),
                 sz - 1, rowRect.height());
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
    const QRect r = closeButtonRect(option.rect);
    p->save();
    QPen pen = p->pen();
    pen.setColor(option.palette.color(QPalette::Disabled, QPalette::WindowText));
    p->setPen(pen);
    p->drawText(r, Qt::AlignCenter, QStringLiteral("✕"));
    p->restore();
}

bool HistoryItemDelegate::eventFilter(QObject *obj, QEvent *event)
{
    if (m_view && obj == m_view->viewport()) {
        // Catch press AND release that land on the ✕. We swallow both
        // so QComboBox doesn't see a "click", which would otherwise
        // close the popup and select the entry being deleted.
        if (event->type() == QEvent::MouseButtonPress ||
            event->type() == QEvent::MouseButtonRelease)
        {
            auto *me = static_cast<QMouseEvent *>(event);
            const QModelIndex idx = m_view->indexAt(me->pos());
            if (idx.isValid()) {
                const QRect rowRect = m_view->visualRect(idx);
                if (closeButtonRect(rowRect).contains(me->pos())) {
                    if (event->type() == QEvent::MouseButtonRelease) {
                        emit removeRequested(idx.row());
                    }
                    return true;
                }
            }
        }
    }
    return QStyledItemDelegate::eventFilter(obj, event);
}
