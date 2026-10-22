// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"
#include "mainwindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QSpinBox>
#include <QIntValidator>
#include <QString>
#include <random>

QString generateRandomPassword(int length = 12) {
    const QString chars =
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789"
        "!@#$%^&*()_+-=[]{};:,.<>?/~";

    QString password;
    password.reserve(length);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, chars.length() - 1);

    for (int i = 0; i < length; ++i) {
        password += chars[dis(gen)];
    }
    return password;
}

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

    // ── Listen address ──────────────────────────────────────
    QHBoxLayout *listenLayout = new QHBoxLayout();
    QLabel *lblHost = new QLabel("Host:", this);
    lineEditHost = new QLineEdit(this);
    lineEditHost->setText("127.0.0.1");
    QLabel *lblPort = new QLabel("Port:", this);
    spinBoxPort = new QSpinBox(this);
    spinBoxPort->setRange(1, 65535);
    spinBoxPort->setValue(8000);
    listenLayout->addWidget(lblHost);
    listenLayout->addWidget(lineEditHost, 1);
    listenLayout->addWidget(lblPort);
    listenLayout->addWidget(spinBoxPort);
    mainLayout->addLayout(listenLayout);

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

    // ── Admin Secret ───────────────────────────────────────
    QHBoxLayout *secretLayout = new QHBoxLayout();
    QLabel *lblSecret = new QLabel("Admin Secret (for suspend/resume):", this);
    lineEditAdminSecret = new QLineEdit(this);
    lineEditAdminSecret->setEchoMode(QLineEdit::Password);
    lineEditAdminSecret->setText(generateRandomPassword());	//"");  // empty by default
    secretLayout->addWidget(lblSecret);
    secretLayout->addWidget(lineEditAdminSecret);
    mainLayout->addLayout(secretLayout);

    // ── Save Button & Status ────────────────────────────────
    pushButtonSave = new QPushButton("Save & Initialize", this);
    mainLayout->addWidget(pushButtonSave);

    // ── Windows service ─────────────────────────────────────
    labelService = new QLabel(this);
    labelService->setWordWrap(true);
    mainLayout->addWidget(labelService);

    QHBoxLayout *serviceLayout = new QHBoxLayout();
    pushButtonInstall = new QPushButton("Install service", this);
    pushButtonUninstall = new QPushButton("Uninstall", this);
    pushButtonStart = new QPushButton("Start", this);
    pushButtonStop = new QPushButton("Stop", this);
    serviceLayout->addWidget(pushButtonInstall);
    serviceLayout->addWidget(pushButtonUninstall);
    serviceLayout->addWidget(pushButtonStart);
    serviceLayout->addWidget(pushButtonStop);
    mainLayout->addLayout(serviceLayout);

    labelStatus = new QLabel(this);
    labelStatus->setWordWrap(true);
    mainLayout->addWidget(labelStatus);
    mainLayout->addStretch();

    refreshService();

    // Connections
    connect(pushButtonBrowse, &QPushButton::clicked, this, &MainWindow::onBrowseClicked);
    connect(pushButtonSave,   &QPushButton::clicked, this, &MainWindow::onSaveClicked);
    connect(pushButtonInstall, &QPushButton::clicked, this, &MainWindow::onInstallClicked);
    connect(pushButtonUninstall, &QPushButton::clicked, this, &MainWindow::onUninstallClicked);
    connect(pushButtonStart, &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(pushButtonStop, &QPushButton::clicked, this, &MainWindow::onStopClicked);

    setWindowTitle("ID Registry Configuration");
    resize(720, 400);
}

bool MainWindow::endpointChanged() const {
    const QString host = lineEditHost->text().trimmed();
    const QString installedHost = service_.host.isEmpty() ? QString("127.0.0.1") : service_.host;
    return service_.db_path != lineEditDbPath->text().trimmed()
        || installedHost != host
        || service_.port != spinBoxPort->value();
}

QStringList MainWindow::installArgs() const {
    return {
        "--install",
        "--db", lineEditDbPath->text().trimmed(),
        "--host", lineEditHost->text().trimmed(),
        "--port", QString::number(spinBoxPort->value())
    };
}

void MainWindow::setStatus(const QString &text, const QString &color) {
    labelStatus->setText(text);
    labelStatus->setStyleSheet("color: " + color + ";");
}

bool MainWindow::requireDatabasePath() {
    if (lineEditDbPath->text().trimmed().isEmpty()) {
        setStatus("Error: Path is required.", "red");
        return false;
    }
    if (lineEditHost->text().trimmed().isEmpty()) {
        setStatus("Error: Host is required.", "red");
        return false;
    }
    return true;
}

