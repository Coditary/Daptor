use std::net::SocketAddr;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Instant;

use http_body_util::BodyExt;
use hudsucker::{
    certificate_authority::RcgenAuthority,
    hyper::{Request, Response},
    rcgen::{
        BasicConstraints, CertificateParams, DistinguishedName, DnType, IsCa, Issuer, KeyPair,
    },
    rustls::crypto::aws_lc_rs,
    *,
};
use tracing::{info, warn};

use super::capture::{CaptureInner, CapturedExchange};

const MAX_BODY_CHARS: usize = 16_384;

struct PendingCapture {
    method: String,
    uri: String,
    request_headers: String,
    request_body: String,
    started: Instant,
}

#[derive(Clone)]
struct CaptureHandler {
    inner: Arc<CaptureInner>,
    pending: Arc<Mutex<Option<PendingCapture>>>,
}

impl HttpHandler for CaptureHandler {
    async fn handle_request(
        &mut self,
        _ctx: &HttpContext,
        req: Request<Body>,
    ) -> RequestOrResponse {
        let method = req.method().to_string();
        let uri = req.uri().to_string();
        let request_headers = format_request_headers(&req);
        let (parts, body) = req.into_parts();
        let bytes = match body.collect().await {
            Ok(collected) => collected.to_bytes(),
            Err(error) => {
                warn!("network capture: failed to read request body: {error}");
                bytes::Bytes::new()
            }
        };
        let request_body = truncate_body(&bytes);
        let req = Request::from_parts(parts, Body::from(bytes.clone()));

        if let Ok(mut pending) = self.pending.lock() {
            *pending = Some(PendingCapture {
                method,
                uri,
                request_headers,
                request_body,
                started: Instant::now(),
            });
        }

        req.into()
    }

    async fn handle_response(&mut self, _ctx: &HttpContext, res: Response<Body>) -> Response<Body> {
        let pending = self.pending.lock().ok().and_then(|mut slot| slot.take());

        let (parts, body) = res.into_parts();
        let bytes = match body.collect().await {
            Ok(collected) => collected.to_bytes(),
            Err(error) => {
                warn!("network capture: failed to read response body: {error}");
                bytes::Bytes::new()
            }
        };

        if let Some(pending) = pending {
            let path = uri_path(&pending.uri);
            let status = parts.status.as_u16() as i32;
            let summary = format!("{status} {} {path}", pending.method);
            self.inner.push_exchange(CapturedExchange {
                id: self.inner.next_exchange_id(),
                state: "completed".into(),
                origin: "captured".into(),
                method: pending.method,
                status_code: status,
                duration_ms: pending.started.elapsed().as_millis().min(i32::MAX as u128) as i32,
                path,
                summary,
                request_headers: pending.request_headers,
                request_body: pending.request_body,
                response_headers: format_response_headers(&parts.headers),
                response_body: truncate_body(&bytes),
            });
        }

        Response::from_parts(parts, Body::from(bytes))
    }
}

pub fn spawn_capture_proxy() -> anyhow::Result<(String, PathBuf)> {
    let listener = std::net::TcpListener::bind("127.0.0.1:0")?;
    let addr = listener.local_addr()?;
    let proxy_address = addr.to_string();

    let ca_dir =
        std::env::temp_dir().join(format!("daptor-proxy-{}", proxy_address.replace(':', "-")));
    std::fs::create_dir_all(&ca_dir)?;
    let ca_cert_path = ca_dir.join("ca.pem");
    write_ca_files(&ca_dir)?;

    Ok((proxy_address, ca_cert_path))
}

pub fn start_proxy_server(inner: Arc<CaptureInner>) -> anyhow::Result<()> {
    let proxy_address = inner.proxy_address.clone();
    let addr: SocketAddr = proxy_address
        .parse()
        .map_err(|error| anyhow::anyhow!("invalid proxy address {proxy_address}: {error}"))?;
    let ca_dir = inner
        .ca_cert_path
        .parent()
        .ok_or_else(|| anyhow::anyhow!("missing proxy certificate directory"))?
        .to_path_buf();

    std::thread::Builder::new()
        .name("daptor-network-proxy".into())
        .spawn(move || {
            let runtime = tokio::runtime::Builder::new_multi_thread()
                .enable_all()
                .worker_threads(2)
                .build()
                .expect("network proxy tokio runtime");
            runtime.block_on(async move {
                if let Err(error) = run_proxy(addr, ca_dir, inner).await {
                    warn!("network capture proxy stopped: {error:#}");
                }
            });
        })
        .map_err(|error| anyhow::anyhow!("failed to spawn network proxy thread: {error}"))?;

    info!("network capture proxy listening on {}", proxy_address);
    Ok(())
}

