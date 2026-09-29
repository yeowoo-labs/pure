//! A QUIC service holding many clients at rest, for M1 of
//! docs/design/011-pure-transport.md: the same exchange as a Pure link's
//! (a client sends one order, the service stores it, says so, and the two
//! stay connected, quiet), over quinn 0.11 with rustls (AES-128-GCM).
//!
//!   idle-quic serve CERT.der KEY.der [PORT]
//!       a service on 127.0.0.1 (the port on stdout as JSON), one thread;
//!       a line of JSON on stdout at every 1,000th order stored. Its
//!       stateless reset key comes from KEY.der, so one started again on
//!       the same port resets the connections of the one before at once
//!   idle-quic connect PORT FIRST COUNT WINDOW
//!       clients FIRST.. each connect, send order 1 and wait until it is
//!       stored, WINDOW at a time over one endpoint; a line of JSON; the
//!       connections stay open until stdin closes; then COUNT orders more
//!       (number 2) over them, and another line
//!   idle-quic steady PORT FIRST COUNT PERIOD_MS SECONDS WINDOW
//!       clients FIRST.. connect (WINDOW at a time), then each makes an
//!       order every PERIOD_MS for SECONDS and sends it over its connection;
//!       one that fails (no answer in a second, or the connection lost)
//!       connects again and sends it again. Then a line of JSON: orders made
//!       and stored, connections made again, the orders stored in each
//!       100 ms, the time from an order made to it stored
//!   idle-quic drain PORT COUNT SIZE
//!       one client sends COUNT messages of SIZE bytes over one stream
//!       (each after its u32 length) and waits until the service says it
//!       read them all; a line of JSON: how long
use std::collections::HashMap;
use std::net::SocketAddr;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use quinn::crypto::rustls::QuicClientConfig;
use quinn::{ClientConfig, Endpoint, IdleTimeout, ServerConfig, TransportConfig, VarInt};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{DigitallySignedStruct, SignatureScheme};
use tokio::io::AsyncReadExt;

fn transport() -> Arc<TransportConfig> {
    let mut t = TransportConfig::default();
    t.max_idle_timeout(Some(IdleTimeout::from(VarInt::from_u32(3_600_000)))); // an hour: connections at rest stay
    t.keep_alive_interval(None);
    Arc::new(t)
}

/// The order: the client's number and the order's.
fn order(client: u64, seq: u64) -> [u8; 64] {
    let mut bytes = [0u8; 64];
    bytes[..8].copy_from_slice(&client.to_le_bytes());
    bytes[8..16].copy_from_slice(&seq.to_le_bytes());
    bytes[16..19].copy_from_slice(b"pen");
    bytes
}

fn serve(cert: Vec<u8>, key: Vec<u8>, port: u16) {
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    runtime.block_on(async move {
        let reset = ring::hmac::Key::new(ring::hmac::HMAC_SHA256, ring::digest::digest(&ring::digest::SHA256, &key).as_ref());
        let mut config = ServerConfig::with_single_cert(
            vec![CertificateDer::from(cert)],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key)),
        )
        .unwrap();
        config.transport_config(transport());
        let socket = std::net::UdpSocket::bind(SocketAddr::from(([127, 0, 0, 1], port))).unwrap();
        let endpoint = Endpoint::new(quinn::EndpointConfig::new(Arc::new(reset)), Some(config), socket,
                                     quinn::default_runtime().unwrap()).unwrap();
        println!("{{\"port\":{}}}", endpoint.local_addr().unwrap().port());
        // What the service keeps: each client's last order heard, and its connection.
        let heard: Arc<Mutex<HashMap<u64, u64>>> = Arc::default();
        let kept: Arc<Mutex<Vec<quinn::Connection>>> = Arc::default();
        let stored = Arc::new(std::sync::atomic::AtomicU64::new(0));
        while let Some(incoming) = endpoint.accept().await {
            let (heard, kept, stored) = (heard.clone(), kept.clone(), stored.clone());
            tokio::spawn(async move {
                let Ok(connection) = incoming.await else { return };
                kept.lock().unwrap().push(connection.clone());
                while let Ok((mut send, mut recv)) = connection.accept_bi().await {
                    let mut first = [0u8; 4];
                    if recv.read_exact(&mut first).await.is_err() {
                        return;
                    }
                    if first == *DRAIN {
                        drain(&mut send, &mut recv).await;
                        continue;
                    }
                    let Ok(rest) = recv.read_to_end(1024).await else { return };
                    let mut bytes = first.to_vec();
                    bytes.extend_from_slice(&rest);
                    if bytes.len() < 16 {
                        return;
                    }
                    let client = u64::from_le_bytes(bytes[..8].try_into().unwrap());
                    let seq = u64::from_le_bytes(bytes[8..16].try_into().unwrap());
                    let fresh = {
                        let mut heard = heard.lock().unwrap();
                        let last = heard.entry(client).or_insert(0);
                        let fresh = seq > *last;
                        *last = (*last).max(seq);
                        fresh
                    };
                    let _ = send.write_all(&[1]).await;
                    let _ = send.finish();
                    if fresh {
                        let n = stored.fetch_add(1, std::sync::atomic::Ordering::Relaxed) + 1;
                        if n % 1000 == 0 {
                            println!("{{\"stored\":{n}}}");
                        }
                    }
                }
            });
        }
    });
}

