#include "MotionEyeClient.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

MotionEyeClient::MotionEyeClient(const ServerConfig &cfg, QObject *parent)
    : QObject(parent)
    , m_cfg(cfg)
    , m_nam(new QNetworkAccessManager(this)) // own cookie jar per server
{
    m_base = cfg.url.trimmed();
    if (!m_base.contains(QStringLiteral("://")))
        m_base.prepend(QStringLiteral("http://"));
    while (m_base.endsWith(QLatin1Char('/')))
        m_base.chop(1);
}

QString MotionEyeClient::displayName() const
{
    return m_cfg.name.isEmpty() ? m_cfg.url : m_cfg.name;
}

void MotionEyeClient::authenticate(QObject *context, AuthCallback cb)
{
    if (m_state == State::Ok) {
        cb(true, QString());
        return;
    }
    m_waiters.push_back({context, std::move(cb)});
    if (m_state == State::Busy)
        return;
    m_state = State::Busy;
    m_mode = Mode::Unknown;
    login();
}

void MotionEyeClient::invalidate()
{
    if (m_state == State::Ok)
        m_state = State::None;
}

void MotionEyeClient::finishAuth(bool ok, const QString &error)
{
    m_state = ok ? State::Ok : State::None;
    if (!ok)
        m_mode = Mode::Unknown;
    std::vector<Waiter> waiters;
    waiters.swap(m_waiters);
    for (Waiter &w : waiters) {
        if (w.context)
            w.fn(ok, error);
    }
}

void MotionEyeClient::login()
{
    QNetworkRequest req(QUrl(m_base + QStringLiteral("/login")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(10000);
    const QJsonObject body{{QStringLiteral("username"), m_cfg.user}, {QStringLiteral("password"), m_cfg.password}};
    QNetworkReply *r = m_nam->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        // motionEye < 0.44 answers every POST /login with an empty 200
        if (status == 200 && QJsonDocument::fromJson(body).object().contains(QStringLiteral("user"))) {
            m_mode = Mode::Session;
            finishAuth(true, QString());
        } else if (status == 0) {
            finishAuth(false, r->errorString());
        } else if (status == 401 && body.contains("invalid credentials")) {
            finishAuth(false, QStringLiteral("Неверный логин или пароль"));
        } else {
            probeLegacy(0); // older motionEye without /login
        }
    });
}

void MotionEyeClient::probeLegacy(int candidate)
{
    // the signing key is the stored admin password: either the sha1 hash or the plain text
    const QString sha = QString::fromLatin1(
        QCryptographicHash::hash(m_cfg.password.toUtf8(), QCryptographicHash::Sha1).toHex());
    const QStringList keys{sha, m_cfg.password};
    if (candidate >= keys.size()) {
        finishAuth(false, QStringLiteral("Ошибка авторизации (проверьте логин и пароль)"));
        return;
    }
    m_mode = Mode::Legacy;
    m_legacyKey = keys.at(candidate);

    QNetworkRequest req(buildUrl(QStringLiteral("/config/list/"), {}));
    req.setTransferTimeout(10000);
    QNetworkReply *r = m_nam->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, candidate] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200)
            finishAuth(true, QString());
        else if (status == 0)
            finishAuth(false, r->errorString());
        else
            probeLegacy(candidate + 1);
    });
}

QString MotionEyeClient::legacySignature(const QString &method, const QString &path, Query query, const QString &key)
{
    // mirrors motioneye.utils.compute_signature()
    std::stable_sort(query.begin(), query.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    QString q;
    for (const auto &p : query) {
        if (!q.isEmpty())
            q += QLatin1Char('&');
        q += p.first + QLatin1Char('=') + QString::fromLatin1(QUrl::toPercentEncoding(p.second, "!'()*~"));
    }
    QString full = path;
    if (!q.isEmpty())
        full += QLatin1Char('?') + q;

    static const QRegularExpression bad(QStringLiteral("[^a-zA-Z0-9/?_.=&{}\\[\\]\":, _-]"));
    full.replace(bad, QStringLiteral("-"));
    QString k = key;
    k.replace(bad, QStringLiteral("-"));

    const QByteArray data = (method + QLatin1Char(':') + full + QStringLiteral("::") + k).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

QUrl MotionEyeClient::buildUrl(const QString &path, Query query) const
{
    if (m_mode == Mode::Legacy) {
        query.append({QStringLiteral("_username"), m_cfg.user});
        query.append({QStringLiteral("_signature"), legacySignature(QStringLiteral("GET"), path, query, m_legacyKey)});
    }
    QString q;
    for (const auto &p : query) {
        if (!q.isEmpty())
            q += QLatin1Char('&');
        q += QString::fromLatin1(QUrl::toPercentEncoding(p.first)) + QLatin1Char('=')
            + QString::fromLatin1(QUrl::toPercentEncoding(p.second, "!'()*~"));
    }
    return QUrl(m_base + path + (q.isEmpty() ? QString() : QLatin1Char('?') + q));
}

QNetworkReply *MotionEyeClient::get(const QString &path, const Query &query, int timeoutMs)
{
    QNetworkRequest req(buildUrl(path, query));
    req.setTransferTimeout(timeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return m_nam->get(req);
}
