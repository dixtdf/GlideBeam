//! Encrypted, password-authenticated, bounded LAN transport behind the stable C ABI.
mod audio;
mod auth;
mod tls;
mod video;
use bytes::Bytes;
use std::{
    collections::{BTreeMap, VecDeque},
    ffi::{c_char, CStr},
    net::{IpAddr, SocketAddr},
    panic::{catch_unwind, AssertUnwindSafe},
    sync::{
        atomic::{AtomicBool, AtomicI32, AtomicU64, Ordering},
        Arc, Mutex,
    },
    thread,
    time::{Duration, Instant},
};
use tokio::sync::{watch, Notify};

const INPUT: u32 = 2;
const VIDEO: u32 = 4;
const MAX_RELIABLE: usize = 1024 * 1024;
const QUEUE_BYTES: usize = 32 * 1024 * 1024;
const QUEUE_MESSAGES: usize = 256;
// Bulk is deliberately absent from the input/video queues and their receive wakeup.
const BULK_BYTES: usize = 8 * 1024 * 1024;
const BULK_MESSAGES: usize = 32;
const KEYFRAME: u32 = 7;
#[derive(Clone)]
struct Message {
    channel: u32,
    data: Vec<u8>,
}
#[derive(Default)]
struct Queue {
    data: VecDeque<Message>,
    bytes: usize,
}
#[derive(Default)]
struct BulkQueue {
    data: VecDeque<Vec<u8>>,
    bytes: usize,
}
impl BulkQueue {
    fn push(&mut self, data: Vec<u8>) -> Result<(), Vec<u8>> {
        if self.data.len() >= BULK_MESSAGES || self.bytes + data.len() > BULK_BYTES {
            return Err(data);
        }
        self.bytes += data.len();
        self.data.push_back(data);
        Ok(())
    }
    fn pop(&mut self) -> Option<Vec<u8>> {
        let data = self.data.pop_front()?;
        self.bytes -= data.len();
        Some(data)
    }
}
impl Queue {
    fn push(&mut self, message: Message) -> Result<(), Message> {
        if message.channel == INPUT && is_move(&message.data) {
            if let Some(last) = self.data.back_mut() {
                if last.channel == INPUT && is_move(&last.data) {
                    self.bytes = self.bytes - last.data.len() + message.data.len();
                    *last = message;
                    return Ok(());
                }
            }
        }
        if self.data.len() >= QUEUE_MESSAGES || self.bytes + message.data.len() > QUEUE_BYTES {
            return Err(message);
        }
        self.bytes += message.data.len();
        self.data.push_back(message);
        Ok(())
    }
    fn pop(&mut self) -> Option<Message> {
        let message = self.data.pop_front()?;
        self.bytes -= message.data.len();
        Some(message)
    }
    fn clear_video(&mut self) {
        self.data.retain(|message| message.channel != VIDEO);
        self.bytes = self.data.iter().map(|message| message.data.len()).sum();
    }
}
fn is_move(data: &[u8]) -> bool {
    data.len() == 32 && u32::from_le_bytes(data[..4].try_into().unwrap()) == 1
}
fn is_idr(data: &[u8]) -> bool {
    data.len() >= 32 && u32::from_le_bytes(data[12..16].try_into().unwrap()) & 1 != 0
}
fn video_sequence(data: &[u8]) -> u64 {
    u64::from_le_bytes(data[16..24].try_into().unwrap())
}
fn valid_video(data: &[u8]) -> bool {
    data.len() > 32
        && data.len() <= video::MAX_FRAME
        && u32::from_le_bytes(data[..4].try_into().unwrap()) == 0x31524652
        && (1..=16384).contains(&u32::from_le_bytes(data[4..8].try_into().unwrap()))
        && (1..=16384).contains(&u32::from_le_bytes(data[8..12].try_into().unwrap()))
}
fn keyframe_event() -> Message {
    let mut data = vec![0u8; 32];
    data[..4].copy_from_slice(&KEYFRAME.to_le_bytes());
    Message {
        channel: INPUT,
        data,
    }
}
struct Shared {
    status: AtomicI32,
    error: Mutex<String>,
    fingerprint: Mutex<String>,
    incoming: Mutex<Queue>,
    outgoing: [Mutex<Queue>; 4],
    send_notify: [Notify; 4],
    receive_space: Notify,
    sender_wait_idr: AtomicBool,
    local_keyframe_pending: AtomicBool,
    rtt_us: AtomicU64,
    token: Mutex<String>,
    policy: Mutex<Option<auth::Policy>>,
    auth_revision: AtomicU64,
    active_connections: Mutex<Vec<quinn::Connection>>,
    bulk_status: AtomicI32,
    bulk_incoming: Mutex<BulkQueue>,
    bulk_outgoing: Mutex<BulkQueue>,
    bulk_send_notify: Notify,
    bulk_receive_space: Notify,
    bulk_connection: Mutex<Option<quinn::Connection>>,
    audio_active: AtomicBool,
    audio_send_enabled: AtomicBool,
    audio_incoming: Mutex<audio::Queue>,
    audio_outgoing: Mutex<audio::Queue>,
    audio_send_notify: Notify,
}
impl Shared {
    fn new() -> Self {
        Self {
            status: AtomicI32::new(0),
            error: Mutex::new(String::new()),
            fingerprint: Mutex::new(String::new()),
            incoming: Mutex::new(Queue::default()),
            outgoing: std::array::from_fn(|_| Mutex::new(Queue::default())),
            send_notify: std::array::from_fn(|_| Notify::new()),
            receive_space: Notify::new(),
            sender_wait_idr: AtomicBool::new(false),
            local_keyframe_pending: AtomicBool::new(false),
            rtt_us: AtomicU64::new(0),
            token: Mutex::new(String::new()),
            policy: Mutex::new(None),
            auth_revision: AtomicU64::new(0),
            active_connections: Mutex::new(Vec::new()),
            bulk_status: AtomicI32::new(0),
            bulk_incoming: Mutex::new(BulkQueue::default()),
            bulk_outgoing: Mutex::new(BulkQueue::default()),
            bulk_send_notify: Notify::new(),
            bulk_receive_space: Notify::new(),
            bulk_connection: Mutex::new(None),
            audio_active: AtomicBool::new(false),
            audio_send_enabled: AtomicBool::new(false),
            audio_incoming: Mutex::new(audio::Queue::default()),
            audio_outgoing: Mutex::new(audio::Queue::default()),
            audio_send_notify: Notify::new(),
        }
    }
    fn error(&self, message: impl Into<String>) {
        *self.error.lock().unwrap() = message.into();
    }
    fn fail(&self, message: impl Into<String>) {
        self.error(message);
        self.status.store(-1, Ordering::Release);
    }
    fn outbound(&self, message: Message) -> Result<(), String> {
        let index = message.channel as usize - 1;
        let mut queue = self.outgoing[index].lock().unwrap();
        if message.channel == VIDEO && queue.data.len() >= 2 {
            return Err("video queue exceeded two frames; message was not accepted".into());
        }
        queue
            .push(message)
            .map_err(|_| "outgoing queue full; message was not accepted".to_string())?;
        self.send_notify[index].notify_one();
        Ok(())
    }
    fn request_remote_keyframe(&self) {
        let _ = self.outbound(keyframe_event());
    }
    fn request_local_keyframe(&self) {
        self.sender_wait_idr.store(true, Ordering::Release);
        self.outgoing[3].lock().unwrap().clear_video();
        // This critical recovery event has a reserved slot even when inbound input is saturated.
        self.local_keyframe_pending.store(true, Ordering::Release);
    }
    async fn inbound_reliable(&self, mut message: Message) {
        if message.channel == 1 {
            self.audio_control(&message.data);
        }
        loop {
            let notified = self.receive_space.notified();
            match self.incoming.lock().unwrap().push(message) {
                Ok(()) => return,
                Err(returned) => message = returned,
            }
            notified.await;
        }
    }
    fn inbound_video(&self, message: Message) -> bool {
        let mut queue = self.incoming.lock().unwrap();
        let video_count = queue.data.iter().filter(|m| m.channel == VIDEO).count();
        if video_count >= 3 || queue.bytes + message.data.len() > QUEUE_BYTES {
            queue.clear_video();
            return false;
        }
        queue.push(message).is_ok()
    }
    fn clear_queues(&self) {
        for queue in &self.outgoing {
            *queue.lock().unwrap() = Queue::default();
        }
        *self.incoming.lock().unwrap() = Queue::default();
        self.sender_wait_idr.store(false, Ordering::Release);
        self.local_keyframe_pending.store(false, Ordering::Release);
        self.clear_bulk();
        self.clear_audio();
    }
    fn clear_bulk(&self) {
        // Serialize against FFI sends: no accepted old-session message survives reconnect.
        let mut outgoing = self.bulk_outgoing.lock().unwrap();
        self.bulk_status.store(0, Ordering::Release);
        *outgoing = BulkQueue::default();
        *self.bulk_incoming.lock().unwrap() = BulkQueue::default();
        if let Some(connection) = self.bulk_connection.lock().unwrap().take() {
            connection.close(0u32.into(), b"bulk session closed");
        }
        self.bulk_receive_space.notify_one();
    }
    fn clear_audio(&self) {
        // Serialize the session boundary with FFI sends before removing old sound.
        let mut outgoing = self.audio_outgoing.lock().unwrap();
        self.audio_active.store(false, Ordering::Release);
        self.audio_send_enabled.store(false, Ordering::Release);
        *outgoing = audio::Queue::default();
        *self.audio_incoming.lock().unwrap() = audio::Queue::default();
    }
    fn begin_audio_session(&self) {
        // Reset even if the native status poll missed a rapid reconnect.
        let mut outgoing = self.audio_outgoing.lock().unwrap();
        self.audio_send_enabled.store(false, Ordering::Release);
        *outgoing = audio::Queue::default();
        *self.audio_incoming.lock().unwrap() = audio::Queue::default();
        self.audio_active.store(true, Ordering::Release);
    }
    fn audio_control(&self, data: &[u8]) {
        let Ok(message) = serde_json::from_slice::<serde_json::Value>(data) else {
            return;
        };
        if message.get("version").and_then(serde_json::Value::as_u64) != Some(1) {
            return;
        }
        let enabled = match message.get("type").and_then(serde_json::Value::as_str) {
            Some("audio-start") => true,
            Some("audio-stop") => false,
            _ => return,
        };
        let mut outgoing = self.audio_outgoing.lock().unwrap();
        if !self.audio_active.load(Ordering::Acquire) {
            return;
        }
        self.audio_send_enabled.store(enabled, Ordering::Release);
        if !enabled {
            *outgoing = audio::Queue::default();
        }
        self.audio_send_notify.notify_one();
    }
}
pub struct Handle {
    shared: Arc<Shared>,
    stop: watch::Sender<bool>,
    worker: Option<thread::JoinHandle<()>>,
}
#[derive(Clone)]
enum Mode {
    Server {
        bind: SocketAddr,
        identity: String,
    },
    Client {
        server: SocketAddr,
        pin: Option<[u8; 32]>,
    },
}

