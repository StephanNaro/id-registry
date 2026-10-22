// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"
#include "httplib.h"
#include "platform.h"

#include <nlohmann/json.hpp>
#include <iostream>
#include <memory>
#include <atomic>
#include <csignal>
#include <string>
#include <optional>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

static httplib::Server* g_server = nullptr;

static void stop_http_server() {
    if (g_server) {
        g_server->stop();
    }
}

static void handle_signal(int signal) {
    if (g_server) {
        std::cout << "\nReceived signal " << signal << ", stopping server..." << std::endl;
        stop_http_server();
    }
}

#ifdef _WIN32
static BOOL WINAPI console_ctrl_handler(DWORD) {
    stop_http_server();
    return TRUE;
}
#endif

static void send_json_error(httplib::Response& res, int status, const std::string& error_code, const std::string& message) {
    nlohmann::json j = {
        {"error", error_code},
        {"message", message}
    };
    res.status = status;
    res.set_content(j.dump(), "application/json");
}

static int run_server(const Options& opt, bool as_service) {
    const int port = opt.port;
    const std::string& host = opt.host;
    const std::string db_path = opt.db_path;

    std::string log_path;
    std::string log_error;
    if (!init_log(db_path, log_path, log_error)) {
        log_event_error(log_error);
        std::cerr << log_error << std::endl;
        if (as_service) {
            service_report_stopped(1);
        }
        return 1;
    }

    std::cout << "Starting ID Registry Server..." << std::endl;
    std::cout << "Database: " << db_path << std::endl;
    std::cout << "Log: " << log_path << std::endl;

    std::shared_ptr<DbPool> pool;
    try {
        pool = DbPool::create(db_path, 10);
    } catch (const std::exception& e) {
        std::cerr << "Failed to create DB pool: " << e.what() << std::endl;
        log_event_error(e.what());
        if (as_service) {
            service_report_stopped(1);
        }
        return 1;
    }

    // Verify WAL mode with a test connection
    try {
        auto conn = pool->acquire();
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(conn.get(), "PRAGMA journal_mode;", -1, &stmt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* mode = sqlite3_column_text(stmt, 0);
                std::cout << "Connection pool created – WAL mode: "
                          << (mode ? reinterpret_cast<const char*>(mode) : "unknown")
                          << std::endl;
            }
            sqlite3_finalize(stmt);
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to initialize test connection: " << e.what() << std::endl;
        log_event_error(e.what());
        if (as_service) {
            service_report_stopped(1);
        }
        return 1;
    }

    // Load settings once at startup
    std::shared_ptr<Settings> settings;
    try {
        auto conn = pool->acquire();
        settings = std::make_shared<Settings>(load_settings(conn.get()));
    } catch (const std::exception& e) {
        std::cerr << "Failed to load settings: " << e.what() << std::endl;
        log_event_error(e.what());
        if (as_service) {
            service_report_stopped(1);
        }
        return 1;
    }

    std::cout << "Database pool ready" << std::endl;
    std::cout << "ID length: " << settings->id_length << std::endl;
    std::cout << "Charset  : " << settings->charset << std::endl;

    auto suspended = std::make_shared<std::atomic<bool>>(false);

    httplib::Server svr;
    g_server = &svr;
    set_service_stop_handler(stop_http_server);

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
#ifdef _WIN32
    // MinGW does not deliver CTRL_CLOSE_EVENT through SIGTERM. A service stop uses this path.
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
#endif

    // POST /suspend?secret=yourpassword
    svr.Post("/suspend", [settings, suspended](const httplib::Request& req, httplib::Response& res) {
        std::string secret = req.get_param_value("secret");
        if (secret != settings->admin_secret) {
            send_json_error(res, 401, "unauthorized", "Authentication required");
            return;
        }

        suspended->store(true);
        res.status = 200;
        res.set_content("Server suspended (new requests rejected)", "text/plain");
    });

    // POST /resume?secret=yourpassword
    svr.Post("/resume", [settings, suspended](const httplib::Request& req, httplib::Response& res) {
        std::string secret = req.get_param_value("secret");
        if (secret != settings->admin_secret) {
            send_json_error(res, 401, "unauthorized", "Authentication required");
            return;
        }

        suspended->store(false);
        res.status = 200;
        res.set_content("Server resumed", "text/plain");
    });

    // GET /health
    svr.Get("/health", [settings, suspended, db_path](const httplib::Request& /*req*/, httplib::Response& res) {
        try {
            nlohmann::json j = {
                {"status", suspended->load() ? "Suspended" : "ok"},
                {"db_path", db_path},
                {"settings", {
                    {"id_length", settings->id_length},
                    {"charset", settings->charset},
                    {"admin_secret", "[redacted]"}	// settings->admin_secret}
                }}
            };
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        } catch (const std::exception& e) {
            send_json_error(res, 500, "internal_error", e.what());
        }
    });

    // GET /preview
    svr.Get("/preview", [settings, pool](const httplib::Request& /*req*/, httplib::Response& res) {
        try {
            auto conn = pool->acquire();
            std::string id = generate_id(conn.get(), *settings);
            nlohmann::json j = {
                {"preview_id", id}
            };
            res.status = 200;
            res.set_content(j.dump(), "application/json");
        } catch (const std::exception& e) {
            std::cerr << "Generation failed: " << e.what() << std::endl;
            send_json_error(res, 500, "internal_error", "Failed to generate preview ID");
        }
    });

    // POST /generate
    svr.Post("/generate", [settings, pool, suspended](const httplib::Request& req, httplib::Response& res) {
        if (suspended->load()) {
            send_json_error(res, 503, "service_unavailable", "Server is temporarily suspended for maintenance");
            return;
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
            return;
        }

        if (!body.contains("owner") || !body["owner"].is_string()) {
            send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
            return;
        }

        std::string owner = body["owner"].get<std::string>();
        // Trim owner
        auto start = owner.find_first_not_of(" \t\r\n");
        auto end = owner.find_last_not_of(" \t\r\n");
        std::string owner_clean = (start == std::string::npos) ? "" : owner.substr(start, end - start + 1);

        if (owner_clean.empty()) {
            send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
            return;
        }

        for (char c : owner_clean) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
                send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
                return;
            }
        }

        std::optional<std::string> table;
        if (body.contains("table") && body["table"].is_string()) {
            table = body["table"].get<std::string>();
        }

        std::cout << "Generate request: owner=" << owner_clean
                  << ", table=" << (table.has_value() ? table.value() : "None")
                  << std::endl;

        try {
            auto conn = pool->acquire();
            std::string id = generate_id(conn.get(), *settings);

            sqlite3_stmt* stmt = nullptr;
            const char* sql = "INSERT INTO ids (id, owner, table_name, confirmed, created_at) "
                              "VALUES (?, ?, ?, 0, CURRENT_TIMESTAMP);";
            if (sqlite3_prepare_v2(conn.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
                send_json_error(res, 500, "internal_error", "Failed to prepare insert statement");
                return;
            }

            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, owner_clean.c_str(), -1, SQLITE_STATIC);
            if (table.has_value()) {
                sqlite3_bind_text(stmt, 3, table->c_str(), -1, SQLITE_STATIC);
            } else {
                sqlite3_bind_null(stmt, 3);
            }

            if (sqlite3_step(stmt) != SQLITE_DONE) {
                sqlite3_finalize(stmt);
                send_json_error(res, 500, "internal_error", "Failed to insert new ID");
                return;
            }
            sqlite3_finalize(stmt);

            // Fetch created_at timestamp
            std::string created_at = "unknown";
            const char* sql_created = "SELECT created_at FROM ids WHERE id = ?;";
            if (sqlite3_prepare_v2(conn.get(), sql_created, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const unsigned char* t = sqlite3_column_text(stmt, 0);
                    if (t) created_at = reinterpret_cast<const char*>(t);
                }
                sqlite3_finalize(stmt);
            }

            nlohmann::json response = {
                {"id", id},
                {"owner", owner_clean},
                {"table", table.has_value() ? nlohmann::json(table.value()) : nlohmann::json(nullptr)},
                {"confirmed", 0},
                {"created_at", created_at}
            };

            res.status = 200;
            res.set_content(response.dump(), "application/json");
        } catch (const std::exception& e) {
            send_json_error(res, 500, "internal_error", e.what());
        }
    });

    // POST /confirm
    svr.Post("/confirm", [pool, suspended](const httplib::Request& req, httplib::Response& res) {
        if (suspended->load()) {
            send_json_error(res, 503, "service_unavailable", "Server is temporarily suspended for maintenance");
            return;
        }

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
            return;
        }

        if (!body.contains("id") || !body["id"].is_string()) {
            send_json_error(res, 400, "bad_request", "Invalid request parameters or body");
            return;
        }

        std::string id = body["id"].get<std::string>();

        try {
            auto conn = pool->acquire();
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "UPDATE ids SET confirmed = 1 WHERE id = ?;";
            if (sqlite3_prepare_v2(conn.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
                send_json_error(res, 500, "internal_error", "Failed to prepare update statement");
                return;
            }

            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            int rows_affected = sqlite3_changes(conn.get());
            if (rows_affected == 0) {
                nlohmann::json r = {
                    {"success", false},
                    {"message", "ID " + id + " not found or already confirmed"}
                };
                res.status = 200;
                res.set_content(r.dump(), "application/json");
            } else {
                nlohmann::json r = {
                    {"success", true},
                    {"message", "ID " + id + " confirmed"}
                };
                res.status = 200;
                res.set_content(r.dump(), "application/json");
            }
        } catch (const std::exception& e) {
            send_json_error(res, 500, "internal_error", e.what());
        }
    });

    // GET /get_id/<id>
    svr.Get(R"(/get_id/([^/]+))", [pool](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];

        try {
            auto conn = pool->acquire();
            sqlite3_stmt* stmt = nullptr;
            const char* sql = "SELECT owner, table_name, confirmed, created_at FROM ids WHERE id = ? AND deleted = 0;";
            if (sqlite3_prepare_v2(conn.get(), sql, -1, &stmt, nullptr) != SQLITE_OK) {
                send_json_error(res, 500, "internal_error", "Database query failed");
                return;
            }

            sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const unsigned char* owner_t = sqlite3_column_text(stmt, 0);
                const unsigned char* table_t = sqlite3_column_text(stmt, 1);
                int confirmed = sqlite3_column_int(stmt, 2);
                const unsigned char* created_t = sqlite3_column_text(stmt, 3);

                std::string owner = owner_t ? reinterpret_cast<const char*>(owner_t) : "";
                std::optional<std::string> table = table_t
                    ? std::optional<std::string>(reinterpret_cast<const char*>(table_t))
                    : std::nullopt;
                std::string created_at = created_t ? reinterpret_cast<const char*>(created_t) : "";

                sqlite3_finalize(stmt);

                nlohmann::json r = {
                    {"id", id},
                    {"owner", owner},
                    {"table", table.has_value() ? nlohmann::json(table.value()) : nlohmann::json(nullptr)},
                    {"confirmed", confirmed},
                    {"created_at", created_at}
                };
                res.status = 200;
                res.set_content(r.dump(), "application/json");
            } else {
                sqlite3_finalize(stmt);
                send_json_error(res, 404, "not_found", "Resource not found");
            }
        } catch (const std::exception& e) {
            send_json_error(res, 500, "internal_error", e.what());
        }
    });

    // PUT /ids/<id>
    svr.Put(R"(/ids/([^/]+))", [suspended](const httplib::Request& /*req*/, httplib::Response& res) {
        if (suspended->load()) {
            send_json_error(res, 503, "service_unavailable", "Server is temporarily suspended for maintenance");
            return;
        }
        send_json_error(res, 501, "not_implemented", "This feature is not yet available");
    });

    // DELETE /ids/<id>
    svr.Delete(R"(/ids/([^/]+))", [suspended](const httplib::Request& /*req*/, httplib::Response& res) {
        if (suspended->load()) {
            send_json_error(res, 503, "service_unavailable", "Server is temporarily suspended for maintenance");
            return;
        }
        send_json_error(res, 501, "not_implemented", "This feature is not yet available");
    });

    // Catch-all handler for unknown endpoints (404)
    svr.set_error_handler([](const httplib::Request& /*req*/, httplib::Response& res) {
        if (res.status == 404) {
            send_json_error(res, 404, "not_found", "Resource not found");
        }
    });

    if (!svr.bind_to_port(host, port)) {
        std::cerr << "Failed to start HTTP server on " << host << ":" << port << std::endl;
        log_event_error("Failed to start HTTP server on " + host + ":" + std::to_string(port));
        if (as_service) {
            service_report_stopped(1);
        }
        g_server = nullptr;
        return 1;
    }

    std::cout << "Listening on http://" << host << ":" << port << "..." << std::endl;
    std::cout.flush();

    if (as_service) {
        service_report_running();
    }
    log_event_info("ID Registry is listening on " + host + ":" + std::to_string(port));

    if (!svr.listen_after_bind()) {
        std::cerr << "HTTP server stopped before accepting connections." << std::endl;
        if (as_service) {
            service_report_stopped(1);
        }
        g_server = nullptr;
        return 1;
    }

    std::cout << "Server stopped." << std::endl;
    if (as_service) {
        service_report_stopped(0);
    }
    g_server = nullptr;
    return 0;
}

int main(int argc, char* argv[]) {
    return entry_point(argc, argv, run_server);
}
