// SPDX-License-Identifier: GPL-3.0-or-later
#include "db.h"

#include <iostream>
#include <stdexcept>
#include <random>
#include <filesystem>

DbPool::Connection::Connection(std::shared_ptr<DbPool> pool, sqlite3* db)
    : pool_(std::move(pool)), db_(db) {}

DbPool::Connection::~Connection() {
    if (pool_ && db_) {
        pool_->release(db_);
        db_ = nullptr;
    }
}

DbPool::Connection::Connection(Connection&& other) noexcept
    : pool_(std::move(other.pool_)), db_(other.db_) {
    other.db_ = nullptr;
}

DbPool::Connection& DbPool::Connection::Connection::operator=(Connection&& other) noexcept {
    if (this != &other) {
        if (pool_ && db_) {
            pool_->release(db_);
        }
        pool_ = std::move(other.pool_);
        db_ = other.db_;
        other.db_ = nullptr;
    }
    return *this;
}

std::shared_ptr<DbPool> DbPool::create(const std::string& db_path, size_t max_size) {
    return std::shared_ptr<DbPool>(new DbPool(db_path, max_size));
}

DbPool::DbPool(std::string db_path, size_t max_size)
    : db_path_(std::move(db_path)), max_size_(max_size) {}

DbPool::~DbPool() {
    std::unique_lock<std::mutex> lock(mutex_);
    closed_ = true;
    for (sqlite3* db : available_) {
        sqlite3_close(db);
    }
    available_.clear();
    cv_.notify_all();
}

sqlite3* DbPool::open_connection() {
    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(db_path_.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unknown SQLite error";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Failed to open database connection: " + err);
    }

    // Set WAL mode and busy timeout
    char* errmsg = nullptr;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errmsg);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, &errmsg);
    if (errmsg) sqlite3_free(errmsg);

    return db;
}

DbPool::Connection DbPool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() {
        return closed_ || !available_.empty() || total_opened_ < max_size_;
    });

    if (closed_) {
        throw std::runtime_error("Database connection pool is closed");
    }

    if (!available_.empty()) {
        sqlite3* db = available_.back();
        available_.pop_back();
        return Connection(shared_from_this(), db);
    }

    if (total_opened_ < max_size_) {
        ++total_opened_;
        lock.unlock();
        try {
            sqlite3* db = open_connection();
            return Connection(shared_from_this(), db);
        } catch (...) {
            lock.lock();
            --total_opened_;
            cv_.notify_one();
            throw;
        }
    }

    throw std::runtime_error("Failed to acquire database connection");
}

void DbPool::release(sqlite3* db) {
    if (!db) return;
    std::unique_lock<std::mutex> lock(mutex_);
    if (closed_) {
        sqlite3_close(db);
        return;
    }
    available_.push_back(db);
    cv_.notify_one();
}

Settings load_settings(sqlite3* db) {
    auto query_val = [db](const std::string& key) -> std::string {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT value FROM settings WHERE key = ?;";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to prepare settings query: " + std::string(sqlite3_errmsg(db)));
        }
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_STATIC);
        int rc = sqlite3_step(stmt);
        std::string val;
        if (rc == SQLITE_ROW) {
            const unsigned char* text = sqlite3_column_text(stmt, 0);
            if (text) {
                val = reinterpret_cast<const char*>(text);
            }
        }
        sqlite3_finalize(stmt);
        if (rc != SQLITE_ROW) {
            throw std::runtime_error("Missing '" + key + "' in settings table");
        }
        return val;
    };

    Settings settings;
    std::string id_length_str = query_val("id_length");
    try {
        settings.id_length = static_cast<uint32_t>(std::stoul(id_length_str));
    } catch (...) {
        throw std::runtime_error("Invalid 'id_length' value");
    }

    settings.charset = query_val("charset");
    settings.admin_secret = query_val("admin_secret");
    return settings;
}

bool is_all_numeric(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

bool id_exists(sqlite3* db, const std::string& id) {
    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT COUNT(*) FROM ids WHERE id = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("Failed to prepare id_exists query");
    }
    sqlite3_bind_text(stmt, 1, id.c_str(), -1, SQLITE_STATIC);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return count > 0;
}

