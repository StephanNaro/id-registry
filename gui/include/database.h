// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DATABASE_H
#define DATABASE_H

#include <QString>
#include <QSqlError>
#include <QSqlDatabase>

class QSqlQuery;

namespace DbUtil {

bool initializeDatabase(const QString &dbPath, QString &errorMessage);

// RAII helper for safe, scoped database connections
class ScopedDbConnection {
public:
    explicit ScopedDbConnection(const QString &dbPath,
                                const QString &connectionName = "default_conn");
    ~ScopedDbConnection();

    bool isOpen() const;
    QSqlDatabase db();               // access the underlying QSqlDatabase
    const QSqlDatabase db() const;

    // Convenience: create a query on this connection
    QSqlQuery query();

private:
    QString m_connectionName;
    bool m_openedSuccessfully = false;
};

} // namespace DbUtil

#endif // DATABASE_H