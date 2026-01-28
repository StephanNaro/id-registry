// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>

#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QSpinBox>

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void onBrowseClicked();
    void onSaveClicked();

private:
    bool loadSettingsFromDb(const QString &dbPath);

    QLineEdit   *lineEditDbPath;
    QSpinBox    *spinBoxIdLength;
    QLineEdit   *lineEditCharset;
    QPushButton *pushButtonBrowse;
    QPushButton *pushButtonSave;
    QLabel      *labelStatus;
};

#endif // MAINWINDOW_H