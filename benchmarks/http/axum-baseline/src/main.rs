//! Reference service for Pure's HTTP benchmarks: the same contract as Pure's
//! document server (GET/PUT /objects/{key}, If-Match / If-None-Match: *,
//! a PUT body of u32le start, u32le remove, then the bytes to insert; the
//! reply is the inserted bytes; ETag is the revision).
//!
//! axum-baseline --port P [--durable DIR | --atomic DIR]
//! Without either, documents live in memory. --durable writes the document to
//! DIR/key and fdatasyncs before answering, as a straightforward durable
//! handler would (not crash-safe: a crash after truncation loses it).
//! --atomic writes DIR/key.tmp, fsyncs it, renames it over DIR/key and fsyncs
//! the directory: the usual crash-safe replacement.
use axum::{
    body::Bytes,
    extract::{Path, State},
    http::{HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::get,
    Router,
};
use std::{collections::HashMap, path::PathBuf, sync::Arc};
use tokio::sync::Mutex;

#[derive(Default)]
struct Document {
    bytes: Vec<u8>,
    revision: u64,
}

struct Service {
    documents: Mutex<HashMap<String, Arc<Mutex<Document>>>>,
    durable: Option<PathBuf>,
    atomic: bool,
}

fn valid_key(key: &str) -> bool {
    !key.is_empty()
        && key.len() <= 64
        && key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
}

fn tagged(status: StatusCode, revision: u64, error: &'static str, body: Bytes) -> Response {
    let mut response = (status, body).into_response();
    let headers = response.headers_mut();
    headers.insert("X-Pure-Error", HeaderValue::from_static(error));
    if revision > 0 {
        headers.insert("ETag", HeaderValue::from_str(&format!("\"{revision}\"")).unwrap());
    }
    response
}

async fn document(service: &Service, key: &str) -> Arc<Mutex<Document>> {
    let mut documents = service.documents.lock().await;
    documents.entry(key.to_string()).or_default().clone()
}

async fn show(State(service): State<Arc<Service>>, Path(key): Path<String>) -> Response {
    if !valid_key(&key) {
        return tagged(StatusCode::BAD_REQUEST, 0, "request", Bytes::new());
    }
    let document = document(&service, &key).await;
    let document = document.lock().await;
    if document.revision == 0 {
        return tagged(StatusCode::NOT_FOUND, 0, "not_found", Bytes::new());
    }
    tagged(StatusCode::OK, document.revision, "none", Bytes::copy_from_slice(&document.bytes))
}

async fn edit(
    State(service): State<Arc<Service>>,
    Path(key): Path<String>,
    headers: HeaderMap,
    body: Bytes,
) -> Response {
    let expected = match (headers.get("if-match"), headers.get("if-none-match")) {
        (Some(tag), None) => tag
            .to_str()
            .ok()
            .and_then(|t| t.strip_prefix('"')?.strip_suffix('"')?.parse::<u64>().ok()),
        (None, Some(star)) if star == "*" => Some(0),
        _ => None,
    };
    let Some(expected) = expected.filter(|_| valid_key(&key) && body.len() >= 8) else {
        return tagged(StatusCode::BAD_REQUEST, 0, "request", Bytes::new());
    };
    let start = u32::from_le_bytes(body[0..4].try_into().unwrap()) as usize;
    let remove = u32::from_le_bytes(body[4..8].try_into().unwrap()) as usize;
    let document = document(&service, &key).await;
    let mut document = document.lock().await;
    if document.revision != expected {
        return tagged(StatusCode::PRECONDITION_FAILED, document.revision, "conflict", Bytes::new());
    }
    if start > document.bytes.len() || remove > document.bytes.len() - start {
        return tagged(StatusCode::PAYLOAD_TOO_LARGE, 0, "limit", Bytes::new());
    }
    let insert = body.slice(8..);
    document.bytes.splice(start..start + remove, insert.iter().copied());
    if let Some(directory) = &service.durable {
        let (directory, path, atomic) = (directory.clone(), directory.join(&key), service.atomic);
        let bytes = document.bytes.clone();
        let written = tokio::task::spawn_blocking(move || -> std::io::Result<()> {
            use std::io::Write;
            if !atomic {
                let mut file = std::fs::File::create(path)?;
                file.write_all(&bytes)?;
                return file.sync_data();
            }
            let temporary = path.with_extension("tmp");
            let mut file = std::fs::File::create(&temporary)?;
            file.write_all(&bytes)?;
            file.sync_all()?;
            std::fs::rename(&temporary, &path)?;
            std::fs::File::open(&directory)?.sync_all()
        })
        .await;
        if !matches!(written, Ok(Ok(()))) {
            return tagged(StatusCode::INTERNAL_SERVER_ERROR, 0, "storage", Bytes::new());
        }
    }
    document.revision += 1;
    tagged(StatusCode::OK, document.revision, "none", insert)
}

#[tokio::main]
async fn main() {
    let arguments: Vec<String> = std::env::args().collect();
    let mut port = 8768u16;
    let mut durable = None;
    let mut atomic = false;
    let mut at = 1;
    while at + 1 < arguments.len() {
        match arguments[at].as_str() {
            "--port" => port = arguments[at + 1].parse().expect("port"),
            "--durable" => durable = Some(PathBuf::from(&arguments[at + 1])),
            "--atomic" => {
                durable = Some(PathBuf::from(&arguments[at + 1]));
                atomic = true;
            }
            other => panic!("unknown argument {other}"),
        }
        at += 2;
    }
    if let Some(directory) = &durable {
        std::fs::create_dir_all(directory).expect("durable directory");
    }
    let service = Arc::new(Service { documents: Mutex::new(HashMap::new()), durable, atomic });
    let app = Router::new()
        .route("/objects/{key}", get(show).put(edit))
        .with_state(service);
    let listener = tokio::net::TcpListener::bind(("127.0.0.1", port)).await.expect("bind");
    println!("{{\"port\":{}}}", listener.local_addr().unwrap().port());
    use std::io::Write;
    std::io::stdout().flush().unwrap();
    axum::serve(listener, app)
        .with_graceful_shutdown(async {
            tokio::signal::ctrl_c().await.ok();
        })
        .await
        .unwrap();
}
