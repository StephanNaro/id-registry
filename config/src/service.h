// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IDREGISTRY_CONFIG_SERVICE_H
#define IDREGISTRY_CONFIG_SERVICE_H

#include <QString>

struct ServiceInfo {
    bool installed = false;
    bool running = false;
    QString db_path;
    QString host;
    int port = 8000;
    QString error;
};

// Reads the installed service without elevation. Missing service is not an error.
ServiceInfo query_service();

// Launches an elevated copy of the server beside this executable.
// Returns an empty string when the elevated command finishes with exit code 0.
QString run_elevated_server(const QStringList& args);

#endif