/// A drain's stream begins with these bytes; an order's never does (a client number that large).
const DRAIN: &[u8; 4] = b"pure";

/// Messages each after its u32 length until a length of 0, read a chunk at
/// a time as they come; then how many, as the answer.
async fn drain(send: &mut quinn::SendStream, recv: &mut quinn::RecvStream) {
    let (mut count, mut pending, mut done) = (0u64, Vec::<u8>::new(), false);
    while !done {
        let Ok(Some(chunk)) = recv.read_chunk(1 << 20, true).await else { return };
        pending.extend_from_slice(&chunk.bytes);
        let mut at = 0;
        while pending.len() - at >= 4 {
            let length = u32::from_le_bytes(pending[at..at + 4].try_into().unwrap()) as usize;
            if length == 0 {
                done = true;
                break;
            }
            if pending.len() - at - 4 < length {
                break;
            }
            at += 4 + length;
            count += 1;
        }
        pending.drain(..at);
    }
    let _ = send.write_all(&count.to_le_bytes()).await;
    let _ = send.finish();
}

#[derive(Debug)]
struct Anyone; // the test's own service: its certificate is not checked

impl ServerCertVerifier for Anyone {
    fn verify_server_cert(&self, _: &CertificateDer<'_>, _: &[CertificateDer<'_>], _: &ServerName<'_>, _: &[u8],
                          _: UnixTime) -> Result<ServerCertVerified, rustls::Error> {
        Ok(ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(&self, message: &[u8], cert: &CertificateDer<'_>, dss: &DigitallySignedStruct)
                              -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(message, cert, dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms)
    }
    fn verify_tls13_signature(&self, message: &[u8], cert: &CertificateDer<'_>, dss: &DigitallySignedStruct)
                              -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(message, cert, dss,
            &rustls::crypto::ring::default_provider().signature_verification_algorithms)
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        rustls::crypto::ring::default_provider().signature_verification_algorithms.supported_schemes()
    }
}

fn percentile(values: &mut [f64], p: f64) -> f64 {
    if values.is_empty() {
        return 0.0;
    }
    values.sort_by(|a, b| a.partial_cmp(b).unwrap());
    values[((values.len() as f64 * p) as usize).min(values.len() - 1)]
}

fn client_config() -> ClientConfig {
    let crypto = rustls::ClientConfig::builder_with_provider(Arc::new(rustls::crypto::ring::default_provider()))
        .with_protocol_versions(&[&rustls::version::TLS13])
        .unwrap()
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(Anyone))
        .with_no_client_auth();
    let mut config = ClientConfig::new(Arc::new(QuicClientConfig::try_from(crypto).unwrap()));
    config.transport_config(transport());
    config
}

async fn exchange(connection: &quinn::Connection, client: u64, seq: u64) -> bool {
    let Ok((mut send, mut recv)) = connection.open_bi().await else { return false };
    if send.write_all(&order(client, seq)).await.is_err() || send.finish().is_err() {
        return false;
    }
    matches!(recv.read_to_end(16).await, Ok(answer) if answer == [1])
}

