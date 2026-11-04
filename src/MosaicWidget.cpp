#include "MosaicWidget.h"

#include <QMouseEvent>

#include <algorithm>

MosaicWidget::MosaicWidget(QWidget *parent)
    : QWidget(parent)
{
    setAutoFillBackground(true);
    setPalette(QPalette(QColor(0x21, 0x21, 0x21)));
}

void MosaicWidget::setItems(const QList<QWidget *> &items)
{
    for (QWidget *old : std::as_const(m_items)) {
        old->hide();
        old->deleteLater();
    }
    m_items = items;
    m_focus = nullptr;
    layoutItems();
}

void MosaicWidget::setFocusItem(QWidget *item)
{
    m_focus = item;
    layoutItems();
}

void MosaicWidget::resizeEvent(QResizeEvent *)
{
    layoutItems();
}

void MosaicWidget::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton)
        emit clicked();
}

void MosaicWidget::relayout()
{
    layoutItems();
}

namespace {
// Size of an item that is fitted into a cell of the given size. Cameras (items with an "aspect"
// property) hug their picture plus frame padding; other items (status labels) fill the cell.
QSize fitItem(const QWidget *w, double cellW, double cellH)
{
    const double a = w->property("aspect").toDouble();
    if (a <= 0.1)
        return QSize(int(cellW), int(cellH));
    const double pad = w->property("pad").toDouble();
    const double iw = qMax(1.0, qMin(cellW - 2 * pad, (cellH - 2 * pad) * a));
    return QSize(int(iw + 2 * pad), int(iw / a + 2 * pad));
}
} // namespace

void MosaicWidget::layoutItems()
{
    const int n = m_items.size();
    if (n == 0)
        return;

    if (m_focus && m_items.contains(m_focus)) {
        for (QWidget *w : std::as_const(m_items))
            w->setVisible(w == m_focus);
        m_focus->setGeometry(QRect(QPoint(0, 0), fitItem(m_focus, width(), height())));
        return;
    }

    constexpr int gap = 2;

    // pick the column count that gives the largest total picture area for the real camera shapes
    int bestCols = 1;
    double bestArea = -1;
    for (int c = 1; c <= n; ++c) {
        const int rows = (n + c - 1) / c;
        const double cw = double(width() - gap * (c - 1)) / c;
        const double ch = double(height() - gap * (rows - 1)) / rows;
        double area = 0;
        for (QWidget *w : std::as_const(m_items)) {
            const QSize s = fitItem(w, cw, ch);
            area += double(s.width()) * s.height();
        }
        if (area > bestArea + 1) {
            bestArea = area;
            bestCols = c;
        }
    }

    // cameras flow from the left edge like inline blocks, row after row (not centred)
    const int cols = bestCols;
    const int rows = (n + cols - 1) / cols;
    const double cw = double(width() - gap * (cols - 1)) / cols;
    const double ch = double(height() - gap * (rows - 1)) / rows;
    int y = 0;
    for (int r = 0; r < rows; ++r) {
        int x = 0;
        int rowH = 0;
        for (int c = 0; c < cols && r * cols + c < n; ++c) {
            QWidget *w = m_items[r * cols + c];
            const QSize s = fitItem(w, cw, ch);
            w->setGeometry(x, y, s.width(), s.height());
            w->show();
            x += s.width() + gap;
            rowH = qMax(rowH, s.height());
        }
        y += rowH + gap;
    }
}
