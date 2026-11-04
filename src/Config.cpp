#include "Config.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

QString AppConfig::filePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/config.json");
}

AppConfig AppConfig::load()
{
    AppConfig cfg;
    QFile f(filePath());
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        cfg.fps = qBound(1, root.value(QStringLiteral("fps")).toInt(5), 25);
        cfg.fullscreen = root.value(QStringLiteral("fullscreen")).toBool(false);
        const QJsonArray arr = root.value(QStringLiteral("servers")).toArray();
        for (const QJsonValue &v : arr) {
            const QJsonObject o = v.toObject();
            ServerConfig s;
            s.name = o.value(QStringLiteral("name")).toString();
            s.url = o.value(QStringLiteral("url")).toString();
            s.user = o.value(QStringLiteral("user")).toString(QStringLiteral("admin"));
            s.password = o.value(QStringLiteral("password")).toString();
            s.enabled = o.value(QStringLiteral("enabled")).toBool(true);
            if (!s.url.isEmpty())
                cfg.servers.append(s);
        }
        return cfg;
    }

    // first start: the local docker instance
    ServerConfig local;
    local.name = QStringLiteral("Локальный");
    local.url = QStringLiteral("http://localhost:8765");
    cfg.servers.append(local);
    cfg.save();
    return cfg;
}

bool AppConfig::save() const
{
    QJsonArray arr;
    for (const ServerConfig &s : servers) {
        arr.append(QJsonObject{{QStringLiteral("name"), s.name},
                               {QStringLiteral("url"), s.url},
                               {QStringLiteral("user"), s.user},
                               {QStringLiteral("password"), s.password},
                               {QStringLiteral("enabled"), s.enabled}});
    }
    const QJsonObject root{{QStringLiteral("fps"), fps},
                           {QStringLiteral("fullscreen"), fullscreen},
                           {QStringLiteral("servers"), arr}};

    const QString path = filePath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit())
        return false;
    // the file holds passwords
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}
