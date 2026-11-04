#include "RecordingsDialog.h"

#include "MotionEyeClient.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMediaPlayer>
#include <QNetworkReply>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QShortcut>
#include <QSlider>
#include <QSplitter>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVideoWidget>

#include <algorithm>
#include <functional>
#include <map>

namespace {
constexpr int IndexRole = Qt::UserRole;

QString formatTime(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}

// Slider that jumps to the clicked position instead of paging towards it, then keeps dragging.
class SeekSlider : public QSlider
{
public:
    using QSlider::QSlider;
    std::function<void(int)> onSeek;

protected:
    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton && maximum() > minimum()) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            const QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
            const QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            const int span = groove.width() - handle.width();
            const int pos = int(e->position().x()) - groove.x() - handle.width() / 2;
            const int v = QStyle::sliderValueFromPosition(minimum(), maximum(), qBound(0, pos, span), span, opt.upsideDown);
            setValue(v);
            if (onSeek)
                onSeek(v);
        }
        QSlider::mousePressEvent(e);
    }
};

QString cacheRoot()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    return dir;
}
} // namespace

RecordingsDialog::RecordingsDialog(MotionEyeClient *client, int cameraId, const QString &title, QWidget *parent)
    : QDialog(parent, Qt::Window)
    , m_client(client)
    , m_cameraId(cameraId)
    , m_tree(new QTreeWidget(this))
    , m_status(new QLabel(this))
    , m_progress(new QProgressBar(this))
    , m_video(new QVideoWidget(this))
    , m_player(new QMediaPlayer(this))
    , m_slider(new SeekSlider(Qt::Horizontal, this))
    , m_time(new QLabel(QStringLiteral("00:00 / 00:00"), this))
    , m_playBtn(new QPushButton(this))
    , m_speed(new QComboBox(this))
    , m_autoNext(new QCheckBox(QStringLiteral("Автопереход к следующей"), this))
    , m_tmp(cacheRoot() + QStringLiteral("/clip-XXXXXX"))
{
    setWindowTitle(QStringLiteral("Записи — %1").arg(title));
    resize(1280, 720);

    m_tree->setHeaderHidden(true);
    m_tree->setMinimumWidth(240);
    m_tree->setRootIsDecorated(true);
    m_tree->setIconSize(QSize(96, 72));
    static_cast<SeekSlider *>(m_slider)->onSeek = [this](int v) {
        m_player->setPosition(v);
        if (m_player->playbackState() == QMediaPlayer::StoppedState && !m_player->source().isEmpty())
            m_player->play(); // clip had ended: resume from the clicked point
    };
    auto *reload = new QPushButton(QStringLiteral("Обновить список"), this);

    auto *left = new QWidget(this);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(m_tree, 1);
    leftLayout->addWidget(reload);

    m_video->setMinimumSize(320, 180);
    m_player->setVideoOutput(m_video);

    auto *prev = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipBackward), QString(), this);
    auto *next = new QPushButton(style()->standardIcon(QStyle::SP_MediaSkipForward), QString(), this);
    auto *full = new QPushButton(QStringLiteral("На весь экран"), this);
    m_playBtn->setIcon(style()->standardIcon(QStyle::SP_MediaPlay));
    prev->setToolTip(QStringLiteral("Предыдущая запись"));
    next->setToolTip(QStringLiteral("Следующая запись"));
    full->setToolTip(QStringLiteral("Видео на весь экран"));
    for (const double rate : {0.5, 1.0, 2.0, 4.0, 8.0})
        m_speed->addItem(QStringLiteral("%1×").arg(rate), rate);
    m_speed->setCurrentIndex(1);
    m_autoNext->setChecked(true);
    m_progress->setRange(0, 100);
    m_progress->hide();

    auto *controls = new QHBoxLayout;
    controls->addWidget(prev);
    controls->addWidget(m_playBtn);
    controls->addWidget(next);
    controls->addWidget(m_slider, 1);
    controls->addWidget(m_time);
    controls->addWidget(m_speed);
    controls->addWidget(full);

    auto *right = new QWidget(this);
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(m_video, 1);
    rightLayout->addLayout(controls);
    rightLayout->addWidget(m_autoNext);
    rightLayout->addWidget(m_progress);
    rightLayout->addWidget(m_status);

    auto *split = new QSplitter(this);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({300, 980});
    auto *root = new QVBoxLayout(this);
    root->addWidget(split);

    m_stall.setSingleShot(true);
    m_stall.setInterval(30000);
    connect(&m_stall, &QTimer::timeout, this, [this] {
        abortDownload();
        m_status->setText(QStringLiteral("Загрузка прервана: нет данных от сервера"));
    });

    connect(reload, &QPushButton::clicked, this, &RecordingsDialog::loadList);
    connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it) {
        const int idx = it->data(0, IndexRole).toInt();
        if (idx >= 0 && idx != m_current)
            play(idx);
    });
    connect(m_tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *it) {
        const int idx = it->data(0, IndexRole).toInt();
        if (idx >= 0 && idx != m_current)
            play(idx);
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *it) { queueThumbs(it); });
    connect(prev, &QPushButton::clicked, this, [this] { stepClip(-1); });
    connect(next, &QPushButton::clicked, this, [this] { stepClip(+1); });
    connect(full, &QPushButton::clicked, this, [this] { m_video->setFullScreen(true); });
    connect(m_playBtn, &QPushButton::clicked, this, [this] {
        if (m_player->playbackState() == QMediaPlayer::PlayingState)
            m_player->pause();
        else if (!m_player->source().isEmpty())
            m_player->play();
    });
    connect(m_speed, &QComboBox::currentIndexChanged, this, [this](int i) {
        m_player->setPlaybackRate(m_speed->itemData(i).toDouble());
    });

    connect(m_slider, &QSlider::sliderPressed, this, [this] { m_sliding = true; });
    connect(m_slider, &QSlider::sliderReleased, this, [this] {
        m_sliding = false;
        m_player->setPosition(m_slider->value());
    });
    connect(m_slider, &QSlider::sliderMoved, this, [this](int v) {
        m_time->setText(QStringLiteral("%1 / %2").arg(formatTime(v), formatTime(m_player->duration())));
    });
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 d) {
        m_slider->setRange(0, int(d));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 p) {
        if (!m_sliding)
            m_slider->setValue(int(p));
        updateTime();
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState st) {
        m_playBtn->setIcon(style()->standardIcon(st == QMediaPlayer::PlayingState ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    });
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus st) {
        if (st == QMediaPlayer::EndOfMedia && m_autoNext->isChecked() && m_current >= 0 && m_current + 1 < m_clips.size())
            play(m_current + 1);
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &msg) {
        m_status->setText(QStringLiteral("Ошибка воспроизведения: %1").arg(msg));
    });

    auto *sp = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(sp, &QShortcut::activated, m_playBtn, &QPushButton::click);
    connect(new QShortcut(QKeySequence(Qt::Key_Left), this), &QShortcut::activated, this, [this] { seekRelative(-5000); });
    connect(new QShortcut(QKeySequence(Qt::Key_Right), this), &QShortcut::activated, this, [this] { seekRelative(5000); });

    loadList();
}

