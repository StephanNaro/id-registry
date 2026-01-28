// SPDX-License-Identifier: GPL-3.0-or-later
#include "database.h"
#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QMessageBox>
#include <QSpinBox>
#include <QIntValidator>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);
    QVBoxLayout *mainLayout = new QVBoxLayout(centralWidget);

    // ── Database Path ───────────────────────────────────────
    QHBoxLayout *pathLayout = new QHBoxLayout();
    QLabel *lblPath = new QLabel("Database Path:", this);
    lineEditDbPath = new QLineEdit(this);
    pushButtonBrowse = new QPushButton("Browse...", this);
    pathLayout->addWidget(lblPath);
    pathLayout->addWidget(lineEditDbPath);
    pathLayout->addWidget(pushButtonBrowse);
    mainLayout->addLayout(pathLayout);

    // ── ID Length ───────────────────────────────────────────
    QHBoxLayout *lengthLayout = new QHBoxLayout();
    QLabel *lblLength = new QLabel("ID Length:", this);
    spinBoxIdLength = new QSpinBox(this);
    spinBoxIdLength->setRange(8, 32);
    spinBoxIdLength->setValue(12);
    lengthLayout->addWidget(lblLength);
    lengthLayout->addWidget(spinBoxIdLength);
    lengthLayout->addStretch();
    mainLayout->addLayout(lengthLayout);

    // ── Character Set ───────────────────────────────────────
    QHBoxLayout *charsetLayout = new QHBoxLayout();
    QLabel *lblCharset = new QLabel("Character Set:", this);
    lineEditCharset = new QLineEdit(this);
    lineEditCharset->setText("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    lineEditCharset->setMaxLength(100);  // reasonable limit
    charsetLayout->addWidget(lblCharset);
    charsetLayout->addWidget(lineEditCharset);
    mainLayout->addLayout(charsetLayout);

    // ── Save Button & Status ────────────────────────────────
    pushButtonSave = new QPushButton("Save & Initialize", this);
    mainLayout->addWidget(pushButtonSave);

    labelStatus = new QLabel(this);
    labelStatus->setWordWrap(true);
    mainLayout->addWidget(labelStatus);
    mainLayout->addStretch();

    // ── Load saved path ─────────────────────────────────────
    QSettings settings("IdRegistry", "Settings");
    QString savedPath = settings.value("DBPath", "").toString();
    lineEditDbPath->setText(savedPath);

    if (!savedPath.isEmpty()) {
        // Try to load current settings from DB
        if (loadSettingsFromDb(savedPath)) {
            labelStatus->setText("Loaded settings from: " + savedPath);
            labelStatus->setStyleSheet("color: green;");
        } else {
            labelStatus->setText("Database found, but could not load settings.");
            labelStatus->setStyleSheet("color: orange;");
        }
    } else {
        labelStatus->setText("Please set the database location.");
        labelStatus->setStyleSheet("color: red;");
    }

    // Connections
    connect(pushButtonBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(pushButtonSave,   &QPushButton::clicked, this, &MainWindow::onSaveClicked);

    setWindowTitle("ID Registry GUI");
    resize(650, 280);
}

MainWindow::~MainWindow() {}

bool MainWindow::loadSettingsFromDb(const QString &dbPath) {
    DbUtil::ScopedDbConnection conn(dbPath, "load_settings");
    if (!conn.isOpen()) {
        return false;
    }

    QSqlQuery q = conn.query();
    if (q.exec("SELECT key, value FROM settings WHERE key IN ('id_length', 'charset')")) {
        while (q.next()) {
            QString key   = q.value(0).toString();
            QString value = q.value(1).toString();
            if (key == "id_length") {
                bool ok;
                int len = value.toInt(&ok);
                if (ok && len >= 8 && len <= 32) {
                    spinBoxIdLength->setValue(len);
                }
            } else if (key == "charset") {
                if (!value.isEmpty()) {
                    lineEditCharset->setText(value);
                }
            }
        }
    }

    return true;  // destructor will close & remove
}

void MainWindow::onBrowseClicked() {
    QString path = QFileDialog::getSaveFileName(this, "Select Database File", "", "SQLite (*.sqlite *.db);;All Files (*)");
    if (!path.isEmpty()) {
        lineEditDbPath->setText(path);
    }
}

void MainWindow::onSaveClicked()
{
    QString path = lineEditDbPath->text().trimmed();
    if (path.isEmpty()) {
        labelStatus->setText("Error: Path is required.");
        labelStatus->setStyleSheet("color: red;");
        return;
    }

    QString errorMsg;
    if (!DbUtil::initializeDatabase(path, errorMsg)) {
        labelStatus->setText("Error: " + errorMsg);
        labelStatus->setStyleSheet("color: red;");
        return;
    }

    // Update settings in DB
    bool settingsUpdated = false;
    {
        DbUtil::ScopedDbConnection conn(path, "update_settings");
        if (conn.isOpen()) {
            QSqlQuery q = conn.query();
            q.prepare("INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");

            q.addBindValue("id_length");
            q.addBindValue(QString::number(spinBoxIdLength->value()));
            q.exec();

            q.addBindValue("charset");
            q.addBindValue(lineEditCharset->text().trimmed());
            q.exec();

            // Optional: check for errors
            if (!q.lastError().isValid()) {
                settingsUpdated = true;
            }
        }
    }  // ScopedDbConnection destructor closes & removes here

    // Save DB path to registry (this was missing!)
    QSettings settings("IdRegistry", "Settings");
    settings.setValue("DBPath", path);

    // Optional: force UI to reflect the saved value
    lineEditDbPath->setText(settings.value("DBPath").toString());

    // Feedback
    if (settingsUpdated) {
        labelStatus->setText("Database initialized and settings saved at " + path);
        labelStatus->setStyleSheet("color: green;");
    } else {
        labelStatus->setText("Database created, but settings update failed. Path saved.");
        labelStatus->setStyleSheet("color: orange;");
    }
}