fn start(mode: Mode, token: String) -> Result<*mut Handle, String> {
    if !auth::valid_token(&token) {
        return Err(
            "pairing token must contain 6 to 128 characters without control characters".into(),
        );
    }
    let shared = Arc::new(Shared::new());
    *shared.token.lock().unwrap() = token.clone();
    if let Mode::Server { identity, .. } = &mode {
        *shared.policy.lock().unwrap() = Some(auth::Policy::load(identity)?);
    }
    let state = shared.clone();
    let (stop, mut stop_rx) = watch::channel(false);
    let worker = thread::Builder::new()
        .name("remote-transport".into())
        .spawn(move || {
            let result = catch_unwind(AssertUnwindSafe(|| {
                let runtime = tokio::runtime::Builder::new_multi_thread()
                    .worker_threads(2)
                    .enable_all()
                    .build()
                    .map_err(|e| e.to_string())?;
                runtime.block_on(async {
                tokio::select! {
                    result = run(mode, token, state.clone()) => result,
                    _ = stop_rx.changed() => {
                        // Send an explicit QUIC close while the runtime still
                        // drives UDP. Dropping the runtime immediately can leave
                        // the peer waiting for its idle timeout on macOS.
                        for connection in state.active_connections.lock().unwrap().iter() {
                            connection.close(0u32.into(), b"endpoint closed");
                        }
                        if let Some(connection) = state.bulk_connection.lock().unwrap().as_ref() {
                            connection.close(0u32.into(), b"endpoint closed");
                        }
                        state.clear_audio();
                        tokio::time::sleep(Duration::from_millis(25)).await;
                        Ok(())
                    }
                }
            })
            }));
            match result {
                Ok(Err(error)) => state.fail(error),
                Err(_) => state.fail("transport worker panicked"),
                _ => {}
            }
            state.clear_audio();
        })
        .map_err(|e| format!("start transport thread: {e}"))?;
    Ok(Box::into_raw(Box::new(Handle {
        shared,
        stop,
        worker: Some(worker),
    })))
}
async fn run(mode: Mode, token: String, state: Arc<Shared>) -> Result<(), String> {
    match mode {
        Mode::Server { bind, identity } => {
            let (config, fingerprint) = tls::server_config(&identity)?;
            let endpoint = quinn::Endpoint::server(config, bind)
                .map_err(|e| format!("listen UDP {bind}: {e}"))?;
            *state.fingerprint.lock().unwrap() = fingerprint;
            state.status.store(0, Ordering::Release);
            loop {
                state.active_connections.lock().unwrap().clear();
                let mut pair = Pair::default();
                while pair.input.is_none() || pair.media.is_none() {
                    let revision = state.auth_revision.load(Ordering::Acquire);
                    if pair.revision != revision {
                        pair.close("pairing token changed");
                        pair = Pair {
                            revision,
                            ..Pair::default()
                        };
                    }
                    let incoming =
                        match tokio::time::timeout(Duration::from_secs(8), endpoint.accept()).await
                        {
                            Ok(Some(incoming)) => incoming,
                            Ok(None) => return Ok(()),
                            Err(_) => {
                                if pair.input.is_some() || pair.media.is_some() {
                                    pair.close("second connection not authenticated in time");
                                    pair = Pair::default();
                                }
                                continue;
                            }
                        };
                    let ip = incoming.remote_address().ip();
                    if state
                        .policy
                        .lock()
                        .unwrap()
                        .as_ref()
                        .is_some_and(|policy| policy.blocked(ip))
                    {
                        incoming.refuse();
                        continue;
                    }
                    let connection =
                        match tokio::time::timeout(Duration::from_secs(5), incoming).await {
                            Ok(Ok(c)) => c,
                            _ => continue,
                        };
                    let (revision, token) = {
                        let token = state.token.lock().unwrap();
                        let revision = state.auth_revision.load(Ordering::Acquire);
                        // The token may have changed while endpoint.accept() or
                        // the TLS handshake was awaiting. A new-generation
                        // control connection must not inherit an old Pair.
                        if pair.revision != revision {
                            pair.close("pairing token changed");
                            pair = Pair {
                                revision,
                                ..Pair::default()
                            };
                        }
                        state
                            .active_connections
                            .lock()
                            .unwrap()
                            .push(connection.clone());
                        (revision, token.clone())
                    };
                    match tokio::time::timeout(
                        Duration::from_secs(5),
                        authenticate_server(&connection, token.as_bytes(), &pair),
                    )
                    .await
                    {
                        Ok(Ok((role, session, ticket, mut reply))) => {
                            if revision != state.auth_revision.load(Ordering::Acquire) {
                                connection.close(3u32.into(), b"pairing token changed");
                                pair.close("pairing token changed");
                                pair = Pair::default();
                                continue;
                            }
                            let same = pair.session.is_none_or(|existing| existing == session)
                                && pair
                                    .peer
                                    .is_none_or(|peer| peer == connection.remote_address().ip());
                            let empty = if role == 1 {
                                pair.input.is_none()
                            } else {
                                pair.media.is_none()
                            };
                            if !same || !empty {
                                let _ = reply.write_all(&[0]).await;
                                connection.close(2u32.into(), b"one paired client supported");
                                continue;
                            }
                            if reply.write_all(&[1]).await.is_err() {
                                continue;
                            }
                            let _ = reply.finish();
                            // Serialize the generation commit with set_token.
                            // No lock is held across a network await.
                            let _auth_guard = state.token.lock().unwrap();
                            if revision != state.auth_revision.load(Ordering::Acquire) {
                                connection.close(3u32.into(), b"pairing token changed");
                                pair.close("pairing token changed");
                                pair = Pair::default();
                                continue;
                            }
                            pair.session = Some(session);
                            pair.peer = Some(connection.remote_address().ip());
                            if role == 1 {
                                state.policy.lock().unwrap().as_mut().unwrap().success(ip)?;
                                pair.ticket = Some(ticket);
                                pair.input = Some(connection);
                            } else {
                                pair.media = Some(connection);
                            }
                        }
                        result => {
                            if matches!(result, Ok(Err(AuthError::WrongProof))) {
                                let _auth_guard = state.token.lock().unwrap();
                                if revision == state.auth_revision.load(Ordering::Acquire) {
                                    state.policy.lock().unwrap().as_mut().unwrap().failure(ip)?;
                                }
                            }
                            connection.close(3u32.into(), b"authentication failed");
                            state
                                .active_connections
                                .lock()
                                .unwrap()
                                .retain(|c| c.close_reason().is_none());
                        }
                    }
                }
                let input = pair.input.take().unwrap();
                let media = pair.media.take().unwrap();
                let bulk = BulkMode::Server {
                    endpoint: endpoint.clone(),
                    capability: bulk_capability(
                        &pair.ticket.unwrap(),
                        &input,
                        &pair.session.unwrap(),
                    )?,
                    session: pair.session.unwrap(),
                    peer: pair.peer.unwrap(),
                    revision: pair.revision,
                };
                state.status.store(1, Ordering::Release);
                let result = session(input, media, state.clone(), Some(bulk)).await;
                state.error(
                    result.err().unwrap_or_else(|| {
                        "client disconnected; listening for reconnection".into()
                    }),
                );
                state.clear_queues();
                state.status.store(0, Ordering::Release);
            }
        }
        Mode::Client { server, pin } => {
            let bind: SocketAddr = if server.is_ipv4() {
                "0.0.0.0:0".parse().unwrap()
            } else {
                "[::]:0".parse().unwrap()
            };
            let mut endpoint =
                quinn::Endpoint::client(bind).map_err(|e| format!("client UDP bind: {e}"))?;
            endpoint.set_default_client_config(tls::client_config(pin)?);
            let session_id: [u8; 16] = rand::random();
            let (input, ticket) =
                connect_control(&endpoint, server, token.as_bytes(), session_id).await?;
            let fingerprint = auth::peer_fingerprint(&input)?;
            *state.fingerprint.lock().unwrap() = fingerprint.clone();
            endpoint.set_default_client_config(tls::client_config(Some(tls::decode_pin(
                &fingerprint,
            )?))?);
            let media = connect_media(&endpoint, server, ticket, session_id).await?;
            *state.active_connections.lock().unwrap() = vec![input.clone(), media.clone()];
            let bulk = BulkMode::Client {
                endpoint,
                server,
                capability: bulk_capability(&ticket, &input, &session_id)?,
                session: session_id,
            };
            state.status.store(1, Ordering::Release);
            session(input, media, state, Some(bulk)).await
        }
    }
}
#[derive(Default)]
struct Pair {
    session: Option<[u8; 16]>,
    peer: Option<IpAddr>,
    ticket: Option<[u8; 32]>,
    revision: u64,
    input: Option<quinn::Connection>,
    media: Option<quinn::Connection>,
}
impl Pair {
    fn close(&self, why: &str) {
        for c in [&self.input, &self.media].into_iter().flatten() {
            c.close(4u32.into(), why.as_bytes());
        }
    }
}
async fn connect_tls(
    endpoint: &quinn::Endpoint,
    server: SocketAddr,
) -> Result<quinn::Connection, String> {
    let connecting = endpoint
        .connect(server, "remote.local")
        .map_err(|e| e.to_string())?;
    tokio::time::timeout(Duration::from_secs(10), connecting)
        .await
        .map_err(|_| "connection handshake timeout")?
        .map_err(|e| format!("QUIC/TLS handshake: {e}"))
}
async fn connect_control(
    endpoint: &quinn::Endpoint,
    server: SocketAddr,
    token: &[u8],
    session: [u8; 16],
) -> Result<(quinn::Connection, [u8; 32]), String> {
    let connection = connect_tls(endpoint, server).await?;
    let authentication = async {
        let (mut tx, mut rx) = connection.open_bi().await.map_err(|e| e.to_string())?;
        let (pake, client_message) = auth::begin_client(token);
        let mut hello = b"RMC2".to_vec();
        hello.push(1);
        hello.extend_from_slice(&session);
        hello.extend_from_slice(&client_message);
        tx.write_all(&hello).await.map_err(|e| e.to_string())?;
        let mut server_message = [0; 33];
        rx.read_exact(&mut server_message)
            .await
            .map_err(|e| format!("pairing authentication rejected: {e}"))?;
        let key = auth::finish(pake, &server_message, b'B')?;
        let context = auth::context(
            &auth::exporter(&connection, &session)?,
            &session,
            &client_message,
            &server_message,
        );
        tx.write_all(&auth::proof(&key, b"client", &context, &[]))
            .await
            .map_err(|e| e.to_string())?;
        tx.finish().map_err(|e| e.to_string())?;
        let mut response = [0; 65];
        rx.read_exact(&mut response)
            .await
            .map_err(|e| format!("pairing authentication rejected: {e}"))?;
        let ticket: [u8; 32] = response[32..64].try_into().unwrap();
        if !auth::verify(&key, b"server", &context, &ticket, &response[..32]) {
            return Err("server pairing proof rejected; connection was not authenticated".into());
        }
        if response[64] != 1 {
            return Err("another paired client is already connected".into());
        }
        Ok::<_, String>(ticket)
    };
    let result = tokio::time::timeout(Duration::from_secs(5), authentication)
        .await
        .map_err(|_| "pairing authentication timeout".to_owned())?;
    match result {
        Ok(ticket) => Ok((connection, ticket)),
        Err(error) => {
            connection.close(3u32.into(), b"pairing rejected");
            Err(error)
        }
    }
}
async fn connect_media(
    endpoint: &quinn::Endpoint,
    server: SocketAddr,
    ticket: [u8; 32],
    session: [u8; 16],
) -> Result<quinn::Connection, String> {
    let connection = connect_tls(endpoint, server).await?;
    let (mut tx, mut rx) = connection.open_bi().await.map_err(|e| e.to_string())?;
    let mut hello = b"RMC2".to_vec();
    hello.push(2);
    hello.extend_from_slice(&session);
    hello.extend_from_slice(&auth::media_proof(
        &ticket,
        &auth::exporter(&connection, &session)?,
        &session,
    ));
    tx.write_all(&hello).await.map_err(|e| e.to_string())?;
    tx.finish().map_err(|e| e.to_string())?;
    let mut ack = [0];
    tokio::time::timeout(Duration::from_secs(5), rx.read_exact(&mut ack))
        .await
        .map_err(|_| "media pairing timeout")?
        .map_err(|e| format!("media pairing rejected: {e}"))?;
    if ack != [1] {
        connection.close(3u32.into(), b"pairing rejected");
        return Err("media pairing rejected".into());
    }
    Ok(connection)
}
enum AuthError {
    WrongProof,
    Other,
}
async fn authenticate_server(
    connection: &quinn::Connection,
    token: &[u8],
    pair: &Pair,
) -> Result<(u8, [u8; 16], [u8; 32], quinn::SendStream), AuthError> {
    let (mut reply, mut rx) = connection.accept_bi().await.map_err(|_| AuthError::Other)?;
    let mut header = [0; 21];
    rx.read_exact(&mut header)
        .await
        .map_err(|_| AuthError::Other)?;
    let role = header[4];
    let session: [u8; 16] = header[5..21].try_into().unwrap();
    if &header[..4] != b"RMC2" || ![1, 2].contains(&role) {
        return Err(AuthError::Other);
    }
    let exporter = auth::exporter(connection, &session).map_err(|_| AuthError::Other)?;
    let ticket: [u8; 32];
    if role == 1 {
        if pair.input.is_some() {
            return Err(AuthError::Other);
        }
        let mut client_message = [0; 33];
        rx.read_exact(&mut client_message)
            .await
            .map_err(|_| AuthError::Other)?;
        let (pake, server_message) = auth::begin_server(token);
        let key = auth::finish(pake, &client_message, b'A').map_err(|_| AuthError::Other)?;
        let context = auth::context(&exporter, &session, &client_message, &server_message);
        reply
            .write_all(&server_message)
            .await
            .map_err(|_| AuthError::Other)?;
        let mut received = [0; 32];
        rx.read_exact(&mut received)
            .await
            .map_err(|_| AuthError::Other)?;
        let mut extra = [0];
        if rx
            .read(&mut extra)
            .await
            .map_err(|_| AuthError::Other)?
            .is_some()
        {
            return Err(AuthError::Other);
        }
        if !auth::verify(&key, b"client", &context, &[], &received) {
            return Err(AuthError::WrongProof);
        }
        ticket = rand::random();
        reply
            .write_all(&auth::proof(&key, b"server", &context, &ticket))
            .await
            .map_err(|_| AuthError::Other)?;
        reply
            .write_all(&ticket)
            .await
            .map_err(|_| AuthError::Other)?;
    } else {
        // No second password guess: media uses a random single-session capability
        // delivered only after successful control-channel mutual authentication.
        if pair.session != Some(session)
            || pair.peer != Some(connection.remote_address().ip())
            || pair.media.is_some()
        {
            return Err(AuthError::Other);
        }
        ticket = pair.ticket.ok_or(AuthError::Other)?;
        let mut received = [0; 32];
        rx.read_exact(&mut received)
            .await
            .map_err(|_| AuthError::Other)?;
        if !tls::token_equal(&auth::media_proof(&ticket, &exporter, &session), &received) {
            return Err(AuthError::Other);
        }
        let mut extra = [0];
        if rx
            .read(&mut extra)
            .await
            .map_err(|_| AuthError::Other)?
            .is_some()
        {
            return Err(AuthError::Other);
        }
    }
    Ok((role, session, ticket, reply))
}

