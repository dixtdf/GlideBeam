//! Password-authenticated pairing. First use never sends the pairing token.
use hmac::{Hmac, Mac};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use spake2::{Ed25519Group, Group, Identity, Password, Spake2};
use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    net::IpAddr,
    path::{Path, PathBuf},
};

pub const THRESHOLD: u8 = 10;
const CLIENT_ID: &[u8] = b"RemoteNative/control/client/v2";
const SERVER_ID: &[u8] = b"RemoteNative/control/server/v2";
const EXPORT_LABEL: &[u8] = b"EXPORTER-RemoteNative-pairing-v2";
pub type Pake = Spake2<Ed25519Group>;

pub fn valid_token(token: &str) -> bool {
    (6..=128).contains(&token.chars().count()) && !token.chars().any(char::is_control)
}
pub fn begin_client(token: &[u8]) -> (Pake, Vec<u8>) {
    Spake2::start_a(
        &Password::new(token),
        &Identity::new(CLIENT_ID),
        &Identity::new(SERVER_ID),
    )
}
pub fn begin_server(token: &[u8]) -> (Pake, Vec<u8>) {
    Spake2::start_b(
        &Password::new(token),
        &Identity::new(CLIENT_ID),
        &Identity::new(SERVER_ID),
    )
}
pub fn finish(pake: Pake, message: &[u8], expected_role: u8) -> Result<Vec<u8>, String> {
    if message.len() != 33 || message[0] != expected_role {
        return Err("invalid PAKE message role or length".into());
    }
    let point = Ed25519Group::bytes_to_element(&message[1..]).ok_or("invalid PAKE curve point")?;
    if !point.is_torsion_free()
        || point.is_small_order()
        || point.compress().as_bytes() != &message[1..]
    {
        return Err("invalid PAKE prime-order group element".into());
    }
    pake.finish(message)
        .map_err(|_| "invalid PAKE message".into())
}
pub fn exporter(connection: &quinn::Connection, session: &[u8; 16]) -> Result<[u8; 32], String> {
    let mut value = [0u8; 32];
    connection
        .export_keying_material(&mut value, EXPORT_LABEL, session)
        .map_err(|_| "TLS exporter unavailable".to_owned())?;
    Ok(value)
}
pub fn context(exporter: &[u8; 32], session: &[u8; 16], client: &[u8], server: &[u8]) -> [u8; 32] {
    let mut hash = Sha256::new();
    hash.update(b"RemoteNative/PAKE-transcript/v2");
    hash.update(exporter);
    hash.update(session);
    hash.update(client);
    hash.update(server);
    hash.finalize().into()
}
pub fn proof(key: &[u8], role: &[u8], context: &[u8; 32], payload: &[u8]) -> [u8; 32] {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts arbitrary key sizes");
    mac.update(role);
    mac.update(context);
    mac.update(payload);
    mac.finalize().into_bytes().into()
}
pub fn verify(
    key: &[u8],
    role: &[u8],
    context: &[u8; 32],
    payload: &[u8],
    received: &[u8],
) -> bool {
    let mut mac = Hmac::<Sha256>::new_from_slice(key).expect("HMAC accepts arbitrary key sizes");
    mac.update(role);
    mac.update(context);
    mac.update(payload);
    mac.verify_slice(received).is_ok()
}
pub fn media_proof(ticket: &[u8; 32], exporter: &[u8; 32], session: &[u8; 16]) -> [u8; 32] {
    proof(ticket, b"media", exporter, session)
}
pub fn peer_fingerprint(connection: &quinn::Connection) -> Result<String, String> {
    let identity = connection
        .peer_identity()
        .ok_or("missing TLS peer identity")?;
    let certificates = identity
        .downcast::<Vec<rustls::pki_types::CertificateDer<'static>>>()
        .map_err(|_| "unexpected TLS peer identity")?;
    certificates
        .first()
        .map(|cert| crate::tls::fingerprint(cert.as_ref()))
        .ok_or_else(|| "missing TLS peer certificate".into())
}

