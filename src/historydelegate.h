/**********************************************************************
 *  Item delegate for the Image File history drop-down. Draws a small
 *  ✕ on the right side of each row; clicking it emits removeRequested
 *  with the row index instead of selecting the entry. The MainWindow
 *  uses that signal to confirm + delete the item from the combo and
 *  the registry-backed history list.
 **********************************************************************/

#ifndef HISTORYDELEGATE_H
#define HISTORYDELEGATE_H

#include <QStyledItemDelegate>
#include <QPointer>

class QAbstractItemView;

class HistoryItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit HistoryItemDelegate(QObject *parent = nullptr);

    // Sets this as the view's delegate AND installs an event filter on
    // its viewport so we can intercept the click before QComboBox
    // swallows it as a row-selection.
    void attachTo(QAbstractItemView *view);

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    bool eventFilter(QObject *obj, QEvent *event) override;

signals:
    void removeRequested(int row);

private:
    static QRect closeButtonRect(const QRect &rowRect);
    QPointer<QAbstractItemView> m_view;
};

#endif // HISTORYDELEGATE_H