RecordingsDialog::~RecordingsDialog()
{
    abortDownload();
    resetThumbs();
    if (m_listReply) {
        m_listReply->disconnect(this);
        m_listReply->abort();
    }
    m_player->stop();
    m_player->setSource(QUrl());
}

void RecordingsDialog::updateTime()
{
    if (!m_sliding)
        m_time->setText(QStringLiteral("%1 / %2").arg(formatTime(m_player->position()), formatTime(m_player->duration())));
}

void RecordingsDialog::seekRelative(qint64 ms)
{
    if (m_player->source().isEmpty())
        return;
    m_player->setPosition(qBound<qint64>(0, m_player->position() + ms, m_player->duration()));
}

void RecordingsDialog::loadList()
{
    if (!m_client || m_listReply)
        return;
    m_status->setText(QStringLiteral("Загрузка списка записей…"));
    m_client->authenticate(this, [this](bool ok, const QString &error) {
        if (!ok) {
            m_status->setText(error);
            return;
        }
        m_listReply = m_client->get(QStringLiteral("/movie/%1/list/").arg(m_cameraId), {}, 120000);
        connect(m_listReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *r = m_listReply;
            m_listReply = nullptr;
            r->deleteLater();
            onList(r);
        });
    });
}

void RecordingsDialog::onList(QNetworkReply *r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (r->error() != QNetworkReply::NoError) {
        if ((status == 401 || status == 403) && m_client)
            m_client->invalidate();
        m_status->setText(QStringLiteral("Не удалось получить список: %1").arg(r->errorString()));
        return;
    }
    const QJsonObject root = QJsonDocument::fromJson(r->readAll()).object();
    if (root.contains(QStringLiteral("error"))) {
        m_status->setText(root.value(QStringLiteral("error")).toString());
        return;
    }

    static const QRegularExpression stamp(QStringLiteral("(\\d{4}-\\d{2}-\\d{2})\\D+(\\d{2})\\D(\\d{2})\\D(\\d{2})"));
    const QString keep = (m_current >= 0 && m_current < m_clips.size()) ? m_clips[m_current].path : QString();
    m_clips.clear();
    const QJsonArray arr = (root.contains(QStringLiteral("mediaList")) ? root.value(QStringLiteral("mediaList"))
                                                                        : root.value(QStringLiteral("media"))).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Clip c;
        c.path = o.value(QStringLiteral("path")).toString();
        c.size = o.value(QStringLiteral("sizeStr")).toString();
        // the file layout is <day>/<HH-MM-SS>; fall back to the file's mtime
        const auto m = stamp.match(c.path);
        if (m.hasMatch())
            c.when = QDateTime(QDate::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd")),
                               QTime(m.captured(2).toInt(), m.captured(3).toInt(), m.captured(4).toInt()));
        if (!c.when.isValid())
            c.when = QDateTime::fromSecsSinceEpoch(qint64(o.value(QStringLiteral("timestamp")).toDouble()));
        m_clips.append(c);
    }
    std::sort(m_clips.begin(), m_clips.end(), [](const Clip &a, const Clip &b) { return a.when < b.when; });
    fillTree(keep);
    m_status->setText(m_clips.isEmpty() ? QStringLiteral("Записей нет")
                                        : QStringLiteral("Записей: %1").arg(m_clips.size()));
}