#[derive(Clone, Serialize, Deserialize, Debug, PartialEq)]
pub struct Entry {
    pub ip: IpAddr,
    pub failures: u8,
    pub blocked: bool,
}
#[derive(Serialize, Deserialize)]
struct PolicyFile {
    threshold: u8,
    entries: Vec<Entry>,
}
pub struct Policy {
    path: PathBuf,
    entries: BTreeMap<IpAddr, Entry>,
}
impl Policy {
    pub fn load(identity: &str) -> Result<Self, String> {
        let directory = Path::new(identity);
        fs::create_dir_all(directory).map_err(|e| format!("create auth directory: {e}"))?;
        let path = directory.join("auth-policy.json");
        let mut policy = Self {
            path,
            entries: BTreeMap::new(),
        };
        if policy.path.exists() {
            let bytes = fs::read(&policy.path).map_err(|e| format!("read auth policy: {e}"))?;
            if bytes.len() > 1024 * 1024 {
                return Err("auth policy exceeds size limit".into());
            }
            let file: PolicyFile =
                serde_json::from_slice(&bytes).map_err(|e| format!("invalid auth policy: {e}"))?;
            if file.threshold != THRESHOLD || file.entries.len() > 4096 {
                return Err("unsupported auth policy".into());
            }
            for entry in file.entries {
                if entry.failures > THRESHOLD || entry.blocked != (entry.failures == THRESHOLD) {
                    return Err("invalid auth policy failure counter".into());
                }
                if entry.failures > 0 {
                    policy.entries.insert(entry.ip, entry);
                }
            }
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                fs::set_permissions(&policy.path, fs::Permissions::from_mode(0o600))
                    .map_err(|e| format!("protect auth policy: {e}"))?;
            }
        }
        Ok(policy)
    }
    pub fn blocked(&self, ip: IpAddr) -> bool {
        self.entries.get(&ip).is_some_and(|e| e.blocked)
    }
    pub fn json(&self) -> Result<String, String> {
        serde_json::to_string(&PolicyFile {
            threshold: THRESHOLD,
            entries: self.entries.values().cloned().collect(),
        })
        .map_err(|e| e.to_string())
    }
    fn save(&self) -> Result<(), String> {
        let temporary = self.path.with_extension("json.tmp");
        let mut options = fs::OpenOptions::new();
        options.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            options.mode(0o600);
        }
        let mut file = options
            .open(&temporary)
            .map_err(|e| format!("write auth policy: {e}"))?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            file.set_permissions(fs::Permissions::from_mode(0o600))
                .map_err(|e| e.to_string())?;
        }
        file.write_all(self.json()?.as_bytes())
            .and_then(|_| file.sync_all())
            .map_err(|e| format!("flush auth policy: {e}"))?;
        drop(file);
        replace_file(&temporary, &self.path)
    }
    pub fn failure(&mut self, ip: IpAddr) -> Result<(), String> {
        if !self.entries.contains_key(&ip) && self.entries.len() >= 4096 {
            return Err("auth policy entry capacity reached".into());
        }
        let old = self.entries.get(&ip).cloned();
        let entry = self.entries.entry(ip).or_insert(Entry {
            ip,
            failures: 0,
            blocked: false,
        });
        entry.failures = entry.failures.saturating_add(1).min(THRESHOLD);
        entry.blocked = entry.failures >= THRESHOLD;
        if let Err(error) = self.save() {
            if let Some(entry) = old {
                self.entries.insert(ip, entry);
            } else {
                self.entries.remove(&ip);
            }
            return Err(error);
        }
        Ok(())
    }
    pub fn success(&mut self, ip: IpAddr) -> Result<(), String> {
        if !self.blocked(ip) {
            self.unblock(ip)?;
        }
        Ok(())
    }
    pub fn unblock(&mut self, ip: IpAddr) -> Result<(), String> {
        if let Some(entry) = self.entries.remove(&ip) {
            if let Err(error) = self.save() {
                self.entries.insert(ip, entry);
                return Err(error);
            }
        }
        Ok(())
    }
}
#[cfg(not(windows))]
fn replace_file(from: &Path, to: &Path) -> Result<(), String> {
    fs::rename(from, to).map_err(|e| format!("replace auth policy: {e}"))
}
#[cfg(windows)]
fn replace_file(from: &Path, to: &Path) -> Result<(), String> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn MoveFileExW(existing: *const u16, destination: *const u16, flags: u32) -> i32;
    }
    let from: Vec<u16> = from.as_os_str().encode_wide().chain(Some(0)).collect();
    let to: Vec<u16> = to.as_os_str().encode_wide().chain(Some(0)).collect();
    if unsafe { MoveFileExW(from.as_ptr(), to.as_ptr(), 1 | 8) } != 0 {
        Ok(())
    } else {
        Err(format!(
            "replace auth policy: {}",
            std::io::Error::last_os_error()
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn password_policy_accepts_custom_unicode_and_six_characters() {
        assert!(valid_token("123456"));
        assert!(valid_token("自定义配对令牌"));
        assert!(valid_token(&"🖥".repeat(128)));
        assert!(!valid_token(&"🖥".repeat(129)));
        assert!(!valid_token("12345"));
        assert!(!valid_token("123456\n"));
        assert!(!valid_token("123456\u{0085}"));
    }
    #[test]
    fn pake_mutual_proofs_bind_password_tls_session_roles_and_ticket() {
        let token = b"123456";
        let (client, a) = begin_client(token);
        let (server, b) = begin_server(token);
        let c = finish(client, &b, b'B').unwrap();
        let s = finish(server, &a, b'A').unwrap();
        assert_eq!(c, s);
        assert_eq!(a.len(), 33);
        assert_eq!(b.len(), 33);
        assert!(!a.windows(token.len()).any(|window| window == token));
        let session = [1; 16];
        let exporter = [2; 32];
        let ctx = context(&exporter, &session, &a, &b);
        let ticket = [3; 32];
        let client_proof = proof(&c, b"client", &ctx, &[]);
        assert!(verify(&s, b"client", &ctx, &[], &client_proof));
        assert!(!verify(&s, b"server", &ctx, &[], &client_proof));
        assert!(!verify(
            &s,
            b"client",
            &context(&[4; 32], &session, &a, &b),
            &[],
            &client_proof
        ));
        assert!(!verify(
            &s,
            b"client",
            &context(&exporter, &[5; 16], &a, &b),
            &[],
            &client_proof
        ));
        let server_proof = proof(&s, b"server", &ctx, &ticket);
        assert!(verify(&c, b"server", &ctx, &ticket, &server_proof));
        assert!(!verify(&c, b"server", &ctx, &[4; 32], &server_proof));
        assert_ne!(
            media_proof(&ticket, &exporter, &session),
            media_proof(&ticket, &[7; 32], &session)
        );
    }
    #[test]
    fn incorrect_password_never_confirms_shared_key() {
        let (client, a) = begin_client(b"123456");
        let (server, b) = begin_server(b"654321");
        let c = finish(client, &b, b'B').unwrap();
        let s = finish(server, &a, b'A').unwrap();
        assert_ne!(c, s);
        let ctx = context(&[1; 32], &[2; 16], &a, &b);
        assert!(!verify(
            &s,
            b"client",
            &ctx,
            &[],
            &proof(&c, b"client", &ctx, &[])
        ));
        assert!(!verify(
            &c,
            b"server",
            &ctx,
            &[3; 32],
            &proof(&s, b"server", &ctx, &[3; 32])
        ));
    }
    #[test]
    fn threshold_per_ip_success_reset_unblock_and_persistence() {
        let dir =
            std::env::temp_dir().join(format!("remote-auth-policy-{}", rand::random::<u64>()));
        let first: IpAddr = "10.0.0.1".parse().unwrap();
        let second: IpAddr = "10.0.0.2".parse().unwrap();
        let mut policy = Policy::load(dir.to_str().unwrap()).unwrap();
        for _ in 0..9 {
            policy.failure(first).unwrap();
        }
        assert!(!policy.blocked(first));
        assert_eq!(policy.entries[&first].failures, 9);
        policy.failure(second).unwrap();
        policy.success(second).unwrap();
        assert!(!policy.entries.contains_key(&second));
        policy.failure(first).unwrap();
        assert!(policy.blocked(first));
        policy.success(first).unwrap();
        assert!(policy.blocked(first));
        let mut restored = Policy::load(dir.to_str().unwrap()).unwrap();
        assert!(restored.blocked(first));
        assert!(!restored.blocked(second));
        restored.unblock(first).unwrap();
        assert!(!Policy::load(dir.to_str().unwrap()).unwrap().blocked(first));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(dir.join("auth-policy.json"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
        fs::remove_dir_all(dir).unwrap();
    }
}
