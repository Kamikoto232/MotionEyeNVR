#include "SettingsDialog.h"

#include "MotionEyeClient.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QNetworkReply>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

SettingsDialog::SettingsDialog(const AppConfig &cfg, QWidget *parent)
    : QDialog(parent)
    , m_cfg(cfg)
    , m_list(new QListWidget(this))
    , m_name(new QLineEdit(this))
    , m_url(new QLineEdit(this))
    , m_user(new QLineEdit(this))
    , m_password(new QLineEdit(this))
    , m_enabled(new QCheckBox(QStringLiteral("Включен"), this))
    , m_testResult(new QLabel(this))
    , m_fps(new QSpinBox(this))
    , m_fullscreen(new QCheckBox(QStringLiteral("Запускать в полноэкранном режиме"), this))
{
    setWindowTitle(QStringLiteral("Настройки"));
    resize(720, 420);

    m_password->setEchoMode(QLineEdit::Password);
    m_url->setPlaceholderText(QStringLiteral("http://192.168.1.10:8765"));
    m_fps->setRange(1, 25);
    m_fps->setValue(cfg.fps);
    m_fps->setSuffix(QStringLiteral(" кадр/с"));
    m_fullscreen->setChecked(cfg.fullscreen);
    m_testResult->setWordWrap(true);

    auto *add = new QPushButton(QStringLiteral("Добавить"), this);
    auto *remove = new QPushButton(QStringLiteral("Удалить"), this);
    auto *test = new QPushButton(QStringLiteral("Проверить подключение"), this);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("Название"), m_name);
    form->addRow(QStringLiteral("Адрес"), m_url);
    form->addRow(QStringLiteral("Пользователь"), m_user);
    form->addRow(QStringLiteral("Пароль"), m_password);
    form->addRow(QString(), m_enabled);
    form->addRow(QString(), test);
    form->addRow(QString(), m_testResult);

    auto *leftButtons = new QHBoxLayout;
    leftButtons->addWidget(add);
    leftButtons->addWidget(remove);
    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("Серверы motionEye:"), this));
    left->addWidget(m_list, 1);
    left->addLayout(leftButtons);

    auto *cols = new QHBoxLayout;
    cols->addLayout(left, 1);
    cols->addLayout(form, 2);

    auto *general = new QFormLayout;
    general->addRow(QStringLiteral("Частота обновления камер"), m_fps);
    general->addRow(QString(), m_fullscreen);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto *root = new QVBoxLayout(this);
    root->addLayout(cols, 1);
    root->addLayout(general);
    root->addWidget(buttons);

    for (int i = 0; i < m_cfg.servers.size(); ++i)
        m_list->addItem(QString());
    for (int i = 0; i < m_cfg.servers.size(); ++i)
        refreshLabel(i);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        storeServer(m_row);
        showServer(row);
    });
    connect(add, &QPushButton::clicked, this, &SettingsDialog::addServer);
    connect(remove, &QPushButton::clicked, this, &SettingsDialog::removeServer);
    connect(test, &QPushButton::clicked, this, &SettingsDialog::testServer);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        storeServer(m_row);
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (!m_cfg.servers.isEmpty())
        m_list->setCurrentRow(0);
    else
        showServer(-1);
}

AppConfig SettingsDialog::result() const
{
    AppConfig out = m_cfg;
    out.fps = m_fps->value();
    out.fullscreen = m_fullscreen->isChecked();
    return out;
}

void SettingsDialog::refreshLabel(int row)
{
    if (row < 0 || row >= m_cfg.servers.size())
        return;
    const ServerConfig &s = m_cfg.servers[row];
    m_list->item(row)->setText(s.name.isEmpty() ? s.url : QStringLiteral("%1 (%2)").arg(s.name, s.url));
}

void SettingsDialog::showServer(int row)
{
    m_row = row;
    const bool valid = row >= 0 && row < m_cfg.servers.size();
    for (QWidget *w : {static_cast<QWidget *>(m_name), static_cast<QWidget *>(m_url), static_cast<QWidget *>(m_user),
                       static_cast<QWidget *>(m_password), static_cast<QWidget *>(m_enabled)})
        w->setEnabled(valid);
    m_testResult->clear();
    const ServerConfig s = valid ? m_cfg.servers[row] : ServerConfig();
    m_name->setText(s.name);
    m_url->setText(s.url);
    m_user->setText(s.user);
    m_password->setText(s.password);
    m_enabled->setChecked(s.enabled);
}

void SettingsDialog::storeServer(int row)
{
    if (row < 0 || row >= m_cfg.servers.size())
        return;
    ServerConfig &s = m_cfg.servers[row];
    s.name = m_name->text().trimmed();
    s.url = m_url->text().trimmed();
    s.user = m_user->text();
    s.password = m_password->text();
    s.enabled = m_enabled->isChecked();
    refreshLabel(row);
}

void SettingsDialog::addServer()
{
    storeServer(m_row);
    ServerConfig s;
    s.name = QStringLiteral("Сервер %1").arg(m_cfg.servers.size() + 1);
    s.url = QStringLiteral("http://");
    m_cfg.servers.append(s);
    m_list->addItem(QString());
    refreshLabel(m_cfg.servers.size() - 1);
    m_list->setCurrentRow(m_cfg.servers.size() - 1);
    m_url->setFocus();
}

void SettingsDialog::removeServer()
{
    const int row = m_row;
    if (row < 0 || row >= m_cfg.servers.size())
        return;
    m_row = -1; // do not store the removed server back
    m_cfg.servers.removeAt(row);
    delete m_list->takeItem(row);
    if (m_cfg.servers.isEmpty())
        showServer(-1);
    else
        m_list->setCurrentRow(qMin(row, m_cfg.servers.size() - 1));
}

void SettingsDialog::testServer()
{
    storeServer(m_row);
    if (m_row < 0)
        return;
    m_testResult->setText(QStringLiteral("Проверка…"));
    auto *client = new MotionEyeClient(m_cfg.servers[m_row], this);
    client->authenticate(client, [this, client](bool ok, const QString &error) {
        if (!ok) {
            m_testResult->setText(QStringLiteral("Ошибка: %1").arg(error));
            client->deleteLater();
            return;
        }
        QNetworkReply *r = client->get(QStringLiteral("/config/list/"));
        connect(r, &QNetworkReply::finished, this, [this, client, r] {
            r->deleteLater();
            if (r->error() != QNetworkReply::NoError) {
                m_testResult->setText(QStringLiteral("Ошибка: %1").arg(r->errorString()));
            } else {
                const int n = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("cameras")).toArray().size();
                m_testResult->setText(QStringLiteral("Подключено. Камер: %1").arg(n));
            }
            client->deleteLater();
        });
    });
}
