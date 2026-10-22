// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DB_H
#define DB_H

#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <optional>
#include <cstdint>
#include <sqlite3.h>

struct Settings {
    uint32_t id_length = 12;
    std::string charset;
    std::string admin_secret;
};

// Thread-safe SQLite connection pool
class DbPool : public std::enable_shared_from_this<DbPool> {
public:
    class Connection {
    public:
        Connection(std::shared_ptr<DbPool> pool, sqlite3* db);
        ~Connection();
        Connection(const Connection&) = delete;
        Connection& operator=(const Connection&) = delete;
        Connection(Connection&& other) noexcept;
        Connection& operator=(Connection&& other) noexcept;

        sqlite3* get() const { return db_; }
        sqlite3* operator->() const { return db_; }

    private:
        std::shared_ptr<DbPool> pool_;
        sqlite3* db_ = nullptr;
    };

    static std::shared_ptr<DbPool> create(const std::string& db_path, size_t max_size = 10);
    ~DbPool();

    Connection acquire();

private:
    friend class Connection;
    DbPool(std::string db_path, size_t max_size);
    void release(sqlite3* db);
    sqlite3* open_connection();

    std::string db_path_;
    size_t max_size_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<sqlite3*> available_;
    size_t total_opened_{0};
    bool closed_{false};
};

// Loads settings from the settings table
Settings load_settings(sqlite3* db);

// Check if string consists only of ASCII digits
bool is_all_numeric(const std::string& s);

// Check if an ID already exists in the ids table
bool id_exists(sqlite3* db, const std::string& id);

// Generates a random unique ID with collision checking and non-numeric requirement
std::string generate_id(sqlite3* db, const Settings& settings);

// Initializes the database: creates tables and inserts default settings if they don't exist.
// Creates the parent directory if needed. Throws std::runtime_error on failure.
void initialize_database(const std::string& db_path);

// Saves settings (id_length, charset, admin_secret) to the database.
// Uses INSERT OR REPLACE. Throws std::runtime_error on failure.
void save_settings(sqlite3* db, const Settings& settings);

#endif // DB_H
