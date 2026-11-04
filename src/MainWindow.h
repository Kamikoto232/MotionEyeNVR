#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>

#include "Config.h"

class CameraTile;
class MosaicWidget;
class MotionEyeClient;
class RecordingsDialog;
class QToolBar;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(const AppConfig &cfg, QWidget *parent = nullptr);
    ~MainWindow() override;

    void toggleFullscreen();
    void setFullscreen(bool on);

protected:
    void keyPressEvent(QKeyEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;

private:
    struct CameraInfo {
        int id;
        QString name;
        bool operator==(const CameraInfo &o) const { return id == o.id && name == o.name; }
    };
    struct ServerState {
        MotionEyeClient *client = nullptr;
        QList<CameraInfo> cameras;
        QString error;
        bool loaded = false;
        bool fetching = false;
        QElapsedTimer lastFetch;
    };

    void applyConfig(const AppConfig &cfg);
    void tick();
    void fetchCameras(ServerState *s);
    void onCameraList(MotionEyeClient *c, class QNetworkReply *r);
    void onFetchFailed(MotionEyeClient *c, const QString &error);
    ServerState *stateFor(MotionEyeClient *c) const { return m_states.value(c); }
    void rebuild();
    void openRecordings(CameraTile *tile);
    void showSettings();
    void toggleHeaders();

    AppConfig m_cfg;
    MosaicWidget *m_mosaic;
    QToolBar *m_toolbar;
    QAction *m_fullscreenAction;
    QTimer m_timer;
    QTimer m_clickTimer; // delays the single-click action so a double click can cancel it
    bool m_headersVisible = true;
    QList<ServerState *> m_serverList;
    QHash<MotionEyeClient *, ServerState *> m_states;
    QList<QPointer<RecordingsDialog>> m_dialogs;
};