std::string generate_id(sqlite3* db, const Settings& settings) {
    constexpr size_t MAX_RETRIES = 100;
    if (settings.charset.empty()) {
        throw std::runtime_error("Charset is empty");
    }

    std::random_device rd;
    std::mt19937_64 rng(rd());
    std::uniform_int_distribution<size_t> dist(0, settings.charset.size() - 1);

    for (size_t attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
        std::string id;
        id.reserve(settings.id_length);
        for (uint32_t i = 0; i < settings.id_length; ++i) {
            id.push_back(settings.charset[dist(rng)]);
        }

        if (is_all_numeric(id)) {
            continue;
        }

        if (!id_exists(db, id)) {
            return id;
        }

        if (attempt % 20 == 0) {
            std::cout << "Collision detected on attempt " << attempt << ", retrying..." << std::endl;
        }
    }

    throw std::runtime_error("Failed to generate unique ID after " + std::to_string(MAX_RETRIES) +
                             " attempts. Database may be very full.");
}


void initialize_database(const std::string& db_path) {
    // Create parent directory if needed
    std::filesystem::path p(db_path);
    std::filesystem::path dir = p.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir)) {
        if (!std::filesystem::create_directories(dir)) {
            throw std::runtime_error("Cannot create directory: " + dir.string());
        }
    }

    sqlite3* db = nullptr;
    int rc = sqlite3_open_v2(db_path.c_str(), &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                             nullptr);
    if (rc != SQLITE_OK) {
        std::string err = db ? sqlite3_errmsg(db) : "Unknown SQLite error";
        if (db) sqlite3_close(db);
        throw std::runtime_error("Failed to open database: " + err);
    }

    auto exec_or_throw = [&](const char* sql, const char* context) {
        char* errmsg = nullptr;
        int result = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
        if (result != SQLITE_OK) {
            std::string err = errmsg ? errmsg : "Unknown error";
            if (errmsg) sqlite3_free(errmsg);
            sqlite3_close(db);
            throw std::runtime_error(std::string(context) + ": " + err);
        }
    };

    exec_or_throw(
        "CREATE TABLE IF NOT EXISTS ids ("
        "    id          TEXT PRIMARY KEY,"
        "    owner       TEXT NOT NULL,"
        "    table_name  TEXT,"
        "    user_id     TEXT,"
        "    confirmed   INTEGER DEFAULT 0,"
        "    created_at  DATETIME DEFAULT CURRENT_TIMESTAMP,"
        "    deleted     INTEGER DEFAULT 0"
        ")",
        "Failed to create ids table"
    );

    exec_or_throw(
        "CREATE TABLE IF NOT EXISTS settings ("
        "    key    TEXT PRIMARY KEY,"
        "    value  TEXT"
        ")",
        "Failed to create settings table"
    );

    exec_or_throw("INSERT OR IGNORE INTO settings (key, value) VALUES ('id_length', '12')",
                  "Failed to insert default id_length");
    exec_or_throw("INSERT OR IGNORE INTO settings (key, value) VALUES ('charset', "
                  "'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789')",
                  "Failed to insert default charset");
    exec_or_throw("INSERT OR IGNORE INTO settings (key, value) VALUES ('admin_secret', 'your-secret-here')",
                  "Failed to insert default admin_secret");

    sqlite3_close(db);
}

void save_settings(sqlite3* db, const Settings& settings) {
    auto upsert = [&](const char* key, const std::string& value) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            throw std::runtime_error(std::string("Failed to prepare save_settings for '") + key + "': " + sqlite3_errmsg(db));
        }
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc != SQLITE_DONE) {
            throw std::runtime_error(std::string("Failed to save setting '") + key + "': " + sqlite3_errmsg(db));
        }
    };

    upsert("id_length", std::to_string(settings.id_length));
    upsert("charset", settings.charset);
    upsert("admin_secret", settings.admin_secret);
}
