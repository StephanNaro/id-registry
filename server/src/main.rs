// SPDX-License-Identifier: GPL-3.0-or-later
use anyhow::Result;
use rocket::{get, post, put, delete, routes, serde::json::Json, State, Request, catch, catchers};
use rocket::http::{ContentType, Status};
use rocket::response::{self, Responder};
use rusqlite::OptionalExtension;
use serde::Serialize;
use std::sync::{Arc, atomic::{AtomicBool, Ordering}};

use id_registry_server::{create_db_pool, DbPool, generate_id, get_db_path, load_settings, Settings};

//
// Structs
//

#[derive(Clone)]
struct AppState {
    settings: Arc<Settings>,
    pool: DbPool,
    suspended: Arc<AtomicBool>,
}

#[derive(Serialize)]
struct ApiError {
    error: String,
    message: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    details: Option<String>,
}

struct JsonError {
    status: Status,
    error: ApiError,
}

#[derive(serde::Serialize)]
struct HealthResponse {
    status: String,
    db_path: String,
    settings: Settings,
}

#[derive(serde::Serialize)]
struct PreviewResponse {
    preview_id: String,
}

#[derive(serde::Deserialize)]
struct GenerateRequest {
    owner: String,
    #[serde(default)]
    table: Option<String>,
}

#[derive(serde::Serialize)]
struct IdDetails {
    id: String,
    owner: String,
    table: Option<String>,
    confirmed: i32,
    created_at: String,
}

#[derive(serde::Deserialize)]
struct ConfirmRequest {
    id: String,
}

#[derive(serde::Serialize)]
struct ConfirmResponse {
    success: bool,
    message: String,
}

//
// Functions
//

impl<'r> Responder<'r, 'r> for JsonError {
    fn respond_to(self, _: &'r Request<'_>) -> response::Result<'r> {
        let body = serde_json::to_string(&self.error).unwrap_or_else(|_| {
            r#"{"error":"internal_error","message":"Failed to serialize error"}"#.to_string()
        });

        response::Response::build()
            .status(self.status)
            .header(ContentType::JSON)
            .sized_body(body.len(), std::io::Cursor::new(body))
            .ok()
    }
}

#[catch(400)]
fn bad_request(_req: &Request<'_>) -> JsonError {
    JsonError {
        status: Status::BadRequest,
        error: ApiError {
            error: "bad_request".to_string(),
            message: "Invalid request parameters or body".to_string(),
            details: None,
        },
    }
}

#[catch(401)]
fn unauthorized(_req: &Request<'_>) -> JsonError {
    JsonError {
        status: Status::Unauthorized,
        error: ApiError {
            error: "unauthorized".to_string(),
            message: "Authentication required".to_string(),
            details: None,
        },
    }
}

#[catch(404)]
fn not_found(_req: &Request<'_>) -> JsonError {
    JsonError {
        status: Status::NotFound,
        error: ApiError {
            error: "not_found".to_string(),
            message: "Resource not found".to_string(),
            details: None,
        },
    }
}

#[catch(501)]
fn not_implemented(_req: &Request<'_>) -> JsonError {
    JsonError {
        status: Status::NotImplemented,
        error: ApiError {
            error: "not_implemented".to_string(),
            message: "This feature is not yet available".to_string(),
            details: None,
        },
    }
}

#[catch(503)]
fn service_unavailable(_req: &Request<'_>) -> JsonError {
    JsonError {
        status: Status::ServiceUnavailable,
        error: ApiError {
            error: "service_unavailable".to_string(),
            message: "Server is temporarily suspended for maintenance".to_string(),
            details: None,
        },
    }
}

#[catch(default)]
fn default_error(status: Status, _req: &Request<'_>) -> JsonError {
    JsonError {
        status,
        error: ApiError {
            error: "internal_error".to_string(),
            message: format!("Unexpected error ({})", status.code),
            details: None,
        },
    }
}

#[rocket::main]
async fn main() -> Result<(), rocket::Error> {
    println!("Starting ID Registry Server...");

    let pool = create_db_pool().expect("Failed to create DB pool");

    // Load settings once at startup (using a connection from pool)
    let conn = pool.get().expect("Failed to get connection for init");
    let settings = load_settings(&conn).expect("Failed to load settings");

    println!("Database pool ready");
    println!("ID length: {}", settings.id_length);
    println!("Charset  : {}", settings.charset);

    let settings_arc = Arc::new(settings);

    let suspended = Arc::new(AtomicBool::new(false));

    rocket::build()
        .manage(AppState {
            settings: settings_arc,
            pool,
            suspended,
        })
        .mount("/", routes![health, preview, generate, confirm, update_id, delete_id, get_id, suspend, resume])
        .register("/", catchers![
            bad_request,
            unauthorized,
            not_found,
            not_implemented,
            service_unavailable,
            default_error
        ])
        .launch()
        .await?;

    Ok(())
}

// POST /suspend?secret=yourpassword
#[post("/suspend?<secret>")]
fn suspend(
    secret: Option<String>,
    state: &State<AppState>,
) -> Result<String, Status> {
    if secret.as_deref() != Some("your-secret") {
        return Err(Status::Unauthorized);
    }

    state.suspended.store(true, Ordering::SeqCst);
    Ok("Server suspended (new requests rejected)".to_string())
}

