# ID Registry

A lightweight, personal ID generation and ownership registry service.

This project provides a centralized system for generating unique, opaque IDs that multiple applications can reference without duplicating data maintenance logic. Each ID is owned by one application ("owner"), which is responsible for the actual data behind it.

The system consists of:
- A Rust-based HTTP server (Rocket) that handles ID generation, lookup, confirmation, and basic CRUD stubs
- A simple C++ Qt GUI for initial setup (database path, default ID length/charset)

All apps run on the same Windows machine. The database is SQLite.

## Why this project?

This project was inspired by seeing "REST API" listed as a required skill in a job advert. The aim is to showcase my ability to create a complete application — with the assistance of AI — even in areas that were still unfamiliar to me: Rust, C++/Qt 6, and REST API development. I also worked with entirely new technologies: Rocket (Rust web framework) and NSSM (Non-Sucking Service Manager for Windows services).

It should be noted that **no thought has been given to security** in this project, as my knowledge of security matters is very limited.

## Features

- Generate random, unique IDs (customizable length & charset, never fully numeric)
- Tag IDs with an owner and optional table name
- Soft-delete support (`deleted` flag)
- Confirmation mechanism (`confirmed` flag)
- Suspend/resume mode for clean backups
- Runs as a Windows service (via NSSM)
- Basic client integration example (PHP → HTTP calls)

## Architecture

```
Client App (PHP / C++ / etc.)
        ↓ HTTP (localhost:8000)
Rust Server (Rocket) ───► SQLite DB (id_registry.db)
        ↑ Registry (HKCU) for DB path
Setup GUI (Qt) ─────────┘
```

- IDs are stored in `ids` table with owner, table_name, confirmed, deleted, created_at
- Settings (length, charset) stored in `settings` table
- Server reads DB path from Windows Registry (HKCU\Software\IdRegistry\Settings\DBPath)

**Important:** Users are **not** dependent on the GUI. The server will start as long as the registry key exists and points to a valid SQLite database file with the correct schema. You can create the database manually (via SQLiteStudio or command line) if preferred.

## Screenshots

(Might be added later – e.g. GUI setup window, curl examples, service manager)

## Requirements

- Windows (tested on Win 10/11)
- Rust 1.80+ (for server)
- Qt 6.x + CMake (for GUI)
- NSSM (to run server as service)
- SQLite browser (optional, for manual inspection)

## Installation

### 1. Clone & build server

```bash
git clone https://github.com/StephanNaro/id-registry.git
```
In server/src/main.rs: change ```your-secret``` to something sutiable.
```bash
cd id-registry/server
cargo build --release
```

The executable will be at `target\release\id-registry-server.exe`.

### 2. Build GUI (optional, but recommended for first-time setup)

```bash
cd ../gui
cmake -DBUILD_DEVELOPMENT_MODE=OFF -G "Ninja" ..\..
cmake --build .
```

Run `build\idRegistryGui.exe` (or whatever your output name is) to set DB path and defaults.

### 3. Set up as Windows service (recommended)

1. Download NSSM: https://nssm.cc/download
2. Extract to e.g. `C:\Tools\nssm`
3. Run as Administrator:

```cmd
C:\Tools\nssm\nssm.exe install IdRegistryService
```

Fill in the GUI:

- Path → full path to `id-registry-server.exe`
- Startup directory → folder containing the .exe
- Log on → This account → `.\YourUsername` + password
- Startup type → Automatic
- Output / Error → paths to log files (optional)

4. Start service:

```cmd
sc start IdRegistryService
```

Check status:

```cmd
sc query IdRegistryService
```

### 4. First-time setup

- Run the GUI once → set database path (e.g. `C:\Users\YourName\AppData\Local\id_registry.db`) → Save & Initialize
- Or manually create the DB file and tables (see schema below)

The server reads the path from the registry on startup.

## Usage (examples via curl)

Generate ID:

```bash
curl -X POST http://127.0.0.1:8000/generate -H "Content-Type: application/json" -d "{\"owner\":\"person_app\",\"table\":\"contacts\"}"
```

Confirm:

Replace ```existing_id``` with an id created earlier
```bash
curl -X POST http://127.0.0.1:8000/confirm -H "Content-Type: application/json" -d "{\"id\":\"existing_id\"}"
```

Get details:

```bash
curl http://127.0.0.1:8000/get_id/existing_id
```

Health check:

```bash
curl http://127.0.0.1:8000/health
```

Suspend (for backup) (replacing ```your-secret``` to match the source file):

```bash
curl -X POST "http://127.0.0.1:8000/suspend?secret=your-secret"
```

Resume (replacing ```your-secret``` to match the source file):

```bash
curl -X POST "http://127.0.0.1:8000/resume?secret=your-secret"
```

## Database Schema

```sql
CREATE TABLE ids (
    id            TEXT PRIMARY KEY,
    owner         TEXT NOT NULL,
    table_name    TEXT,
    user_id       TEXT,
    confirmed     INTEGER DEFAULT 0,
   deleted       INTEGER DEFAULT 0,
   created_at    DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);
```

Default settings inserted on first run:

```sql
INSERT OR IGNORE INTO settings (key, value) VALUES ('id_length', '12');
INSERT OR IGNORE INTO settings (key, value) VALUES ('charset', 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789');
```

## PHP Client Example

```php
function createRemoteId($owner, $table = null) {
    $client = new GuzzleHttp\Client();
    $response = $client->post('http://127.0.0.1:8000/generate', [
        'json' => [
            'owner' => $owner,
            'table' => $table,
        ],
    ]);

    $data = json_decode($response->getBody(), true);
    return $data['id'] ?? null;
}
```

## Backup / Maintenance

1. Suspend writes: `curl -X POST "http://127.0.0.1:8000/suspend?secret=..."`
2. Wait ~10 seconds
3. Copy the `.db` file (and `-wal`/`-shm` if present)
4. Resume: `curl -X POST "http://127.0.0.1:8000/resume?secret=..."`

## Possible future improvements

1. The most pressing need is for storing ```your-secret``` in the database in stead of hard-coding them in the source code.
2. The configuration app should clearly be named ```config```, not ```gui```.

Please see [Suggestions.md](Suggestions.md) and [SuggestShareResources.md](SuggestShareResources.md) for further ideas.

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0). See [LICENSE](LICENSE) for details.

## Contributing

Bug reports, feature ideas, and PRs welcome!