void RecordingsDialog::fillTree(const QString &keep)
{
    resetThumbs();
    m_tree->clear();
    m_items = QVector<QTreeWidgetItem *>(m_clips.size(), nullptr);
    m_current = -1;

    std::map<QDate, QList<int>> days;
    for (int i = 0; i < m_clips.size(); ++i)
        days[m_clips[i].when.date()].append(i);

    const QLocale loc;
    bool first = true;
    for (auto it = days.crbegin(); it != days.crend(); ++it) { // newest day first
        auto *dayItem = new QTreeWidgetItem(m_tree);
        dayItem->setText(0, QStringLiteral("%1 — %2").arg(loc.toString(it->first, QStringLiteral("dd.MM.yyyy, dddd"))).arg(it->second.size()));
        dayItem->setData(0, IndexRole, -1);
        QFont f = dayItem->font(0);
        f.setBold(true);
        dayItem->setFont(0, f);
        for (const int i : it->second) {
            auto *clipItem = new QTreeWidgetItem(dayItem);
            clipItem->setText(0, QStringLiteral("%1    %2").arg(m_clips[i].when.time().toString(QStringLiteral("HH:mm:ss")), m_clips[i].size));
            clipItem->setData(0, IndexRole, i);
            m_items[i] = clipItem;
            if (m_clips[i].path == keep) {
                m_current = i;
                m_tree->setCurrentItem(clipItem);
            }
        }
        dayItem->setExpanded(first || (m_current >= 0 && m_items[m_current]->parent() == dayItem));
        if (dayItem->isExpanded())
            queueThumbs(dayItem);
        first = false;
    }
}

void RecordingsDialog::resetThumbs()
{
    ++m_thumbGen;
    m_thumbQueue.clear();
    m_thumbQueued.clear();
    for (const QPointer<QNetworkReply> &r : std::as_const(m_thumbReplies)) {
        if (r) {
            r->disconnect(this);
            r->abort();
            r->deleteLater();
        }
    }
    m_thumbReplies.clear();
}

// Previews are fetched only for expanded days, two at a time.
void RecordingsDialog::queueThumbs(QTreeWidgetItem *day)
{
    for (int i = 0; i < day->childCount(); ++i) {
        const int idx = day->child(i)->data(0, IndexRole).toInt();
        if (idx >= 0 && !m_thumbQueued.contains(idx)) {
            m_thumbQueued.insert(idx);
            m_thumbQueue.append(idx);
        }
    }
    pumpThumbs();
}

