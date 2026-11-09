#include "CameraTile.h"

#include "MotionEyeClient.h"

#include <turbojpeg.h>

#include <QBuffer>
#include <QElapsedTimer>
#include <cstdio>
#include <QDateTime>
#include <QImageReader>
#include <QMouseEvent>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QPainter>
#include <QPushButton>

namespace {
// Optional diagnostics: MENVR_STATS=1 prints decode/paint cost every 5 s to stderr.
struct Stats {
    bool on = qEnvironmentVariableIsSet("MENVR_STATS");
    QElapsedTimer window;
    int frames = 0, paints = 0;
    qint64 decodeNs = 0, paintNs = 0, bytes = 0;
    QSize src, dec;
    bool motionKnown = false, motion = false;
    int interval = 0;
    void report()
    {
        if (!window.isValid()) {
            window.start();
            return;
        }
        const qint64 ms = window.elapsed();
        if (ms < 5000)
            return;
        fprintf(stderr,
                "[stats] %.1f frames/s, %.1f paints/s | decode %.1f ms/frame | paint %.1f ms/paint | "
                "jpeg %lld KB/frame | source %dx%d -> decoded %dx%d | busy: decode %.0f%% paint %.0f%% | "
                "motion cookie %s, motion %s, interval %d ms\n",
                frames * 1000.0 / ms, paints * 1000.0 / ms, frames ? decodeNs / 1e6 / frames : 0.0,
                paints ? paintNs / 1e6 / paints : 0.0, frames ? bytes / frames / 1024 : 0LL, src.width(),
                src.height(), dec.width(), dec.height(), decodeNs / 1e4 / ms, paintNs / 1e4 / ms,
                motionKnown ? "seen" : "NOT seen", motion ? "yes" : "no", interval);
        frames = paints = 0;
        decodeNs = paintNs = bytes = 0;
        window.restart();
    }
};
Stats g_stats;

constexpr double kNearFit = 0.04; // max relative size mismatch drawn 1:1 (cropped / padded)

struct TurboHandle {
    tjhandle h = tjInitDecompress();
    ~TurboHandle()
    {
        if (h)
            tjDestroy(h);
    }
};

// Decodes a JPEG at (about) the size it will be shown at: libjpeg-turbo scales by 1/2, 1/4, 1/8 while
// decoding (fast DCT/upsampling), the small remainder is resized once here instead of on every paint.
// Falls back to Qt's decoder if libjpeg-turbo cannot handle the data. `full` gets the source size.
QImage decodeJpeg(const QByteArray &data, QSize box, QSize *full)
{
    static TurboHandle turbo;
    int w = 0, h = 0, subsamp = 0, cs = 0;
    if (turbo.h
        && tjDecompressHeader3(turbo.h, reinterpret_cast<const unsigned char *>(data.constData()),
                               static_cast<unsigned long>(data.size()), &w, &h, &subsamp, &cs) == 0
        && w > 0 && h > 0) {
        *full = QSize(w, h);
        const QSize target = box.isValid() && !box.isEmpty() ? QSize(w, h).scaled(box, Qt::KeepAspectRatio)
                                                              : QSize(w, h);
        // libjpeg-turbo scales in steps of 1/8. A size within a few percent of the target is used as is
        // (paintEvent crops or pads by a few pixels instead of resampling, which costs more than decoding);
        // otherwise take the smallest size that is still >= target and resample it once.
        int n = 0;
        const tjscalingfactor *sf = tjGetScalingFactors(&n);
        tjscalingfactor best{1, 1};
        long bestPixels = long(w) * h;
        bool close = false;
        double closestErr = 1e9;
        for (int i = 0; sf && i < n; ++i) {
            const int sw = TJSCALED(w, sf[i]);
            const int sh = TJSCALED(h, sf[i]);
            const double err = qMax(qAbs(sw - target.width()) / double(qMax(1, target.width())),
                                    qAbs(sh - target.height()) / double(qMax(1, target.height())));
            if (err <= kNearFit && err < closestErr) {
                closestErr = err;
                best = sf[i];
                close = true;
            } else if (!close && sw >= target.width() && sh >= target.height() && long(sw) * sh < bestPixels) {
                best = sf[i];
                bestPixels = long(sw) * sh;
            }
        }
        const int sw = TJSCALED(w, best);
        const int sh = TJSCALED(h, best);
        QImage img(sw, sh, QImage::Format_RGB32);
        if (!img.isNull()
            && tjDecompress2(turbo.h, reinterpret_cast<const unsigned char *>(data.constData()),
                             static_cast<unsigned long>(data.size()), img.bits(), sw, int(img.bytesPerLine()), sh, TJPF_BGRX,
                             TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE)
                == 0) {
            if (!close && qAbs(sw - target.width()) > 2 && target.width() > 0 && target.height() > 0)
                img = img.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            return img;
        }
    }
    QBuffer buf;
    buf.setData(data);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf, "JPEG");
    *full = reader.size();
    return full->isValid() ? reader.read() : QImage();
}
} // namespace

