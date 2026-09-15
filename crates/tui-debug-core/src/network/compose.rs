use std::time::Instant;

use anyhow::{Context, Result, bail};
use reqwest::blocking::Client;
use reqwest::header::{HeaderMap, HeaderName, HeaderValue};
use reqwest::{Method, StatusCode};

use super::capture::{CapturedExchange, CaptureInner};
use super::proxy::{truncate_body, uri_path};

const DEFAULT_TIMEOUT_MS: i32 = 30_000;
const MIN_TIMEOUT_MS: i32 = 1_000;
const MAX_TIMEOUT_MS: i32 = 600_000;

pub(crate) fn send_compose_request(
    capture: Option<&CaptureInner>,
    method: &str,
    url: &str,
    headers: &str,
    body: &str,
    timeout_ms: i32,
) -> Result<CapturedExchange> {
    let started = Instant::now();
    let parsed_method = parse_method(method)?;
    let header_map = parse_headers(headers)?;
    let timeout_ms = normalize_timeout_ms(timeout_ms);
    let client = Client::builder()
        .timeout(std::time::Duration::from_millis(timeout_ms as u64))
        .redirect(reqwest::redirect::Policy::limited(10))
        .build()
        .context("failed to build HTTP client")?;

    let mut request = client.request(parsed_method.clone(), url);
    request = request.headers(header_map);
    if !body.is_empty() && parsed_method != Method::GET && parsed_method != Method::HEAD {
        request = request.body(body.to_string());
    }

    let request_headers = format_compose_request_headers(&parsed_method, url, headers);
    let request_body = body.to_string();
    let path = uri_path(url);
    let id = capture
        .map(|inner| inner.next_exchange_id())
        .unwrap_or_else(|| "1".to_string());

    let exchange = match request.send() {
        Ok(response) => {
            let status = response.status();
            let response_headers = format_response_headers(status, response.headers());
            let response_bytes = response.bytes().context("failed to read response body")?;
            let response_body = truncate_body(&response_bytes);
            let status_code = status.as_u16() as i32;
            let duration_ms = started.elapsed().as_millis().min(i32::MAX as u128) as i32;
            let summary = format!("{status_code} {} {path}", parsed_method);
            CapturedExchange {
                id,
                state: "completed".into(),
                origin: "compose".into(),
                method: parsed_method.to_string(),
                status_code,
                duration_ms,
                path,
                summary,
                request_headers,
                request_body,
                response_headers,
                response_body,
            }
        }
        Err(error) => {
            let duration_ms = started.elapsed().as_millis().min(i32::MAX as u128) as i32;
            let summary = format!("failed {} {path}: {error}", parsed_method);
            CapturedExchange {
                id,
                state: "dropped".into(),
                origin: "compose".into(),
                method: parsed_method.to_string(),
                status_code: 0,
                duration_ms,
                path,
                summary,
                request_headers,
                request_body,
                response_headers: String::new(),
                response_body: error.to_string(),
            }
        }
    };

    // Compose sends are returned to the UI directly — never mixed into proxy capture.
    let _ = capture;
    Ok(exchange)
}

fn normalize_timeout_ms(timeout_ms: i32) -> i32 {
    if timeout_ms <= 0 {
        return DEFAULT_TIMEOUT_MS;
    }
    timeout_ms.clamp(MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)
}

fn parse_method(method: &str) -> Result<Method> {
    let trimmed = method.trim();
    if trimmed.is_empty() {
        bail!("HTTP method is required");
    }
    let upper = trimmed.to_ascii_uppercase();
    Method::from_bytes(upper.as_bytes()).with_context(|| format!("unsupported HTTP method: {trimmed}"))
}

fn parse_headers(headers: &str) -> Result<HeaderMap> {
    let mut map = HeaderMap::new();
    for line in headers.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let Some((name, value)) = line.split_once(':') else {
            continue;
        };
        let name = name.trim();
        let value = value.trim();
        if name.is_empty() || name.eq_ignore_ascii_case("host") {
            continue;
        }
        let header_name = HeaderName::from_bytes(name.as_bytes())
            .with_context(|| format!("invalid header name: {name}"))?;
        let header_value = HeaderValue::from_str(value)
            .with_context(|| format!("invalid header value for {name}"))?;
        map.insert(header_name, header_value);
    }
    Ok(map)
}

fn format_compose_request_headers(method: &Method, url: &str, headers: &str) -> String {
    let mut lines = format!("{} {}\n", method, url);
    for line in headers.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        lines.push_str(line);
        lines.push('\n');
    }
    lines
}

#[cfg(test)]
mod tests {
    use super::*;
    use reqwest::Method;

    #[test]
    fn parse_method_rejects_empty_and_unknown() {
        assert!(parse_method("").is_err());
        assert!(parse_method("   ").is_err());
        assert!(parse_method("GET POST").is_err());
        assert_eq!(parse_method("get").unwrap(), Method::GET);
        assert_eq!(parse_method("POST").unwrap(), Method::POST);
    }

    #[test]
    fn parse_headers_skips_host_and_malformed_lines() {
        let map = parse_headers(
            "Host: example.com\nAccept: application/json\nbroken-line\nX-Test: 1\n",
        )
        .expect("headers");
        assert!(!map.contains_key("host"));
        assert_eq!(map.get("accept").and_then(|v| v.to_str().ok()), Some("application/json"));
        assert_eq!(map.get("x-test").and_then(|v| v.to_str().ok()), Some("1"));
    }

    #[test]
    fn normalize_timeout_ms_clamps_and_defaults() {
        assert_eq!(normalize_timeout_ms(0), DEFAULT_TIMEOUT_MS);
        assert_eq!(normalize_timeout_ms(-5), DEFAULT_TIMEOUT_MS);
        assert_eq!(normalize_timeout_ms(500), MIN_TIMEOUT_MS);
        assert_eq!(normalize_timeout_ms(999_999), MAX_TIMEOUT_MS);
        assert_eq!(normalize_timeout_ms(5_000), 5_000);
    }

    #[test]
    fn format_compose_request_headers_includes_method_and_custom_headers() {
        let formatted = format_compose_request_headers(
            &Method::POST,
            "https://api.example.com/v1/items",
            "Content-Type: application/json\n",
        );
        assert!(formatted.starts_with("POST https://api.example.com/v1/items\n"));
        assert!(formatted.contains("Content-Type: application/json"));
    }

    #[test]
    #[ignore = "requires network access to httpbin.org"]
    fn send_compose_request_hits_real_http_endpoint() {
        let exchange = send_compose_request(
            None,
            "GET",
            "https://httpbin.org/get",
            "Accept: application/json",
            "",
            30_000,
        )
        .expect("compose GET should succeed");
        assert_eq!(exchange.origin, "compose");
        assert_eq!(exchange.method, "GET");
        assert!(exchange.status_code >= 200 && exchange.status_code < 300);
        assert!(exchange.response_body.contains("httpbin.org"));
    }
}

fn format_response_headers(status: StatusCode, headers: &HeaderMap) -> String {
    let mut lines = format!("HTTP/1.1 {}\n", status);
    for (name, value) in headers.iter() {
        if let Ok(value) = value.to_str() {
            lines.push_str(name.as_str());
            lines.push_str(": ");
            lines.push_str(value);
            lines.push('\n');
        }
    }
    lines
}
