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

class HistoryItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit HistoryItemDelegate(QObject *parent = nullptr);

    void paint(QPainter *p, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override;

signals:
    void removeRequested(int row);

private:
    static QRect closeButtonRect(const QStyleOptionViewItem &option);
};

#endif // HISTORYDELEGATE_H
