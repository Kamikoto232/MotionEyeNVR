#pragma once

#include <QList>
#include <QPointer>
#include <QWidget>

// Lays its children out as a grid that maximises the 16:9 tile area for the current size.
// A single child can be put in focus to fill the whole area.
class MosaicWidget : public QWidget
{
    Q_OBJECT
public:
    explicit MosaicWidget(QWidget *parent = nullptr);

    // Items must be children of this widget. Previous items are deleted.
    void setItems(const QList<QWidget *> &items);
    void setFocusItem(QWidget *item);
    QWidget *focusItem() const { return m_focus; }

signals:
    void clicked();

public slots:
    // call after an item's "aspect" property changed
    void relayout();

protected:
    void resizeEvent(QResizeEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    void layoutItems();

    QList<QWidget *> m_items;
    QPointer<QWidget> m_focus;
};
