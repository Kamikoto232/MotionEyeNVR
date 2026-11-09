#pragma once

#include <QList>
#include <QString>

struct ServerConfig {
    QString name;
    QString url;
    QString user = QStringLiteral("admin");
    QString password;
    bool enabled = true;
};

struct AppConfig {
    QList<ServerConfig> servers;
    int fps = 3;
    int idleFps = 5; // rate of a camera without motion; 0 = always use `fps`
    bool fullscreen = false;

    static QString filePath();
    static AppConfig load();
    bool save() const;
};
