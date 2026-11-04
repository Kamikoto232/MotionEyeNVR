#pragma once

#include <QDateTime>
#include <QDialog>
#include <QFile>
#include <QPointer>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QVector>

class MotionEyeClient;
class QCheckBox;
class QComboBox;
class QLabel;
class QMediaPlayer;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QSlider;
class QTreeWidget;
class QTreeWidgetItem;
class QVideoWidget;

// Lists the recordings of one camera grouped by day and plays them.
// Clips are downloaded to a temporary file first (works the same for local and remote
// servers and gives seeking for free); only the current clip is kept on disk.
class RecordingsDialog : public QDialog
{
    Q_OBJECT
public:
    RecordingsDialog(MotionEyeClient *client, int cameraId, const QString &title, QWidget *parent = nullptr);
    ~RecordingsDialog() override;

    MotionEyeClient *client() const { return m_client; }
    int cameraId() const { return m_cameraId; }

private:
    struct Clip {
        QString path;
        QDateTime when;
        QString size;
    };

    void loadList();
    void onList(QNetworkReply *r);
    void fillTree(const QString &keepPath);
    void resetThumbs();
    void queueThumbs(QTreeWidgetItem *day);
    void pumpThumbs();
    void play(int index);
    void stepClip(int delta);
    void abortDownload();
    void downloadFinished();
    void releaseCurrentFile();
    void seekRelative(qint64 ms);
    void updateTime();

    QPointer<MotionEyeClient> m_client;
    int m_cameraId;

    QTreeWidget *m_tree;
    QLabel *m_status;
    QProgressBar *m_progress;
    QVideoWidget *m_video;
    QMediaPlayer *m_player;
    QSlider *m_slider;
    QLabel *m_time;
    QPushButton *m_playBtn;
    QComboBox *m_speed;
    QCheckBox *m_autoNext;

    QVector<Clip> m_clips; // chronological
    QVector<QTreeWidgetItem *> m_items; // parallel to m_clips
    int m_current = -1;
    QPointer<QNetworkReply> m_listReply;
    QPointer<QNetworkReply> m_dlReply;
    QFile m_dlFile;
    QTimer m_stall;
    QList<int> m_thumbQueue;
    QSet<int> m_thumbQueued;
    QList<QPointer<QNetworkReply>> m_thumbReplies;
    quint64 m_thumbGen = 0;
    QTemporaryDir m_tmp;
    QString m_playingFile;
    int m_seq = 0;
    bool m_sliding = false;
};
