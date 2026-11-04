#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QPair>
#include <QPointer>

#include <functional>
#include <vector>

#include "Config.h"

class QNetworkReply;

// HTTP client for one motionEye instance (local or remote).
// Supports both authentication schemes in the wild:
//  * motionEye >= 0.44: POST /login, session cookie
//  * older versions:    "_username" + "_signature" query parameters
class MotionEyeClient : public QObject
{
    Q_OBJECT
public:
    using Query = QList<QPair<QString, QString>>;
    using AuthCallback = std::function<void(bool ok, const QString &error)>;

    explicit MotionEyeClient(const ServerConfig &cfg, QObject *parent = nullptr);

    const ServerConfig &config() const { return m_cfg; }
    QString displayName() const;

    // Calls cb once authenticated (possibly immediately). The callback is dropped if
    // `context` is destroyed meanwhile. Concurrent callers share a single login.
    void authenticate(QObject *context, AuthCallback cb);

    // `path` must already be percent-encoded. Only valid after a successful authenticate().
    // timeoutMs == 0 disables the transfer timeout.
    QNetworkReply *get(const QString &path, const Query &query = {}, int timeoutMs = 15000);

    // Call after HTTP 401/403 so the next authenticate() logs in again.
    void invalidate();

private:
    enum class State { None, Busy, Ok };
    enum class Mode { Unknown, Session, Legacy };
    struct Waiter {
        QPointer<QObject> context;
        AuthCallback fn;
    };

    void login();
    void probeLegacy(int candidate);
    void finishAuth(bool ok, const QString &error);
    QUrl buildUrl(const QString &path, Query query) const;
    static QString legacySignature(const QString &method, const QString &path, Query query, const QString &key);

    ServerConfig m_cfg;
    QString m_base;
    QNetworkAccessManager *m_nam;
    State m_state = State::None;
    Mode m_mode = Mode::Unknown;
    QString m_legacyKey;
    std::vector<Waiter> m_waiters;
};
