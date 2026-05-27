/**********************************************************************
 *  HistoryItemDelegate implementation — see historydelegate.h.
 **********************************************************************/

#include "historydelegate.h"

#include <QAbstractItemView>
#include <QFont>
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
    // when the popup is first shown (the private popup container is
    // lazy-created). Qt activates the *most recently installed* filter
    // first — so if QComboBox installs after we do, ITS filter runs
    // first and swallows our ✕ click as a row-selection.
    //
    // Strategy: install on the view itself to catch Show events, and
    // re-install our viewport filter each time the popup becomes
    // visible. removeEventFilter + installEventFilter moves us back to
    // the front of the filter chain.
    view->installEventFilter(this);
    view->viewport()->installEventFilter(this);
    // Mouse tracking so paint() can render a hover state on the ✕ as
    // the cursor passes over it, instead of only highlighting on press.
    view->viewport()->setMouseTracking(true);
}

QRect HistoryItemDelegate::closeButtonRect(const QRect &rowRect)
{
    // Square area on the right edge, sized to the row height. The full
    // square is the click target — the visible glyph rendered inside
    // it is smaller, but the hover background fills the square so the
    // user sees the click area clearly before clicking.
    //
    // Caller must pass a rowRect whose right edge matches the viewport's
    // right edge (i.e. all rows uniform width). sizeHint() below forces
    // this by clamping every row to viewport width — otherwise QListView
    // sizes rows to their natural text width and the ✕ drifts to the
    // right of the visible viewport on longer paths.
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

    const QRect r = closeButtonRect(option.rect);
    const bool hovered = r.contains(m_hoverPos);

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // Pill-shaped hover background — same diameter as row height so the
    // entire click target is visible to the user when they're aiming
    // for it. Drops away when the cursor leaves so it never competes
    // with the entry text for attention.
    if (hovered) {
        QColor bg = option.palette.color(QPalette::Highlight);
        bg.setAlpha(110);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        const int pad = std::max(1, r.height() / 6);
        p->drawEllipse(r.adjusted(pad, pad, -pad, -pad));
    }

    // Render the ✕ glyph: bigger and bolder than the entry-text font
    // so it reads clearly even at low DPI, and brightened when hovered.
    QFont f = option.font;
    f.setPointSizeF(f.pointSizeF() * 1.3);
    f.setBold(true);
    p->setFont(f);
    QPen pen = p->pen();
    pen.setColor(hovered
                     ? option.palette.color(QPalette::HighlightedText)
                     : option.palette.color(QPalette::WindowText));
    p->setPen(pen);
    p->drawText(r, Qt::AlignCenter, QStringLiteral("✕"));
    p->restore();
}

QSize HistoryItemDelegate::sizeHint(const QStyleOptionViewItem &option,
                                    const QModelIndex &index) const
{
    QSize s = QStyledItemDelegate::sizeHint(option, index);
    // Force every row to viewport width so all visualRects line up at
    // the same right edge. QListView's default is to size each row to
    // its text's natural width (uniformItemSizes=false), which makes
    // the ✕ click target drift right for rows with longer paths —
    // visually the ✕ ends up at different x positions per row, and
    // the user's click (aimed at one row's ✕ location) lands in the
    // empty space next to a different row's ✕.
    if (m_view && m_view->viewport()) {
        const int vw = m_view->viewport()->width();
        if (vw > 0 && vw > s.width()) {
            s.setWidth(vw);
        }
    }
    return s;
}

bool HistoryItemDelegate::eventFilter(QObject *obj, QEvent *event)
{
    // Re-front our viewport filter every time the popup view shows.
    // Without this, QComboBox's filter (installed lazily on first
    // showPopup) sits in front of ours and consumes mouse events
    // before we see them, making ✕ clicks register as row selections.
    if (m_view && obj == m_view && event->type() == QEvent::Show) {
        QWidget *vp = m_view->viewport();
        vp->removeEventFilter(this);
        vp->installEventFilter(this);
        vp->setMouseTracking(true);
    }

    if (m_view && obj == m_view->viewport()) {
        // Track cursor for paint()'s hover state. MouseMove is the
        // primary feed; press/release events also carry positions and
        // are forwarded below.
        if (event->type() == QEvent::MouseMove) {
            auto *me = static_cast<QMouseEvent *>(event);
            const QPoint prev = m_hoverPos;
            m_hoverPos = me->pos();
            // Repaint the rows that changed hovered-state. Cheap, only
            // hits visible rows touched by the move.
            if (m_view) {
                const QModelIndex prevIdx = m_view->indexAt(prev);
                const QModelIndex curIdx  = m_view->indexAt(m_hoverPos);
                if (prevIdx.isValid()) m_view->update(prevIdx);
                if (curIdx.isValid() && curIdx != prevIdx) m_view->update(curIdx);
            }
            return false;   // don't swallow — let normal hover handling run
        }
        if (event->type() == QEvent::Leave) {
            const QPoint prev = m_hoverPos;
            m_hoverPos = QPoint(-1, -1);
            if (m_view) {
                const QModelIndex prevIdx = m_view->indexAt(prev);
                if (prevIdx.isValid()) m_view->update(prevIdx);
            }
            return false;
        }
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
