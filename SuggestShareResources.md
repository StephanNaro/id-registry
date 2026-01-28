### Shared constants / registry key path between C++ and Rust

Yes — good idea to reduce duplication and inconsistency risk.

**Options for sharing the registry key path** (`"Software\\IdRegistry\\Settings"` and `"DBPath"`):

**Best practical approach (no shared file needed)**:
- Keep it as a **string literal** in both projects, but put it in a **header file** for C++ and a **const** in Rust.
- Use the exact same value everywhere.

C++ (`include/constants.h` or similar):

```cpp
#pragma once

namespace Constants {
    inline constexpr const char* REGISTRY_COMPANY   = "IdRegistry";
    inline constexpr const char* REGISTRY_APP       = "Settings";
    inline constexpr const char* REGISTRY_KEY_PATH  = "Software\\IdRegistry\\Settings";
    inline constexpr const char* REGISTRY_VALUE_DB  = "DBPath";
}
```

Rust (`server/src/constants.rs`):

```rust
pub const REGISTRY_KEY_PATH: &str = r"Software\IdRegistry\Settings";
pub const REGISTRY_VALUE_DB: &str = "DBPath";
```

Then in Rust `get_db_path()`:

```rust
let key = hkcu.open_subkey_with_flags(REGISTRY_KEY_PATH, KEY_READ)?;
let path: String = key.get_value(REGISTRY_VALUE_DB)?;
```

This is simple, zero-runtime cost, and easy to grep/search/change.

**Alternative: Shared text/JSON resource file**  
(e.g. `shared/config.toml` or `shared/constants.json` in repo root)

```toml
# shared/config.toml
[registry]
company = "IdRegistry"
app = "Settings"
db_value = "DBPath"
```

Then in Rust (with `toml` crate):

```toml
[dependencies]
toml = "0.8"
serde = { version = "1.0", features = ["derive"] }
```

But: Adds a dependency + parsing overhead for something that never changes at runtime.  
Only worth it if you later add many more shared constants (ports, default charset, etc.).

**For id_length and charset defaults**  
These are already in the DB as defaults — that's perfect.  
The Rust code already falls back to 12 / alphanumeric if missing.  
No need to duplicate them in code unless you want compile-time fallbacks for very early bootstrap (but the DB is authoritative, so current approach is fine).

If you want compile-time constants anyway (for symmetry with registry strings):

Rust:

```rust
pub const DEFAULT_ID_LENGTH: u32 = 12;
pub const DEFAULT_CHARSET: &str = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
```

And use them in `load_settings`:

```rust
let id_length: u32 = ... .unwrap_or(DEFAULT_ID_LENGTH);
let charset: String = ... .unwrap_or_else(|| DEFAULT_CHARSET.to_string());
```

Same in C++ if you add preview generation there later.