enum BulkMode {
    Server {
        endpoint: quinn::Endpoint,
        capability: [u8; 32],
        session: [u8; 16],
        peer: IpAddr,
        revision: u64,
    },
    Client {
        endpoint: quinn::Endpoint,
        server: SocketAddr,
        capability: [u8; 32],
        session: [u8; 16],
    },
}
fn bulk_capability(
    ticket: &[u8; 32],
    control: &quinn::Connection,
    session: &[u8; 16],
) -> Result<[u8; 32], String> {
    // Role separation prevents a media proof or capability from authorizing bulk.
    Ok(auth::proof(
        ticket,
        b"bulk-capability/v1",
        &auth::exporter(control, session)?,
        session,
    ))
}
fn bulk_proof(capability: &[u8; 32], exporter: &[u8; 32], session: &[u8; 16]) -> [u8; 32] {
    auth::proof(capability, b"bulk/v1", exporter, session)
}
async fn connect_bulk(
    endpoint: &quinn::Endpoint,
    server: SocketAddr,
    capability: &[u8; 32],
    session: &[u8; 16],
) -> Result<quinn::Connection, String> {
    let connection = connect_tls(endpoint, server).await?;
    let result = tokio::time::timeout(Duration::from_secs(5), async {
        let (mut tx, mut rx) = connection.open_bi().await.map_err(|e| e.to_string())?;
        let mut hello = b"RMC2".to_vec();
        hello.push(3);
        hello.extend_from_slice(session);
        hello.extend_from_slice(&bulk_proof(
            capability,
            &auth::exporter(&connection, session)?,
            session,
        ));
        tx.write_all(&hello).await.map_err(|e| e.to_string())?;
        tx.finish().map_err(|e| e.to_string())?;
        let mut ack = [0];
        rx.read_exact(&mut ack)
            .await
            .map_err(|e| format!("bulk pairing rejected: {e}"))?;
        if ack != [1] {
            return Err("bulk pairing rejected".to_owned());
        }
        Ok::<_, String>(())
    })
    .await;
    match result {
        Ok(Ok(())) => Ok(connection),
        other => {
            connection.close(3u32.into(), b"bulk pairing rejected");
            Err(match other {
                Ok(Err(error)) => error,
                _ => "bulk pairing timeout".into(),
            })
        }
    }
}
async fn authenticate_bulk(
    connection: &quinn::Connection,
    capability: &[u8; 32],
    session: &[u8; 16],
    peer: IpAddr,
) -> Result<quinn::SendStream, String> {
    if connection.remote_address().ip() != peer {
        return Err("bulk peer mismatch".into());
    }
    let (reply, mut rx) = connection.accept_bi().await.map_err(|e| e.to_string())?;
    let mut hello = [0; 53];
    rx.read_exact(&mut hello).await.map_err(|e| e.to_string())?;
    if &hello[..4] != b"RMC2" || hello[4] != 3 || &hello[5..21] != session {
        return Err("bulk role/session mismatch".into());
    }
    let expected = bulk_proof(capability, &auth::exporter(connection, session)?, session);
    if !tls::token_equal(&expected, &hello[21..]) {
        return Err("bulk capability rejected".into());
    }
    let mut extra = [0];
    if rx
        .read(&mut extra)
        .await
        .map_err(|e| e.to_string())?
        .is_some()
    {
        return Err("bulk authentication trailing data".into());
    }
    Ok(reply)
}
async fn bulk_manager(mode: BulkMode, state: Arc<Shared>) -> Result<(), String> {
    match mode {
        BulkMode::Client {
            endpoint,
            server,
            capability,
            session,
        } => {
            let mut backoff = 1u64;
            loop {
                match connect_bulk(&endpoint, server, &capability, &session).await {
                    Ok(connection) => {
                        backoff = 1;
                        let result = bulk_session(connection, state.clone(), None).await;
                        state.clear_bulk();
                        state.bulk_status.store(-1, Ordering::Release);
                        if let Err(error) = result {
                            state.error(format!("bulk unavailable: {error}"));
                        }
                    }
                    Err(error) => {
                        state.bulk_status.store(-1, Ordering::Release);
                        state.error(format!("bulk unavailable: {error}"));
                    }
                }
                // Old two-connection servers can refuse bulk without affecting the session.
                tokio::time::sleep(Duration::from_secs(backoff)).await;
                backoff = (backoff * 2).min(30);
            }
        }
        BulkMode::Server {
            endpoint,
            capability,
            session,
            peer,
            revision,
        } => {
            loop {
                let Some(incoming) = endpoint.accept().await else {
                    return Ok(());
                };
                if incoming.remote_address().ip() != peer {
                    incoming.refuse();
                    continue;
                }
                let connection = match tokio::time::timeout(Duration::from_secs(3), incoming).await
                {
                    Ok(Ok(connection)) => connection,
                    _ => continue,
                };
                let authentication = tokio::time::timeout(
                    Duration::from_secs(3),
                    authenticate_bulk(&connection, &capability, &session, peer),
                )
                .await;
                let mut reply = match authentication {
                    Ok(Ok(reply)) => reply,
                    _ => {
                        connection.close(3u32.into(), b"bulk pairing rejected");
                        continue;
                    }
                };
                if revision != state.auth_revision.load(Ordering::Acquire) {
                    connection.close(3u32.into(), b"pairing token changed");
                    continue;
                }
                if reply.write_all(&[1]).await.is_err() {
                    continue;
                }
                let _ = reply.finish();
                // Commit is serialized with token rotation; no lock crosses an await.
                {
                    let _auth_guard = state.token.lock().unwrap();
                    if revision != state.auth_revision.load(Ordering::Acquire) {
                        connection.close(3u32.into(), b"pairing token changed");
                        continue;
                    }
                    *state.bulk_connection.lock().unwrap() = Some(connection.clone());
                }
                let result = bulk_session(connection, state.clone(), Some(endpoint.clone())).await;
                state.clear_bulk();
                state.bulk_status.store(-1, Ordering::Release);
                if let Err(error) = result {
                    state.error(format!("bulk unavailable: {error}"));
                }
            }
        }
    }
}
async fn bulk_session(
    connection: quinn::Connection,
    state: Arc<Shared>,
    server_endpoint: Option<quinn::Endpoint>,
) -> Result<(), String> {
    let mut tasks = tokio::task::JoinSet::new();
    tasks.spawn(bulk_sender(connection.clone(), state.clone()));
    tasks.spawn(bulk_receiver(connection.clone(), state.clone()));
    if let Some(endpoint) = server_endpoint {
        tasks.spawn(async move {
            while let Some(incoming) = endpoint.accept().await {
                incoming.refuse();
            }
            Ok(())
        });
    }
    *state.bulk_connection.lock().unwrap() = Some(connection.clone());
    state.bulk_status.store(1, Ordering::Release);
    let result = tokio::select! {
        error = connection.closed() => Err(format!("bulk connection closed: {error}")),
        result = tasks.join_next() => match result {
            Some(Ok(result)) => result,
            Some(Err(error)) => Err(error.to_string()),
            None => Err("bulk tasks ended".into()),
        },
    };
    connection.close(0u32.into(), b"bulk closed");
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    result
}
async fn bulk_sender(connection: quinn::Connection, state: Arc<Shared>) -> Result<(), String> {
    let (mut stream, mut ack) = connection.open_bi().await.map_err(|e| e.to_string())?;
    stream.write_all(b"BUL1").await.map_err(|e| e.to_string())?;
    loop {
        let notified = state.bulk_send_notify.notified();
        let message = state.bulk_outgoing.lock().unwrap().pop();
        if let Some(data) = message {
            stream
                .write_all(&(data.len() as u32).to_le_bytes())
                .await
                .map_err(|e| e.to_string())?;
            stream.write_all(&data).await.map_err(|e| e.to_string())?;
            // A queue slot is acknowledged only after the receiver has room. This
            // bounds in-flight bytes instead of hiding an unbounded QUIC backlog.
            let mut received = [0];
            ack.read_exact(&mut received)
                .await
                .map_err(|e| e.to_string())?;
            if received != [1] {
                return Err("invalid bulk frame acknowledgement".into());
            }
        } else {
            notified.await;
        }
    }
}
async fn bulk_receiver(connection: quinn::Connection, state: Arc<Shared>) -> Result<(), String> {
    let (mut ack, mut stream) = connection.accept_bi().await.map_err(|e| e.to_string())?;
    let mut header = [0; 4];
    tokio::time::timeout(Duration::from_secs(3), stream.read_exact(&mut header))
        .await
        .map_err(|_| "bulk stream header timeout")?
        .map_err(|e| e.to_string())?;
    if &header != b"BUL1" {
        return Err("invalid bulk stream header".into());
    }
    loop {
        let mut size = [0; 4];
        tokio::select! {
            result = stream.read_exact(&mut size) => result.map_err(|e| e.to_string())?,
            extra = connection.accept_bi() => {
                let _ = extra.map_err(|e| e.to_string())?;
                return Err("duplicate bulk stream".into());
            },
        }
        let len = u32::from_le_bytes(size) as usize;
        if len == 0 || len > MAX_RELIABLE {
            return Err("bulk message length exceeds bound".into());
        }
        let mut data = vec![0; len];
        stream
            .read_exact(&mut data)
            .await
            .map_err(|e| e.to_string())?;
        loop {
            let notified = state.bulk_receive_space.notified();
            match state.bulk_incoming.lock().unwrap().push(data) {
                Ok(()) => break,
                Err(returned) => data = returned,
            }
            notified.await;
        }
        ack.write_all(&[1]).await.map_err(|e| e.to_string())?;
    }
}

async fn session(
    input: quinn::Connection,
    media: quinn::Connection,
    state: Arc<Shared>,
    bulk_mode: Option<BulkMode>,
) -> Result<(), String> {
    state.begin_audio_session();
    let mut tasks = tokio::task::JoinSet::new();
    for channel in 1..=3 {
        tasks.spawn(reliable_sender(input.clone(), state.clone(), channel));
    }
    tasks.spawn(reliable_receiver(input.clone(), state.clone()));
    tasks.spawn(media_sender(media.clone(), state.clone()));
    tasks.spawn(media_receiver(media.clone(), state.clone()));
    let measured_connection = input.clone();
    let measured_state = state.clone();
    tasks.spawn(async move {
        let mut tick = tokio::time::interval(Duration::from_millis(100));
        loop {
            tick.tick().await;
            measured_state.rtt_us.store(
                measured_connection.rtt().as_micros().min(u64::MAX as u128) as u64,
                Ordering::Release,
            );
        }
        #[allow(unreachable_code)]
        Ok(())
    });
    if let Some(mode) = bulk_mode {
        tasks.spawn(bulk_manager(mode, state.clone()));
    }
    let result = tokio::select! {
        error = input.closed() => Err(format!("input connection closed: {error}")),
        error = media.closed() => Err(format!("video connection closed: {error}")),
        done = tasks.join_next() => match done { Some(Ok(result)) => result, Some(Err(e)) => Err(format!("session task failed: {e}")), None => Err("session ended".into()) },
    };
    input.close(0u32.into(), b"session closed");
    media.close(0u32.into(), b"session closed");
    tasks.abort_all();
    while tasks.join_next().await.is_some() {}
    state.clear_bulk();
    state.clear_audio();
    result
}
async fn reliable_sender(
    connection: quinn::Connection,
    state: Arc<Shared>,
    channel: u32,
) -> Result<(), String> {
    let mut stream = connection.open_uni().await.map_err(|e| e.to_string())?;
    stream
        .write_all(&[channel as u8])
        .await
        .map_err(|e| e.to_string())?;
    let index = channel as usize - 1;
    loop {
        let notified = state.send_notify[index].notified();
        let message = state.outgoing[index].lock().unwrap().pop();
        if let Some(message) = message {
            stream
                .write_all(&(message.data.len() as u32).to_le_bytes())
                .await
                .map_err(|e| e.to_string())?;
            stream
                .write_all(&message.data)
                .await
                .map_err(|e| e.to_string())?;
        } else {
            notified.await;
        }
    }
}
async fn reliable_receiver(
    connection: quinn::Connection,
    state: Arc<Shared>,
) -> Result<(), String> {
    let mut readers = tokio::task::JoinSet::new();
    let mut seen = 0u8;
    loop {
        tokio::select! {
            stream = connection.accept_uni() => {
                let mut stream = stream.map_err(|e| e.to_string())?; let mut channel = [0];
                tokio::time::timeout(Duration::from_secs(3), stream.read_exact(&mut channel)).await.map_err(|_| "stream channel header timeout")?.map_err(|e| e.to_string())?;
                if !(1..=3).contains(&channel[0]) || seen & (1 << channel[0]) != 0 { return Err("duplicate or invalid reliable channel".into()); }
                seen |= 1 << channel[0]; let state = state.clone();
                readers.spawn(async move {
                    loop {
                        let mut size = [0; 4]; stream.read_exact(&mut size).await.map_err(|e| e.to_string())?;
                        let len = u32::from_le_bytes(size) as usize; if len == 0 || len > MAX_RELIABLE { return Err("reliable message length exceeds bound".into()); }
                        let mut data = vec![0; len]; stream.read_exact(&mut data).await.map_err(|e| e.to_string())?;
                        state.inbound_reliable(Message { channel: channel[0] as u32, data }).await;
                    }
                });
            }
            result = readers.join_next(), if !readers.is_empty() => { return match result { Some(Ok(r)) => r, Some(Err(e)) => Err(e.to_string()), None => Err("reliable receiver ended".into()) }; }
        }
    }
}
async fn send_audio_packet(
    connection: &quinn::Connection,
    state: &Shared,
    packet: audio::Packet,
) -> bool {
    let data = Bytes::from(packet.data);
    loop {
        let notified = state.audio_send_notify.notified();
        let remaining = audio::MAX_AGE.saturating_sub(packet.queued.elapsed());
        if remaining.is_zero() || !state.audio_send_enabled.load(Ordering::Acquire) {
            return false;
        }
        tokio::select! {
            biased;
            _ = notified => {},
            result = tokio::time::timeout(remaining, connection.send_datagram_wait(data.clone())) => {
                return matches!(result, Ok(Ok(())));
            },
        }
    }
}