CameraTile::CameraTile(MotionEyeClient *client, int cameraId, const QString &name, int fps, int idleFps,
                       QWidget *parent)
    : QWidget(parent)
    , m_client(client)
    , m_cameraId(cameraId)
    , m_name(name)
    , m_frameInterval(1000 / qMax(1, fps))
    , m_idleFrameInterval(idleFps > 0 ? qMax(1000 / idleFps, 1000 / qMax(1, fps)) : 1000 / qMax(1, fps))
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
    // No "width" parameter: with it motionEye (single-threaded Python) decodes, resizes and
    // re-encodes every frame, which saturates a CPU core. Without it the JPEG is passed through.
    m_cycle.start();
    m_reply = m_client->get(QStringLiteral("/picture/%1/current/").arg(m_cameraId),
                            {{QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch())}});
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
    // Decode straight to the on-screen size (libjpeg scales in the DCT domain, which is far
    // cheaper than decoding full size and smooth-scaling on every paint).
    QElapsedTimer decodeTimer;
    if (g_stats.on)
        decodeTimer.start();
    const QByteArray data = r->readAll();
    QSize full;
    QImage img = decodeJpeg(data, innerRect().size() * devicePixelRatioF(), &full);
    if (img.isNull()) {
        fail(QStringLiteral("Нет изображения"));
        return;
    }
    const QSize srcSize = full;
    if (g_stats.on) {
        g_stats.decodeNs += decodeTimer.nsecsElapsed();
        ++g_stats.frames;
        g_stats.bytes += data.size();
        g_stats.src = full;
        g_stats.dec = img.size();
        g_stats.motionKnown = m_motionKnown;
        g_stats.motion = m_motion;
        g_stats.interval = currentInterval();
        g_stats.report();
    }
    // motionEye flags a running motion event in a cookie of the frame response
    const QByteArray motionName = QByteArray("motion_detected_") + QByteArray::number(m_cameraId);
    const auto cookies = qvariant_cast<QList<QNetworkCookie>>(r->header(QNetworkRequest::SetCookieHeader));
    for (const QNetworkCookie &c : cookies) {
        if (c.name() == motionName) {
            const bool motion = c.value() == "true";
            m_motionKnown = true;
            if (motion)
                m_lastMotion.restart();
            if (motion != m_motion) {
                m_motion = motion;
                update();
            }
        }
    }
    m_frame = img;
    m_fails = 0;
    layoutButton();
    const double aspect = double(srcSize.width()) / srcSize.height();
    if (qAbs(aspect - property("aspect").toDouble()) > 0.01) {
        setProperty("aspect", aspect); // the mosaic sizes cells by the real camera shape
        if (parentWidget())
            QMetaObject::invokeMethod(parentWidget(), "relayout");
    }
    update();
    schedule(currentInterval() - int(m_cycle.elapsed()));
}

// Full rate while the camera reports motion (and for a few seconds after it, so the end of an event
// is smooth) or while we cannot tell; the slower idle rate otherwise.
int CameraTile::currentInterval() const
{
    constexpr qint64 holdMs = 5000;
    if (!m_motionKnown || m_motion || (m_lastMotion.isValid() && m_lastMotion.elapsed() < holdMs))
        return m_frameInterval;
    return m_idleFrameInterval;
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
    QElapsedTimer paintTimer;
    if (g_stats.on)
        paintTimer.start();
    struct PaintDone {
        QElapsedTimer &t;
        ~PaintDone()
        {
            if (g_stats.on) {
                g_stats.paintNs += t.nsecsElapsed();
                ++g_stats.paints;
            }
        }
    } paintDone{paintTimer};
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
    // The frame is decoded at about the size it is shown at. If it is within a few percent, blit it
    // 1:1 centred (crop or black padding of a few pixels) - resampling would cost more than decoding.
    const QSize want = img.size() * devicePixelRatioF();
    const bool near = qAbs(want.width() - m_frame.width()) <= kNearFit * want.width() + 2
        && qAbs(want.height() - m_frame.height()) <= kNearFit * want.height() + 2;
    if (near) {
        p.setClipRect(img);
        p.drawImage(QPoint(img.left() + (img.width() - m_frame.width()) / 2,
                           img.top() + (img.height() - m_frame.height()) / 2),
                    m_frame);
        p.setClipping(false);
    } else {
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        p.drawImage(img, m_frame);
    }

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