fn connect(port: u16, first: u64, count: u64, window: usize) {
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    runtime.block_on(async move {
        let mut endpoint = Endpoint::client(SocketAddr::from(([127, 0, 0, 1], 0))).unwrap();
        endpoint.set_default_client_config(client_config());
        let server = SocketAddr::from(([127, 0, 0, 1], port));
        let window = Arc::new(tokio::sync::Semaphore::new(window));
        let kept: Arc<Mutex<Vec<quinn::Connection>>> = Arc::default();
        let waited: Arc<Mutex<Vec<f64>>> = Arc::default();
        let began = Instant::now();
        let mut tasks = Vec::new();
        for client in first..first + count {
            let permit = window.clone().acquire_owned().await.unwrap();
            let (endpoint, kept, waited) = (endpoint.clone(), kept.clone(), waited.clone());
            tasks.push(tokio::spawn(async move {
                let started = Instant::now();
                let Ok(connecting) = endpoint.connect(server, "localhost") else { return false };
                let Ok(connection) = connecting.await else { return false };
                let ok = exchange(&connection, client, 1).await;
                waited.lock().unwrap().push(started.elapsed().as_secs_f64() * 1000.0);
                kept.lock().unwrap().push(connection);
                drop(permit);
                ok
            }));
        }
        let mut failed = 0;
        for task in tasks {
            if !task.await.unwrap_or(false) {
                failed += 1;
            }
        }
        let seconds = began.elapsed().as_secs_f64();
        let mut w = waited.lock().unwrap().clone();
        println!("{{\"clients\":{count},\"failed\":{failed},\"seconds\":{seconds:.3},\"per_second\":{:.0},\"stored_ms_p50\":{:.3},\"stored_ms_p99\":{:.3}}}",
                 count as f64 / seconds, percentile(&mut w, 0.5), percentile(&mut w, 0.99));
        // At rest until stdin closes; then one more order each, over the same connections.
        let mut rest = Vec::new();
        let _ = tokio::io::stdin().read_to_end(&mut rest).await;
        let connections = kept.lock().unwrap().clone();
        let began = Instant::now();
        let mut again = 0;
        for (k, connection) in connections.iter().enumerate() {
            if exchange(connection, first + k as u64, 2).await {
                again += 1;
            }
        }
        println!("{{\"again\":{again},\"seconds\":{:.3}}}", began.elapsed().as_secs_f64());
        tokio::time::sleep(Duration::from_millis(100)).await;
    });
}

/// Clients that keep sending, connecting again when their connection fails.
fn steady(port: u16, first: u64, count: u64, period: Duration, seconds: u64, window: usize) {
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    runtime.block_on(async move {
        let mut endpoint = Endpoint::client(SocketAddr::from(([127, 0, 0, 1], 0))).unwrap();
        endpoint.set_default_client_config(client_config());
        let server = SocketAddr::from(([127, 0, 0, 1], port));
        let connecting = Arc::new(tokio::sync::Semaphore::new(window));
        let began = Instant::now();
        let end = began + Duration::from_secs(seconds) + Duration::from_secs(2);
        let buckets = ((seconds + 70) * 10) as usize;
        let timeline: Arc<Vec<std::sync::atomic::AtomicU32>> =
            Arc::new((0..buckets).map(|_| std::sync::atomic::AtomicU32::new(0)).collect());
        let waited: Arc<Mutex<Vec<f64>>> = Arc::default();
        let (made, again) = (Arc::new(std::sync::atomic::AtomicU64::new(0)), Arc::new(std::sync::atomic::AtomicU64::new(0)));
        let mut tasks = Vec::new();
        for (k, client) in (first..first + count).enumerate() {
            let (endpoint, connecting, timeline, waited, made, again) =
                (endpoint.clone(), connecting.clone(), timeline.clone(), waited.clone(), made.clone(), again.clone());
            let start = began + Duration::from_secs(2) + period.mul_f64(k as f64 / count as f64);
            tasks.push(tokio::spawn(async move {
                let mut connection: Option<quinn::Connection> = None;
                let mut seq = 0u64;
                let mut next = start;
                loop {
                    if connection.is_none() {
                        let permit = connecting.acquire().await.unwrap();
                        if let Ok(c) = endpoint.connect(server, "localhost") {
                            if let Ok(Ok(c)) = tokio::time::timeout(Duration::from_secs(2), c).await {
                                connection = Some(c);
                            }
                        }
                        drop(permit);
                        if connection.is_none() {
                            tokio::time::sleep(Duration::from_millis(200)).await;
                            continue;
                        }
                    }
                    if Instant::now() >= end {
                        return;
                    }
                    tokio::time::sleep_until(next.into()).await;
                    seq += 1;
                    made.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let order_made = Instant::now();
                    next += period;
                    loop {
                        let c = connection.clone().unwrap();
                        let done = tokio::time::timeout(Duration::from_secs(1), exchange(&c, client, seq)).await;
                        if matches!(done, Ok(true)) {
                            let now = Instant::now();
                            let bucket = ((now - began).as_millis() / 100) as usize;
                            if bucket < timeline.len() {
                                timeline[bucket].fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                            }
                            waited.lock().unwrap().push((now - order_made).as_secs_f64() * 1000.0);
                            break;
                        }
                        // Lost: a new connection (a handshake), and the order again.
                        again.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                        connection = None;
                        while connection.is_none() {
                            let permit = connecting.acquire().await.unwrap();
                            if let Ok(c) = endpoint.connect(server, "localhost") {
                                if let Ok(Ok(c)) = tokio::time::timeout(Duration::from_secs(2), c).await {
                                    connection = Some(c);
                                }
                            }
                            drop(permit);
                            if connection.is_none() {
                                tokio::time::sleep(Duration::from_millis(200)).await;
                            }
                        }
                    }
                    if Instant::now() >= end {
                        return;
                    }
                }
            }));
        }
        for task in tasks {
            let _ = task.await;
        }
        let mut w = waited.lock().unwrap().clone();
        let stored = w.len();
        let timeline: Vec<u32> = timeline.iter().map(|b| b.load(std::sync::atomic::Ordering::Relaxed)).collect();
        let last = timeline.iter().rposition(|&b| b > 0).unwrap_or(0);
        let (mut longest, mut gap, mut since) = (0usize, 0usize, None::<usize>);
        for (b, &n) in timeline.iter().enumerate().take(last + 1) {
            if n > 0 {
                if let Some(s) = since {
                    if b - s - 1 > longest {
                        longest = b - s - 1;
                        gap = s + 1;
                    }
                }
                since = Some(b);
            }
        }
        let series: Vec<String> = timeline[..=last].iter().map(|n| n.to_string()).collect();
        println!("{{\"clients\":{count},\"made\":{},\"stored\":{stored},\"again\":{},\"stored_ms_p50\":{:.3},\"stored_ms_p99\":{:.3},\"stored_ms_max\":{:.3},\"longest_gap_ms\":{},\"gap_from_ms\":{},\"timeline_100ms\":[{}]}}",
                 made.load(std::sync::atomic::Ordering::Relaxed), again.load(std::sync::atomic::Ordering::Relaxed),
                 percentile(&mut w, 0.5), percentile(&mut w, 0.99), percentile(&mut w, 1.0), longest * 100, gap * 100,
                 series.join(","));
    });
}

