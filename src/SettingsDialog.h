#pragma once

#include <QDialog>

#include "Config.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QSpinBox;

class SettingsDialog : public QDialog
{
    Q_OBJECT
public:
    SettingsDialog(const AppConfig &cfg, QWidget *parent = nullptr);

    AppConfig result() const;

private:
    void showServer(int row);
    void storeServer(int row);
    void refreshLabel(int row);
    void addServer();
    void removeServer();
    void testServer();

    AppConfig m_cfg;
    int m_row = -1;
    QListWidget *m_list;
    QLineEdit *m_name;
    QLineEdit *m_url;
    QLineEdit *m_user;
    QLineEdit *m_password;
    QCheckBox *m_enabled;
    QLabel *m_testResult;
    QSpinBox *m_fps;
    QCheckBox *m_fullscreen;
};
