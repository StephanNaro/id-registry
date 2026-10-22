// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IDREGISTRY_PLATFORM_H
#define IDREGISTRY_PLATFORM_H

#include <string>

struct Options {
    std::string db_path;
    std::string host = "127.0.0.1";
    int port = 8000;
    enum class Action { Run, Install, Uninstall, Start, Stop, Help };
    Action action = Action::Run;
};

std::string usage_text();

// argv[0] is ignored. Paths in argv are UTF-8.
bool parse_options(int argc, char** argv, Options& opt, std::string& error);

// Makes a relative database path absolute against the executable directory.
std::string normalize_db_path(const std::string& utf8_path);

// Mirrors std::cout and std::cerr into <db directory>/logs/idRegistry.log.
bool init_log(const std::string& db_path_utf8, std::string& log_path_utf8, std::string& error);

void log_event_error(const std::string& message);
void log_event_info(const std::string& message);

void service_report_start_pending();
void service_report_running();
void service_report_stopped(int exit_code);
void set_service_stop_handler(void (*fn)());

// Service commands, service dispatch, or a console run of run_server.
int entry_point(int argc, char** argv, int (*run_server)(const Options&, bool as_service));

#endif