/// One client, COUNT messages of SIZE bytes over one stream, and the answer.
fn drain_client(port: u16, count: u64, size: usize) {
    let runtime = tokio::runtime::Builder::new_current_thread().enable_all().build().unwrap();
    runtime.block_on(async move {
        let mut endpoint = Endpoint::client(SocketAddr::from(([127, 0, 0, 1], 0))).unwrap();
        endpoint.set_default_client_config(client_config());
        let connection = endpoint.connect(SocketAddr::from(([127, 0, 0, 1], port)), "localhost").unwrap().await.unwrap();
        let (mut send, mut recv) = connection.open_bi().await.unwrap();
        // The messages, each after its length, written 64 KiB at a time.
        let mut frame = (size as u32).to_le_bytes().to_vec();
        frame.extend(std::iter::repeat_n(b'x', size));
        let per_chunk = ((64 << 10) / frame.len()).max(1);
        let chunk: Vec<u8> = frame.iter().copied().cycle().take(frame.len() * per_chunk).collect();
        let began = Instant::now();
        send.write_all(DRAIN).await.unwrap();
        let mut left = count as usize;
        while left > 0 {
            let n = left.min(per_chunk);
            send.write_all(&chunk[..n * frame.len()]).await.unwrap();
            left -= n;
        }
        send.write_all(&0u32.to_le_bytes()).await.unwrap();
        send.finish().unwrap();
        let answer = recv.read_to_end(16).await.unwrap();
        let seconds = began.elapsed().as_secs_f64();
        let read = u64::from_le_bytes(answer[..8].try_into().unwrap());
        println!("{{\"messages\":{count},\"size\":{size},\"read\":{read},\"seconds\":{seconds:.3},\"per_second\":{:.0}}}",
                 count as f64 / seconds);
    });
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("serve") if args.len() == 4 || args.len() == 5 => serve(std::fs::read(&args[2]).unwrap(),
                                                                    std::fs::read(&args[3]).unwrap(),
                                                                    args.get(4).map_or(0, |p| p.parse().unwrap())),
        Some("drain") if args.len() == 5 => drain_client(args[2].parse().unwrap(), args[3].parse().unwrap(),
                                                         args[4].parse().unwrap()),
        Some("steady") if args.len() == 8 => steady(args[2].parse().unwrap(), args[3].parse().unwrap(),
                                                    args[4].parse().unwrap(),
                                                    Duration::from_millis(args[5].parse().unwrap()),
                                                    args[6].parse().unwrap(), args[7].parse().unwrap()),
        Some("connect") if args.len() == 6 => connect(args[2].parse().unwrap(), args[3].parse().unwrap(),
                                                      args[4].parse().unwrap(), args[5].parse().unwrap()),
        _ => {
            eprintln!("idle-quic serve CERT.der KEY.der [PORT] | idle-quic connect PORT FIRST COUNT WINDOW | \
                       idle-quic steady PORT FIRST COUNT PERIOD_MS SECONDS WINDOW | idle-quic drain PORT COUNT SIZE");
            std::process::exit(2);
        }
    }
}
