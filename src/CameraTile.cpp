#include "CameraTile.h"

#include "MotionEyeClient.h"

#include <QDateTime>
#include <QMouseEvent>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>

CameraTile::CameraTile(MotionEyeClient *client, int cameraId, const QString &name, int fps, QWidget *parent)
    : QWidget(parent)
    , m_client(client)
    , m_cameraId(cameraId)
    , m_name(name)
    , m_frameInterval(1000 / qMax(1, fps))
    , m_recButton(new QPushButton(QStringLiteral("Записи"), this))
{
    setMinimumSize(160, 90);
    setProperty("aspect", 16.0 / 9.0); // until the first frame tells the real shape
    setProperty("pad", framePad());
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &CameraTile::poll);

    m_recButton->setCursor(Qt::PointingHandCursor);
    m_recButton->setFocusPolicy(Qt::NoFocus);
    m_recButton->setStyleSheet(QStringLiteral(
        "QPushButton{background:rgba(0,0,0,160);color:white;border:1px solid #3498db;border-radius:4px;padding:4px 12px;}"
        "QPushButton:hover{background:rgba(52,152,219,220);}"));
    connect(m_recButton, &QPushButton::clicked, this, [this] { emit recordingsRequested(this); });
}

QString CameraTile::title() const
{
    return m_client ? m_client->displayName() + QStringLiteral(" / ") + m_name : m_name;
}

void CameraTile::resizeEvent(QResizeEvent *)
{
    layoutButton();
}

void CameraTile::showEvent(QShowEvent *)
{
    m_active = true;
    poll();
}

void CameraTile::hideEvent(QHideEvent *)
{
    m_active = false;
    m_timer.stop();
    cancel();
}

void CameraTile::mouseDoubleClickEvent(QMouseEvent *)
{
    m_ignoreRelease = true; // the release that ends a double click is not a click
    emit activated(this);
}

void CameraTile::mouseReleaseEvent(QMouseEvent *e)
{
    if (m_ignoreRelease) {
        m_ignoreRelease = false;
        return;
    }
    if (e->button() == Qt::LeftButton)
        emit clicked();
}

void CameraTile::setHeaderVisible(bool visible)
{
    m_headerVisible = visible;
    m_recButton->setVisible(visible);
    update();
}

void CameraTile::cancel()
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
}

void CameraTile::schedule(int ms)
{
    if (m_active)
        m_timer.start(qMax(1, ms));
}

void CameraTile::poll()
{
    if (!m_active || m_reply || m_authPending || !m_client)
        return;
    m_authPending = true;
    m_client->authenticate(this, [this](bool ok, const QString &error) {
        m_authPending = false;
        if (!m_active)
            return;
        if (ok)
            fetch();
        else
            fail(error);
    });
}

void CameraTile::fetch()
{
    if (m_reply || !m_client)
        return;
    const int w = qBound(320, int(width() * devicePixelRatioF()), 1920);
    m_cycle.start();
    m_reply = m_client->get(QStringLiteral("/picture/%1/current/").arg(m_cameraId),
                            {{QStringLiteral("width"), QString::number(w)},
                             {QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch())}});
    connect(m_reply, &QNetworkReply::finished, this, &CameraTile::onFinished);
}

void CameraTile::onFinished()
{
    QNetworkReply *r = m_reply;
    m_reply = nullptr;
    r->deleteLater();

    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (r->error() != QNetworkReply::NoError) {
        if ((status == 401 || status == 403) && m_client)
            m_client->invalidate();
        fail(status ? QStringLiteral("HTTP %1").arg(status) : r->errorString());
        return;
    }
    QImage img;
    if (!img.loadFromData(r->readAll(), "JPEG")) {
        fail(QStringLiteral("Нет изображения"));
        return;
    }
    // motionEye flags a running motion event in a cookie of the frame response
    const QByteArray motionName = QByteArray("motion_detected_") + QByteArray::number(m_cameraId);
    const auto cookies = qvariant_cast<QList<QNetworkCookie>>(r->header(QNetworkRequest::SetCookieHeader));
    for (const QNetworkCookie &c : cookies) {
        if (c.name() == motionName) {
            const bool motion = c.value() == "true";
            if (motion != m_motion) {
                m_motion = motion;
                update();
            }
        }
    }
    m_frame = img;
    m_fails = 0;
    layoutButton();
    const double aspect = double(img.width()) / img.height();
    if (qAbs(aspect - property("aspect").toDouble()) > 0.01) {
        setProperty("aspect", aspect); // the mosaic sizes cells by the real camera shape
        if (parentWidget())
            QMetaObject::invokeMethod(parentWidget(), "relayout");
    }
    update();
    schedule(m_frameInterval - int(m_cycle.elapsed()));
}

void CameraTile::fail(const QString &message)
{
    ++m_fails;
    m_status = message;
    if (m_fails >= 3)
        m_frame = QImage();
    update();
    schedule(qMin(2000 * m_fails, 10000));
}

int CameraTile::framePad() const
{
    return qMax(2, qRound(fontMetrics().height() * 0.2)); // 0.2em
}

QRect CameraTile::innerRect() const
{
    const int p = framePad();
    return rect().adjusted(p, p, -p, -p);
}

QRect CameraTile::imageRect() const
{
    const QRect inner = innerRect();
    if (m_frame.isNull())
        return inner;
    QRect r(inner.topLeft(), m_frame.size().scaled(inner.size(), Qt::KeepAspectRatio));
    r.moveCenter(inner.center());
    return r;
}

int CameraTile::headerHeight() const
{
    return qMax(m_recButton->sizeHint().height() + 8, fontMetrics().height() + 12);
}

void CameraTile::layoutButton()
{
    const QRect img = imageRect();
    m_recButton->adjustSize();
    m_recButton->move(img.right() - m_recButton->width() - 6 + 1,
                      img.top() + (headerHeight() - m_recButton->height()) / 2);
}

void CameraTile::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), m_motion ? QColor(0x71, 0x27, 0x27) : QColor(0x31, 0x31, 0x31)); // frame
    const QRect inner = innerRect();
    p.fillRect(inner, Qt::black);

    if (m_frame.isNull()) {
        p.setPen(QColor(200, 200, 200));
        p.drawText(inner, Qt::AlignCenter | Qt::TextWordWrap, QStringLiteral("%1\n%2").arg(title(), m_status));
        return;
    }

    const QRect img = imageRect();
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(img, m_frame);

    // header bar over the picture: title on the left, "Записи" button on the right
    if (m_headerVisible) {
        const QRect bar(img.left(), img.top(), img.width(), headerHeight());
        p.fillRect(bar, m_motion ? QColor(0x71, 0x27, 0x27, 150) : QColor(0, 0, 0, 150));
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        const QRect textRect = bar.adjusted(8, 0, -(m_recButton->width() + 14), 0);
        p.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                   QFontMetrics(f).elidedText(title(), Qt::ElideRight, textRect.width()));
    }

    if (m_fails > 0) {
        p.fillRect(img, QColor(0, 0, 0, 120));
        p.setPen(QColor(255, 120, 120));
        p.drawText(img, Qt::AlignCenter, QStringLiteral("Нет сигнала: %1").arg(m_status));
    }
}
