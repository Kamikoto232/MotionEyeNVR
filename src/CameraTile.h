#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QWidget>

class MotionEyeClient;
class QNetworkReply;
class QPushButton;

// One camera of the mosaic. Pulls JPEG frames from /picture/<id>/current/ strictly one
// request at a time, and only while the widget is visible.
class CameraTile : public QWidget
{
    Q_OBJECT
public:
    CameraTile(MotionEyeClient *client, int cameraId, const QString &name, int fps, QWidget *parent = nullptr);

    MotionEyeClient *client() const { return m_client; }
    int cameraId() const { return m_cameraId; }
    const QString &cameraName() const { return m_name; }
    QString title() const;
    void setHeaderVisible(bool visible);

signals:
    void recordingsRequested(CameraTile *tile);
    void activated(CameraTile *tile);
    void clicked();

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    void poll();
    void fetch();
    void onFinished();
    void fail(const QString &message);
    void schedule(int ms);
    void cancel();
    int framePad() const;
    QRect innerRect() const;
    QRect imageRect() const;
    int headerHeight() const;
    void layoutButton();

    QPointer<MotionEyeClient> m_client;
    int m_cameraId;
    QString m_name;
    int m_frameInterval;

    QImage m_frame;
    QString m_status = QStringLiteral("Подключение…");
    QNetworkReply *m_reply = nullptr;
    QTimer m_timer;
    QElapsedTimer m_cycle;
    bool m_active = false;
    bool m_authPending = false;
    bool m_headerVisible = true;
    bool m_motion = false; // motionEye reports motion (recording) for this camera
    bool m_ignoreRelease = false;
    int m_fails = 0;
    QPushButton *m_recButton;
};
