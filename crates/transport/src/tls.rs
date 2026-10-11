use rustls::{
    client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier},
    pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime},
    DigitallySignedStruct, SignatureScheme,
};
use sha2::{Digest, Sha256};
use std::{fs, path::Path, sync::Arc};

const ALPN: &[u8] = b"remote-flat/2";
pub fn fingerprint(cert: &[u8]) -> String {
    Sha256::digest(cert)
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}
pub fn decode_pin(pin: &str) -> Result<[u8; 32], String> {
    let cleaned = pin.replace(':', "");
    if cleaned.len() != 64 {
        return Err("certificate pin must contain 64 SHA-256 hex digits".into());
    }
    let mut out = [0; 32];
    for (index, part) in cleaned.as_bytes().chunks_exact(2).enumerate() {
        let hex = std::str::from_utf8(part).map_err(|_| "invalid certificate pin")?;
        out[index] =
            u8::from_str_radix(hex, 16).map_err(|_| "invalid certificate pin hex digit")?;
    }
    Ok(out)
}
pub fn token_equal(a: &[u8], b: &[u8]) -> bool {
    let mut different = a.len() ^ b.len();
    for i in 0..a.len().max(b.len()) {
        different |= (a.get(i).copied().unwrap_or(0) ^ b.get(i).copied().unwrap_or(0)) as usize;
    }
    different == 0
}
pub fn server_config(dir: &str) -> Result<(quinn::ServerConfig, String), String> {
    let path = Path::new(dir);
    fs::create_dir_all(path).map_err(|e| format!("create identity directory: {e}"))?;
    let cert_path = path.join("identity-cert.der");
    let key_path = path.join("identity-key.der");
    let (cert, key) = if cert_path.exists() || key_path.exists() {
        (
            fs::read(&cert_path).map_err(|e| format!("read persisted certificate: {e}"))?,
            fs::read(&key_path).map_err(|e| format!("read persisted private key: {e}"))?,
        )
    } else {
        let pair = rcgen::generate_simple_self_signed(vec!["remote.local".into()])
            .map_err(|e| format!("generate identity: {e}"))?;
        let cert = pair.cert.der().to_vec();
        let key = pair.key_pair.serialize_der();
        // create_new refuses to overwrite another process's identity.
        write_new(&key_path, &key, true)?;
        write_new(&cert_path, &cert, false)?;
        (cert, key)
    };
    let fp = fingerprint(&cert);
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let mut crypto = rustls::ServerConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .with_no_client_auth()
        .with_single_cert(
            vec![CertificateDer::from(cert)],
            PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key)),
        )
        .map_err(|e| format!("identity certificate/key mismatch: {e}"))?;
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let quic =
        quinn::crypto::rustls::QuicServerConfig::try_from(crypto).map_err(|e| e.to_string())?;
    let mut config = quinn::ServerConfig::with_crypto(Arc::new(quic));
    config.transport_config(transport());
    Ok((config, fp))
}
fn write_new(path: &Path, bytes: &[u8], private: bool) -> Result<(), String> {
    use std::io::Write;
    let mut options = fs::OpenOptions::new();
    options.write(true).create_new(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(if private { 0o600 } else { 0o644 });
    }
    #[cfg(not(unix))]
    let _ = private;
    let mut file = options
        .open(path)
        .map_err(|e| format!("persist identity {}: {e}", path.display()))?;
    file.write_all(bytes)
        .and_then(|_| file.sync_all())
        .map_err(|e| format!("flush identity: {e}"))
}
pub fn transport() -> Arc<quinn::TransportConfig> {
    let mut config = quinn::TransportConfig::default();
    config.max_concurrent_uni_streams(8u32.into());
    config.max_concurrent_bidi_streams(2u32.into());
    config.keep_alive_interval(Some(std::time::Duration::from_secs(2)));
    config.max_idle_timeout(Some(std::time::Duration::from_secs(10).try_into().unwrap()));
    config.datagram_receive_buffer_size(Some(16 * 1024 * 1024));
    // Keep Quinn's hidden datagram backlog below ~20 ms of stereo PCM. All media
    // uses send_datagram_wait so sound never silently evicts video reference packets.
    // Video send deadlines still trigger explicit IDR recovery under congestion.
    config.datagram_send_buffer_size(4 * 1024);
    Arc::new(config)
}
#[derive(Debug)]
struct PinnedCertificate {
    pin: Option<[u8; 32]>,
    provider: Arc<rustls::crypto::CryptoProvider>,
}
impl ServerCertVerifier for PinnedCertificate {
    fn verify_server_cert(
        &self,
        cert: &CertificateDer<'_>,
        _: &[CertificateDer<'_>],
        _: &ServerName<'_>,
        _: &[u8],
        _: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        // With no prior identity, SPAKE2 plus a TLS-exporter-bound mutual proof
        // authenticates this connection before any application data is allowed.
        // TLS signatures are still verified below; no plaintext password is sent.
        if self
            .pin
            .is_none_or(|pin| token_equal(&Sha256::digest(cert.as_ref()), &pin))
        {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::General(
                "server certificate SHA-256 fingerprint does not match pinned identity".into(),
            ))
        }
    }
    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }
    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}
pub fn client_config(pin: Option<[u8; 32]>) -> Result<quinn::ClientConfig, String> {
    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let verifier = Arc::new(PinnedCertificate {
        pin,
        provider: provider.clone(),
    });
    let mut crypto = rustls::ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
        .map_err(|e| e.to_string())?
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let quic =
        quinn::crypto::rustls::QuicClientConfig::try_from(crypto).map_err(|e| e.to_string())?;
    let mut config = quinn::ClientConfig::new(Arc::new(quic));
    config.transport_config(transport());
    Ok(config)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn token_comparison_handles_length() {
        assert!(token_equal(b"paired-secret", b"paired-secret"));
        assert!(!token_equal(b"paired-secret", b"paired-secreu"));
        assert!(!token_equal(b"same", b"same\0"));
    }
    #[test]
    fn pin_parsing_is_strict() {
        assert!(decode_pin(&"ab".repeat(32)).is_ok());
        assert!(decode_pin("insecure").is_err());
        assert!(decode_pin(&"gg".repeat(32)).is_err());
    }
}
