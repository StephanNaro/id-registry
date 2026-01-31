// SPDX-License-Identifier: GPL-3.0-or-later
#include "database.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include <QDir>

namespace DbUtil {

bool initializeDatabase(const QString &dbPath, QString &errorMessage) {
    errorMessage.clear();

    if (dbPath.isEmpty()) {
        errorMessage = "Database path is empty.";
        return false;
    }

    QFileInfo fi(dbPath);
    QDir dir = fi.dir();
    if (!dir.exists() && !dir.mkpath(".")) {
        errorMessage = "Cannot create directory: " + dir.absolutePath();
        return false;
    }

    ScopedDbConnection conn(dbPath, "init_connection");
    if (!conn.isOpen()) {
        errorMessage = "Failed to open database: " + conn.db().lastError().text();
        return false;
    }

    QSqlQuery query = conn.query();
    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS ids ("
        "    id          TEXT PRIMARY KEY,"
        "    owner       TEXT NOT NULL,"
        "    table_name  TEXT,"
        "    user_id     TEXT,"
        "    confirmed   INTEGER DEFAULT 0,"
        "    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "    deleted     INTEGER DEFAULT 0"
        ")")) {
        errorMessage = "Failed to create ids table: " + query.lastError().text();
        return false;
    }

    if (!query.exec(
        "CREATE TABLE IF NOT EXISTS settings ("
        "    key    TEXT PRIMARY KEY,"
        "    value  TEXT"
        ")")) {
        errorMessage = "Failed to create settings table: " + query.lastError().text();
        return false;
    }

    query.exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('id_length', '12')");
    query.exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('charset', "
               "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')");
    query.exec("INSERT OR IGNORE INTO settings (key, value) VALUES ('admin_secret', 'your-secret-here')");

/*
    query.prepare(
        "INSERT OR IGNORE INTO settings (key, value) "
        "VALUES (?, ?)"
    );
    query.addBindValue("admin_secret");     // position 1
    query.addBindValue(secret);             // position 2
    if (!query.exec()) {
        errorMessage = "Insert failed: " + query.lastError().text();
        return false;
    }
*/

    return true;
}

// ────────────────────────────────────────────────────────────────

ScopedDbConnection::ScopedDbConnection(const QString &dbPath, const QString &connectionName)
    : m_connectionName(connectionName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", m_connectionName);
    db.setDatabaseName(dbPath);
    m_openedSuccessfully = db.open();
    if (!m_openedSuccessfully) {
        qDebug() << "Failed to open connection" << m_connectionName << ":" << db.lastError().text();
    }
}

ScopedDbConnection::~ScopedDbConnection() {
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            // Extra scope to force temporary death
            QSqlDatabase db = QSqlDatabase::database(m_connectionName);
            if (db.isOpen()) {
                db.close();
            }
        }  // ← temporary db dies here

        QSqlDatabase::removeDatabase(m_connectionName);
    }
}

bool ScopedDbConnection::isOpen() const {
    return QSqlDatabase::contains(m_connectionName) &&
           QSqlDatabase::database(m_connectionName).isOpen();
}

QSqlDatabase ScopedDbConnection::db() {
    return QSqlDatabase::database(m_connectionName);
}

const QSqlDatabase ScopedDbConnection::db() const {
    return QSqlDatabase::database(m_connectionName);
}

QSqlQuery ScopedDbConnection::query() {
    return QSqlQuery(db());
}

} // namespace DbUtil