async fn drain_audio(connection: &quinn::Connection, state: &Shared) {
    // A producer cannot monopolize the scheduler: at most four blocks before
    // the next video fragment. Older sound is discarded rather than retried.
    for _ in 0..4 {
        let packet = {
            let mut outgoing = state.audio_outgoing.lock().unwrap();
            if state.audio_send_enabled.load(Ordering::Acquire) {
                outgoing.pop(Instant::now())
            } else {
                None
            }
        };
        let Some(packet) = packet else {
            break;
        };
        if !send_audio_packet(connection, state, packet).await {
            state.audio_outgoing.lock().unwrap().mark_discontinuity();
        }
    }
}

async fn send_video_fragment(
    connection: &quinn::Connection,
    state: &Shared,
    packet: Bytes,
) -> Result<(), String> {
    loop {
        let audio_ready = state.audio_send_notify.notified();
        drain_audio(connection, state).await;
        // While a video fragment waits for congestion relief, new sound can
        // interrupt that wait. The unsent video fragment is retained, and no
        // accepted datagram is silently evicted from Quinn's send buffer.
        tokio::select! {
            biased;
            _ = audio_ready => {},
            result = connection.send_datagram_wait(packet.clone()) => {
                return result.map_err(|e| e.to_string());
            },
        }
    }
}

async fn media_sender(connection: quinn::Connection, state: Arc<Shared>) -> Result<(), String> {
    loop {
        let notified = state.send_notify[3].notified();
        let audio_ready = state.audio_send_notify.notified();
        drain_audio(&connection, &state).await;
        let message = state.outgoing[3].lock().unwrap().pop();
        let Some(message) = message else {
            // Inner send waits can consume a notification while this bounded
            // drain leaves a source burst pending. Do not wait for the next
            // callback and expire the remaining sound on a static desktop.
            if state.audio_outgoing.lock().unwrap().len(Instant::now()) > 0 {
                tokio::task::yield_now().await;
                continue;
            }
            tokio::select! { _ = notified => {}, _ = audio_ready => {} }
            continue;
        };
        if state.sender_wait_idr.load(Ordering::Acquire) && !is_idr(&message.data) {
            continue;
        }
        let mtu = connection
            .max_datagram_size()
            .ok_or("peer disabled QUIC video datagrams")?;
        let packets = video::fragment(&message.data, video_sequence(&message.data), mtu)?;
        let result = tokio::time::timeout(Duration::from_millis(100), async {
            for packet in packets {
                send_video_fragment(&connection, &state, Bytes::from(packet)).await?;
            }
            Ok::<(), String>(())
        })
        .await;
        if !matches!(result, Ok(Ok(()))) {
            state.error("video frame missed send deadline; requesting IDR");
            state.request_local_keyframe();
        }
    }
}
async fn media_receiver(connection: quinn::Connection, state: Arc<Shared>) -> Result<(), String> {
    let mut assembly = video::Reassembler::new();
    let mut pending: BTreeMap<u64, Vec<u8>> = BTreeMap::new();
    let mut wait_idr = true;
    let mut expected = 0u64;
    let mut last_request = Instant::now() - Duration::from_secs(1);
    let mut tick = tokio::time::interval(Duration::from_millis(20));
    loop {
        tokio::select! {
            datagram = connection.read_datagram() => {
                let datagram = datagram.map_err(|e| e.to_string())?;
                if datagram.len() < 4 { continue; }
                let magic = u32::from_le_bytes(datagram[..4].try_into().unwrap());
                if magic == audio::MAGIC {
                    if audio::valid_packet(&datagram) {
                        state.audio_incoming.lock().unwrap().push(datagram.to_vec(), Instant::now());
                    }
                    // Malformed or lost audio cannot terminate video/control.
                    continue;
                }
                if magic != video::MAGIC { continue; }
                if datagram.len() >= 12 && !wait_idr && u64::from_le_bytes(datagram[4..12].try_into().unwrap()) < expected { continue; }
                if let Some((sequence, data)) = assembly.push(&datagram, Instant::now())? {
                    if !valid_video(&data) || video_sequence(&data) != sequence { return Err("invalid assembled video frame header".into()); }
                    if is_idr(&data) { pending.clear(); assembly = video::Reassembler::new(); wait_idr = false; expected = sequence; }
                    if !wait_idr && sequence >= expected { pending.insert(sequence, data); }
                    if pending.len() > 3 { assembly.lost = true; }
                }
            }
            _ = tick.tick() => assembly.expire(Instant::now()),
        }
        if assembly.lost {
            assembly.lost = false;
            pending.clear();
            wait_idr = true;
        }
        while !wait_idr {
            let Some(data) = pending.remove(&expected) else {
                break;
            };
            if !state.inbound_video(Message {
                channel: VIDEO,
                data,
            }) {
                pending.clear();
                wait_idr = true;
                break;
            }
            expected = expected.wrapping_add(1);
        }
        if wait_idr && last_request.elapsed() >= Duration::from_millis(150) {
            state.request_remote_keyframe();
            last_request = Instant::now();
        }
    }
}

