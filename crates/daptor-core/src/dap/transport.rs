use std::io::{BufRead, Write};

use anyhow::{Context, Result};
use serde_json::Value;

/// Read one DAP message using Content-Length framing from `reader`.
pub fn read_message<R: BufRead>(reader: &mut R) -> Result<Value> {
    let body = read_message_bytes(reader)?;
    serde_json::from_slice(&body).context("failed to parse DAP JSON body")
}

/// Write one DAP message with Content-Length framing to `writer`.
pub fn write_message<W: Write>(writer: &mut W, message: &Value) -> Result<()> {
    let body = serde_json::to_vec(message).context("failed to serialize DAP message")?;
    write_message_bytes(writer, &body)
}

pub fn write_message_bytes<W: Write>(writer: &mut W, body: &[u8]) -> Result<()> {
    let header = format!("Content-Length: {}\r\n\r\n", body.len());
    writer.write_all(header.as_bytes()).context("failed to write DAP header")?;
    writer.write_all(body).context("failed to write DAP body")?;
    writer.flush().context("failed to flush DAP transport")?;
    Ok(())
}

fn read_message_bytes<R: BufRead>(reader: &mut R) -> Result<Vec<u8>> {
    let mut content_length: Option<usize> = None;

    loop {
        let mut line = String::new();
        let bytes = reader.read_line(&mut line).context("failed to read DAP header line")?;

        if bytes == 0 {
            anyhow::bail!("unexpected EOF while reading DAP header");
        }

        let line = line.trim_end_matches(['\r', '\n']);
        if line.is_empty() {
            break;
        }

        if let Some((key, value)) = line.split_once(':') {
            if key.eq_ignore_ascii_case("Content-Length") {
                content_length =
                    Some(value.trim().parse().context("invalid Content-Length value")?);
            }
        }
    }

    let length = content_length.context("DAP message missing Content-Length header")?;
    let mut body = vec![0u8; length];
    reader.read_exact(&mut body).context("failed to read DAP message body")?;
    Ok(body)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::io::Cursor;

    #[test]
    fn round_trip_message_framing() {
        let payload = json!({
            "seq": 1,
            "type": "request",
            "command": "initialize",
            "arguments": {}
        });

        let mut buffer = Vec::new();
        write_message(&mut buffer, &payload).expect("write");

        let mut reader = Cursor::new(buffer);
        let decoded = read_message(&mut reader).expect("read");
        assert_eq!(decoded, payload);
    }

    #[test]
    fn read_message_rejects_missing_content_length() {
        let mut reader = Cursor::new(b"\r\n{}");
        let error = read_message(&mut reader).expect_err("missing header");
        assert!(error.to_string().contains("Content-Length"));
    }
}
