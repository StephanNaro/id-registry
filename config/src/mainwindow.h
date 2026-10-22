// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>

#include "service.h"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onBrowseClicked();
    void onSaveClicked();
    void onInstallClicked();
    void onUninstallClicked();
    void onStartClicked();
    void onStopClicked();

private:
    bool loadSettingsFromDb(const QString &dbPath);
    void refreshService();
    void setStatus(const QString &text, const QString &color);
    bool requireDatabasePath();
    bool endpointChanged() const;
    QStringList installArgs() const;

    QLineEdit   *lineEditDbPath;
    QLineEdit   *lineEditHost;
    QSpinBox    *spinBoxPort;
    QSpinBox    *spinBoxIdLength;
    QLineEdit   *lineEditCharset;
    QPushButton *pushButtonBrowse;
    QPushButton *pushButtonSave;
    QPushButton *pushButtonInstall;
    QPushButton *pushButtonUninstall;
    QPushButton *pushButtonStart;
    QPushButton *pushButtonStop;
    QLabel      *labelService;
    QLabel      *labelStatus;
    QLineEdit   *lineEditAdminSecret;
    ServiceInfo service_;
};

#endif // MAINWINDOW_H