// SPDX-License-Identifier: GPL-3.0-or-later
use anyhow::{Context, Result};
use rand::seq::SliceRandom;
use r2d2::Pool;
use r2d2_sqlite::SqliteConnectionManager;
use serde::Serialize;
use winreg::enums::*;
use winreg::RegKey;

#[derive(Debug, Clone, Serialize)]
pub struct Settings {
    pub id_length: u32,
    pub charset: String,
    pub admin_secret: String,
}

pub type DbPool = Pool<SqliteConnectionManager>;

pub fn get_db_path() -> Result<String> {
    let hkcu = RegKey::predef(HKEY_CURRENT_USER);
    let key = hkcu
        .open_subkey_with_flags("Software\\IdRegistry\\Settings", KEY_READ)
        .context("Failed to open IdRegistry registry key")?;

    let path: String = key
        .get_value("DBPath")
        .context("DBPath value not found in registry")?;

    if path.trim().is_empty() {
        anyhow::bail!("DBPath is empty in registry");
    }

    Ok(path)
}

pub fn load_settings(conn: &r2d2::PooledConnection<SqliteConnectionManager>) -> Result<Settings> {
    let mut stmt = conn.prepare("SELECT value FROM settings WHERE key = ?1")?;

    let id_length_str: String = stmt
        .query_row(["id_length"], |row| row.get(0))
        .context("Missing 'id_length' in settings table")?;

    let id_length: u32 = id_length_str
        .parse()
        .context("Invalid 'id_length' value")?;

    let charset: String = stmt
        .query_row(["charset"], |row| row.get(0))
        .context("Missing 'charset' in settings table")?;

    let admin_secret: String = stmt
        .query_row(["admin_secret"], |row| row.get(0))
        .context("Missing 'admin_secret' in settings table")?;

    Ok(Settings {
        id_length,
        charset,
        admin_secret,
    })
}

pub fn create_db_pool() -> Result<DbPool> {
    let path = get_db_path()
        .context("No database path configured in registry")?;

    let manager = SqliteConnectionManager::file(path)
        .with_init(|conn| {
            // Optional: set WAL mode on every new connection
            conn.execute_batch("PRAGMA journal_mode=WAL;")?;
            Ok(())
        });

    let pool = r2d2::Pool::builder()
        .max_size(10)           // adjust based on expected load
        .build(manager)
        .context("Failed to create connection pool")?;

    // Test one connection at startup
    let conn = pool.get()?;
    let mode: String = conn.query_row("PRAGMA journal_mode", [], |row| row.get(0))?;
    println!("Connection pool created – WAL mode: {}", mode);

    Ok(pool)
}

// Returns true if the string consists only of digits 0-9
fn is_all_numeric(s: &str) -> bool {
    s.chars().all(|c| c.is_ascii_digit())
}

// Checks whether the ID already exists in the ids table
fn id_exists(conn: &r2d2::PooledConnection<SqliteConnectionManager>, id: &str) -> Result<bool> {
    let count: u64 = conn.query_row(
        "SELECT COUNT(*) FROM ids WHERE id = ?1",
        [id],
        |row| row.get(0),
    )?;
    Ok(count > 0)
}

/// Generates one random ID using current settings.
/// Retries on collision or all-numeric result.
/// Returns Ok(id) or Err after max retries.
pub fn generate_id(conn: &r2d2::PooledConnection<SqliteConnectionManager>, settings: &Settings) -> Result<String> {
    const MAX_RETRIES: usize = 100;

    let charset_chars: Vec<char> = settings.charset.chars().collect();
    if charset_chars.is_empty() {
        anyhow::bail!("Charset is empty");
    }

    let mut rng = rand::thread_rng();

    for attempt in 1..=MAX_RETRIES {
        let mut id = String::with_capacity(settings.id_length as usize);

        for _ in 0..settings.id_length {
            let c = *charset_chars
                .choose(&mut rng)
                .expect("Charset cannot be empty here");
            id.push(c);
        }

        // Skip if all numeric
        if is_all_numeric(&id) {
            continue;
        }

        // Check collision
        if !id_exists(conn, &id)? {
            return Ok(id);
        }

        // Optional: log attempts in dev mode
        if attempt % 20 == 0 {
            println!("Collision detected on attempt {}, retrying...", attempt);
        }
    }

    anyhow::bail!(
        "Failed to generate unique ID after {} attempts. Database may be very full.",
        MAX_RETRIES
    );
}