void MainWindow::refreshService() {
    service_ = query_service();
    if (!service_.error.isEmpty()) {
        labelService->setText("Service: " + service_.error);
        pushButtonInstall->setEnabled(false);
        pushButtonUninstall->setEnabled(false);
        pushButtonStart->setEnabled(false);
        pushButtonStop->setEnabled(false);
        return;
    }

    if (!service_.installed) {
        labelService->setText("Service: not installed. Install registers IdRegistryService to start automatically as LocalSystem.");
        pushButtonInstall->setText("Install service");
        pushButtonInstall->setEnabled(true);
        pushButtonUninstall->setEnabled(false);
        pushButtonStart->setEnabled(false);
        pushButtonStop->setEnabled(false);
        setStatus("Please set the database location.", "red");
        return;
    }

    if (!service_.db_path.isEmpty()) {
        lineEditDbPath->setText(service_.db_path);
    }
    lineEditHost->setText(service_.host.isEmpty() ? QString("127.0.0.1") : service_.host);
    spinBoxPort->setValue(service_.port);
    labelService->setText(service_.running
        ? "Service: running as LocalSystem."
        : "Service: installed and stopped.");
    pushButtonInstall->setText("Update service");
    pushButtonInstall->setEnabled(true);
    pushButtonUninstall->setEnabled(true);
    pushButtonStart->setEnabled(!service_.running);
    pushButtonStop->setEnabled(service_.running);

    if (service_.db_path.isEmpty()) {
        setStatus("The installed service has no database path.", "orange");
    } else if (loadSettingsFromDb(service_.db_path)) {
        setStatus("Loaded settings from: " + service_.db_path, "green");
    } else {
        setStatus("Service is installed, but the database settings could not be loaded.", "orange");
    }
}

MainWindow::~MainWindow() {}

bool MainWindow::loadSettingsFromDb(const QString &dbPath) {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(dbPath.toStdString().c_str(), &db,
                             SQLITE_OPEN_READONLY, nullptr);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }

    try {
        Settings s = load_settings(db);
        if (s.id_length >= 8 && s.id_length <= 32) {
            spinBoxIdLength->setValue(static_cast<int>(s.id_length));
        }
        if (!s.charset.empty()) {
            lineEditCharset->setText(QString::fromStdString(s.charset));
        }
        if (!s.admin_secret.empty()) {
            lineEditAdminSecret->setText(QString::fromStdString(s.admin_secret));
        }
    } catch (...) {
        sqlite3_close(db);
        return false;
    }

    sqlite3_close(db);
    return true;
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
        setStatus("Error: Path is required.", "red");
        return;
    }

    // Initialize the database (create tables + default settings)
    try {
        initialize_database(path.toStdString());
    } catch (const std::exception& e) {
        setStatus("Error: " + QString::fromStdString(e.what()), "red");
        return;
    }

    // Update settings in DB
    bool settingsUpdated = false;
    {
        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(path.toStdString().c_str(), &db,
                                 SQLITE_OPEN_READWRITE, nullptr);
        if (rc == SQLITE_OK && db) {
            try {
                Settings s;
                s.id_length = static_cast<uint32_t>(spinBoxIdLength->value());
                s.charset = lineEditCharset->text().trimmed().toStdString();
                s.admin_secret = lineEditAdminSecret->text().trimmed().toStdString();
                save_settings(db, s);
                settingsUpdated = true;
            } catch (...) {
                // settingsUpdated remains false
            }
            sqlite3_close(db);
        }
    }

    QString serviceNote;
    if (service_.installed && endpointChanged()) {
        QString error = run_elevated_server(installArgs());
        if (!error.isEmpty()) {
            setStatus("Database saved, but the service command was not updated. " + error, "orange");
            refreshService();
            return;
        }
        serviceNote = " Service command updated.";
        if (service_.running) {
            error = run_elevated_server({"--stop"});
            if (error.isEmpty()) {
                error = run_elevated_server({"--start"});
            }
            if (!error.isEmpty()) {
                setStatus("Database saved and service command updated, but the restart failed. " + error, "orange");
                refreshService();
                return;
            }
            serviceNote = " Service restarted with the new listen settings.";
        }
    }

    refreshService();
    if (settingsUpdated) {
        setStatus("Database initialized and settings saved at " + path + serviceNote, "green");
    } else {
        setStatus("Database created, but settings update failed." + serviceNote, "orange");
    }
}

void MainWindow::onInstallClicked() {
    if (!requireDatabasePath()) {
        return;
    }

    QString path = lineEditDbPath->text().trimmed();
    try {
        initialize_database(path.toStdString());
    } catch (const std::exception& e) {
        setStatus("Error: " + QString::fromStdString(e.what()), "red");
        return;
    }

    bool was_installed = service_.installed;
    QString error = run_elevated_server(installArgs());
    if (!error.isEmpty()) {
        setStatus(error, "red");
        refreshService();
        return;
    }

    refreshService();
    setStatus(was_installed
        ? "Service command updated for " + path + ". Logs go to logs\\idRegistry.log beside the database."
        : "Service installed for " + path + ". Logs go to logs\\idRegistry.log beside the database.",
        "green");
}

void MainWindow::onUninstallClicked() {
    auto answer = QMessageBox::question(
        this,
        "Remove service",
        "Stop and remove the ID Registry service? The database file will be left in place.");
    if (answer != QMessageBox::Yes) {
        return;
    }

    QString error = run_elevated_server({"--uninstall"});
    if (!error.isEmpty()) {
        setStatus(error, "red");
        refreshService();
        return;
    }
    refreshService();
    setStatus("Service removed. The database file was left in place.", "green");
}

void MainWindow::onStartClicked() {
    QString error = run_elevated_server({"--start"});
    if (!error.isEmpty()) {
        setStatus(error, "red");
        refreshService();
        return;
    }
    refreshService();
    setStatus("Service is running.", "green");
}

void MainWindow::onStopClicked() {
    QString error = run_elevated_server({"--stop"});
    if (!error.isEmpty()) {
        setStatus(error, "red");
        refreshService();
        return;
    }
    refreshService();
    setStatus("Service stopped.", "green");
}