// POST /resume?secret=yourpassword
#[post("/resume?<secret>")]
fn resume(
    secret: Option<String>,
    state: &State<AppState>,
) -> Result<String, Status> {
    if secret.as_deref() != Some("your-secret") {
        return Err(Status::Unauthorized);
    }

    state.suspended.store(false, Ordering::SeqCst);
    Ok("Server resumed".to_string())
}

#[get("/health")]
fn health(state: &State<AppState>,) -> Result<Json<HealthResponse>, Status> {
    let db_path = get_db_path().map_err(|_| Status::InternalServerError)?;

    Ok(Json(HealthResponse {
        status: if state.suspended.load(Ordering::SeqCst) { "Suspended".to_string() } else { "ok".to_string() },
        db_path,
        settings: state.settings.as_ref().clone(),
    }))
}

#[get("/preview")]
fn preview(state: &State<AppState>,) -> Result<Json<PreviewResponse>, Status> {
    let conn = &state.pool.get()
        .map_err(|e| {
            eprintln!("Pool error: {}", e);
            Status::InternalServerError
        })?;

    match generate_id(&conn, &state.settings.as_ref()) {
        Ok(id) => Ok(Json(PreviewResponse { preview_id: id })),
        Err(e) => {
            eprintln!("Generation failed: {}", e);
            Err(Status::InternalServerError)
        }
    }
}

#[post("/generate", format = "json", data = "<request>")]
fn generate(
    request: Json<GenerateRequest>,
    state: &State<AppState>,
) -> Result<Json<IdDetails>, Status> {
    if state.suspended.load(Ordering::SeqCst) {
        return Err(Status::ServiceUnavailable);
    }

    println!("Generate request: owner={}, table={:?}", request.owner, request.table);

    let owner_clean = request.owner.trim().to_string();
    if owner_clean.is_empty() || !owner_clean.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return Err(Status::BadRequest);
    }

    let conn = &state.pool.get()
        .map_err(|_| Status::InternalServerError)?;

    let id = generate_id(&conn, &state.settings.as_ref())
        .map_err(|_| Status::InternalServerError)?;

    let mut stmt = conn.prepare(
        "INSERT INTO ids (id, owner, table_name, confirmed, created_at)
         VALUES (?1, ?2, ?3, 0, CURRENT_TIMESTAMP)"
    ).map_err(|_| Status::InternalServerError)?;

    stmt.execute(rusqlite::params![&id, &owner_clean, &request.table])
        .map_err(|_| Status::InternalServerError)?;

    let created_at: String = conn.query_row(
        "SELECT created_at FROM ids WHERE id = ?1",
        [&id],
        |row| row.get(0),
    ).unwrap_or_else(|_| "unknown".to_string());

    Ok(Json(IdDetails {
        id,
        owner: owner_clean,
        table: request.table.clone(),
        confirmed: 0,
        created_at,
    }))
}

#[post("/confirm", format = "json", data = "<request>")]
fn confirm(
    request: Json<ConfirmRequest>,
    state: &State<AppState>,
) -> Result<Json<ConfirmResponse>, Status> {
    if state.suspended.load(Ordering::SeqCst) {
        return Err(Status::ServiceUnavailable);
    }

    let conn = &state.pool.get()
        .map_err(|_| Status::InternalServerError)?;

    let rows_affected = conn.execute(
        "UPDATE ids SET confirmed = 1 WHERE id = ?1",
        [&request.id],
    ).map_err(|_| Status::InternalServerError)?;

    if rows_affected == 0 {
        return Ok(Json(ConfirmResponse {
            success: false,
            message: format!("ID {} not found or already confirmed", request.id),
        }));
    }

    Ok(Json(ConfirmResponse {
        success: true,
        message: format!("ID {} confirmed", request.id),
    }))
}

#[get("/get_id/<id>")]
fn get_id(id: &str, state: &State<AppState>) -> Result<Json<IdDetails>, Status> {
    let conn = &state.pool.get()
        .map_err(|_| Status::InternalServerError)?;

    let mut stmt = conn.prepare(
        "SELECT owner, table_name, confirmed, created_at FROM ids WHERE id = ?1 AND deleted = 0"
    ).map_err(|_| Status::InternalServerError)?;

    let details: Option<IdDetails> = stmt.query_row([&id], |row| {
        Ok(IdDetails {
            id: id.to_string(),
            owner: row.get(0)?,
            table: row.get(1)?,
            confirmed: row.get(2)?,
            created_at: row.get(3)?,
        })
    }).optional().map_err(|_| Status::InternalServerError)?;

    match details {
        Some(d) => Ok(Json(d)),
        None => Err(Status::NotFound),
    }
}

// "/ids/" should probably be called something else
#[put("/ids/<_id>", format = "json", data = "<_data>")]
fn update_id(_id: &str, _data: Json<serde_json::Value>, state: &State<AppState>,) -> Result<String, Status> {
    if state.suspended.load(Ordering::SeqCst) {
        return Err(Status::ServiceUnavailable);
    }

    Err(Status::NotImplemented)  // 501
}

// "/ids/" should probably be called something else
#[delete("/ids/<_id>")]
fn delete_id(_id: &str, state: &State<AppState>,) -> Result<String, Status> {
    if state.suspended.load(Ordering::SeqCst) {
        return Err(Status::ServiceUnavailable);
    }

    Err(Status::NotImplemented)  // 501
}