void RecordingsDialog::pumpThumbs()
{
    while (m_client && m_thumbReplies.size() < 2 && !m_thumbQueue.isEmpty()) {
        const int idx = m_thumbQueue.takeFirst();
        const quint64 gen = m_thumbGen;
        const QString path = QStringLiteral("/movie/%1/preview").arg(m_cameraId)
            + QString::fromLatin1(QUrl::toPercentEncoding(m_clips[idx].path, "/"));
        QNetworkReply *r = m_client->get(path, {{QStringLiteral("width"), QStringLiteral("192")}}, 30000);
        m_thumbReplies.append(r);
        connect(r, &QNetworkReply::finished, this, [this, r, idx, gen] {
            r->deleteLater();
            if (gen != m_thumbGen)
                return; // list was reloaded, resetThumbs() already dropped the bookkeeping
            m_thumbReplies.removeAll(r);
            QImage img;
            if (r->error() == QNetworkReply::NoError && img.loadFromData(r->readAll()) && idx < m_items.size() && m_items[idx])
                m_items[idx]->setIcon(0, QIcon(QPixmap::fromImage(img).scaled(m_tree->iconSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
            pumpThumbs();
        });
    }
}

void RecordingsDialog::abortDownload()
{
    m_stall.stop();
    if (m_dlReply) {
        QNetworkReply *r = m_dlReply;
        m_dlReply = nullptr;
        r->disconnect(this);
        r->abort();
        r->deleteLater();
    }
    if (m_dlFile.isOpen()) {
        m_dlFile.close();
        m_dlFile.remove();
    }
    m_progress->hide();
}

void RecordingsDialog::releaseCurrentFile()
{
    m_player->stop();
    m_player->setSource(QUrl());
    if (!m_playingFile.isEmpty()) {
        QFile::remove(m_playingFile);
        m_playingFile.clear();
    }
}

void RecordingsDialog::stepClip(int delta)
{
    const int idx = (m_current < 0 ? (delta > 0 ? 0 : m_clips.size() - 1) : m_current + delta);
    if (idx >= 0 && idx < m_clips.size())
        play(idx);
}

void RecordingsDialog::play(int index)
{
    if (!m_client || index < 0 || index >= m_clips.size())
        return;
    abortDownload();
    releaseCurrentFile();
    m_current = index;
    if (m_items[index]) {
        m_tree->setCurrentItem(m_items[index]);
        m_tree->scrollToItem(m_items[index]);
    }

    const Clip &clip = m_clips[index];
    const QString suffix = QFileInfo(clip.path).suffix();
    m_dlFile.setFileName(m_tmp.filePath(QStringLiteral("clip%1.%2").arg(++m_seq).arg(suffix.isEmpty() ? QStringLiteral("mp4") : suffix)));
    if (!m_dlFile.open(QIODevice::WriteOnly)) {
        m_status->setText(QStringLiteral("Не удалось создать временный файл: %1").arg(m_dlFile.errorString()));
        return;
    }
    m_status->setText(QStringLiteral("Загрузка %1…").arg(clip.when.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss"))));
    m_progress->setValue(0);
    m_progress->show();

    const int seq = m_seq;
    const QString path = QStringLiteral("/movie/%1/download").arg(m_cameraId)
        + QString::fromLatin1(QUrl::toPercentEncoding(clip.path, "/"));
    m_client->authenticate(this, [this, path, seq](bool ok, const QString &error) {
        if (seq != m_seq || !m_dlFile.isOpen())
            return; // another clip was chosen meanwhile
        if (!ok) {
            abortDownload();
            m_status->setText(error);
            return;
        }
        m_dlReply = m_client->get(path, {}, 0);
        m_stall.start();
        connect(m_dlReply, &QNetworkReply::readyRead, this, [this] {
            m_dlFile.write(m_dlReply->readAll());
            m_stall.start();
        });
        connect(m_dlReply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
            if (total > 0)
                m_progress->setValue(int(got * 100 / total));
        });
        connect(m_dlReply, &QNetworkReply::finished, this, &RecordingsDialog::downloadFinished);
    });
}

void RecordingsDialog::downloadFinished()
{
    QNetworkReply *r = m_dlReply;
    m_dlReply = nullptr;
    r->deleteLater();
    m_stall.stop();
    m_progress->hide();

    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString error = r->error() == QNetworkReply::NoError ? QString() : r->errorString();
    if (error.isEmpty())
        m_dlFile.write(r->readAll());
    const QString file = m_dlFile.fileName();
    const qint64 size = m_dlFile.size();
    m_dlFile.close();

    if (!error.isEmpty() || size == 0) {
        if ((status == 401 || status == 403) && m_client)
            m_client->invalidate();
        QFile::remove(file);
        m_status->setText(QStringLiteral("Не удалось загрузить запись: %1").arg(error.isEmpty() ? QStringLiteral("пустой файл") : error));
        return;
    }
    m_playingFile = file;
    m_status->setText(m_clips[m_current].when.toString(QStringLiteral("dd.MM.yyyy HH:mm:ss")));
    m_player->setSource(QUrl::fromLocalFile(file));
    m_player->setPlaybackRate(m_speed->currentData().toDouble());
    m_player->play();
}
