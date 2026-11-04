#include "Config.h"
#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QLockFile>
#include <QPalette>
#include <QStandardPaths>
#include <QStyleFactory>

#include <unistd.h>

static void applyDarkTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, QColor(37, 37, 38));
    p.setColor(QPalette::WindowText, QColor(220, 220, 220));
    p.setColor(QPalette::Base, QColor(30, 30, 30));
    p.setColor(QPalette::AlternateBase, QColor(45, 45, 48));
    p.setColor(QPalette::Text, QColor(220, 220, 220));
    p.setColor(QPalette::Button, QColor(55, 55, 58));
    p.setColor(QPalette::ButtonText, QColor(220, 220, 220));
    p.setColor(QPalette::ToolTipBase, QColor(45, 45, 48));
    p.setColor(QPalette::ToolTipText, QColor(220, 220, 220));
    p.setColor(QPalette::Highlight, QColor(38, 110, 190));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::Text, QColor(120, 120, 120));
    p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(120, 120, 120));
    app.setPalette(p);
}

// Clips of a previous run that crashed
static void cleanStaleCache()
{
    QDir cache(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    const QStringList stale = cache.entryList({QStringLiteral("clip-*")}, QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &d : stale)
        QDir(cache.filePath(d)).removeRecursively();
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("motioneye-nvr"));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("DVR-клиент для motionEye"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption fullscreenOpt(QStringLiteral("fullscreen"), QStringLiteral("Запустить в полноэкранном режиме"));
    const QCommandLineOption windowedOpt(QStringLiteral("windowed"), QStringLiteral("Запустить в окне"));
    parser.addOption(fullscreenOpt);
    parser.addOption(windowedOpt);
    parser.process(app);

    // one instance per user (autostart + manual launch)
    QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = QDir::tempPath();
    QLockFile lock(runtime + QStringLiteral("/motioneye-nvr-%1.lock").arg(getuid()));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 0;

    cleanStaleCache();
    applyDarkTheme(app);

    AppConfig cfg = AppConfig::load();
    if (parser.isSet(fullscreenOpt))
        cfg.fullscreen = true;
    if (parser.isSet(windowedOpt))
        cfg.fullscreen = false;

    MainWindow w(cfg);
    if (cfg.fullscreen)
        w.show(); // the window already requested fullscreen
    else
        w.showMaximized();
    return app.exec();
}
