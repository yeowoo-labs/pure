//! Reference service for Pure's HTTP benchmarks: the same contract as Pure's
//! document server (GET/PUT /objects/{key}, If-Match / If-None-Match: *,
//! a PUT body of u32le start, u32le remove, then the bytes to insert; the
//! reply is the inserted bytes; ETag is the revision).
//!
//! The scoreboard app of the app benchmarks, as Pure serves an aup app's views
//! and inputs: POST /inputs/score?points=N with a name as the body adds N to
//! that name's points and replies the name; GET /views/total is the sum of
//! the points; GET /views/leaders?from=F&count=C is a page of the board in name
//! order as JSON ({"rows":N,"from":F,"items":[{"name":..,"points":..}]}).
//! Durable (either flag), each score is appended to DIR/scores.log and
//! fdatasynced before the answer, and the log is replayed at start: an
//! append-only log is crash safe, a torn last record was never acknowledged.
//!
//! axum-baseline --port P [--durable DIR | --atomic DIR]
//! Without either, documents live in memory. --durable writes the document to
//! DIR/key and fdatasyncs before answering, as a straightforward durable
//! handler would (not crash-safe: a crash after truncation loses it).
//! --atomic writes DIR/key.tmp, fsyncs it, renames it over DIR/key and fsyncs
//! the directory: the usual crash-safe replacement.
use axum::{
    body::Bytes,
    extract::{Path, RawQuery, State},
    http::{header, HeaderMap, HeaderValue, StatusCode},
    response::{IntoResponse, Response},
    routing::{get, post},
    Router,
};
use std::{
    collections::{BTreeMap, HashMap},
    path::PathBuf,
    sync::Arc,
};
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
    board: Mutex<Board>,
}

/// The scoreboard: points by name, in name (byte) order, and the log.
struct Board {
    points: BTreeMap<String, u64>,
    log: Option<Arc<std::fs::File>>,
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

/// The value of a name=value pair of a query, parsed.
fn query_value<T: std::str::FromStr>(query: &Option<String>, name: &str) -> Option<T> {
    query.as_deref()?.split('&').find_map(|pair| pair.strip_prefix(name)?.strip_prefix('=')?.parse().ok())
}

fn text(status: StatusCode, error: &'static str, body: String) -> Response {
    let mut response = (status, body).into_response();
    response.headers_mut().insert("X-Pure-Error", HeaderValue::from_static(error));
    response
}

async fn score(State(service): State<Arc<Service>>, RawQuery(query): RawQuery, body: Bytes) -> Response {
    let (Some(points), Ok(name)) = (query_value::<u32>(&query, "points"), std::str::from_utf8(&body)) else {
        return tagged(StatusCode::BAD_REQUEST, 0, "request", Bytes::new());
    };
    let mut board = service.board.lock().await;
    if let Some(log) = board.log.clone() {
        let mut record = Vec::with_capacity(8 + name.len());
        record.extend_from_slice(&(name.len() as u32).to_le_bytes());
        record.extend_from_slice(name.as_bytes());
        record.extend_from_slice(&points.to_le_bytes());
        let written = tokio::task::spawn_blocking(move || -> std::io::Result<()> {
            use std::io::Write;
            (&*log).write_all(&record)?;
            log.sync_data()
        })
        .await;
        if !matches!(written, Ok(Ok(()))) {
            return tagged(StatusCode::INTERNAL_SERVER_ERROR, 0, "storage", Bytes::new());
        }
    }
    *board.points.entry(name.to_string()).or_default() += points as u64;
    tagged(StatusCode::OK, 0, "none", body)
}

async fn total(State(service): State<Arc<Service>>) -> Response {
    let board = service.board.lock().await;
    text(StatusCode::OK, "none", board.points.values().sum::<u64>().to_string())
}

fn json_text(out: &mut String, text: &str) {
    out.push('"');
    for c in text.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

async fn leaders(State(service): State<Arc<Service>>, RawQuery(query): RawQuery) -> Response {
    let from = query_value::<usize>(&query, "from").unwrap_or(0);
    let count = query_value::<usize>(&query, "count").unwrap_or(usize::MAX);
    let board = service.board.lock().await;
    let rows = board.points.len();
    let from = from.min(rows);
    let mut out = format!("{{\"rows\":{rows},\"from\":{from},\"items\":[");
    for (i, (name, points)) in board.points.iter().skip(from).take(count).enumerate() {
        out.push_str(if i > 0 { ",{\"name\":" } else { "{\"name\":" });
        json_text(&mut out, name);
        out.push_str(&format!(",\"points\":{points}}}"));
    }
    out.push_str("]}");
    let mut response = text(StatusCode::OK, "none", out);
    response.headers_mut().insert(header::CONTENT_TYPE, HeaderValue::from_static("application/json"));
    response
}

/// Replay the scoreboard's log up to its last whole record, and keep it open for appending.
fn open_board(directory: &Option<PathBuf>) -> Board {
    let mut points = BTreeMap::new();
    let Some(directory) = directory else { return Board { points, log: None } };
    let path = directory.join("scores.log");
    let bytes = std::fs::read(&path).unwrap_or_default();
    let mut at = 0;
    while at + 4 <= bytes.len() {
        let length = u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) as usize;
        if at + 8 + length > bytes.len() {
            break;
        }
        let name = String::from_utf8_lossy(&bytes[at + 4..at + 4 + length]).into_owned();
        let added = u32::from_le_bytes(bytes[at + 4 + length..at + 8 + length].try_into().unwrap());
        *points.entry(name).or_insert(0u64) += added as u64;
        at += 8 + length;
    }
    let log = std::fs::OpenOptions::new().create(true).write(true).open(&path).expect("scores.log");
    log.set_len(at as u64).expect("trim the log");
    use std::io::Seek;
    (&log).seek(std::io::SeekFrom::End(0)).expect("seek");
    Board { points, log: Some(Arc::new(log)) }
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
    let board = Mutex::new(open_board(&durable));
    let service = Arc::new(Service { documents: Mutex::new(HashMap::new()), durable, atomic, board });
    let app = Router::new()
        .route("/objects/{key}", get(show).put(edit))
        .route("/inputs/score", post(score))
        .route("/views/total", get(total))
        .route("/views/leaders", get(leaders))
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
