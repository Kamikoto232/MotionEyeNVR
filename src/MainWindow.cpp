#include "MainWindow.h"

#include "CameraTile.h"
#include "MosaicWidget.h"
#include "MotionEyeClient.h"
#include "RecordingsDialog.h"
#include "SettingsDialog.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QNetworkReply>
#include <QToolBar>

MainWindow::MainWindow(const AppConfig &cfg, QWidget *parent)
    : QMainWindow(parent)
    , m_mosaic(new MosaicWidget(this))
    , m_toolbar(addToolBar(QStringLiteral("main")))
{
    setWindowTitle(QStringLiteral("MotionEye NVR"));
    resize(1280, 720);
    setCentralWidget(m_mosaic);

    m_toolbar->setMovable(false);
    m_toolbar->addAction(QStringLiteral("Обновить"), this, [this] {
        for (ServerState *s : std::as_const(m_serverList))
            fetchCameras(s);
    });
    m_toolbar->addAction(QStringLiteral("Настройки"), this, &MainWindow::showSettings);
    m_fullscreenAction = m_toolbar->addAction(QStringLiteral("Полный экран (F11)"), this, &MainWindow::toggleFullscreen);
    m_toolbar->setContextMenuPolicy(Qt::PreventContextMenu);

    m_clickTimer.setSingleShot(true);
    m_clickTimer.setInterval(QApplication::doubleClickInterval());
    connect(&m_clickTimer, &QTimer::timeout, this, &MainWindow::toggleHeaders);
    connect(m_mosaic, &MosaicWidget::clicked, &m_clickTimer, qOverload<>(&QTimer::start));

    connect(&m_timer, &QTimer::timeout, this, &MainWindow::tick);
    m_timer.start(5000);

    applyConfig(cfg);
    if (cfg.fullscreen)
        setFullscreen(true);
}

MainWindow::~MainWindow()
{
    qDeleteAll(m_serverList); // clients are children of the window
}

void MainWindow::applyConfig(const AppConfig &cfg)
{
    m_cfg = cfg;

    for (const QPointer<RecordingsDialog> &d : std::as_const(m_dialogs)) {
        if (d)
            d->close();
    }
    m_dialogs.clear();
    m_mosaic->setItems({}); // tiles reference the old clients
    for (ServerState *s : std::as_const(m_serverList)) {
        delete s->client;
        delete s;
    }
    m_serverList.clear();
    m_states.clear();

    for (const ServerConfig &sc : cfg.servers) {
        if (!sc.enabled)
            continue;
        auto *s = new ServerState;
        s->client = new MotionEyeClient(sc, this);
        m_serverList.append(s);
        m_states.insert(s->client, s);
    }
    rebuild();
    tick();
}

void MainWindow::tick()
{
    for (ServerState *s : std::as_const(m_serverList)) {
        // retry quickly until the server answers, then just watch for added/removed cameras
        if (!s->loaded || !s->lastFetch.isValid() || s->lastFetch.elapsed() > 60000)
            fetchCameras(s);
    }
}

void MainWindow::fetchCameras(ServerState *s)
{
    if (s->fetching)
        return;
    s->fetching = true;
    s->lastFetch.start();
    MotionEyeClient *c = s->client;
    c->authenticate(c, [this, c](bool ok, const QString &error) {
        if (!ok) {
            onFetchFailed(c, error);
            return;
        }
        QNetworkReply *r = c->get(QStringLiteral("/config/list/"));
        connect(r, &QNetworkReply::finished, c, [this, c, r] {
            r->deleteLater();
            onCameraList(c, r);
        });
    });
}

void MainWindow::onFetchFailed(MotionEyeClient *c, const QString &error)
{
    ServerState *s = stateFor(c);
    if (!s)
        return;
    s->fetching = false;
    const bool changed = s->error != error;
    s->error = error;
    if (changed && !s->loaded)
        rebuild(); // show the error in place of the cameras
}