unsafe fn string(ptr: *const c_char) -> Result<String, String> {
    if ptr.is_null() {
        return Err("null C string".into());
    }
    CStr::from_ptr(ptr)
        .to_str()
        .map(str::to_owned)
        .map_err(|_| "C string must be UTF-8".into())
}
unsafe fn handle<'a>(ptr: *mut Handle) -> Option<&'a Handle> {
    ptr.as_ref()
}
fn guarded<T>(fallback: T, action: impl FnOnce() -> T) -> T {
    catch_unwind(AssertUnwindSafe(action)).unwrap_or(fallback)
}
#[no_mangle]
pub unsafe extern "C" fn remote_server_start(
    bind: *const c_char,
    token: *const c_char,
    identity: *const c_char,
) -> *mut Handle {
    guarded(std::ptr::null_mut(), || {
        (|| {
            let bind = string(bind)?
                .parse::<SocketAddr>()
                .map_err(|_| "bind address must be numeric IP:port")?;
            start(
                Mode::Server {
                    bind,
                    identity: string(identity)?,
                },
                string(token)?,
            )
        })()
        .unwrap_or(std::ptr::null_mut())
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_client_start(
    server: *const c_char,
    token: *const c_char,
    pin: *const c_char,
) -> *mut Handle {
    guarded(std::ptr::null_mut(), || {
        (|| {
            let server = string(server)?
                .parse::<SocketAddr>()
                .map_err(|_| "server address must be numeric IP:port")?;
            let pin = if pin.is_null() {
                String::new()
            } else {
                string(pin)?
            };
            let pin = if pin.is_empty() {
                None
            } else {
                Some(tls::decode_pin(&pin)?)
            };
            start(Mode::Client { server, pin }, string(token)?)
        })()
        .unwrap_or(std::ptr::null_mut())
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_status(ptr: *mut Handle) -> i32 {
    guarded(-1, || {
        handle(ptr).map_or(-1, |h| h.shared.status.load(Ordering::Acquire))
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_bulk_status(ptr: *mut Handle) -> i32 {
    guarded(-1, || {
        handle(ptr).map_or(-1, |h| {
            if h.shared.status.load(Ordering::Acquire) != 1 {
                0
            } else {
                h.shared.bulk_status.load(Ordering::Acquire)
            }
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_bulk_send(ptr: *mut Handle, data: *const u8, len: usize) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let state = &handle.shared;
        if data.is_null() || len == 0 || len > MAX_RELIABLE {
            return -1;
        }
        let mut queue = state.bulk_outgoing.lock().unwrap();
        if state.status.load(Ordering::Acquire) != 1
            || state.bulk_status.load(Ordering::Acquire) != 1
        {
            return -1;
        }
        // Check available capacity before copying a caller's possibly large buffer.
        if queue.data.len() >= BULK_MESSAGES || queue.bytes + len > BULK_BYTES {
            return 0;
        }
        queue
            .push(std::slice::from_raw_parts(data, len).to_vec())
            .expect("checked bulk queue capacity");
        state.bulk_send_notify.notify_one();
        1
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_bulk_poll(ptr: *mut Handle, data: *mut u8, capacity: usize) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let state = &handle.shared;
        if data.is_null() {
            return -1;
        }
        let mut queue = state.bulk_incoming.lock().unwrap();
        let Some(front) = queue.data.front() else {
            return if state.bulk_status.load(Ordering::Acquire) < 0 {
                -1
            } else {
                0
            };
        };
        if front.len() > capacity {
            return -1;
        }
        let message = queue.pop().unwrap();
        std::ptr::copy_nonoverlapping(message.as_ptr(), data, message.len());
        state.bulk_receive_space.notify_one();
        message.len() as i32
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_bulk_pending(ptr: *mut Handle) -> u32 {
    guarded(0, || {
        handle(ptr).map_or(0, |h| {
            h.shared.bulk_incoming.lock().unwrap().data.len() as u32
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_audio_send(ptr: *mut Handle, data: *const u8, len: usize) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        if data.is_null() || len != audio::PACKET_BYTES {
            return -1;
        }
        let packet = std::slice::from_raw_parts(data, len);
        if !audio::valid_packet(packet) {
            return -1;
        }
        let state = &handle.shared;
        let mut queue = state.audio_outgoing.lock().unwrap();
        if state.status.load(Ordering::Acquire) != 1 || !state.audio_active.load(Ordering::Acquire)
        {
            return -1;
        }
        if !state.audio_send_enabled.load(Ordering::Acquire) {
            return 0;
        }
        queue.push(packet.to_vec(), Instant::now());
        state.audio_send_notify.notify_one();
        1
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_audio_poll(
    ptr: *mut Handle,
    data: *mut u8,
    capacity: usize,
) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        if data.is_null() {
            return -1;
        }
        let state = &handle.shared;
        let mut queue = state.audio_incoming.lock().unwrap();
        let now = Instant::now();
        let Some(len) = queue.front_len(now) else {
            return if state.status.load(Ordering::Acquire) == 1
                && state.audio_active.load(Ordering::Acquire)
            {
                0
            } else {
                -1
            };
        };
        if len > capacity {
            return -1;
        }
        let packet = queue.pop(now).unwrap();
        std::ptr::copy_nonoverlapping(packet.data.as_ptr(), data, len);
        len as i32
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_audio_pending(ptr: *mut Handle) -> u32 {
    guarded(0, || {
        handle(ptr).map_or(0, |h| {
            h.shared.audio_incoming.lock().unwrap().len(Instant::now()) as u32
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_rtt_ms(ptr: *mut Handle) -> f64 {
    guarded(0.0, || {
        handle(ptr)
            .filter(|h| h.shared.status.load(Ordering::Acquire) == 1)
            .map_or(0.0, |h| {
                h.shared.rtt_us.load(Ordering::Acquire) as f64 / 1000.0
            })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_pending(ptr: *mut Handle, channel: u32) -> u32 {
    guarded(0, || {
        handle(ptr).map_or(0, |h| {
            h.shared
                .incoming
                .lock()
                .unwrap()
                .data
                .iter()
                .filter(|m| m.channel == channel)
                .count() as u32
                + u32::from(
                    channel == INPUT && h.shared.local_keyframe_pending.load(Ordering::Acquire),
                )
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_send(
    ptr: *mut Handle,
    channel: u32,
    data: *const u8,
    len: usize,
) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let state = &handle.shared;
        if state.status.load(Ordering::Acquire) != 1 {
            state.error("endpoint is not connected");
            return -1;
        }
        if !(1..=4).contains(&channel)
            || data.is_null()
            || len == 0
            || len
                > if channel == VIDEO {
                    video::MAX_FRAME
                } else {
                    MAX_RELIABLE
                }
        {
            state.error("invalid channel or payload length");
            return -1;
        }
        let data = std::slice::from_raw_parts(data, len);
        if channel == VIDEO {
            if !valid_video(data) {
                state.error("invalid RemoteFrameHeader");
                return -1;
            }
            if state.sender_wait_idr.load(Ordering::Acquire) && !is_idr(data) {
                state.error("video recovery is awaiting an IDR frame");
                return -1;
            }
        }
        match state.outbound(Message {
            channel,
            data: data.to_vec(),
        }) {
            Ok(()) => {
                if channel == VIDEO && is_idr(data) {
                    state.sender_wait_idr.store(false, Ordering::Release);
                }
                1
            }
            Err(error) => {
                state.error(error);
                if channel == VIDEO {
                    state.request_local_keyframe();
                }
                -1
            }
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_poll(
    ptr: *mut Handle,
    channel: *mut u32,
    data: *mut u8,
    capacity: usize,
) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let state = &handle.shared;
        if channel.is_null() || data.is_null() {
            state.error("null poll output pointer");
            return -1;
        }
        if state.local_keyframe_pending.load(Ordering::Acquire) {
            if capacity < 32 {
                state.error(
                    "poll buffer requires 32 bytes for pending IDR request; message retained",
                );
                return -1;
            }
            if state.local_keyframe_pending.swap(false, Ordering::AcqRel) {
                let event = keyframe_event();
                *channel = INPUT;
                std::ptr::copy_nonoverlapping(event.data.as_ptr(), data, 32);
                return 32;
            }
        }
        let mut queue = state.incoming.lock().unwrap();
        let Some(message) = queue.data.front() else {
            return if state.status.load(Ordering::Acquire) < 0 {
                -1
            } else {
                0
            };
        };
        if message.data.len() > capacity {
            state.error(format!(
                "poll buffer requires {} bytes; message retained",
                message.data.len()
            ));
            return -1;
        }
        let message = queue.pop().unwrap();
        let len = message.data.len();
        *channel = message.channel;
        std::ptr::copy_nonoverlapping(message.data.as_ptr(), data, len);
        state.receive_space.notify_one();
        len as i32
    })
}
unsafe fn write_string(value: &str, data: *mut c_char, capacity: usize) -> i32 {
    if data.is_null() || capacity == 0 {
        return -1;
    }
    let len = value.len().min(capacity - 1);
    std::ptr::copy_nonoverlapping(value.as_ptr(), data.cast::<u8>(), len);
    *data.add(len) = 0;
    len as i32
}
#[no_mangle]
pub unsafe extern "C" fn remote_last_error(
    ptr: *mut Handle,
    data: *mut c_char,
    capacity: usize,
) -> i32 {
    guarded(-1, || {
        handle(ptr).map_or(-1, |h| {
            write_string(&h.shared.error.lock().unwrap(), data, capacity)
        })
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_fingerprint(
    ptr: *mut Handle,
    data: *mut c_char,
    capacity: usize,
) -> i32 {
    guarded(-1, || {
        handle(ptr).map_or(-1, |h| {
            write_string(&h.shared.fingerprint.lock().unwrap(), data, capacity)
        })
    })
}
/// Server-only policy inspection. No password, proof, or media capability is exposed.
#[no_mangle]
pub unsafe extern "C" fn remote_auth_policy_json(
    ptr: *mut Handle,
    data: *mut c_char,
    capacity: usize,
) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let policy = handle.shared.policy.lock().unwrap();
        let Some(policy) = policy.as_ref() else {
            return -1;
        };
        match policy.json() {
            Ok(json) => write_string(&json, data, capacity),
            Err(error) => {
                handle.shared.error(error);
                -1
            }
        }
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_unblock_ip(ptr: *mut Handle, ip: *const c_char) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        let ip = match string(ip).and_then(|ip| {
            ip.parse::<IpAddr>()
                .map_err(|_| "invalid IP address".into())
        }) {
            Ok(ip) => ip,
            Err(error) => {
                handle.shared.error(error);
                return -1;
            }
        };
        let mut policy = handle.shared.policy.lock().unwrap();
        let Some(policy) = policy.as_mut() else {
            return -1;
        };
        match policy.unblock(ip) {
            Ok(()) => 1,
            Err(error) => {
                handle.shared.error(error);
                -1
            }
        }
    })
}
/// Server token update does not persist the token; the host owns its private token file.
#[no_mangle]
pub unsafe extern "C" fn remote_set_token(ptr: *mut Handle, token: *const c_char) -> i32 {
    guarded(-1, || {
        let Some(handle) = handle(ptr) else {
            return -1;
        };
        if handle.shared.policy.lock().unwrap().is_none() {
            return -1;
        }
        let token = match string(token) {
            Ok(token) if auth::valid_token(&token) => token,
            _ => {
                handle.shared.error(
                    "pairing token must contain 6 to 128 characters without control characters",
                );
                return -1;
            }
        };
        let mut current = handle.shared.token.lock().unwrap();
        if *current != token {
            *current = token;
            handle.shared.auth_revision.fetch_add(1, Ordering::AcqRel);
            for connection in handle.shared.active_connections.lock().unwrap().drain(..) {
                connection.close(3u32.into(), b"pairing token changed");
            }
            if let Some(connection) = handle.shared.bulk_connection.lock().unwrap().as_ref() {
                connection.close(3u32.into(), b"pairing token changed");
            }
        }
        1
    })
}
#[no_mangle]
pub unsafe extern "C" fn remote_close(ptr: *mut Handle) {
    guarded((), || {
        if !ptr.is_null() {
            let mut handle = Box::from_raw(ptr);
            let _ = handle.stop.send(true);
            if let Some(worker) = handle.worker.take() {
                let _ = worker.join();
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn audio_negotiation_is_versioned_and_cannot_inherit_a_fast_reconnect() {
        let shared = Arc::new(Shared::new());
        shared.status.store(1, Ordering::Release);
        shared.begin_audio_session();
        let (stop, _) = watch::channel(false);
        let mut handle = Handle {
            shared,
            stop,
            worker: None,
        };
        let ptr = &mut handle as *mut _;
        let packet = audio::test_packet(0);
        for request in [
            b"not json".as_slice(),
            br#"{"type":"audio-start"}"#,
            br#"{"type":"audio-start","version":2}"#,
            br#"{"type":"audio-start","version":"1"}"#,
            br#"{"type":"audio-start","version":1.0}"#,
            br#"{"type":"not-audio","version":1}"#,
        ] {
            handle.shared.audio_control(request);
            assert_eq!(
                unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
                0
            );
        }
        handle
            .shared
            .audio_control(br#"{"type":"audio-start","version":1}"#);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
            1
        );
        handle
            .shared
            .audio_control(br#"{"type":"audio-stop","version":1}"#);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
            0
        );
        assert_eq!(
            handle
                .shared
                .audio_outgoing
                .lock()
                .unwrap()
                .len(Instant::now()),
            0
        );
        handle
            .shared
            .audio_control(br#"{"type":"audio-start","version":1}"#);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
            1
        );
        // Simulate a native caller that never observed the intermediate status 0.
        // A new media session independently discards requests and packets.
        handle.shared.begin_audio_session();
        assert_eq!(unsafe { remote_status(ptr) }, 1);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
            0
        );
        assert_eq!(
            handle
                .shared
                .audio_outgoing
                .lock()
                .unwrap()
                .len(Instant::now()),
            0
        );
    }

    #[test]
    fn authenticated_audio_request_is_not_inherited_by_a_legacy_reconnecting_client() {
        let (server, address, identity) = network_fixture("audio-negotiate");
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        let deadline = Instant::now() + Duration::from_secs(3);
        while !unsafe { &*server }
            .shared
            .audio_active
            .load(Ordering::Acquire)
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        let packet = audio::test_packet(0);
        assert_eq!(
            unsafe { remote_audio_send(server, packet.as_ptr(), packet.len()) },
            0
        );
        let request = br#"{"type":"audio-start","version":1}"#;
        assert_eq!(
            unsafe { remote_send(client, 1, request.as_ptr(), request.len()) },
            1
        );
        while !unsafe { &*server }
            .shared
            .audio_send_enabled
            .load(Ordering::Acquire)
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(
            unsafe { remote_audio_send(server, packet.as_ptr(), packet.len()) },
            1
        );
        unsafe {
            remote_close(client);
        }
        wait_status(server, 0);
        let legacy = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(legacy, 1);
        wait_status(server, 1);
        let deadline = Instant::now() + Duration::from_secs(3);
        while !unsafe { &*server }
            .shared
            .audio_active
            .load(Ordering::Acquire)
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        assert_eq!(
            unsafe { remote_audio_send(server, packet.as_ptr(), packet.len()) },
            0
        );
        assert_eq!(unsafe { remote_audio_pending(legacy) }, 0);
        let input = [72; 32];
        assert_eq!(
            unsafe { remote_send(legacy, INPUT, input.as_ptr(), input.len()) },
            1
        );
        let mut buffer = [0; 128];
        let mut channel = 0;
        loop {
            if unsafe { remote_poll(server, &mut channel, buffer.as_mut_ptr(), buffer.len()) } == 32
                && channel == INPUT
                && buffer[..32] == input
            {
                break;
            }
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        unsafe {
            remote_close(legacy);
            remote_close(server);
        }
        std::fs::remove_dir_all(identity).unwrap();
    }

    #[test]
    fn audio_ffi_validates_bounds_retains_small_poll_and_clears_on_session_end() {
        let shared = Arc::new(Shared::new());
        shared.status.store(1, Ordering::Release);
        shared.audio_active.store(true, Ordering::Release);
        shared.audio_send_enabled.store(true, Ordering::Release);
        let (stop, _) = watch::channel(false);
        let mut handle = Handle {
            shared,
            stop,
            worker: None,
        };
        let ptr = &mut handle as *mut _;
        let packet = audio::test_packet(9);
        assert_eq!(
            unsafe { remote_audio_send(ptr, std::ptr::null(), packet.len()) },
            -1
        );
        assert_eq!(unsafe { remote_audio_send(ptr, packet.as_ptr(), 0) }, -1);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len() + 1) },
            -1
        );
        let mut invalid = packet.clone();
        invalid[12] = 2;
        assert_eq!(
            unsafe { remote_audio_send(ptr, invalid.as_ptr(), invalid.len()) },
            -1
        );
        assert_eq!(
            unsafe { remote_audio_send(std::ptr::null_mut(), packet.as_ptr(), packet.len()) },
            -1
        );
        for _ in 0..40 {
            assert_eq!(
                unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
                1
            );
        }
        assert_eq!(
            handle
                .shared
                .audio_outgoing
                .lock()
                .unwrap()
                .len(Instant::now()),
            audio::QUEUE_PACKETS
        );
        handle
            .shared
            .audio_incoming
            .lock()
            .unwrap()
            .push(packet.clone(), Instant::now());
        assert_eq!(unsafe { remote_pending(ptr, VIDEO) }, 0);
        let mut small = [0; 32];
        assert_eq!(
            unsafe { remote_audio_poll(ptr, small.as_mut_ptr(), small.len()) },
            -1
        );
        assert_eq!(unsafe { remote_audio_pending(ptr) }, 1);
        let mut buffer = vec![0; audio::PACKET_BYTES];
        assert_eq!(
            unsafe { remote_audio_poll(ptr, buffer.as_mut_ptr(), buffer.len()) },
            audio::PACKET_BYTES as i32
        );
        assert_eq!(buffer, packet);
        handle
            .shared
            .audio_incoming
            .lock()
            .unwrap()
            .push(packet.clone(), Instant::now());
        handle.shared.clear_audio();
        assert_eq!(unsafe { remote_audio_pending(ptr) }, 0);
        assert_eq!(
            unsafe { remote_audio_send(ptr, packet.as_ptr(), packet.len()) },
            -1
        );
        assert_eq!(
            unsafe { remote_audio_poll(ptr, buffer.as_mut_ptr(), buffer.len()) },
            -1
        );
        assert_eq!(
            handle
                .shared
                .audio_outgoing
                .lock()
                .unwrap()
                .len(Instant::now()),
            0
        );
    }

    #[test]
    fn encrypted_audio_coexists_with_video_input_and_ignores_malformed_sound() {
        let (server, address, identity) = network_fixture("audio");
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        let deadline = Instant::now() + Duration::from_secs(3);
        while !unsafe { &*server }
            .shared
            .audio_active
            .load(Ordering::Acquire)
            || !unsafe { &*client }
                .shared
                .audio_active
                .load(Ordering::Acquire)
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        let audio_start = br#"{"type":"audio-start","version":1}"#;
        assert_eq!(
            unsafe { remote_send(client, 1, audio_start.as_ptr(), audio_start.len()) },
            1
        );
        while !unsafe { &*server }
            .shared
            .audio_send_enabled
            .load(Ordering::Acquire)
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(1));
        }
        // Authenticated media connection, but an invalid packet from a peer:
        // sound corruption must not take down the other channel tasks.
        let media = unsafe { &*server }
            .shared
            .active_connections
            .lock()
            .unwrap()[1]
            .clone();
        let mut malformed = audio::test_packet(999);
        malformed[12] = 2;
        media.send_datagram(Bytes::from(malformed)).unwrap();
        media.send_datagram(Bytes::from_static(b"RFA1")).unwrap();

        let mut frame = vec![17; 160000];
        frame[..4].copy_from_slice(&0x31524652u32.to_le_bytes());
        frame[4..8].copy_from_slice(&1920u32.to_le_bytes());
        frame[8..12].copy_from_slice(&1080u32.to_le_bytes());
        frame[12..16].copy_from_slice(&1u32.to_le_bytes());
        frame[16..24].copy_from_slice(&0u64.to_le_bytes());
        assert_eq!(
            unsafe { remote_send(server, VIDEO, frame.as_ptr(), frame.len()) },
            1
        );
        let input = [61; 32];
        assert_eq!(
            unsafe { remote_send(client, INPUT, input.as_ptr(), input.len()) },
            1
        );

        let mut audio_buffer = vec![0; audio::PACKET_BYTES];
        let mut video_buffer = vec![0; video::MAX_FRAME];
        let mut input_buffer = [0; 64];
        // Unreliable datagrams and the 20 ms queue TTL intentionally discard
        // stale sound. Verify bounded progress with fresh samples, not a fixed
        // delivery ratio that depends on the test runner's scheduling load.
        const REQUIRED_AUDIO_PACKETS: usize = 32;
        const MAX_AUDIO_PACKETS: usize = 256;
        let mut audio_received = 0usize;
        let mut seen_audio = [false; MAX_AUDIO_PACKETS];
        let mut last_audio = None;
        let mut got_video = false;
        let mut got_input = false;
        let mut sequence = 0;
        let mut next_send = Instant::now();
        let deadline = Instant::now() + Duration::from_secs(3);
        let mut last_audio_poll = Instant::now();
        let mut max_audio_poll_gap = Duration::ZERO;
        loop {
            if sequence < MAX_AUDIO_PACKETS as u64 && Instant::now() >= next_send {
                let packet = audio::test_packet(sequence);
                assert_eq!(
                    unsafe { remote_audio_send(server, packet.as_ptr(), packet.len()) },
                    1
                );
                sequence += 1;
                next_send = Instant::now() + Duration::from_millis(5);
            }
            let now = Instant::now();
            max_audio_poll_gap = max_audio_poll_gap.max(now.duration_since(last_audio_poll));
            last_audio_poll = now;
            loop {
                let length = unsafe {
                    remote_audio_poll(client, audio_buffer.as_mut_ptr(), audio_buffer.len())
                };
                assert!(length >= 0, "audio poll failed: {length}");
                if length <= 0 {
                    break;
                }
                assert_eq!(length as usize, audio::PACKET_BYTES);
                assert!(audio::valid_packet(&audio_buffer));
                let received_sequence =
                    u64::from_le_bytes(audio_buffer[16..24].try_into().unwrap());
                assert!(received_sequence < sequence);
                assert!(
                    !seen_audio[received_sequence as usize],
                    "duplicate audio packet"
                );
                seen_audio[received_sequence as usize] = true;
                assert_eq!(
                    &audio_buffer[audio::HEADER_BYTES..],
                    &audio::test_packet(received_sequence)[audio::HEADER_BYTES..]
                );
                audio_received += 1;
                last_audio = Some(received_sequence);
            }
            let mut channel = 0;
            let video_len = unsafe {
                remote_poll(
                    client,
                    &mut channel,
                    video_buffer.as_mut_ptr(),
                    video_buffer.len(),
                )
            };
            if channel == VIDEO && video_len > 0 {
                assert_eq!(&video_buffer[..video_len as usize], frame);
                got_video = true;
            }
            let input_len = unsafe {
                remote_poll(
                    server,
                    &mut channel,
                    input_buffer.as_mut_ptr(),
                    input_buffer.len(),
                )
            };
            if channel == INPUT && input_len == 32 && input_buffer[..32] == input {
                got_input = true;
            }
            if audio_received >= REQUIRED_AUDIO_PACKETS && got_video && got_input {
                break;
            }
            assert!(Instant::now() < deadline, "media/input incomplete: sent={sequence}, audio={audio_received}, last={last_audio:?}, max_poll_gap={max_audio_poll_gap:?}, video={got_video}, input={got_input}");
            thread::sleep(Duration::from_millis(1));
        }
        assert!(
            audio_received >= REQUIRED_AUDIO_PACKETS,
            "insufficient fresh audio progress: {audio_received}/{sequence}"
        );
        assert_eq!(unsafe { remote_status(server) }, 1);
        assert_eq!(unsafe { remote_status(client) }, 1);
        let before = Instant::now();
        unsafe {
            remote_close(client);
        }
        wait_status(server, 0);
        assert_eq!(unsafe { remote_audio_pending(server) }, 0);
        assert_eq!(
            unsafe { remote_audio_send(server, audio_buffer.as_ptr(), audio_buffer.len()) },
            -1
        );
        unsafe {
            remote_close(server);
        }
        assert!(
            before.elapsed() < Duration::from_secs(2),
            "audio did not stop promptly"
        );
        std::fs::remove_dir_all(identity).unwrap();
    }

    #[test]
    fn bulk_capability_proofs_separate_roles_exporters_and_sessions() {
        let ticket = [19; 32];
        let control_exporter = [23; 32];
        let exporter = [29; 32];
        let session = [31; 16];
        let capability = auth::proof(&ticket, b"bulk-capability/v1", &control_exporter, &session);
        let proof = bulk_proof(&capability, &exporter, &session);
        assert_ne!(capability, ticket);
        assert_ne!(proof, auth::media_proof(&ticket, &exporter, &session));
        assert_ne!(proof, bulk_proof(&ticket, &exporter, &session));
        assert_ne!(proof, bulk_proof(&capability, &[30; 32], &session));
        assert_ne!(proof, bulk_proof(&capability, &exporter, &[32; 16]));
        assert_ne!(
            capability,
            auth::proof(&ticket, b"bulk-capability/v1", &[24; 32], &session)
        );
    }
    #[test]
    fn bulk_ffi_bounds_backpressure_poll_retention_and_input_isolation() {
        let shared = Arc::new(Shared::new());
        shared.status.store(1, Ordering::Release);
        shared.bulk_status.store(1, Ordering::Release);
        let (stop, _) = watch::channel(false);
        let mut handle = Handle {
            shared,
            stop,
            worker: None,
        };
        let ptr = &mut handle as *mut _;
        let frame = vec![37; MAX_RELIABLE];
        assert_eq!(unsafe { remote_bulk_send(ptr, frame.as_ptr(), 0) }, -1);
        assert_eq!(
            unsafe { remote_bulk_send(ptr, frame.as_ptr(), MAX_RELIABLE + 1) },
            -1
        );
        assert_eq!(unsafe { remote_bulk_send(ptr, std::ptr::null(), 1) }, -1);
        for _ in 0..BULK_BYTES / MAX_RELIABLE {
            assert_eq!(
                unsafe { remote_bulk_send(ptr, frame.as_ptr(), frame.len()) },
                1
            );
        }
        assert_eq!(
            unsafe { remote_bulk_send(ptr, frame.as_ptr(), frame.len()) },
            0
        );
        assert_eq!(
            handle.shared.bulk_outgoing.lock().unwrap().bytes,
            BULK_BYTES
        );
        handle
            .shared
            .bulk_incoming
            .lock()
            .unwrap()
            .push(vec![41; 100])
            .unwrap();
        handle
            .shared
            .incoming
            .lock()
            .unwrap()
            .push(Message {
                channel: INPUT,
                data: vec![43; 32],
            })
            .ok()
            .unwrap();
        let mut small = [0; 32];
        let mut large = [0; 100];
        assert_eq!(unsafe { remote_bulk_pending(ptr) }, 1);
        assert_eq!(
            unsafe { remote_bulk_poll(ptr, small.as_mut_ptr(), small.len()) },
            -1
        );
        assert_eq!(unsafe { remote_bulk_pending(ptr) }, 1);
        let mut channel = 0;
        assert_eq!(
            unsafe { remote_poll(ptr, &mut channel, small.as_mut_ptr(), small.len()) },
            32
        );
        assert_eq!(channel, INPUT);
        assert_eq!(small, [43; 32]);
        assert_eq!(
            unsafe { remote_bulk_poll(ptr, large.as_mut_ptr(), large.len()) },
            100
        );
        assert_eq!(large, [41; 100]);
        handle.shared.clear_bulk();
        assert_eq!(unsafe { remote_bulk_status(ptr) }, 0);
        assert_eq!(unsafe { remote_bulk_pending(ptr) }, 0);
        assert_eq!(unsafe { remote_bulk_send(ptr, frame.as_ptr(), 1) }, -1);
        assert_eq!(unsafe { remote_status(ptr) }, 1);
        let mut tiny = BulkQueue::default();
        for _ in 0..BULK_MESSAGES {
            tiny.push(vec![1]).unwrap();
        }
        assert!(tiny.push(vec![1]).is_err());
    }
    fn wait_bulk(handle: *mut Handle, status: i32) {
        let deadline = Instant::now() + Duration::from_secs(12);
        while unsafe { remote_bulk_status(handle) } != status {
            assert!(
                Instant::now() < deadline,
                "bulk timeout: expected {status}, got {}",
                unsafe { remote_bulk_status(handle) }
            );
            thread::sleep(Duration::from_millis(5));
        }
    }
    fn network_fixture(label: &str) -> (*mut Handle, SocketAddr, std::path::PathBuf) {
        let identity =
            std::env::temp_dir().join(format!("remote-bulk-{label}-{}", rand::random::<u64>()));
        let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        drop(socket);
        let server = start(
            Mode::Server {
                bind: address,
                identity: identity.to_str().unwrap().into(),
            },
            "123456".into(),
        )
        .unwrap();
        server_ready(server);
        (server, address, identity)
    }
    #[test]
    fn encrypted_bulk_backpressure_preserves_input_video_and_recovers_independently() {
        let (server, address, identity) = network_fixture("isolation");
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        wait_bulk(client, 1);
        wait_bulk(server, 1);
        let frame = vec![47; MAX_RELIABLE];
        let deadline = Instant::now() + Duration::from_secs(5);
        // Stop bulk polling: receive flow control must fill a finite queue while
        // input and video continue on their unrelated connection/receive queue.
        loop {
            let _ = unsafe { remote_bulk_send(client, frame.as_ptr(), frame.len()) };
            if unsafe { remote_bulk_pending(server) } == (BULK_BYTES / MAX_RELIABLE) as u32 {
                break;
            }
            assert!(Instant::now() < deadline, "bulk queue did not fill");
            thread::sleep(Duration::from_millis(2));
        }
        assert!(
            unsafe { &*server }
                .shared
                .bulk_incoming
                .lock()
                .unwrap()
                .bytes
                <= BULK_BYTES
        );
        let input = [53; 32];
        assert_eq!(
            unsafe { remote_send(client, INPUT, input.as_ptr(), input.len()) },
            1
        );
        let mut video = vec![59; 6000];
        video[..4].copy_from_slice(&0x31524652u32.to_le_bytes());
        video[4..8].copy_from_slice(&1920u32.to_le_bytes());
        video[8..12].copy_from_slice(&1080u32.to_le_bytes());
        video[12..16].copy_from_slice(&1u32.to_le_bytes());
        video[16..24].copy_from_slice(&0u64.to_le_bytes());
        assert_eq!(
            unsafe { remote_send(server, VIDEO, video.as_ptr(), video.len()) },
            1
        );
        let deadline = Instant::now() + Duration::from_secs(2);
        let mut got_input = false;
        let mut got_video = false;
        let mut buffer = vec![0; video::MAX_FRAME];
        while !got_input || !got_video {
            let mut channel = 0;
            let len =
                unsafe { remote_poll(server, &mut channel, buffer.as_mut_ptr(), buffer.len()) };
            if channel == INPUT && len == 32 && buffer[..32] == input {
                got_input = true;
            }
            let len =
                unsafe { remote_poll(client, &mut channel, buffer.as_mut_ptr(), buffer.len()) };
            if channel == VIDEO && len > 0 {
                assert_eq!(&buffer[..len as usize], video);
                got_video = true;
            }
            assert!(
                Instant::now() < deadline,
                "bulk backpressure blocked input/video"
            );
            thread::sleep(Duration::from_millis(2));
        }
        let mut small = [0; 10];
        assert_eq!(
            unsafe { remote_bulk_poll(server, small.as_mut_ptr(), small.len()) },
            -1
        );
        assert_eq!(
            unsafe { remote_bulk_pending(server) },
            (BULK_BYTES / MAX_RELIABLE) as u32
        );
        // Closing only bulk must clear old payloads and allow another TLS-bound
        // bulk generation without another token proof or remote disconnect.
        unsafe { &*server }
            .shared
            .bulk_connection
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .close(0u32.into(), b"test bulk interruption");
        wait_bulk(client, -1);
        assert_eq!(unsafe { remote_status(server) }, 1);
        assert_eq!(unsafe { remote_status(client) }, 1);
        wait_bulk(client, 1);
        wait_bulk(server, 1);
        assert_eq!(unsafe { remote_bulk_pending(server) }, 0);
        assert_eq!(unsafe { remote_bulk_pending(client) }, 0);
        for (sender, receiver, data) in [
            (client, server, b"new client payload".as_slice()),
            (server, client, b"new server payload".as_slice()),
        ] {
            assert_eq!(
                unsafe { remote_bulk_send(sender, data.as_ptr(), data.len()) },
                1
            );
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                let len = unsafe { remote_bulk_poll(receiver, buffer.as_mut_ptr(), buffer.len()) };
                if len > 0 {
                    assert_eq!(&buffer[..len as usize], data);
                    break;
                }
                assert!(Instant::now() < deadline);
                thread::sleep(Duration::from_millis(2));
            }
        }
        assert!(policy_json(server)["entries"]
            .as_array()
            .unwrap()
            .is_empty());
        let before = Instant::now();
        unsafe {
            remote_close(client);
            remote_close(server);
        }
        assert!(before.elapsed() < Duration::from_secs(2));
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn old_two_connection_client_and_invalid_bulk_are_not_password_failures() {
        let (server, address, identity) = network_fixture("auth");
        tokio::runtime::Runtime::new().unwrap().block_on(async {
            let mut endpoint = quinn::Endpoint::client("127.0.0.1:0".parse().unwrap()).unwrap();
            endpoint.set_default_client_config(tls::client_config(None).unwrap());
            let session = [61; 16];
            let (input, ticket) = connect_control(&endpoint, address, b"123456", session)
                .await
                .unwrap();
            let media = connect_media(&endpoint, address, ticket, session)
                .await
                .unwrap();
            let capability = bulk_capability(&ticket, &input, &session).unwrap();
            tokio::time::sleep(Duration::from_millis(30)).await;
            assert_eq!(unsafe { remote_status(server) }, 1);
            assert_eq!(unsafe { remote_bulk_status(server) }, 0);
            for (bad_capability, bad_session) in [
                ([0; 32], session),
                (ticket, session),
                (capability, [62; 16]),
            ] {
                assert!(
                    connect_bulk(&endpoint, address, &bad_capability, &bad_session)
                        .await
                        .is_err()
                );
                assert!(policy_json(server)["entries"]
                    .as_array()
                    .unwrap()
                    .is_empty());
                assert_eq!(unsafe { remote_status(server) }, 1);
            }
            // A captured proof is unusable on a fresh TLS connection, even for
            // the same authenticated control session and source IP.
            let previous = connect_tls(&endpoint, address).await.unwrap();
            let replayed = bulk_proof(
                &capability,
                &auth::exporter(&previous, &session).unwrap(),
                &session,
            );
            previous.close(0u32.into(), b"test old TLS generation");
            let fresh = connect_tls(&endpoint, address).await.unwrap();
            let (mut tx, mut rx) = fresh.open_bi().await.unwrap();
            let mut hello = b"RMC2".to_vec();
            hello.push(3);
            hello.extend_from_slice(&session);
            hello.extend_from_slice(&replayed);
            tx.write_all(&hello).await.unwrap();
            tx.finish().unwrap();
            assert!(
                tokio::time::timeout(Duration::from_secs(2), rx.read_exact(&mut [0]))
                    .await
                    .unwrap()
                    .is_err()
            );
            assert!(policy_json(server)["entries"]
                .as_array()
                .unwrap()
                .is_empty());
            assert_eq!(unsafe { remote_status(server) }, 1);
            let mut stream = input.open_uni().await.unwrap();
            stream.write_all(&[INPUT as u8]).await.unwrap();
            stream.write_all(&32u32.to_le_bytes()).await.unwrap();
            stream.write_all(&[67; 32]).await.unwrap();
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                let mut buffer = [0; 32];
                let mut channel = 0;
                if unsafe { remote_poll(server, &mut channel, buffer.as_mut_ptr(), buffer.len()) }
                    == 32
                {
                    assert_eq!(channel, INPUT);
                    assert_eq!(buffer, [67; 32]);
                    break;
                }
                assert!(Instant::now() < deadline);
                tokio::time::sleep(Duration::from_millis(2)).await;
            }
            let bulk = connect_bulk(&endpoint, address, &capability, &session)
                .await
                .unwrap();
            let (mut stream, _reply) = bulk.open_bi().await.unwrap();
            stream.write_all(b"BUL1").await.unwrap();
            stream
                .write_all(&((MAX_RELIABLE + 1) as u32).to_le_bytes())
                .await
                .unwrap();
            tokio::time::timeout(Duration::from_secs(2), bulk.closed())
                .await
                .unwrap();
            assert_eq!(unsafe { remote_status(server) }, 1);
            assert!(policy_json(server)["entries"]
                .as_array()
                .unwrap()
                .is_empty());
            input.close(0u32.into(), b"test completed");
            media.close(0u32.into(), b"test completed");
        });
        unsafe {
            remote_close(server);
        }
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn new_client_bulk_refusal_keeps_legacy_server_remote_session_ready() {
        let identity =
            std::env::temp_dir().join(format!("remote-bulk-legacy-{}", rand::random::<u64>()));
        tokio::runtime::Runtime::new().unwrap().block_on(async {
            let (config, _) = tls::server_config(identity.to_str().unwrap()).unwrap();
            let endpoint = quinn::Endpoint::server(config, "127.0.0.1:0".parse().unwrap()).unwrap();
            let client = start(
                Mode::Client {
                    server: endpoint.local_addr().unwrap(),
                    pin: None,
                },
                "123456".into(),
            )
            .unwrap();
            let mut pair = Pair::default();
            // Emulate the unchanged v2 server wire contract: control and media
            // are accepted, then all further connections are refused.
            for _ in 0..2 {
                let connection = endpoint.accept().await.unwrap().await.unwrap();
                let (role, id, ticket, mut reply) =
                    authenticate_server(&connection, b"123456", &pair)
                        .await
                        .ok()
                        .unwrap();
                reply.write_all(&[1]).await.unwrap();
                reply.finish().unwrap();
                pair.session = Some(id);
                pair.peer = Some(connection.remote_address().ip());
                if role == 1 {
                    pair.ticket = Some(ticket);
                    pair.input = Some(connection);
                } else {
                    pair.media = Some(connection);
                }
            }
            let state = Arc::new(Shared::new());
            state.status.store(1, Ordering::Release);
            let mut tasks = tokio::task::JoinSet::new();
            tasks.spawn(session(
                pair.input.take().unwrap(),
                pair.media.take().unwrap(),
                state.clone(),
                None,
            ));
            tasks.spawn(async move {
                while let Some(incoming) = endpoint.accept().await {
                    incoming.refuse();
                }
                Ok(())
            });
            let deadline = Instant::now() + Duration::from_secs(5);
            while unsafe { remote_bulk_status(client) } != -1 {
                assert!(Instant::now() < deadline);
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
            assert_eq!(unsafe { remote_status(client) }, 1);
            let data = [71; 32];
            assert_eq!(
                unsafe { remote_send(client, INPUT, data.as_ptr(), data.len()) },
                1
            );
            loop {
                if let Some(message) = state.incoming.lock().unwrap().pop() {
                    if message.channel == INPUT && message.data == data {
                        break;
                    }
                }
                assert!(Instant::now() < deadline);
                tokio::time::sleep(Duration::from_millis(2)).await;
            }
            unsafe {
                remote_close(client);
            }
            tasks.abort_all();
            while tasks.join_next().await.is_some() {}
        });
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn video_overflow_reserves_recovery_even_when_input_queue_is_full() {
        let shared = Arc::new(Shared::new());
        for _ in 0..QUEUE_MESSAGES {
            shared
                .incoming
                .lock()
                .unwrap()
                .push(Message {
                    channel: INPUT,
                    data: vec![2; 32],
                })
                .ok()
                .unwrap();
        }
        shared.request_local_keyframe();
        shared.request_local_keyframe();
        let (stop, _) = watch::channel(false);
        let mut handle = Handle {
            shared,
            stop,
            worker: None,
        };
        let ptr = &mut handle as *mut _;
        let mut channel = 0;
        let mut small = [0; 10];
        let mut buffer = [0; 32];
        assert_eq!(
            unsafe { remote_poll(ptr, &mut channel, small.as_mut_ptr(), 10) },
            -1
        );
        assert_eq!(
            unsafe { remote_pending(ptr, INPUT) },
            QUEUE_MESSAGES as u32 + 1
        );
        assert_eq!(
            unsafe { remote_poll(ptr, &mut channel, buffer.as_mut_ptr(), 32) },
            32
        );
        assert_eq!(
            u32::from_le_bytes(buffer[..4].try_into().unwrap()),
            KEYFRAME
        );
        assert_eq!(unsafe { remote_pending(ptr, INPUT) }, QUEUE_MESSAGES as u32);
        assert!(handle.shared.sender_wait_idr.load(Ordering::Acquire));
    }
    #[test]
    fn viewer_overflow_clears_encoded_reference_chain() {
        let state = Shared::new();
        for _ in 0..3 {
            assert!(state.inbound_video(Message {
                channel: VIDEO,
                data: vec![42; 128]
            }));
        }
        assert!(!state.inbound_video(Message {
            channel: VIDEO,
            data: vec![42; 128]
        }));
        assert!(state.incoming.lock().unwrap().data.is_empty());
    }
    #[test]
    fn only_adjacent_mouse_moves_are_coalesced() {
        let mut q = Queue::default();
        let mut data = vec![0; 32];
        data[..4].copy_from_slice(&1u32.to_le_bytes());
        q.push(Message {
            channel: INPUT,
            data: data.clone(),
        })
        .ok()
        .unwrap();
        data[8] = 55;
        q.push(Message {
            channel: INPUT,
            data: data.clone(),
        })
        .ok()
        .unwrap();
        assert_eq!(q.data.len(), 1);
        q.push(Message {
            channel: INPUT,
            data: vec![2; 32],
        })
        .ok()
        .unwrap();
        q.push(Message {
            channel: INPUT,
            data,
        })
        .ok()
        .unwrap();
        assert_eq!(q.data.len(), 3);
    }
    #[test]
    fn poll_capacity_does_not_drop_message() {
        let shared = Arc::new(Shared::new());
        shared
            .incoming
            .lock()
            .unwrap()
            .push(Message {
                channel: 1,
                data: vec![42; 100],
            })
            .ok()
            .unwrap();
        let (stop, _) = watch::channel(false);
        let mut handle = Handle {
            shared,
            stop,
            worker: None,
        };
        let ptr = &mut handle as *mut _;
        let mut channel = 0;
        let mut small = [0; 10];
        let mut large = [0; 100];
        assert_eq!(
            unsafe { remote_poll(ptr, &mut channel, small.as_mut_ptr(), 10) },
            -1
        );
        assert_eq!(
            unsafe { remote_poll(ptr, &mut channel, large.as_mut_ptr(), 100) },
            100
        );
        assert_eq!(large, [42; 100]);
        assert_eq!(channel, 1);
    }
    fn wait_status(handle: *mut Handle, status: i32) {
        let deadline = Instant::now() + Duration::from_secs(12);
        while unsafe { remote_status(handle) } != status {
            if Instant::now() >= deadline {
                let mut error = [0i8; 512];
                unsafe {
                    remote_last_error(handle, error.as_mut_ptr(), error.len());
                }
                panic!(
                    "status timeout: wanted {status}, got {}, error {}",
                    unsafe { remote_status(handle) },
                    unsafe { CStr::from_ptr(error.as_ptr()) }.to_string_lossy()
                );
            }
            thread::sleep(Duration::from_millis(10));
        }
    }
    fn server_ready(server: *mut Handle) {
        let deadline = Instant::now() + Duration::from_secs(5);
        while unsafe { &*server }
            .shared
            .fingerprint
            .lock()
            .unwrap()
            .is_empty()
        {
            assert!(Instant::now() < deadline);
            thread::sleep(Duration::from_millis(5));
        }
    }
    fn policy_json(server: *mut Handle) -> serde_json::Value {
        let mut data = vec![0i8; 1024 * 1024];
        let length = unsafe { remote_auth_policy_json(server, data.as_mut_ptr(), data.len()) };
        assert!(length > 0);
        serde_json::from_str(unsafe { CStr::from_ptr(data.as_ptr()) }.to_str().unwrap()).unwrap()
    }
    #[test]
    fn idle_token_rotation_accepts_first_connection_of_new_generation() {
        use std::ffi::CString;
        let identity =
            std::env::temp_dir().join(format!("remote-auth-idle-{}", rand::random::<u64>()));
        let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        drop(socket);
        let server = start(
            Mode::Server {
                bind: address,
                identity: identity.to_str().unwrap().into(),
            },
            "123456".into(),
        )
        .unwrap();
        server_ready(server);
        for index in 0..3 {
            // Let the server enter endpoint.accept() before updating its token.
            thread::sleep(Duration::from_millis(30));
            let token = format!("custom-idle-{index}");
            let value = CString::new(token.clone()).unwrap();
            assert_eq!(unsafe { remote_set_token(server, value.as_ptr()) }, 1);
            let client = start(
                Mode::Client {
                    server: address,
                    pin: None,
                },
                token,
            )
            .unwrap();
            wait_status(client, 1);
            wait_status(server, 1);
            assert!(policy_json(server)["entries"]
                .as_array()
                .unwrap()
                .is_empty());
            unsafe {
                remote_close(client);
            }
            wait_status(server, 0);
        }
        unsafe {
            remote_close(server);
        }
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn real_network_tenth_wrong_token_persists_blocks_unblocks_and_rotates_token() {
        use std::ffi::CString;
        let identity =
            std::env::temp_dir().join(format!("remote-auth-network-{}", rand::random::<u64>()));
        let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        drop(socket);
        let mode = Mode::Server {
            bind: address,
            identity: identity.to_str().unwrap().into(),
        };
        let mut server = start(mode.clone(), "123456".into()).unwrap();
        server_ready(server);
        for attempt in 1..=10 {
            let client = start(
                Mode::Client {
                    server: address,
                    pin: None,
                },
                "654321".into(),
            )
            .unwrap();
            wait_status(client, -1);
            unsafe {
                remote_close(client);
            }
            let policy = policy_json(server);
            assert_eq!(policy["entries"][0]["failures"], attempt);
            assert_eq!(policy["entries"][0]["blocked"], attempt == 10);
            assert_eq!(
                policy["entries"].as_array().unwrap().len(),
                1,
                "one logical guess must be counted once"
            );
        }
        unsafe {
            remote_close(server);
        }
        server = start(mode, "123456".into()).unwrap();
        server_ready(server);
        let blocked = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(blocked, -1);
        unsafe {
            remote_close(blocked);
        }
        assert_eq!(policy_json(server)["entries"][0]["failures"], 10);
        let ip = CString::new("127.0.0.1").unwrap();
        let invalid = CString::new("not-an-ip").unwrap();
        assert_eq!(unsafe { remote_unblock_ip(server, invalid.as_ptr()) }, -1);
        assert_eq!(unsafe { remote_unblock_ip(server, ip.as_ptr()) }, 1);
        assert!(policy_json(server)["entries"]
            .as_array()
            .unwrap()
            .is_empty());
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        let short = CString::new("12345").unwrap();
        assert_eq!(unsafe { remote_set_token(server, short.as_ptr()) }, -1);
        assert_eq!(unsafe { remote_status(client) }, 1);
        let replacement = CString::new("自定义配对令牌").unwrap();
        assert_eq!(unsafe { remote_set_token(server, replacement.as_ptr()) }, 1);
        wait_status(client, -1);
        unsafe {
            remote_close(client);
        }
        wait_status(server, 0);
        let old = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "123456".into(),
        )
        .unwrap();
        wait_status(old, -1);
        unsafe {
            remote_close(old);
        }
        assert_eq!(policy_json(server)["entries"][0]["failures"], 1);
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            "自定义配对令牌".into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        assert!(policy_json(server)["entries"]
            .as_array()
            .unwrap()
            .is_empty());
        unsafe {
            remote_close(client);
            remote_close(server);
        }
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn truncated_control_and_invalid_media_capability_are_not_password_failures() {
        let identity =
            std::env::temp_dir().join(format!("remote-auth-incomplete-{}", rand::random::<u64>()));
        let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        drop(socket);
        let server = start(
            Mode::Server {
                bind: address,
                identity: identity.to_str().unwrap().into(),
            },
            "123456".into(),
        )
        .unwrap();
        server_ready(server);
        let runtime = tokio::runtime::Runtime::new().unwrap();
        runtime.block_on(async {
            let mut endpoint = quinn::Endpoint::client("127.0.0.1:0".parse().unwrap()).unwrap();
            endpoint.set_default_client_config(tls::client_config(None).unwrap());
            let connection = connect_tls(&endpoint, address).await.unwrap();
            let (mut tx, mut rx) = connection.open_bi().await.unwrap();
            let (_, message) = auth::begin_client(b"123456");
            let session = [7; 16];
            let mut hello = b"RMC2\x01".to_vec();
            hello.extend_from_slice(&session);
            hello.extend_from_slice(&message);
            tx.write_all(&hello).await.unwrap();
            let mut reply = [0; 33];
            rx.read_exact(&mut reply).await.unwrap();
            tx.finish().unwrap();
            connection.close(0u32.into(), b"network interrupted before confirmation");
            tokio::time::sleep(Duration::from_millis(30)).await;
            assert!(policy_json(server)["entries"]
                .as_array()
                .unwrap()
                .is_empty());
            let (input, _ticket) = connect_control(&endpoint, address, b"123456", session)
                .await
                .unwrap();
            assert!(connect_media(&endpoint, address, [0; 32], session)
                .await
                .is_err());
            assert!(policy_json(server)["entries"]
                .as_array()
                .unwrap()
                .is_empty());
            input.close(0u32.into(), b"test completed");
        });
        unsafe {
            remote_close(server);
        }
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn first_use_client_does_not_reveal_token_or_trust_unconfirmed_server() {
        let identity =
            std::env::temp_dir().join(format!("remote-auth-first-use-{}", rand::random::<u64>()));
        let runtime = tokio::runtime::Runtime::new().unwrap();
        runtime.block_on(async {
            let (config, _) = tls::server_config(identity.to_str().unwrap()).unwrap();
            let endpoint = quinn::Endpoint::server(config, "127.0.0.1:0".parse().unwrap()).unwrap();
            let client = start(
                Mode::Client {
                    server: endpoint.local_addr().unwrap(),
                    pin: None,
                },
                "123456".into(),
            )
            .unwrap();
            let connection = endpoint.accept().await.unwrap().await.unwrap();
            let (mut tx, mut rx) = connection.accept_bi().await.unwrap();
            let mut first_message = [0; 54];
            rx.read_exact(&mut first_message).await.unwrap();
            assert_eq!(&first_message[..5], b"RMC2\x01");
            assert!(!first_message.windows(6).any(|chunk| chunk == b"123456"));
            let (_, server_message) = auth::begin_server(b"654321");
            tx.write_all(&server_message).await.unwrap();
            let mut confirmation = [0; 32];
            rx.read_exact(&mut confirmation).await.unwrap();
            // A fake peer cannot produce the server's password/TLS-bound proof.
            let mut fake = [0; 65];
            fake[64] = 1;
            tx.write_all(&fake).await.unwrap();
            tx.finish().unwrap();
            let client_address = client as usize;
            tokio::task::spawn_blocking(move || wait_status(client_address as *mut Handle, -1))
                .await
                .unwrap();
            assert!(unsafe { &*client }
                .shared
                .fingerprint
                .lock()
                .unwrap()
                .is_empty());
            unsafe {
                remote_close(client);
            }
        });
        std::fs::remove_dir_all(identity).unwrap();
    }
    #[test]
    fn encrypted_loopback_pin_token_streams_video_and_close() {
        let identity =
            std::env::temp_dir().join(format!("remote-transport-test-{}", rand::random::<u64>()));
        let socket = std::net::UdpSocket::bind("127.0.0.1:0").unwrap();
        let address = socket.local_addr().unwrap();
        drop(socket);
        let token = "loopback-test-pairing-secret";
        let server = start(
            Mode::Server {
                bind: address,
                identity: identity.to_str().unwrap().into(),
            },
            token.into(),
        )
        .unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        let fingerprint = loop {
            let fp = unsafe { &*server }
                .shared
                .fingerprint
                .lock()
                .unwrap()
                .clone();
            if !fp.is_empty() {
                break fp;
            }
            assert!(Instant::now() < deadline, "server did not acquire identity");
            thread::sleep(Duration::from_millis(10));
        };
        let wrong_pin = start(
            Mode::Client {
                server: address,
                pin: Some([0; 32]),
            },
            token.into(),
        )
        .unwrap();
        wait_status(wrong_pin, -1);
        unsafe {
            remote_close(wrong_pin);
        }
        assert!(unsafe { &*server }
            .shared
            .policy
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .json()
            .unwrap()
            .contains("\"entries\":[]"));
        let wrong_token = start(
            Mode::Client {
                server: address,
                pin: Some(tls::decode_pin(&fingerprint).unwrap()),
            },
            "incorrect-pairing-token".into(),
        )
        .unwrap();
        wait_status(wrong_token, -1);
        unsafe {
            remote_close(wrong_token);
        }
        assert!(unsafe { &*server }
            .shared
            .policy
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .json()
            .unwrap()
            .contains("\"failures\":1"));
        let client = start(
            Mode::Client {
                server: address,
                pin: None,
            },
            token.into(),
        )
        .unwrap();
        wait_status(client, 1);
        wait_status(server, 1);
        assert_eq!(
            *unsafe { &*client }.shared.fingerprint.lock().unwrap(),
            fingerprint
        );
        assert!(unsafe { &*server }
            .shared
            .policy
            .lock()
            .unwrap()
            .as_ref()
            .unwrap()
            .json()
            .unwrap()
            .contains("\"entries\":[]"));
        for channel in 1..=3 {
            let data = vec![channel as u8 + 40; 80];
            assert_eq!(
                unsafe { remote_send(client, channel, data.as_ptr(), data.len()) },
                1
            );
        }
        let mut received = [false; 3];
        let deadline = Instant::now() + Duration::from_secs(3);
        while !received.iter().all(|v| *v) {
            let mut buffer = vec![0; 2 * 1024 * 1024];
            let mut channel = 0;
            let len =
                unsafe { remote_poll(server, &mut channel, buffer.as_mut_ptr(), buffer.len()) };
            if len == 80 {
                assert_eq!(&buffer[..80], &vec![channel as u8 + 40; 80]);
                received[channel as usize - 1] = true;
            }
            assert!(
                Instant::now() < deadline,
                "reliable channels did not arrive"
            );
            thread::sleep(Duration::from_millis(2));
        }
        let mut frame = vec![17; 160000];
        frame[..4].copy_from_slice(&0x31524652u32.to_le_bytes());
        frame[4..8].copy_from_slice(&1920u32.to_le_bytes());
        frame[8..12].copy_from_slice(&1080u32.to_le_bytes());
        frame[12..16].copy_from_slice(&1u32.to_le_bytes());
        frame[16..24].copy_from_slice(&0u64.to_le_bytes());
        assert_eq!(
            unsafe { remote_send(server, VIDEO, frame.as_ptr(), frame.len()) },
            1
        );
        let deadline = Instant::now() + Duration::from_secs(3);
        let mut buffer = vec![0; video::MAX_FRAME];
        loop {
            let mut channel = 0;
            let len =
                unsafe { remote_poll(client, &mut channel, buffer.as_mut_ptr(), buffer.len()) };
            if channel == VIDEO && len > 0 {
                assert_eq!(&buffer[..len as usize], frame);
                break;
            }
            assert!(Instant::now() < deadline, "video frame did not arrive");
            thread::sleep(Duration::from_millis(2));
        }
        let before = Instant::now();
        unsafe {
            remote_close(client);
            remote_close(server);
        }
        assert!(
            before.elapsed() < Duration::from_secs(2),
            "close did not terminate workers"
        );
        let (_, persisted) = tls::server_config(identity.to_str().unwrap()).unwrap();
        assert_eq!(persisted, fingerprint);
        std::fs::remove_dir_all(identity).unwrap();
    }
}