async fn run_proxy(
    addr: SocketAddr,
    ca_dir: PathBuf,
    inner: Arc<CaptureInner>,
) -> anyhow::Result<()> {
    let ca_cert = std::fs::read_to_string(ca_dir.join("ca.pem"))?;
    let ca_key = std::fs::read_to_string(ca_dir.join("ca.key"))?;
    let key_pair = KeyPair::from_pem(&ca_key)?;
    let issuer = Issuer::from_ca_cert_pem(&ca_cert, key_pair)?;
    let ca = RcgenAuthority::new(issuer, 1_000, aws_lc_rs::default_provider());
    let handler = CaptureHandler { inner, pending: Arc::new(Mutex::new(None)) };
    let proxy = Proxy::builder()
        .with_addr(addr)
        .with_ca(ca)
        .with_rustls_connector(aws_lc_rs::default_provider())
        .with_http_handler(handler)
        .build()?;
    proxy.start().await?;
    Ok(())
}

fn write_ca_files(ca_dir: &std::path::Path) -> anyhow::Result<()> {
    let key_pair = KeyPair::generate()?;
    let mut params = CertificateParams::default();
    params.distinguished_name = DistinguishedName::new();
    params.distinguished_name.push(DnType::CommonName, "daptor network capture");
    params.is_ca = IsCa::Ca(BasicConstraints::Unconstrained);
    let cert = params.self_signed(&key_pair)?;
    std::fs::write(ca_dir.join("ca.pem"), cert.pem())?;
    std::fs::write(ca_dir.join("ca.key"), key_pair.serialize_pem())?;
    Ok(())
}

pub(crate) fn uri_path(uri: &str) -> String {
    if let Ok(parsed) = uri.parse::<hyper::Uri>() {
        if let Some(path) = parsed.path_and_query() {
            return path.as_str().to_string();
        }
        if !parsed.path().is_empty() {
            return parsed.path().to_string();
        }
    }
    uri.to_string()
}

fn format_request_headers(req: &Request<Body>) -> String {
    let mut lines = format!("{} {}\n", req.method(), req.uri());
    for (name, value) in req.headers().iter() {
        if let Ok(value) = value.to_str() {
            lines.push_str(name.as_str());
            lines.push_str(": ");
            lines.push_str(value);
            lines.push('\n');
        }
    }
    lines
}

fn format_response_headers(headers: &hyper::HeaderMap) -> String {
    let mut lines = String::new();
    for (name, value) in headers.iter() {
        if let Ok(value) = value.to_str() {
            if !lines.is_empty() {
                lines.push('\n');
            }
            lines.push_str(name.as_str());
            lines.push_str(": ");
            lines.push_str(value);
        }
    }
    lines
}

pub(crate) fn truncate_body(bytes: &bytes::Bytes) -> String {
    let text = String::from_utf8_lossy(bytes);
    if text.len() <= MAX_BODY_CHARS {
        return text.into_owned();
    }
    let mut truncated = text[..MAX_BODY_CHARS].to_string();
    truncated.push_str("\n… (truncated)");
    truncated
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn uri_path_extracts_path_and_query() {
        assert_eq!(uri_path("https://example.com/api/v1?q=1"), "/api/v1?q=1");
        assert_eq!(uri_path("/local/path"), "/local/path");
    }

    #[test]
    fn truncate_body_keeps_short_text_and_marks_long_text() {
        let short = bytes::Bytes::from_static(b"hello");
        assert_eq!(truncate_body(&short), "hello");

        let long = bytes::Bytes::from("x".repeat(MAX_BODY_CHARS + 10));
        let truncated = truncate_body(&long);
        assert!(truncated.ends_with("… (truncated)"));
        assert!(truncated.len() > MAX_BODY_CHARS);
    }
}
