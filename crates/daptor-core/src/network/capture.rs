use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use serde::Serialize;

use super::proxy;

#[derive(Clone, Debug, Serialize)]
pub struct CapturedExchange {
    pub id: String,
    pub state: String,
    pub origin: String,
    pub method: String,
    pub status_code: i32,
    pub duration_ms: i32,
    pub path: String,
    pub summary: String,
    pub request_headers: String,
    pub request_body: String,
    pub response_headers: String,
    pub response_body: String,
}

#[derive(Serialize)]
pub struct NetworkDrain {
    pub proxy_address: String,
    pub intercept_enabled: bool,
    pub exchanges: Vec<CapturedExchange>,
}

pub(crate) struct CaptureInner {
    pub proxy_address: String,
    pub ca_cert_path: PathBuf,
    pub pending: Mutex<Vec<CapturedExchange>>,
    pub next_id: AtomicU64,
}

impl CaptureInner {
    pub fn next_exchange_id(&self) -> String {
        self.next_id.fetch_add(1, Ordering::Relaxed).to_string()
    }

    pub fn push_exchange(&self, exchange: CapturedExchange) {
        if let Ok(mut pending) = self.pending.lock() {
            pending.push(exchange);
        }
    }
}

pub struct NetworkCapture {
    inner: Arc<CaptureInner>,
}

impl NetworkCapture {
    pub fn start() -> anyhow::Result<Self> {
        let (proxy_address, ca_cert_path) = proxy::spawn_capture_proxy()?;
        let inner = Arc::new(CaptureInner {
            proxy_address,
            ca_cert_path,
            pending: Mutex::new(Vec::new()),
            next_id: AtomicU64::new(1),
        });
        proxy::start_proxy_server(inner.clone())?;
        Ok(Self { inner })
    }

    pub fn proxy_address(&self) -> &str {
        &self.inner.proxy_address
    }

    pub fn ca_cert_path(&self) -> &Path {
        &self.inner.ca_cert_path
    }

    pub fn drain(&self) -> NetworkDrain {
        let exchanges = self
            .inner
            .pending
            .lock()
            .map(|mut pending| pending.drain(..).collect())
            .unwrap_or_default();
        NetworkDrain {
            proxy_address: self.inner.proxy_address.clone(),
            intercept_enabled: false,
            exchanges,
        }
    }
}