void MainWindow::onCameraList(MotionEyeClient *c, QNetworkReply *r)
{
    const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (r->error() != QNetworkReply::NoError) {
        if (status == 401 || status == 403)
            c->invalidate();
        onFetchFailed(c, status ? QStringLiteral("HTTP %1").arg(status) : r->errorString());
        return;
    }
    ServerState *s = stateFor(c);
    if (!s)
        return;
    s->fetching = false;
    s->error.clear();

    QList<CameraInfo> cams;
    const QJsonArray arr = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("cameras")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        if (!o.value(QStringLiteral("enabled")).toBool(true))
            continue;
        cams.append({o.value(QStringLiteral("id")).toInt(), o.value(QStringLiteral("name")).toString()});
    }
    if (!s->loaded || cams != s->cameras) {
        s->loaded = true;
        s->cameras = cams;
        rebuild();
    }
}

void MainWindow::rebuild()
{
    QList<QWidget *> items;
    for (ServerState *s : std::as_const(m_serverList)) {
        if (!s->loaded) {
            auto *label = new QLabel(QStringLiteral("%1\n%2").arg(s->client->displayName(),
                                         s->error.isEmpty() ? QStringLiteral("Подключение…") : s->error),
                                     m_mosaic);
            label->setAlignment(Qt::AlignCenter);
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("background:#212121;color:#c8c8c8;"));
            items.append(label);
            continue;
        }
        for (const CameraInfo &ci : std::as_const(s->cameras)) {
            auto *tile = new CameraTile(s->client, ci.id, ci.name, m_cfg.fps, m_cfg.idleFps, m_mosaic);
            connect(tile, &CameraTile::recordingsRequested, this, &MainWindow::openRecordings);
            tile->setHeaderVisible(m_headersVisible);
            connect(tile, &CameraTile::clicked, &m_clickTimer, qOverload<>(&QTimer::start));
            connect(tile, &CameraTile::activated, this, [this](CameraTile *t) {
                m_clickTimer.stop();
                m_mosaic->setFocusItem(m_mosaic->focusItem() ? nullptr : t);
            });
            items.append(tile);
        }
    }
    if (items.isEmpty()) {
        auto *label = new QLabel(QStringLiteral("Нет камер.\nПравый клик → Настройки, чтобы добавить сервер motionEye."),
                                 m_mosaic);
        label->setAlignment(Qt::AlignCenter);
        label->setStyleSheet(QStringLiteral("background:#212121;color:#c8c8c8;"));
        items.append(label);
    }
    m_mosaic->setItems(items);
}

void MainWindow::openRecordings(CameraTile *tile)
{
    for (const QPointer<RecordingsDialog> &d : std::as_const(m_dialogs)) {
        if (d && d->client() == tile->client() && d->cameraId() == tile->cameraId()) {
            d->showNormal();
            d->raise();
            d->activateWindow();
            return;
        }
    }
    auto *dlg = new RecordingsDialog(tile->client(), tile->cameraId(), tile->title(), this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    m_dialogs.append(dlg);
    dlg->show();
}

void MainWindow::toggleHeaders()
{
    m_headersVisible = !m_headersVisible;
    for (CameraTile *t : m_mosaic->findChildren<CameraTile *>())
        t->setHeaderVisible(m_headersVisible);
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(m_cfg, this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    AppConfig cfg = dlg.result();
    cfg.fullscreen = isFullScreen();
    cfg.save();
    applyConfig(cfg);
}

void MainWindow::setFullscreen(bool on)
{
    m_toolbar->setVisible(!on);
    if (on)
        showFullScreen();
    else
        showNormal();
    m_cfg.fullscreen = on;
    m_cfg.save();
}

void MainWindow::toggleFullscreen()
{
    setFullscreen(!isFullScreen());
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_F11) {
        toggleFullscreen();
    } else if (e->key() == Qt::Key_Escape) {
        if (m_mosaic->focusItem())
            m_mosaic->setFocusItem(nullptr);
        else if (isFullScreen())
            setFullscreen(false);
    } else if (e->key() == Qt::Key_Q && (e->modifiers() & Qt::ControlModifier)) {
        qApp->quit();
    } else {
        QMainWindow::keyPressEvent(e);
    }
}

void MainWindow::contextMenuEvent(QContextMenuEvent *e)
{
    QMenu menu(this);
    menu.addAction(QStringLiteral("Настройки…"), this, &MainWindow::showSettings);
    menu.addAction(isFullScreen() ? QStringLiteral("Выйти из полноэкранного режима") : QStringLiteral("Полный экран"),
                   this, &MainWindow::toggleFullscreen);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Выход"), qApp, &QApplication::quit);
    menu.exec(e->globalPos());
}
